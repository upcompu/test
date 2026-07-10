#include <esp_log.h>
#include <math.h>
#include <driver/ledc.h>
#include <driver/gpio.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <esp_matter.h>
#include <esp_matter_attribute_utils.h>

#include "app_priv.h"

static const char *TAG = "app_driver";

using namespace esp_matter;
using namespace esp_matter::attribute;

typedef struct {
    bool power;
    uint8_t brightness;
} led_strip_ctx_t;

static led_strip_ctx_t s_led_ctx = {
    .power = false,
    .brightness = DEFAULT_BRIGHTNESS,
};

#define LED_FADE_TIME_MS    200
#define LED_DEBOUNCE_MS     180

static esp_timer_handle_t s_debounce_timer = nullptr;

#define LED_GAMMA       2.2f
#define LED_MIN_DUTY_FRACTION   0.05f

static uint32_t compute_duty(bool power, uint8_t brightness)
{
    if (!power) {
        return 0;
    }

    uint32_t max_duty = (1 << LED_LEDC_DUTY_RES) - 1;

    if (brightness == 0) {
        return (uint32_t)(LED_MIN_DUTY_FRACTION * (float)max_duty);
    }

    float linear_fraction = (float)brightness / 254.0f;
    float corrected_fraction = powf(linear_fraction, LED_GAMMA);
    float final_fraction = LED_MIN_DUTY_FRACTION +
                            (1.0f - LED_MIN_DUTY_FRACTION) * corrected_fraction;

    uint32_t duty = (uint32_t)(final_fraction * (float)max_duty);

    return duty;
}

static void ledc_apply(void)
{
    uint32_t duty = compute_duty(s_led_ctx.power, s_led_ctx.brightness);

    ledc_set_fade_with_time(LED_LEDC_MODE, LED_LEDC_CHANNEL, duty, LED_FADE_TIME_MS);
    ledc_fade_start(LED_LEDC_MODE, LED_LEDC_CHANNEL, LEDC_FADE_NO_WAIT);

    ESP_LOGI(TAG, "LED strip: power=%d brightness=%d -> duty=%" PRIu32,
             s_led_ctx.power, s_led_ctx.brightness, duty);
}

static void debounce_timer_cb(void *arg)
{
    ledc_apply();
}

static void schedule_apply(void)
{
    if (s_debounce_timer == nullptr) {
        const esp_timer_create_args_t timer_args = {
            .callback = &debounce_timer_cb,
            .arg = nullptr,
            .dispatch_method = ESP_TIMER_TASK,
            .name = "led_debounce",
        };
        ESP_ERROR_CHECK(esp_timer_create(&timer_args, &s_debounce_timer));
    } else {
        esp_timer_stop(s_debounce_timer);
    }

    esp_timer_start_once(s_debounce_timer, (uint64_t)LED_DEBOUNCE_MS * 1000);
}

app_driver_handle_t app_driver_light_init(void)
{
    ledc_timer_config_t timer_cfg = {};
    timer_cfg.speed_mode      = LED_LEDC_MODE;
    timer_cfg.timer_num       = LED_LEDC_TIMER;
    timer_cfg.duty_resolution = LED_LEDC_DUTY_RES;
    timer_cfg.freq_hz         = LED_LEDC_FREQUENCY_HZ;
    timer_cfg.clk_cfg         = LEDC_AUTO_CLK;
    ESP_ERROR_CHECK(ledc_timer_config(&timer_cfg));

    ledc_channel_config_t ch_cfg = {};
    ch_cfg.gpio_num   = LED_STRIP_GPIO;
    ch_cfg.speed_mode = LED_LEDC_MODE;
    ch_cfg.channel    = LED_LEDC_CHANNEL;
    ch_cfg.timer_sel  = LED_LEDC_TIMER;
    ch_cfg.duty       = 0;
    ch_cfg.hpoint     = 0;
    ESP_ERROR_CHECK(ledc_channel_config(&ch_cfg));

    ESP_ERROR_CHECK(ledc_fade_func_install(0));

    ledc_apply();

    return (app_driver_handle_t)&s_led_ctx;
}

esp_err_t app_driver_light_set_power(app_driver_handle_t handle, bool power)
{
    s_led_ctx.power = power;
    schedule_apply();
    return ESP_OK;
}

esp_err_t app_driver_light_set_brightness(app_driver_handle_t handle, uint8_t brightness)
{
    if (brightness > 254) {
        brightness = 254;
    }

    s_led_ctx.brightness = brightness;
    schedule_apply();
    return ESP_OK;
}

esp_err_t app_driver_light_set_power_immediate(app_driver_handle_t handle, bool power)
{
    s_led_ctx.power = power;
    ledc_apply();
    return ESP_OK;
}

esp_err_t app_driver_light_set_brightness_immediate(app_driver_handle_t handle, uint8_t brightness)
{
    if (brightness > 254) {
        brightness = 254;
    }

    s_led_ctx.brightness = brightness;
    ledc_apply();
    return ESP_OK;
}

bool app_driver_light_get_power(app_driver_handle_t handle)
{
    return s_led_ctx.power;
}

uint8_t app_driver_light_get_brightness(app_driver_handle_t handle)
{
    return s_led_ctx.brightness;
}

esp_err_t app_driver_attribute_update(app_driver_handle_t driver_handle,
                                       uint16_t endpoint_id,
                                       uint32_t cluster_id,
                                       uint32_t attribute_id,
                                       void *val_ptr)
{
    esp_err_t err = ESP_OK;
    esp_matter_attr_val_t *val = (esp_matter_attr_val_t *)val_ptr;

    if (cluster_id == chip::app::Clusters::OnOff::Id) {
        if (attribute_id == chip::app::Clusters::OnOff::Attributes::OnOff::Id) {
            err = app_driver_light_set_power(driver_handle, val->val.b);
        }
    } else if (cluster_id == chip::app::Clusters::LevelControl::Id) {
        if (attribute_id == chip::app::Clusters::LevelControl::Attributes::CurrentLevel::Id) {
            err = app_driver_light_set_brightness(driver_handle, val->val.u8);
        }
    }

    return err;
}

typedef enum {
    BTN_STATE_IDLE,
    BTN_STATE_DEBOUNCE_PRESS,
    BTN_STATE_HELD,
    BTN_STATE_DIMMING,
    BTN_STATE_DEBOUNCE_RELEASE,
} btn_state_t;

static bool s_dim_direction_up = true;

static void button_task(void *arg)
{
    struct button_callbacks {
        app_button_callback_t on_press;
        app_button_dim_end_callback_t on_dim_end;
    } *callbacks = (struct button_callbacks *)arg;

    btn_state_t state = BTN_STATE_IDLE;
    TickType_t debounce_start = 0;
    TickType_t press_start = 0;
    TickType_t last_dim_step = 0;
    bool did_dim_this_session = false;

    while (true) {
        bool raw_pressed = (gpio_get_level((gpio_num_t)APP_BUTTON_GPIO) == 0);
        TickType_t now = xTaskGetTickCount();

        switch (state) {
        case BTN_STATE_IDLE:
            if (raw_pressed) {
                state = BTN_STATE_DEBOUNCE_PRESS;
                debounce_start = now;
            }
            break;

        case BTN_STATE_DEBOUNCE_PRESS:
            if (!raw_pressed) {
                state = BTN_STATE_IDLE;
            } else if ((now - debounce_start) >= pdMS_TO_TICKS(APP_BUTTON_DEBOUNCE_MS)) {
                state = BTN_STATE_HELD;
                press_start = now;
                did_dim_this_session = false;
            }
            break;

        case BTN_STATE_HELD:
            if (!raw_pressed) {
                state = BTN_STATE_DEBOUNCE_RELEASE;
                debounce_start = now;
            } else if ((now - press_start) >= pdMS_TO_TICKS(APP_BUTTON_LONGPRESS_MS)) {
                state = BTN_STATE_DIMMING;
                last_dim_step = now;
                did_dim_this_session = true;
            }
            break;

        case BTN_STATE_DIMMING:
            if (!raw_pressed) {
                state = BTN_STATE_DEBOUNCE_RELEASE;
                debounce_start = now;
            } else if ((now - last_dim_step) >= pdMS_TO_TICKS(APP_BUTTON_DIM_STEP_MS)) {
                last_dim_step = now;

                uint8_t current = app_driver_light_get_brightness(nullptr);
                int new_level = current;

                if (s_dim_direction_up) {
                    new_level += APP_BUTTON_DIM_STEP_SIZE;
                    if (new_level > 254) {
                        new_level = 254;
                    }
                } else {
                    new_level -= APP_BUTTON_DIM_STEP_SIZE;
                    if (new_level < 1) {
                        new_level = 1;
                    }
                }

                if (!app_driver_light_get_power(nullptr)) {
                    app_driver_light_set_power_immediate(nullptr, true);
                }
                app_driver_light_set_brightness_immediate(nullptr, (uint8_t)new_level);
            }
            break;

        case BTN_STATE_DEBOUNCE_RELEASE:
            if (raw_pressed) {
                state = did_dim_this_session ? BTN_STATE_DIMMING : BTN_STATE_HELD;
            } else if ((now - debounce_start) >= pdMS_TO_TICKS(APP_BUTTON_DEBOUNCE_MS)) {
                state = BTN_STATE_IDLE;

                if (did_dim_this_session) {
                    s_dim_direction_up = !s_dim_direction_up;
                    if (callbacks->on_dim_end) {
                        callbacks->on_dim_end();
                    }
                } else {
                    if (callbacks->on_press) {
                        callbacks->on_press();
                    }
                }
            }
            break;
        }

        vTaskDelay(pdMS_TO_TICKS(APP_BUTTON_POLL_MS));
    }
}

app_driver_handle_t app_driver_button_init(app_button_callback_t on_press,
                                            app_button_dim_end_callback_t on_dim_end)
{
    gpio_config_t io_conf = {};
    io_conf.pin_bit_mask = (1ULL << APP_BUTTON_GPIO);
    io_conf.mode = GPIO_MODE_INPUT;
    io_conf.pull_up_en = GPIO_PULLUP_ENABLE;
    io_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;
    io_conf.intr_type = GPIO_INTR_DISABLE;
    ESP_ERROR_CHECK(gpio_config(&io_conf));

    static struct {
        app_button_callback_t on_press;
        app_button_dim_end_callback_t on_dim_end;
    } s_callbacks;
    s_callbacks.on_press = on_press;
    s_callbacks.on_dim_end = on_dim_end;

    BaseType_t ret = xTaskCreate(button_task, "app_button", 3072, (void *)&s_callbacks, 5, NULL);
    if (ret != pdPASS) {
        ESP_LOGE(TAG, "Button task creation failed");
        return nullptr;
    }

    ESP_LOGI(TAG, "Button initialized on GPIO%d", APP_BUTTON_GPIO);

    return (app_driver_handle_t)1;
}

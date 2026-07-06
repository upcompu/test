/*
 * app_driver.cpp
 *
 * Driver jednobarevného LED pásku pro ESP32-C6.
 * LED pásek (12V/24V) se připojuje přes N-MOSFET (např. IRLZ44N) na GPIO,
 * ktery je rizen PWM signálem z periferie LEDC.
 *
 * Zapojení (příklad):
 *   GPIO8 --[1k]--> Gate MOSFETu
 *   Drain MOSFETu  -> (-) LED pásku
 *   Source MOSFETu -> GND (společná se zdrojem LED pásku)
 *   (+) LED pásku  -> +12V/+24V přímo ze zdroje
 *   GND zdroje LED pásku propojit s GND ESP32-C6
 */
#include <esp_log.h>
#include <driver/ledc.h>
#include <esp_matter.h>
#include <esp_matter_attribute_utils.h>
#include "app_priv.h"

static const char *TAG = "app_driver";

using namespace esp_matter;
using namespace esp_matter::attribute;

/* Interní stav driveru */
typedef struct {
    bool power;
    uint8_t brightness; /* 0-254 dle Matter Level Control */
} led_strip_ctx_t;

static led_strip_ctx_t s_led_ctx = {
    .power = DEFAULT_POWER,
    .brightness = DEFAULT_BRIGHTNESS,
};

/* Přepočet Matter úrovně (0-254) na LEDC duty (0-1023) s ohledem na power.
 *
 * POZOR - INVERTOVANA LOGIKA MOSFET MODULU:
 * Pouzity MOSFET modul ma aktivni-LOW chovani (0V na signalnim vstupu =
 * vystup ZAPNUT/propousti napeti, 3.3V na vstupu = vystup VYPNUT). Overeno
 * mereni: GPIO LOW (vypnuto v HA) -> ~22V na vystupu (skoro plny vykon),
 * GPIO trvale HIGH (100% jas) -> 0V na vystupu (zhasnuto). Proto tady
 * hodnotu duty invertujeme (max_duty - duty), aby vysledne chovani
 * pro uzivatele v aplikaci odpovidalo ocekavani (0 = zhasnuto, 100% = plny jas). */
static uint32_t compute_duty(bool power, uint8_t brightness)
{
    uint32_t max_duty = (1 << LED_LEDC_DUTY_RES) - 1; /* 1023 pro 10 bit */

    if (!power) {
        /* Chceme LED zhasnutou -> modul ma byt VYPNUTY -> GPIO musi byt HIGH -> duty = max */
        return max_duty;
    }

    uint32_t duty = (uint32_t)brightness * max_duty / 254;
    /* Invertujeme: vyssi pozadovany jas -> nizsi realne GPIO HIGH cas ->
     * modul (aktivni-LOW) je zapnuty vetsinu casu -> vice svetla na pasku. */
    return max_duty - duty;
}

static void ledc_apply(void)
{
    uint32_t duty = compute_duty(s_led_ctx.power, s_led_ctx.brightness);
    ledc_set_duty(LED_LEDC_MODE, LED_LEDC_CHANNEL, duty);
    ledc_update_duty(LED_LEDC_MODE, LED_LEDC_CHANNEL);
    ESP_LOGI(TAG, "LED strip: power=%d brightness=%d -> duty=%" PRIu32,
             s_led_ctx.power, s_led_ctx.brightness, duty);
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

    ledc_apply();

    return (app_driver_handle_t)&s_led_ctx;
}

esp_err_t app_driver_light_set_power(app_driver_handle_t handle, bool power)
{
    s_led_ctx.power = power;
    ledc_apply();
    return ESP_OK;
}

esp_err_t app_driver_light_set_brightness(app_driver_handle_t handle, uint8_t brightness)
{
    s_led_ctx.brightness = brightness;
    ledc_apply();
    return ESP_OK;
}

/*
 * Tato funkce se volá z app_main.cpp při každé změně atributu přijaté
 * z Matter fabric (např. z Apple Home / Google Home / Home Assistant
 * přes Thread Border Router).
 */
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

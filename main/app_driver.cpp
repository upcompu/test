/*
 * app_driver.cpp
 *
 * Driver jednobarevného LED pásku pro ESP32-C6.
 * LED pásek (12V/24V) se připojuje přes N-MOSFET (např. IRLZ44N) na GPIO,
 * ktery je rizen PWM signálem z periferie LEDC.
 *
 * Zapojení (příklad):
 *   GPIO18 --[1k]--> Gate MOSFETu
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
 * POZNAMKA K HISTORII: puvodne jsme se domnivali, ze MOSFET modul ma
 * invertovanou (aktivni-LOW) logiku, na zaklade mereni napeti na vystupu
 * BEZ pripojene zateze (LED pasku). Takove mereni naprazdno ale bylo
 * zavadejici - modul spina zapornou (-) vetev pasku (low-side zapojeni),
 * takze bez pripojene zateze byl vystupni bod elektricky "plovouci" a
 * multimetr zachytil nahodne, nesmyslne hodnoty. Realny test s pripojenym
 * LED paskem potvrdil, ze modul funguje SPRAVNE s touto standardni
 * (neinvertovanou) logikou - proto zustava puvodni, jednoducha varianta. */
static uint32_t compute_duty(bool power, uint8_t brightness)
{
    if (!power) {
        return 0;
    }
    uint32_t max_duty = (1 << LED_LEDC_DUTY_RES) - 1; /* 1023 pro 10 bit */
    uint32_t duty = (uint32_t)brightness * max_duty / 254;
    return duty;
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

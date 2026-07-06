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
#include <math.h>
#include <driver/ledc.h>
#include <esp_matter.h>
#include <esp_matter_attribute_utils.h>

#include "app_priv.h"

static const char *TAG = "app_driver";

using namespace esp_matter;
using namespace esp_matter::attribute;

/* Interní stav driveru.
 *
 * DULEZITE: power se VZDY inicializuje jako "false" (vypnuto), bez ohledu
 * na DEFAULT_POWER makro (to se pouziva jen pro pocatecni hodnotu Matter
 * atributu v app_main.cpp, ne pro hardware). Duvod: pokud by se hardware
 * hned pri startu rozsvitil (DEFAULT_POWER=true) a Matter/ZCL vzapeti
 * podle ulozene "StartUpOnOff" preference svetlo zase vypnul, dochazelo
 * by k viditelnemu bliknuti (vypnuto->zapnuto->vypnuto behem par ms) -
 * presne to, co bylo pozorovano. Zacit vzdy z bezpecneho "vypnuto" stavu
 * a nechat Matter/ZCL nastavit spravny stav podle ulozene preference
 * eliminuje tohle bliknuti uplne. */
typedef struct {
    bool power;
    uint8_t brightness; /* 0-254 dle Matter Level Control */
} led_strip_ctx_t;

static led_strip_ctx_t s_led_ctx = {
    .power = false,
    .brightness = DEFAULT_BRIGHTNESS,
};

/* Doba (v ms) hardwaroveho "fade" prechodu mezi soucasnym a novym duty
 * cyklem. Reseni bliknuti pri zapnuti/prepnuti - misto okamziteho skoku
 * z 0 na cilovou hodnotu LEDC hardwarove plynule "najede" na novou hodnotu,
 * coz eliminuje vizualni cvaknuti/bliknuti a navic vypada esteticky lepe
 * (jemne rozsviceni misto tvrdeho skoku). */
#define LED_FADE_TIME_MS    200

/* Gamma korekce pro linearni vnimani jasu lidskym okem.
 * Lidske oko vnima jas logaritmicky, ne linearne - primy linearni prevod
 * Matter urovne (0-254) na PWM duty cyklus zpusobuje, ze pri 50% Matter
 * urovne vypada pasek subjektivne mnohem jasnejsi nez "poloviste" (typicky
 * 70-80% vnimaneho jasu), a teprve blizko 0% jas rychle klesa. Standardni
 * reseni je aplikovat mocninnou (gamma) korekci na vstupni hodnotu pred
 * prevodem na PWM duty. Gamma ~2.2 je bezny, dobre osvedceny kompromis. */
#define LED_GAMMA       2.2f

/* Minimalni "podlaha" PWM duty cyklu (jako zlomek max_duty), pod kterou
 * nikdy nejdeme, pokud je svetlo zapnute. Bez tohohle by gamma korekce
 * pri nizkem jasu (napr. 5%) vytvorila extremne kratky impuls (< 1 mikrosekunda
 * pri nasi frekvenci 5kHz), ktery levny MOSFET modul nestiha spolehlive
 * zpracovat a vysledkem je viditelne blikani. S podlahou 5% mame jistotu
 * dostatecne dlouheho impulsu pro spolehlive spinani, a zbytek rozsahu jasu
 * (5%-100%) se namapuje s gamma korekci nad tuto podlahu. */
#define LED_MIN_DUTY_FRACTION   0.05f

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

    if (brightness == 0) {
        /* Svetlo "zapnute", ale s nulovym jasem z ovladace - drzime aspon
         * minimalni podlahu, ať pasek uplne nezhasne pri "on" s brightness=0. */
        return (uint32_t)(LED_MIN_DUTY_FRACTION * (float)max_duty);
    }

    /* Normalizace na rozsah 0.0 - 1.0 */
    float linear_fraction = (float)brightness / 254.0f;

    /* Gamma korekce - mocninna krivka misto primeho linearniho prevodu */
    float corrected_fraction = powf(linear_fraction, LED_GAMMA);

    /* Namapovani vysledku nad minimalni podlahu, aby zadny impuls nebyl
     * prilis kratky pro spolehlive spinani modulu. */
    float final_fraction = LED_MIN_DUTY_FRACTION +
                            (1.0f - LED_MIN_DUTY_FRACTION) * corrected_fraction;

    uint32_t duty = (uint32_t)(final_fraction * (float)max_duty);

    return duty;
}

/* Aplikace noveho duty cyklu s hardwarovym plynulym prechodem (fade),
 * misto okamziteho skoku - eliminuje bliknuti/cvaknuti pri zmene stavu. */
static void ledc_apply(void)
{
    uint32_t duty = compute_duty(s_led_ctx.power, s_led_ctx.brightness);

    ledc_set_fade_with_time(LED_LEDC_MODE, LED_LEDC_CHANNEL, duty, LED_FADE_TIME_MS);
    ledc_fade_start(LED_LEDC_MODE, LED_LEDC_CHANNEL, LEDC_FADE_NO_WAIT);

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

    /* Nutne pro pouziti ledc_set_fade_with_time() / ledc_fade_start() */
    ESP_ERROR_CHECK(ledc_fade_func_install(0));

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

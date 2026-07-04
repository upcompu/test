#pragma once

#include <esp_err.h>
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Handle na driver LED pásku (opaque typ, viz esp_matter driver API) */
typedef void *app_driver_handle_t;

/* GPIO, na který je připojen (přes MOSFET/tranzistor) jednobarevný LED pásek.
 * Uprav podle svého zapojení. */
#define LED_STRIP_GPIO          8

/* LEDC nastavení pro PWM stmívání */
#define LED_LEDC_TIMER          LEDC_TIMER_0
#define LED_LEDC_MODE           LEDC_LOW_SPEED_MODE
#define LED_LEDC_CHANNEL        LEDC_CHANNEL_0
#define LED_LEDC_DUTY_RES       LEDC_TIMER_10_BIT   /* 0-1023 */
#define LED_LEDC_FREQUENCY_HZ   5000                /* nad slyšitelné pásmo, bez blikání */

/* Výchozí stav po startu */
#define DEFAULT_POWER           true
#define DEFAULT_BRIGHTNESS      128   /* 0-254, dle Matter Level Control clusteru */

/* Inicializace GPIO/LEDC driveru LED pásku */
app_driver_handle_t app_driver_light_init(void);

/* Nastavení on/off */
esp_err_t app_driver_light_set_power(app_driver_handle_t handle, bool power);

/* Nastavení jasu, hodnota 0-254 (Matter CurrentLevel) */
esp_err_t app_driver_light_set_brightness(app_driver_handle_t handle, uint8_t brightness);

/* Callback volaný Matter stackem při změně libovolného atributu */
esp_err_t app_driver_attribute_update(app_driver_handle_t driver_handle,
                                       uint16_t endpoint_id,
                                       uint32_t cluster_id,
                                       uint32_t attribute_id,
                                       void *val);

/* Inicializace tlačítka pro factory reset / identify (volitelné, BOOT tlačítko) */
app_driver_handle_t app_driver_button_init(void);

#ifdef __cplusplus
}
#endif

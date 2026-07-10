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
 * POZOR: GPIO8 na vetsine ESP32-C6 DevKit desek je obsazeny vestavenou
 * adresovatelnou RGB LED (WS2812) - nase PWM signal se s ni bije a LED
 * zustava "zaseknuta" nezavisle na povelech.
 * GPIO10/11 NEJSOU vubec fyzicky vyvedeny na pin u modulu s vestavenou
 * flash pameti (potvrzeno oficialni ESP-IDF dokumentaci) - nepouzitelne.
 * Pouzivame GPIO18 - bezny, volny pin (neni strapping, neni USB-JTAG,
 * neni UART konzole [to je GPIO16/17], neni SPI flash [GPIO24-30]). */
#define LED_STRIP_GPIO          18

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

/* GPIO pro externi fyzicke tlacitko (vyvedene na krabici), zapojene mezi
 * GPIO a GND, spina proti zemi (pouzivame interni pull-up). GPIO2 je bezny,
 * volny pin - neni strapping (4,5,8,9,15), neni USB-JTAG (12,13), neni
 * UART konzole (16,17), neni SPI flash (24-30), neni vestavena RGB LED (8). */
#define APP_BUTTON_GPIO          2
#define APP_BUTTON_DEBOUNCE_MS   50
#define APP_BUTTON_POLL_MS       20

/* Prah pro rozliseni kratkeho stisku (toggle on/off) od podrzeni (stmivani).
 * Pokud je tlacitko drzeno dele nez tohle, prepne se do stmivaciho rezimu. */
#define APP_BUTTON_LONGPRESS_MS  400

/* Jak casto (ms) a o kolik urovni (z 0-254) se meni jas behem podrzeni. */
#define APP_BUTTON_DIM_STEP_MS   150
#define APP_BUTTON_DIM_STEP_SIZE 20

/* Typ callback funkce volane po platnem kratkem stisknuti (toggle on/off) */
typedef void (*app_button_callback_t)(void);

/* Typ callback funkce volane po skonceni stmivaci relace (podrzeni a
 * uvolneni) - umoznuje synchronizovat vysledny jas do Matter/appky. */
typedef void (*app_button_dim_end_callback_t)(void);

/* Inicializace externiho tlacitka.
 *   on_press   - zavola se po kratkem stisk+uvolneni cyklu (toggle on/off)
 *   on_dim_end - zavola se po uvolneni tlacitka, pokud predtim probehlo
 *                stmivani (podrzeni), aby se dal vysledny jas synchronizovat
 *                do Matter atributu */
app_driver_handle_t app_driver_button_init(app_button_callback_t on_press,
                                            app_button_dim_end_callback_t on_dim_end);

/* Cteni aktualniho stavu driveru - pro synchronizaci po stmivani tlacitkem */
bool app_driver_light_get_power(app_driver_handle_t handle);
uint8_t app_driver_light_get_brightness(app_driver_handle_t handle);

#ifdef __cplusplus
}
#endif

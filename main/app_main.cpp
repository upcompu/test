/*
 * app_main.cpp
 *
 * Matter over Thread zařízení: stmívatelné jednobarevné LED světlo
 * (Dimmable Light - device type 0x0101) pro ESP32-C6.
 */

#include <esp_err.h>
#include <esp_log.h>
#include <nvs_flash.h>

#include <esp_matter.h>
#include <esp_matter_console.h>
#include <esp_matter_ota.h>

#include <app/server/CommissioningWindowManager.h>
#include <app/server/Server.h>

#if CONFIG_ENABLE_CHIP_SHELL
#include <esp_matter_console.h>
#endif

#include "app_priv.h"

static const char *TAG = "app_main";

using namespace esp_matter;
using namespace esp_matter::attribute;
using namespace esp_matter::endpoint;
using namespace chip::app::Clusters;

static uint16_t light_endpoint_id = 0;

/* Handle na LED driver, ulozeny globalne pro pouziti v callbacku */
static app_driver_handle_t s_light_handle = nullptr;

/* Callback pro udalosti Matter stacku (commissioning, factory reset, ...) */
static void app_event_cb(const ChipDeviceEvent *event, intptr_t arg)
{
    switch (event->Type) {
    case chip::DeviceLayer::DeviceEventType::kInterfaceIpAddressChanged:
        ESP_LOGI(TAG, "Zmena IP adresy rozhrani");
        break;
    case chip::DeviceLayer::DeviceEventType::kCommissioningComplete:
        ESP_LOGI(TAG, "Commissioning dokoncen - zarizeni je sparovano");
        break;
    case chip::DeviceLayer::DeviceEventType::kThreadStateChange:
        ESP_LOGI(TAG, "Zmena stavu Thread site");
        break;
    default:
        break;
    }
}

/* Callback volany pri identifikaci (blikani pro "Identify" v Home apps) */
static esp_err_t app_identification_cb(identification::callback_type_t type, uint16_t endpoint_id,
                                        uint8_t effect_id, uint8_t effect_variant, void *priv_data)
{
    ESP_LOGI(TAG, "Identify callback: type=%d, endpoint=%d", type, endpoint_id);
    return ESP_OK;
}

/* Hlavni callback pri zmene libovolneho atributu z Matter site */
static esp_err_t app_attribute_update_cb(attribute::callback_type_t type, uint16_t endpoint_id,
                                          uint32_t cluster_id, uint32_t attribute_id,
                                          esp_matter_attr_val_t *val, void *priv_data)
{
    esp_err_t err = ESP_OK;

    if (type == attribute::PRE_UPDATE) {
        /* Zde muzeme zmenu jeste odmitnout, pokud by byla neplatna */
        if (endpoint_id == light_endpoint_id) {
            err = app_driver_attribute_update(s_light_handle, endpoint_id, cluster_id,
                                               attribute_id, (void *)val);
        }
    }

    return err;
}

extern "C" void app_main()
{
    esp_err_t err = ESP_OK;

    /* Inicializace NVS - Matter potrebuje pro ulozeni parovacich dat */
    err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    /* Inicializace hardwaroveho driveru LED pasku (LEDC PWM) */
    s_light_handle = app_driver_light_init();

    /* Vytvoreni Matter node */
    node::config_t node_config;
    node_t *node = node::create(&node_config, app_attribute_update_cb, app_identification_cb);
    if (!node) {
        ESP_LOGE(TAG, "Vytvoreni Matter node selhalo");
        abort();
    }

    /* Vytvoreni endpointu typu "Dimmable Light" (on/off + level control) */
    dimmable_light::config_t light_config;
    light_config.on_off.on_off = DEFAULT_POWER;
    light_config.level_control.current_level = DEFAULT_BRIGHTNESS;

    endpoint_t *light_endpoint = dimmable_light::create(node, &light_config, ENDPOINT_FLAG_NONE, s_light_handle);
    if (!light_endpoint) {
        ESP_LOGE(TAG, "Vytvoreni endpointu svetla selhalo");
        abort();
    }

    light_endpoint_id = endpoint::get_id(light_endpoint);
    ESP_LOGI(TAG, "Svetlo vytvoreno, endpoint_id=%d", light_endpoint_id);

    /* Volitelne: pridani OTA requestor clusteru pro update firmwaru pres Matter */
#if CONFIG_ENABLE_OTA_REQUESTOR
    ota_requestor::config_t ota_config;
    endpoint_t *ota_endpoint = ota_requestor::create(node, &ota_config, ENDPOINT_FLAG_NONE, nullptr);
#endif

    /* Spusteni Matter stacku (vcetne OpenThread pro Thread transport) */
    err = esp_matter::start(app_event_cb);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Spusteni Matter stacku selhalo: %d", err);
        abort();
    }

#if CONFIG_ENABLE_CHIP_SHELL
    esp_matter::console::diagnostics_register_commands();
    esp_matter::console::wifi_register_commands();
    esp_matter::console::factoryreset_register_commands();
    esp_matter::console::init();
#endif

    ESP_LOGI(TAG, "Matter over Thread zarizeni (LED pasek) pripraveno k parovani");
}

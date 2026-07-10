#include <esp_err.h>
#include <esp_log.h>
#include <nvs_flash.h>

#include <esp_matter.h>
#include <esp_matter_console.h>
#include <esp_matter_ota.h>

#include <app/server/CommissioningWindowManager.h>
#include <app/server/Server.h>
#include <setup_payload/OnboardingCodesUtil.h>
#include <platform/CHIPDeviceLayer.h>

#if CONFIG_OPENTHREAD_ENABLED
#include <platform/ESP32/OpenthreadLauncher.h>
#endif

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

static app_driver_handle_t s_light_handle = nullptr;

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

static esp_err_t app_identification_cb(identification::callback_type_t type, uint16_t endpoint_id,
                                        uint8_t effect_id, uint8_t effect_variant, void *priv_data)
{
    ESP_LOGI(TAG, "Identify callback: type=%d, endpoint=%d", type, endpoint_id);
    return ESP_OK;
}

/* Actual work done on the CHIP/Matter stack thread - safe to call
 * attribute::update() here. Called via ScheduleLambda from the button task. */
static void do_button_toggle_on_matter_thread(void)
{
    attribute_t *onoff_attr = attribute::get(light_endpoint_id, OnOff::Id,
                                               OnOff::Attributes::OnOff::Id);
    if (!onoff_attr) {
        ESP_LOGE(TAG, "Button: OnOff attribute not found");
        return;
    }

    esp_matter_attr_val_t val = esp_matter_invalid(NULL);
    attribute::get_val(onoff_attr, &val);

    if (val.type != ESP_MATTER_VAL_TYPE_BOOLEAN) {
        ESP_LOGE(TAG, "Button: unexpected OnOff attribute type");
        return;
    }

    val.val.b = !val.val.b;
    attribute::update(light_endpoint_id, OnOff::Id, OnOff::Attributes::OnOff::Id, &val);

    ESP_LOGI(TAG, "Button: short press - light toggled to power=%d", val.val.b);
}

static void do_button_dim_sync_on_matter_thread(void)
{
    bool power = app_driver_light_get_power(s_light_handle);
    uint8_t brightness = app_driver_light_get_brightness(s_light_handle);

    attribute_t *onoff_attr = attribute::get(light_endpoint_id, OnOff::Id,
                                               OnOff::Attributes::OnOff::Id);
    if (onoff_attr) {
        esp_matter_attr_val_t onoff_val = esp_matter_invalid(NULL);
        attribute::get_val(onoff_attr, &onoff_val);
        onoff_val.val.b = power;
        attribute::update(light_endpoint_id, OnOff::Id, OnOff::Attributes::OnOff::Id, &onoff_val);
    }

    attribute_t *level_attr = attribute::get(light_endpoint_id, LevelControl::Id,
                                               LevelControl::Attributes::CurrentLevel::Id);
    if (level_attr) {
        esp_matter_attr_val_t level_val = esp_matter_invalid(NULL);
        attribute::get_val(level_attr, &level_val);
        level_val.val.u8 = brightness;
        attribute::update(light_endpoint_id, LevelControl::Id,
                           LevelControl::Attributes::CurrentLevel::Id, &level_val);
    }

    ESP_LOGI(TAG, "Button: dim session ended - synced power=%d, brightness=%d",
             power, brightness);
}

/* These run in the button's own FreeRTOS task context. Matter attribute
 * API is not safe to call directly from a non-CHIP task, so we marshal
 * the actual work onto the CHIP/Matter stack thread via ScheduleLambda. */
static void app_button_pressed_cb(void)
{
    chip::DeviceLayer::SystemLayer().ScheduleLambda([]() {
        do_button_toggle_on_matter_thread();
    });
}

static void app_button_dim_end_cb(void)
{
    chip::DeviceLayer::SystemLayer().ScheduleLambda([]() {
        do_button_dim_sync_on_matter_thread();
    });
}

static esp_err_t app_attribute_update_cb(attribute::callback_type_t type, uint16_t endpoint_id,
                                          uint32_t cluster_id, uint32_t attribute_id,
                                          esp_matter_attr_val_t *val, void *priv_data)
{
    esp_err_t err = ESP_OK;

    if (type == attribute::PRE_UPDATE) {
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

    err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    s_light_handle = app_driver_light_init();

    node::config_t node_config;
    node_t *node = node::create(&node_config, app_attribute_update_cb, app_identification_cb);
    if (!node) {
        ESP_LOGE(TAG, "Vytvoreni Matter node selhalo");
        abort();
    }

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

    app_driver_button_init(app_button_pressed_cb, app_button_dim_end_cb);

#if CONFIG_ENABLE_OTA_REQUESTOR
    ota_requestor::config_t ota_config;
    endpoint_t *ota_endpoint = ota_requestor::create(node, &ota_config, ENDPOINT_FLAG_NONE, nullptr);
#endif

#if CONFIG_OPENTHREAD_ENABLED
    esp_openthread_platform_config_t ot_config = {};
    ot_config.radio_config.radio_mode = RADIO_MODE_NATIVE;
    ot_config.host_config.host_connection_mode = HOST_CONNECTION_MODE_NONE;
    ot_config.port_config.storage_partition_name = "nvs";
    ot_config.port_config.netif_queue_size = 10;
    ot_config.port_config.task_queue_size = 10;
    set_openthread_platform_config(&ot_config);
#endif

    err = esp_matter::start(app_event_cb);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Spusteni Matter stacku selhalo: %d", err);
        abort();
    }

    {
        attribute_t *onoff_attr = attribute::get(light_endpoint_id, OnOff::Id,
                                                   OnOff::Attributes::OnOff::Id);
        attribute_t *level_attr = attribute::get(light_endpoint_id, LevelControl::Id,
                                                   LevelControl::Attributes::CurrentLevel::Id);

        esp_matter_attr_val_t onoff_val = esp_matter_invalid(NULL);
        esp_matter_attr_val_t level_val = esp_matter_invalid(NULL);

        if (onoff_attr) {
            attribute::get_val(onoff_attr, &onoff_val);
        }
        if (level_attr) {
            attribute::get_val(level_attr, &level_val);
        }

        if (onoff_val.type == ESP_MATTER_VAL_TYPE_BOOLEAN) {
            app_driver_light_set_power(s_light_handle, onoff_val.val.b);
        }
        if (level_val.type == ESP_MATTER_VAL_TYPE_UINT8) {
            app_driver_light_set_brightness(s_light_handle, level_val.val.u8);
        }

        ESP_LOGI(TAG, "Pocatecni synchronizace hardwaru: power=%d, brightness=%d",
                 onoff_val.val.b, level_val.val.u8);
    }

#if CONFIG_ENABLE_CHIP_SHELL
    esp_matter::console::diagnostics_register_commands();
    esp_matter::console::wifi_register_commands();
    esp_matter::console::factoryreset_register_commands();
#if CONFIG_OPENTHREAD_CLI
    esp_matter::console::otcli_register_commands();
#endif
    esp_matter::console::init();
#endif

    PrintOnboardingCodes(chip::RendezvousInformationFlags(chip::RendezvousInformationFlag::kBLE));

    ESP_LOGI(TAG, "Matter over Thread zarizeni (LED pasek) pripraveno k parovani");
}

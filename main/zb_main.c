/*
 * SPDX-License-Identifier: CC0-1.0
 *
 * NOTE ON SDK VERSION: this targets esp-zigbee-sdk >=2.0.0, which dropped the
 * ZBOSS-based `esp_zb_*` API family in favor of the `ezb_*` API used below
 * (headers under the `ezbee` directory). If you've seen esp_zb_init()/esp_zb_ep_list_create()/
 * esp_zb_temperature_meas_cluster_create() in older tutorials, that's the pre-2.0
 * API - this SDK release ships a `CONFIG_ZB_SDK_1xx` Kconfig option that
 * re-exposes it as a compatibility shim if you'd rather use that surface, but
 * this file uses the current native API directly.
 */
#include <stdlib.h>

#include "esp_check.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_zigbee.h"
#include "ezbee/zha.h"

#include "app_config.h"
#include "relay.h"
#include "zb_main.h"

static const char *TAG = "ZB_MAIN";

/* ---------------------------------------------------------------------- */
/* Device role / platform configuration                                   */
/* ---------------------------------------------------------------------- */

/* Mains-powered relay controller -> Router, not the sleepy End Device
 * (EZB_NWK_DEVICE_TYPE_END_DEVICE / CONFIG_ZB_ZED) the stock SDK examples
 * default to. See sdkconfig.defaults for the matching CONFIG_ZB_ZCZR=y. */
#define ESP_ZIGBEE_ZR_CONFIG()                       \
    {                                                 \
        .device_type = EZB_NWK_DEVICE_TYPE_ROUTER,   \
        .install_code_policy = false,                \
        .zczr_config = {                             \
            .max_children = 10,                      \
        },                                            \
    }

/* ESP32-C6 has a native 802.15.4 radio - no UART/RCP split like classic
 * ESP32/S3 gateway setups need. */
#define ESP_ZIGBEE_PLATFORM_CONFIG()                                    \
    {                                                                   \
        .storage_partition_name = APP_ZB_STORAGE_PARTITION_NAME,       \
        .radio_config = {                                               \
            .radio_mode = ESP_ZIGBEE_RADIO_MODE_NATIVE,                 \
        },                                                              \
    }

#define ESP_ZIGBEE_DEFAULT_CONFIG()                      \
    {                                                     \
        .device_config = ESP_ZIGBEE_ZR_CONFIG(),          \
        .platform_config = ESP_ZIGBEE_PLATFORM_CONFIG(),  \
    }

/* Joining an existing SLZB-06-coordinated network without knowing its
 * channel in advance - scan every 2.4GHz channel (11-26) rather than the
 * single hardcoded channel the SDK's own CI examples use. */
#define ESP_ZIGBEE_PRIMARY_CHANNEL_MASK   (0x07FFF800U)
#define ESP_ZIGBEE_SECONDARY_CHANNEL_MASK (0x00000000U)

/* ---------------------------------------------------------------------- */
/* State shared with the control loop (pump_control.c) via zb_main.h       */
/* ---------------------------------------------------------------------- */

/* Guards the SET_ATTR_VALUE callback below from mistaking our own
 * automatic-control writes for a manual override from the network.
 *
 * ASSUMPTION TO VERIFY: this assumes ezb_zcl_set_attr_value() invokes the
 * EZB_ZCL_CORE_SET_ATTR_VALUE_CB_ID callback synchronously, in the same call
 * stack, before returning - so setting this flag immediately before the call
 * and clearing it immediately after is sufficient. Confirm this holds (e.g.
 * by logging timestamps) before relying on it; if the callback is ever
 * dispatched asynchronously instead, this flag needs to become a queue/tag
 * keyed on the write instead of a simple boolean. */
static volatile bool s_applying_local_pump_state = false;

static volatile bool    s_manual_override_active = false;
static volatile int64_t s_manual_override_deadline_us = 0;

/* ---------------------------------------------------------------------- */
/* One-shot retry timer (mirrors the SDK examples' `alarm_timer` utility,   */
/* reimplemented inline so this project doesn't depend on the SDK repo's   */
/* internal examples/utils components).                                   */
/* ---------------------------------------------------------------------- */

typedef struct {
    esp_timer_handle_t timer;
    ezb_bdb_comm_mode_mask_t mode;
} zb_retry_ctx_t;

static void zb_retry_timer_cb(void *arg)
{
    zb_retry_ctx_t *ctx = (zb_retry_ctx_t *)arg;
    esp_timer_handle_t timer = ctx->timer;

    esp_zigbee_lock_acquire(portMAX_DELAY);
    ezb_bdb_start_top_level_commissioning(ctx->mode);
    esp_zigbee_lock_release();

    esp_timer_delete(timer);
    free(ctx);
}

static void zb_schedule_commissioning_retry(ezb_bdb_comm_mode_mask_t mode, uint32_t delay_ms)
{
    zb_retry_ctx_t *ctx = calloc(1, sizeof(*ctx));
    if (ctx == NULL) {
        ESP_LOGE(TAG, "Out of memory scheduling commissioning retry");
        return;
    }
    ctx->mode = mode;

    const esp_timer_create_args_t timer_args = {
        .callback = zb_retry_timer_cb,
        .arg = ctx,
        .name = "zb_retry",
    };
    if (esp_timer_create(&timer_args, &ctx->timer) != ESP_OK) {
        free(ctx);
        return;
    }
    esp_timer_start_once(ctx->timer, (uint64_t)delay_ms * 1000);
}

/* ---------------------------------------------------------------------- */
/* Attribute reporting configuration (Temperature Measurement, ep 10/11)   */
/* ---------------------------------------------------------------------- */

static void zb_configure_temperature_reporting(uint8_t endpoint_id)
{
    ezb_zcl_reporting_info_t info = ezb_zcl_reporting_info_find(
        endpoint_id, EZB_ZCL_CLUSTER_ID_TEMPERATURE_MEASUREMENT, EZB_ZCL_CLUSTER_SERVER,
        EZB_ZCL_ATTR_TEMPERATURE_MEASUREMENT_MEASURED_VALUE_ID, EZB_ZCL_STD_MANUF_CODE);
    if (info == EZB_ZCL_INVALID_REPORTING_INFO) {
        ESP_LOGW(TAG, "No reporting slot found for endpoint %d MeasuredValue", endpoint_id);
        return;
    }

    ezb_zcl_attr_variable_t delta = { .s16 = APP_REPORT_DELTA_CENTIDEGREES };
    ezb_err_t err = ezb_zcl_reporting_info_update(info, APP_REPORT_MIN_INTERVAL_S, APP_REPORT_MAX_INTERVAL_S, &delta);
    if (err != EZB_ERR_NONE) {
        ESP_LOGW(TAG, "Failed to configure reporting for endpoint %d (0x%04x)", endpoint_id, err);
        return;
    }
    ezb_zcl_reporting_start_attr_report(info);
    ESP_LOGI(TAG, "Configured reporting for endpoint %d: min=%ds max=%ds delta=%d centidegrees", endpoint_id,
             APP_REPORT_MIN_INTERVAL_S, APP_REPORT_MAX_INTERVAL_S, APP_REPORT_DELTA_CENTIDEGREES);
}

static void zb_configure_all_reporting(void)
{
    zb_configure_temperature_reporting(APP_EP_THERMISTOR_1);
    zb_configure_temperature_reporting(APP_EP_THERMISTOR_2);
}

/* ---------------------------------------------------------------------- */
/* On/Off cluster write handling (Home Assistant/Z2M -> device)            */
/* ---------------------------------------------------------------------- */

static void zb_on_off_attr_change_handler(ezb_zcl_set_attr_value_message_t *message)
{
    if (message == NULL || message->info.dst_ep != APP_EP_PUMP_RELAY ||
        message->info.cluster_id != EZB_ZCL_CLUSTER_ID_ON_OFF ||
        message->in.attribute.id != EZB_ZCL_ATTR_ON_OFF_ON_OFF_ID) {
        return;
    }

    bool on = *(uint8_t *)message->in.attribute.data.value;

    if (!s_applying_local_pump_state) {
        s_manual_override_active = true;
        s_manual_override_deadline_us = esp_timer_get_time() + (int64_t)APP_MANUAL_OVERRIDE_TIMEOUT_MS * 1000;
        ESP_LOGI(TAG, "Manual On/Off write from network: %s (overriding automatic control for %d min)",
                 on ? "ON" : "OFF", APP_MANUAL_OVERRIDE_TIMEOUT_MS / 60000);
    }

    relay_set(on);
}

static void zb_zcl_core_action_handler(ezb_zcl_core_action_callback_id_t callback_id, void *message)
{
    switch (callback_id) {
    case EZB_ZCL_CORE_SET_ATTR_VALUE_CB_ID:
        zb_on_off_attr_change_handler((ezb_zcl_set_attr_value_message_t *)message);
        break;
    case EZB_ZCL_CORE_DEFAULT_RSP_CB_ID: {
        ezb_zcl_cmd_default_rsp_message_t *rsp = (ezb_zcl_cmd_default_rsp_message_t *)message;
        ESP_LOGD(TAG, "Received ZCL Default Response: status(0x%02x)", rsp->in.status_code);
    } break;
    default:
        ESP_LOGD(TAG, "Unhandled ZCL Core Action: ID(0x%04lx)", callback_id);
        break;
    }
}

/* ---------------------------------------------------------------------- */
/* BDB commissioning signal handler (mandatory boilerplate)                */
/* ---------------------------------------------------------------------- */

static bool zb_app_signal_handler(const ezb_app_signal_t *app_signal)
{
    ezb_app_signal_type_t signal_type = ezb_app_signal_get_type(app_signal);

    switch (signal_type) {
    case EZB_ZDO_SIGNAL_SKIP_STARTUP:
        ESP_LOGI(TAG, "Initialize Zigbee stack");
        ezb_bdb_start_top_level_commissioning(EZB_BDB_MODE_INITIALIZATION);
        break;
    case EZB_BDB_SIGNAL_DEVICE_FIRST_START:
    case EZB_BDB_SIGNAL_DEVICE_REBOOT: {
        ezb_bdb_comm_status_t status = *((ezb_bdb_comm_status_t *)ezb_app_signal_get_params(app_signal));
        if (status == EZB_BDB_STATUS_SUCCESS) {
            /* Endpoint/cluster/attribute tables are rebuilt from scratch every
             * boot (see zb_main_task below), so reporting info has to be
             * (re)configured every boot too - not just on first commissioning. */
            zb_configure_all_reporting();
            ESP_LOGI(TAG, "Device started up in%s factory-reset mode", ezb_bdb_is_factory_new() ? "" : " non");
            if (ezb_bdb_is_factory_new()) {
                ezb_bdb_start_top_level_commissioning(EZB_BDB_MODE_NETWORK_STEERING);
            } else {
                ESP_LOGI(TAG, "Device reboot, already joined - rejoining automatically");
            }
        } else {
            ESP_LOGW(TAG, "%s failed with status(0x%02x), retrying", ezb_app_signal_to_string(signal_type), status);
            zb_schedule_commissioning_retry(EZB_BDB_MODE_INITIALIZATION, 1000);
        }
    } break;
    case EZB_BDB_SIGNAL_STEERING: {
        ezb_bdb_comm_status_t status = *((ezb_bdb_comm_status_t *)ezb_app_signal_get_params(app_signal));
        if (status == EZB_BDB_STATUS_SUCCESS) {
            ezb_extpanid_t extended_pan_id;
            ezb_nwk_get_extended_panid(&extended_pan_id);
            ESP_LOGI(TAG, "Joined network successfully: PAN ID(0x%04hx, EXT: 0x%llx), Channel(%d), Short Address(0x%04hx)",
                     ezb_nwk_get_panid(), extended_pan_id.u64, ezb_nwk_get_current_channel(), ezb_nwk_get_short_address());
        } else {
            ESP_LOGW(TAG, "Failed to join network with status(0x%02x), retrying", status);
            zb_schedule_commissioning_retry(EZB_BDB_MODE_NETWORK_STEERING, 1000);
        }
    } break;
    case EZB_ZDO_SIGNAL_LEAVE: {
        const ezb_zdo_signal_leave_params_t *leave_params = ezb_app_signal_get_params(app_signal);
        ESP_LOGI(TAG, "Left network with type(0x%02x)", leave_params->leave_type);
    } break;
    case EZB_NWK_SIGNAL_PERMIT_JOIN_STATUS: {
        uint8_t duration = *(uint8_t *)ezb_app_signal_get_params(app_signal);
        if (duration) {
            ESP_LOGI(TAG, "Network(0x%04hx) is open for %d seconds", ezb_nwk_get_panid(), duration);
        } else {
            ESP_LOGW(TAG, "Network(0x%04hx) closed, devices joining not allowed.", ezb_nwk_get_panid());
        }
    } break;
    default:
        ESP_LOGI(TAG, "Zigbee APP Signal: %s(type: 0x%02x)", ezb_app_signal_to_string(signal_type), signal_type);
        break;
    }
    return true;
}

/* ---------------------------------------------------------------------- */
/* Endpoint / cluster / device descriptor creation                        */
/* ---------------------------------------------------------------------- */

static void zb_set_basic_cluster_strings(ezb_af_ep_desc_t ep_desc)
{
    ezb_zcl_cluster_desc_t basic_desc = ezb_af_endpoint_get_cluster_desc(ep_desc, EZB_ZCL_CLUSTER_ID_BASIC, EZB_ZCL_CLUSTER_SERVER);
    ezb_zcl_basic_cluster_desc_add_attr(basic_desc, EZB_ZCL_ATTR_BASIC_MANUFACTURER_NAME_ID, (void *)APP_ZB_MANUFACTURER_NAME);
    ezb_zcl_basic_cluster_desc_add_attr(basic_desc, EZB_ZCL_ATTR_BASIC_MODEL_IDENTIFIER_ID, (void *)APP_ZB_MODEL_IDENTIFIER);
}

static esp_err_t zb_create_device(void)
{
    ezb_af_device_desc_t dev_desc = ezb_af_create_device_desc();

    ezb_zha_temperature_sensor_config_t therm_cfg = EZB_ZHA_TEMPERATURE_SENSOR_CONFIG();
    therm_cfg.temp_meas_cfg.min_measured_value = APP_TEMP_SENSOR_MIN_CENTIDEGREES;
    therm_cfg.temp_meas_cfg.max_measured_value = APP_TEMP_SENSOR_MAX_CENTIDEGREES;

    ezb_af_ep_desc_t therm1_ep = ezb_zha_create_temperature_sensor(APP_EP_THERMISTOR_1, &therm_cfg);
    ezb_af_ep_desc_t therm2_ep = ezb_zha_create_temperature_sensor(APP_EP_THERMISTOR_2, &therm_cfg);
    zb_set_basic_cluster_strings(therm1_ep);
    zb_set_basic_cluster_strings(therm2_ep);
    ESP_RETURN_ON_ERROR(ezb_af_device_add_endpoint_desc(dev_desc, therm1_ep), TAG, "Failed to add thermistor 1 endpoint");
    ESP_RETURN_ON_ERROR(ezb_af_device_add_endpoint_desc(dev_desc, therm2_ep), TAG, "Failed to add thermistor 2 endpoint");

    /* "Mains Power Outlet" is the SDK's dedicated HA device type for a
     * mains-powered switched load (device ID 0x0009) - same On/Off cluster
     * shape as ezb_zha_create_on_off_light(), but semantically correct for a
     * relay driving a pump rather than a light. */
    ezb_zha_mains_power_outlet_config_t pump_cfg = EZB_ZHA_MAINS_POWER_OUTLET_CONFIG();
    ezb_af_ep_desc_t pump_ep = ezb_zha_create_mains_power_outlet(APP_EP_PUMP_RELAY, &pump_cfg);
    zb_set_basic_cluster_strings(pump_ep);
    ESP_RETURN_ON_ERROR(ezb_af_device_add_endpoint_desc(dev_desc, pump_ep), TAG, "Failed to add pump relay endpoint");

    ESP_RETURN_ON_ERROR(ezb_af_device_desc_register(dev_desc), TAG, "Failed to register device descriptor");

    ezb_zcl_core_action_handler_register(zb_zcl_core_action_handler);

    return ESP_OK;
}

static esp_err_t zb_setup_commissioning(void)
{
    ezb_aps_secur_enable_distributed_security(false);
    ESP_RETURN_ON_ERROR(ezb_bdb_set_primary_channel_set(ESP_ZIGBEE_PRIMARY_CHANNEL_MASK), TAG, "Failed to set primary channel set");
    ESP_RETURN_ON_ERROR(ezb_bdb_set_secondary_channel_set(ESP_ZIGBEE_SECONDARY_CHANNEL_MASK), TAG, "Failed to set secondary channel set");
    ESP_RETURN_ON_ERROR(ezb_app_signal_add_handler(zb_app_signal_handler), TAG, "Failed to register signal handler");
    return ESP_OK;
}

static void zb_main_task(void *pvParameters)
{
    esp_zigbee_config_t config = ESP_ZIGBEE_DEFAULT_CONFIG();

    ESP_ERROR_CHECK(esp_zigbee_init(&config));
    ESP_ERROR_CHECK(zb_setup_commissioning());
    ESP_ERROR_CHECK(zb_create_device());
    ESP_ERROR_CHECK(esp_zigbee_start(false));

    esp_zigbee_launch_mainloop();

    esp_zigbee_deinit();
    vTaskDelete(NULL);
}

/* ---------------------------------------------------------------------- */
/* Public API                                                              */
/* ---------------------------------------------------------------------- */

void zb_main_start(void)
{
    ESP_ERROR_CHECK(nvs_flash_init_partition(APP_ZB_STORAGE_PARTITION_NAME));
    xTaskCreate(zb_main_task, "Zigbee_main", 4096, NULL, 5, NULL);
}

void zb_main_report_temperature(uint8_t endpoint_id, int16_t centidegrees)
{
    esp_zigbee_lock_acquire(portMAX_DELAY);
    ezb_zcl_set_attr_value(endpoint_id, EZB_ZCL_CLUSTER_ID_TEMPERATURE_MEASUREMENT, EZB_ZCL_CLUSTER_SERVER,
                           EZB_ZCL_ATTR_TEMPERATURE_MEASUREMENT_MEASURED_VALUE_ID, EZB_ZCL_STD_MANUF_CODE,
                           &centidegrees, false);
    esp_zigbee_lock_release();
}

void zb_main_set_pump_state(bool on)
{
    uint8_t value = on ? 1 : 0;

    s_applying_local_pump_state = true;
    esp_zigbee_lock_acquire(portMAX_DELAY);
    ezb_zcl_set_attr_value(APP_EP_PUMP_RELAY, EZB_ZCL_CLUSTER_ID_ON_OFF, EZB_ZCL_CLUSTER_SERVER,
                           EZB_ZCL_ATTR_ON_OFF_ON_OFF_ID, EZB_ZCL_STD_MANUF_CODE, &value, false);
    esp_zigbee_lock_release();
    s_applying_local_pump_state = false;
}

bool zb_main_is_manual_override_active(void)
{
    if (s_manual_override_active && esp_timer_get_time() >= s_manual_override_deadline_us) {
        s_manual_override_active = false;
        ESP_LOGI(TAG, "Manual override window elapsed; resuming automatic pump control");
    }
    return s_manual_override_active;
}

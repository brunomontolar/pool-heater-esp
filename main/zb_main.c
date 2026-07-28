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

#include "driver/gpio.h"
#include "esp_check.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_zigbee.h"
#include "ezbee/zha.h"

#include "app_config.h"
#include "led.h"
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

/* True while a button-triggered pairing window (see zb_start_pairing_mode())
 * is open. Distinguishes an explicit, time-boxed join attempt (which gives
 * up and stops retrying after APP_PAIRING_WINDOW_MS) from the ordinary
 * background reconnect attempted on every boot when the device is already
 * paired (which retries indefinitely) - both go through the same
 * EZB_BDB_SIGNAL_STEERING signal, so this flag is how that handler tells
 * them apart. Also drives the onboard LED blink in zb_boot_button_task. */
static volatile bool    s_pairing_mode_active = false;
static volatile int64_t s_pairing_deadline_us = 0;

/* Mirrors of the two control-gate endpoints below, read by pump_control.c's
 * hysteresis loop every cycle. Initialized to these compiled-in defaults,
 * then overwritten by zb_load_persisted_state() at boot if a previously
 * persisted value exists in NVS (see the persistence section below). */
static volatile bool    s_auto_control_enabled = true;
static volatile int16_t s_pool_setpoint_centidegrees = APP_DEFAULT_POOL_SETPOINT_CENTIDEGREES;

/* ---------------------------------------------------------------------- */
/* NVS persistence for s_auto_control_enabled / s_pool_setpoint_centidegrees */
/* ---------------------------------------------------------------------- */

/* Loads any previously-persisted values over the compiled-in defaults above.
 * Must run before zb_create_device() applies s_auto_control_enabled /
 * s_pool_setpoint_centidegrees to the ZCL attributes' startup values. Missing
 * namespace/keys (fresh device, or NVS erased independently of zb_storage)
 * just leave the defaults in place. */
static void zb_load_persisted_state(void)
{
    nvs_handle_t handle;
    if (nvs_open(APP_NVS_NAMESPACE, NVS_READONLY, &handle) != ESP_OK) {
        return;
    }

    uint8_t auto_enable;
    if (nvs_get_u8(handle, APP_NVS_KEY_AUTO_ENABLE, &auto_enable) == ESP_OK) {
        s_auto_control_enabled = auto_enable;
    }

    int16_t setpoint;
    if (nvs_get_i16(handle, APP_NVS_KEY_SETPOINT, &setpoint) == ESP_OK) {
        s_pool_setpoint_centidegrees = setpoint;
    }

    nvs_close(handle);
    ESP_LOGI(TAG, "Loaded persisted state: auto_control=%s setpoint=%dcC",
             s_auto_control_enabled ? "on" : "off", s_pool_setpoint_centidegrees);
}

static void zb_persist_auto_enable(bool enabled)
{
    nvs_handle_t handle;
    if (nvs_open(APP_NVS_NAMESPACE, NVS_READWRITE, &handle) != ESP_OK) {
        ESP_LOGW(TAG, "Failed to open NVS to persist auto-enable state");
        return;
    }
    nvs_set_u8(handle, APP_NVS_KEY_AUTO_ENABLE, enabled ? 1 : 0);
    nvs_commit(handle);
    nvs_close(handle);
}

static void zb_persist_setpoint(int16_t centidegrees)
{
    nvs_handle_t handle;
    if (nvs_open(APP_NVS_NAMESPACE, NVS_READWRITE, &handle) != ESP_OK) {
        ESP_LOGW(TAG, "Failed to open NVS to persist pool setpoint");
        return;
    }
    nvs_set_i16(handle, APP_NVS_KEY_SETPOINT, centidegrees);
    nvs_commit(handle);
    nvs_close(handle);
}

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
/* Attribute reporting configuration (Temperature Measurement ep 10/11,    */
/* On/Off ep 12/13, Thermostat OccupiedHeatingSetpoint ep 14)              */
/* ---------------------------------------------------------------------- */

/* Shared by the per-cluster helpers below. `delta` is the ZCL "reportable
 * change" field - meaningful only for analog attribute types (temperature,
 * setpoint); callers configuring a discrete attribute (On/Off) pass a
 * zero-initialized variable since it doesn't apply there.
 *
 * ASSUMPTION TO VERIFY: this assumes every endpoint/cluster/attribute passed
 * in has a reporting slot pre-allocated by the ZHA config macros in
 * zb_create_device() - true for Temperature Measurement's MeasuredValue
 * (confirmed working), unconfirmed for On/Off's OnOff attribute and
 * especially for Thermostat's OccupiedHeatingSetpoint (added to the cluster
 * descriptor after creation as an optional attribute, see zb_create_device).
 * If unallocated, ezb_zcl_reporting_info_find() below just logs a warning at
 * boot and that attribute falls back to read/write-only (no push reports) -
 * check the boot log after flashing. */
static void zb_configure_reporting(uint8_t endpoint_id, uint16_t cluster_id, uint16_t attr_id,
                                    uint16_t min_interval_s, uint16_t max_interval_s,
                                    const ezb_zcl_attr_variable_t *delta, const char *label)
{
    ezb_zcl_reporting_info_t info = ezb_zcl_reporting_info_find(
        endpoint_id, cluster_id, EZB_ZCL_CLUSTER_SERVER, attr_id, EZB_ZCL_STD_MANUF_CODE);
    if (info == EZB_ZCL_INVALID_REPORTING_INFO) {
        ESP_LOGW(TAG, "No reporting slot found for endpoint %d %s", endpoint_id, label);
        return;
    }

    ezb_err_t err = ezb_zcl_reporting_info_update(info, min_interval_s, max_interval_s, delta);
    if (err != EZB_ERR_NONE) {
        ESP_LOGW(TAG, "Failed to configure reporting for endpoint %d %s (0x%04x)", endpoint_id, label, err);
        return;
    }
    ezb_zcl_reporting_start_attr_report(info);
    ESP_LOGI(TAG, "Configured reporting for endpoint %d %s: min=%ds max=%ds", endpoint_id, label,
             min_interval_s, max_interval_s);
}

static void zb_configure_temperature_reporting(uint8_t endpoint_id)
{
    ezb_zcl_attr_variable_t delta = { .s16 = APP_REPORT_DELTA_CENTIDEGREES };
    zb_configure_reporting(endpoint_id, EZB_ZCL_CLUSTER_ID_TEMPERATURE_MEASUREMENT,
                            EZB_ZCL_ATTR_TEMPERATURE_MEASUREMENT_MEASURED_VALUE_ID,
                            APP_REPORT_MIN_INTERVAL_S, APP_REPORT_MAX_INTERVAL_S, &delta, "MeasuredValue");
}

/* Reports the pump relay (ep 12) and auto-control switch (ep 13) On/Off
 * state whenever it changes on its own - e.g. the hysteresis loop driving
 * the relay, or the manual-override timeout elapsing - not just when
 * written to over the network. Min interval 0 = report immediately on
 * change, no throttling. */
static void zb_configure_on_off_reporting(uint8_t endpoint_id)
{
    ezb_zcl_attr_variable_t delta = { 0 };
    zb_configure_reporting(endpoint_id, EZB_ZCL_CLUSTER_ID_ON_OFF, EZB_ZCL_ATTR_ON_OFF_ON_OFF_ID,
                            APP_ONOFF_REPORT_MIN_INTERVAL_S, APP_ONOFF_REPORT_MAX_INTERVAL_S, &delta, "OnOff");
}

/* Reports the pool heating setpoint (ep 14) so Z2M/Home Assistant see a
 * setpoint change made e.g. via a factory-reset-surviving NVS reload at
 * boot, not just changes it wrote itself. */
static void zb_configure_setpoint_reporting(uint8_t endpoint_id)
{
    ezb_zcl_attr_variable_t delta = { .s16 = APP_SETPOINT_REPORT_DELTA_CENTIDEGREES };
    zb_configure_reporting(endpoint_id, EZB_ZCL_CLUSTER_ID_THERMOSTAT,
                            EZB_ZCL_ATTR_THERMOSTAT_OCCUPIED_HEATING_SETPOINT_ID,
                            APP_SETPOINT_REPORT_MIN_INTERVAL_S, APP_SETPOINT_REPORT_MAX_INTERVAL_S, &delta,
                            "OccupiedHeatingSetpoint");
}

static void zb_configure_all_reporting(void)
{
    zb_configure_temperature_reporting(APP_EP_THERMISTOR_1);
    zb_configure_temperature_reporting(APP_EP_THERMISTOR_2);
    zb_configure_on_off_reporting(APP_EP_PUMP_RELAY);
    zb_configure_on_off_reporting(APP_EP_AUTO_ENABLE);
    zb_configure_setpoint_reporting(APP_EP_POOL_SETPOINT);
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

/* ---------------------------------------------------------------------- */
/* Auto-control enable switch (ep 13, On/Off cluster)                     */
/* ---------------------------------------------------------------------- */

static void zb_auto_enable_attr_change_handler(ezb_zcl_set_attr_value_message_t *message)
{
    if (message == NULL || message->info.dst_ep != APP_EP_AUTO_ENABLE ||
        message->info.cluster_id != EZB_ZCL_CLUSTER_ID_ON_OFF ||
        message->in.attribute.id != EZB_ZCL_ATTR_ON_OFF_ON_OFF_ID) {
        return;
    }

    s_auto_control_enabled = *(uint8_t *)message->in.attribute.data.value;
    zb_persist_auto_enable(s_auto_control_enabled);
    ESP_LOGI(TAG, "Automatic pump control %s from network", s_auto_control_enabled ? "ENABLED" : "DISABLED");
}

/* ---------------------------------------------------------------------- */
/* Pool heating setpoint (ep 14, Thermostat cluster)                      */
/* ---------------------------------------------------------------------- */

static void zb_setpoint_attr_change_handler(ezb_zcl_set_attr_value_message_t *message)
{
    if (message == NULL || message->info.dst_ep != APP_EP_POOL_SETPOINT ||
        message->info.cluster_id != EZB_ZCL_CLUSTER_ID_THERMOSTAT ||
        message->in.attribute.id != EZB_ZCL_ATTR_THERMOSTAT_OCCUPIED_HEATING_SETPOINT_ID) {
        return;
    }

    s_pool_setpoint_centidegrees = *(int16_t *)message->in.attribute.data.value;
    zb_persist_setpoint(s_pool_setpoint_centidegrees);
    ESP_LOGI(TAG, "Pool heating setpoint changed to %d centidegrees C from network", s_pool_setpoint_centidegrees);
}

static void zb_zcl_core_action_handler(ezb_zcl_core_action_callback_id_t callback_id, void *message)
{
    switch (callback_id) {
    case EZB_ZCL_CORE_SET_ATTR_VALUE_CB_ID: {
        ezb_zcl_set_attr_value_message_t *msg = (ezb_zcl_set_attr_value_message_t *)message;
        zb_on_off_attr_change_handler(msg);
        zb_auto_enable_attr_change_handler(msg);
        zb_setpoint_attr_change_handler(msg);
    } break;
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
/* Button-triggered pairing window                                        */
/* ---------------------------------------------------------------------- */

/* Starts a time-boxed join attempt (see APP_PAIRING_WINDOW_MS), called from
 * zb_boot_button_task on a 5s BOOT hold while factory-new. The
 * EZB_BDB_SIGNAL_STEERING handler below checks s_pairing_mode_active/
 * s_pairing_deadline_us on each retry to decide whether to keep trying or
 * give up once the window closes. */
static void zb_start_pairing_mode(void)
{
    s_pairing_mode_active = true;
    s_pairing_deadline_us = esp_timer_get_time() + (int64_t)APP_PAIRING_WINDOW_MS * 1000;
    ESP_LOGI(TAG, "Pairing window started (%ds) - put your Zigbee coordinator in pairing mode now",
             APP_PAIRING_WINDOW_MS / 1000);

    esp_zigbee_lock_acquire(portMAX_DELAY);
    ezb_bdb_start_top_level_commissioning(EZB_BDB_MODE_NETWORK_STEERING);
    esp_zigbee_lock_release();
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
            bool factory_new = ezb_bdb_is_factory_new();
            ESP_LOGI(TAG, "Device started up in%s factory-reset mode", factory_new ? "" : " non");
            if (factory_new) {
                /* Matches how most commercial Zigbee end devices behave: a
                 * factory-new device (including right after a factory
                 * reset) does NOT try to join on its own - only an explicit
                 * 5s BOOT hold starts a (time-boxed) pairing attempt, via
                 * zb_start_pairing_mode() in zb_boot_button_task. Pump
                 * control keeps running normally in the meantime (it never
                 * depends on Zigbee join state), just with whatever
                 * auto-control/setpoint values are currently loaded. */
                ESP_LOGI(TAG, "Factory-new - hold BOOT for %dms to start pairing (window: %ds)", APP_BUTTON_HOLD_MS,
                         APP_PAIRING_WINDOW_MS / 1000);
            } else {
                /* Already paired: reconnect automatically in the background,
                 * retried indefinitely by the EZB_BDB_SIGNAL_STEERING
                 * handler below (s_pairing_mode_active stays false here, so
                 * that handler won't apply the pairing window's timeout) -
                 * matches typical mains-powered Zigbee router behavior. */
                ESP_LOGI(TAG, "Already paired - attempting to reconnect in the background");
                ezb_bdb_start_top_level_commissioning(EZB_BDB_MODE_NETWORK_STEERING);
            }
        } else {
            ESP_LOGW(TAG, "%s failed with status(0x%02x), retrying", ezb_app_signal_to_string(signal_type), status);
            zb_schedule_commissioning_retry(EZB_BDB_MODE_INITIALIZATION, 1000);
        }
    } break;
    case EZB_BDB_SIGNAL_STEERING: {
        ezb_bdb_comm_status_t status = *((ezb_bdb_comm_status_t *)ezb_app_signal_get_params(app_signal));
        if (status == EZB_BDB_STATUS_SUCCESS) {
            s_pairing_mode_active = false; /* no-op if this was a background reconnect, not a pairing window */
            ezb_extpanid_t extended_pan_id;
            ezb_nwk_get_extended_panid(&extended_pan_id);
            ESP_LOGI(TAG, "Joined network successfully: PAN ID(0x%04hx, EXT: 0x%llx), Channel(%d), Short Address(0x%04hx)",
                     ezb_nwk_get_panid(), extended_pan_id.u64, ezb_nwk_get_current_channel(), ezb_nwk_get_short_address());
            int8_t tx_power = 0;
            ezb_get_tx_power(&tx_power);
            ESP_LOGI(TAG, "Radio TX power: %d dBm", tx_power);
        } else if (s_pairing_mode_active && esp_timer_get_time() >= s_pairing_deadline_us) {
            /* Pairing window (button-triggered) expired without joining -
             * unlike the background-reconnect case below, give up instead
             * of retrying forever; needs another 5s BOOT hold to try again. */
            s_pairing_mode_active = false;
            ESP_LOGW(TAG, "Pairing window closed without joining (status 0x%02x) - hold BOOT again to retry", status);
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

    /* Plain On/Off cluster used as a kill switch for the hysteresis loop
     * itself (pump_control.c), not to drive any hardware directly - modeled
     * as an on/off light purely because that's the simplest HA device type
     * exposing a writable/readable On/Off server cluster. Default state (on
     * = automatic control enabled) is forced below since ezb_zha_create_*
     * config macros don't expose a documented "startup on/off" field. */
    ezb_zha_on_off_light_config_t auto_enable_cfg = EZB_ZHA_ON_OFF_LIGHT_CONFIG();
    ezb_af_ep_desc_t auto_enable_ep = ezb_zha_create_on_off_light(APP_EP_AUTO_ENABLE, &auto_enable_cfg);
    zb_set_basic_cluster_strings(auto_enable_ep);
    ESP_RETURN_ON_ERROR(ezb_af_device_add_endpoint_desc(dev_desc, auto_enable_ep), TAG, "Failed to add auto-enable endpoint");

    /* Thermostat cluster used only for its OccupiedHeatingSetpoint attribute,
     * so Home Assistant/Z2M gets a normal editable temperature setpoint
     * entity instead of a bespoke custom-cluster number. Everything else
     * about the cluster (LocalTemperature, SystemMode, etc.) is left at the
     * config macro's defaults - unused by this device.
     *
     * OccupiedHeatingSetpoint is an optional attribute, so - unlike the
     * mandatory LocalTemperature/ControlSequenceOfOperation/SystemMode fields
     * in ezb_zha_thermostat_config_t.thermostat_cfg - it isn't part of the
     * create-time config struct at all; it has to be added to the cluster
     * descriptor after creation, the same way zb_set_basic_cluster_strings()
     * adds the (also optional) manufacturer/model strings to the Basic
     * cluster below. */
    ezb_zha_thermostat_config_t setpoint_cfg = EZB_ZHA_THERMOSTAT_CONFIG();
    ezb_af_ep_desc_t setpoint_ep = ezb_zha_create_thermostat(APP_EP_POOL_SETPOINT, &setpoint_cfg);
    ezb_zcl_cluster_desc_t thermostat_desc =
        ezb_af_endpoint_get_cluster_desc(setpoint_ep, EZB_ZCL_CLUSTER_ID_THERMOSTAT, EZB_ZCL_CLUSTER_SERVER);
    /* add_attr copies this into the cluster's own attribute storage, so a
     * transient non-volatile local is fine here (and required - s_pool_..._'s
     * volatile qualifier can't implicitly convert to the plain `const void *`
     * parameter). */
    int16_t initial_setpoint_centidegrees = s_pool_setpoint_centidegrees;
    ezb_zcl_thermostat_cluster_desc_add_attr(thermostat_desc, EZB_ZCL_ATTR_THERMOSTAT_OCCUPIED_HEATING_SETPOINT_ID,
                                             &initial_setpoint_centidegrees);
    zb_set_basic_cluster_strings(setpoint_ep);
    ESP_RETURN_ON_ERROR(ezb_af_device_add_endpoint_desc(dev_desc, setpoint_ep), TAG, "Failed to add pool setpoint endpoint");

    ESP_RETURN_ON_ERROR(ezb_af_device_desc_register(dev_desc), TAG, "Failed to register device descriptor");

    uint8_t auto_enable_startup_value = s_auto_control_enabled ? 1 : 0;
    ezb_zcl_set_attr_value(APP_EP_AUTO_ENABLE, EZB_ZCL_CLUSTER_ID_ON_OFF, EZB_ZCL_CLUSTER_SERVER,
                           EZB_ZCL_ATTR_ON_OFF_ON_OFF_ID, EZB_ZCL_STD_MANUF_CODE, &auto_enable_startup_value, false);

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
/* Boot button (pairing / factory reset) + pairing-window LED blink        */
/* ---------------------------------------------------------------------- */

static void zb_boot_button_task(void *pvParameters)
{
    const TickType_t poll_interval = pdMS_TO_TICKS(50);
    int64_t press_start_us = 0;
    bool hold_armed = true; /* disarmed after firing once, until the button is released */
    int64_t last_led_toggle_us = 0;
    bool led_on = false;

    for (;;) {
        bool pressed = (gpio_get_level(APP_BOOT_BUTTON_GPIO) == 0);

        if (!pressed) {
            press_start_us = 0;
            hold_armed = true;
        } else {
            if (press_start_us == 0) {
                press_start_us = esp_timer_get_time();
            } else if (hold_armed && (esp_timer_get_time() - press_start_us) >= (int64_t)APP_BUTTON_HOLD_MS * 1000) {
                hold_armed = false;

                /* Same 5s hold, different effect depending on current state -
                 * see the comment on APP_BUTTON_HOLD_MS in app_config.h. */
                esp_zigbee_lock_acquire(portMAX_DELAY);
                bool factory_new = ezb_bdb_is_factory_new();
                if (!factory_new) {
                    ESP_LOGW(TAG, "BOOT button held %dms - factory resetting Zigbee stack", APP_BUTTON_HOLD_MS);
                    /* esp_zigbee_factory_reset() is documented __attribute__((noreturn)) -
                     * it clears the Zigbee datasets and restarts the device itself, so
                     * there's no lock_release()/further code after it to run. */
                    esp_zigbee_factory_reset();
                }
                esp_zigbee_lock_release();

                if (factory_new) {
                    ESP_LOGW(TAG, "BOOT button held %dms - starting pairing window", APP_BUTTON_HOLD_MS);
                    zb_start_pairing_mode();
                }
            }
        }

        if (s_pairing_mode_active) {
            int64_t now = esp_timer_get_time();
            if (now - last_led_toggle_us >= (int64_t)APP_LED_BLINK_INTERVAL_MS * 1000) {
                led_on = !led_on;
                led_set(led_on);
                last_led_toggle_us = now;
            }
        } else if (led_on) {
            led_on = false;
            led_set(false);
        }

        vTaskDelay(poll_interval);
    }
}

static esp_err_t zb_boot_button_init(void)
{
    const gpio_config_t cfg = {
        .pin_bit_mask = 1ULL << APP_BOOT_BUTTON_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&cfg), TAG, "Failed to configure boot button GPIO");
    ESP_RETURN_ON_ERROR(led_init(), TAG, "Failed to configure onboard LED");
    xTaskCreate(zb_boot_button_task, "zb_boot_btn", 2048, NULL, 3, NULL);
    return ESP_OK;
}

/* ---------------------------------------------------------------------- */
/* Public API                                                              */
/* ---------------------------------------------------------------------- */

void zb_main_start(void)
{
    ESP_ERROR_CHECK(nvs_flash_init_partition(APP_ZB_STORAGE_PARTITION_NAME));
    /* Default "nvs" partition: already initialized by nvs_flash_init() in
     * app_main() before this is called. Must run before zb_main_task spins
     * up and calls zb_create_device(), which applies these as the ZCL
     * attributes' startup values. */
    zb_load_persisted_state();
    ESP_ERROR_CHECK(zb_boot_button_init());
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

bool zb_main_is_auto_control_enabled(void)
{
    return s_auto_control_enabled;
}

int16_t zb_main_get_pool_setpoint_centidegrees(void)
{
    return s_pool_setpoint_centidegrees;
}

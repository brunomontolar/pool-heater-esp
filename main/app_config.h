/* Central place for pins, Zigbee endpoint IDs, and control-loop tuning. */
#pragma once

#include <stdint.h>
#include "driver/gpio.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---------------------------------------------------------------------- */
/* I2C bus (ADS1115)                                                       */
/* ---------------------------------------------------------------------- */

#define APP_I2C_PORT       0
#define APP_I2C_SDA_GPIO   GPIO_NUM_22 /* board silkscreen D4 */
#define APP_I2C_SCL_GPIO   GPIO_NUM_23 /* board silkscreen D5 */
#define APP_I2C_FREQ_HZ    400000

#define APP_ADS1115_I2C_ADDR 0x48 /* ADDR pin tied to GND */
#define APP_ADS1115_CH_THERM1 3  /* AIN3 - moved during bring-up while chasing what turned out to be
                                  * a polling-delay bug (see ads1115.c), not a bad AIN0 pin. Fine to
                                  * move back to channel 0 (AIN0) if that's more convenient wiring. */
#define APP_ADS1115_CH_THERM2 1  /* AIN1 */

/* ---------------------------------------------------------------------- */
/* Relay / pump                                                           */
/* ---------------------------------------------------------------------- */

#define APP_RELAY_GPIO        GPIO_NUM_21 /* board silkscreen D3 */
#define APP_RELAY_ACTIVE_HIGH 1           /* set to 0 if your relay board is active-low */

/* ---------------------------------------------------------------------- */
/* Boot button (Zigbee factory reset)                                     */
/* ---------------------------------------------------------------------- */

/* Stock BOOT button on ESP32-C6-DevKitC-1 - active-low, external button ties
 * it to GND, internal pull-up holds it high when released. Also a strapping
 * pin at power-on/reset, but that only matters before/during boot; safe to
 * reconfigure as a plain input once the app is running. */
#define APP_BOOT_BUTTON_GPIO      GPIO_NUM_9
#define APP_FACTORY_RESET_HOLD_MS (5000) /* hold BOOT this long to leave the network and re-pair */

/* ---------------------------------------------------------------------- */
/* Zigbee endpoints (HA profile)                                          */
/* ---------------------------------------------------------------------- */

#define APP_EP_THERMISTOR_1  10 /* Temperature Measurement cluster (0x0402) */
#define APP_EP_THERMISTOR_2  11 /* Temperature Measurement cluster (0x0402) */
#define APP_EP_PUMP_RELAY    12 /* On/Off cluster (0x0006)                  */
#define APP_EP_AUTO_ENABLE   13 /* On/Off cluster (0x0006) - gates hysteresis control */
#define APP_EP_POOL_SETPOINT 14 /* Thermostat cluster (0x0201) - OccupiedHeatingSetpoint */

/* ZCL string attributes are length-prefixed: first byte = length, then the
 * bytes themselves (no NUL terminator). Keep the \xNN prefix in sync with the
 * string length if you rename these. */
#define APP_ZB_MANUFACTURER_NAME "\x08" "PoolCtrl"
#define APP_ZB_MODEL_IDENTIFIER  "\x08" "TempPump"

#define APP_ZB_STORAGE_PARTITION_NAME "zb_storage"

/* Temperature Measurement cluster's MeasuredValue range, in centidegrees C
 * (e.g. 2345 = 23.45C). TODO: narrow this to your actual expected range. */
#define APP_TEMP_SENSOR_MIN_CENTIDEGREES (-1000)
#define APP_TEMP_SENSOR_MAX_CENTIDEGREES (12000)

/* ---------------------------------------------------------------------- */
/* Control loop timing                                                    */
/* ---------------------------------------------------------------------- */

#define APP_CONTROL_LOOP_INTERVAL_MS 5000

/* Hysteresis thresholds on (temp1 - temp2), in centidegrees C.
 * THRESHOLD_ON must be > THRESHOLD_OFF or the relay will chatter. */
#define APP_DELTA_THRESHOLD_ON_CENTIDEGREES  300 /* 3.00C */
#define APP_DELTA_THRESHOLD_OFF_CENTIDEGREES 200 /* 2.00C */

/* How long a manual on/off write from Home Assistant/Z2M suppresses automatic
 * control before the hysteresis loop resumes driving the relay itself. */
#define APP_MANUAL_OVERRIDE_TIMEOUT_MS (30 * 60 * 1000) /* 30 minutes */

/* Default pool heating setpoint (APP_EP_POOL_SETPOINT's OccupiedHeatingSetpoint),
 * in centidegrees C. Once the pool thermistor reads at or above this, the
 * hysteresis loop won't (re)start the pump even if the collector is hot
 * enough - see the comment above the setpoint check in pump_control.c for
 * which thermistor is treated as "the pool". Used only until a persisted
 * value is loaded from NVS at boot (see APP_NVS_NAMESPACE below), and again
 * if none has ever been persisted (fresh device / erased NVS). */
#define APP_DEFAULT_POOL_SETPOINT_CENTIDEGREES 3000 /* 30.00C */

/* Namespace/keys for persisting the auto-enable switch (ep 13) and pool
 * setpoint (ep 14) across reboots in the default "nvs" partition - separate
 * from APP_ZB_STORAGE_PARTITION_NAME, which only holds Zigbee network state
 * and is wiped by the BOOT-button factory reset. These survive that reset. */
#define APP_NVS_NAMESPACE       "pool_ctrl"
#define APP_NVS_KEY_AUTO_ENABLE "auto_en"
#define APP_NVS_KEY_SETPOINT    "setpoint"

/* Guaranteed heartbeat ceiling shared by all five endpoints' reporting: even
 * with nothing changed, each pushes an update at least this often. Kept as
 * one shared constant so the "is it still alive" cadence is the same across
 * every endpoint rather than a confusing mix of intervals. */
#define APP_HEARTBEAT_MAX_INTERVAL_S 90

/* Zigbee attribute reporting (Temperature Measurement, endpoints 10 & 11). */
#define APP_REPORT_MIN_INTERVAL_S 5 /* do not report more often than this */
#define APP_REPORT_MAX_INTERVAL_S APP_HEARTBEAT_MAX_INTERVAL_S
#define APP_REPORT_DELTA_CENTIDEGREES 20 /* 0.20C - report early if it changes by at least this much */

/* Zigbee attribute reporting for the On/Off cluster (pump relay ep 12,
 * auto-control switch ep 13). No reportable-change field - On/Off is a
 * discrete attribute type. */
#define APP_ONOFF_REPORT_MIN_INTERVAL_S 0 /* report immediately on every change */
#define APP_ONOFF_REPORT_MAX_INTERVAL_S APP_HEARTBEAT_MAX_INTERVAL_S

/* Zigbee attribute reporting for the pool heating setpoint (Thermostat
 * cluster's OccupiedHeatingSetpoint, endpoint 14). Kept for documentation/
 * in case a future SDK version allocates a native reporting slot for it -
 * as of this SDK version it doesn't, and there's no working fallback either
 * (ezb_zcl_report_attr_cmd_req() was tried and confirmed tied to the same
 * unallocated slot - see the comment above zb_on_off_attr_change_handler in
 * zb_main.c for the full story). Endpoint 14 stays fully readable/writable,
 * just without a proactive heartbeat like the other four endpoints get. */
#define APP_SETPOINT_REPORT_MIN_INTERVAL_S 1
#define APP_SETPOINT_REPORT_MAX_INTERVAL_S APP_HEARTBEAT_MAX_INTERVAL_S
#define APP_SETPOINT_REPORT_DELTA_CENTIDEGREES 10 /* 0.10C */

#ifdef __cplusplus
}
#endif

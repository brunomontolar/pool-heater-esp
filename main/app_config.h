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
#define APP_ADS1115_CH_THERM1 0  /* AIN0 */
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
#define APP_DELTA_THRESHOLD_ON_CENTIDEGREES  500 /* 5.00C */
#define APP_DELTA_THRESHOLD_OFF_CENTIDEGREES 200 /* 2.00C */

/* How long a manual on/off write from Home Assistant/Z2M suppresses automatic
 * control before the hysteresis loop resumes driving the relay itself. */
#define APP_MANUAL_OVERRIDE_TIMEOUT_MS (30 * 60 * 1000) /* 30 minutes */

/* Default pool heating setpoint (APP_EP_POOL_SETPOINT's OccupiedHeatingSetpoint),
 * in centidegrees C. Once the pool thermistor reads at or above this, the
 * hysteresis loop won't (re)start the pump even if the collector is hot
 * enough - see the comment above the setpoint check in pump_control.c for
 * which thermistor is treated as "the pool". Live in RAM only: resets to
 * this default on every boot, changeable at runtime via Zigbee. */
#define APP_DEFAULT_POOL_SETPOINT_CENTIDEGREES 3000 /* 30.00C */

/* Zigbee attribute reporting (Temperature Measurement, endpoints 10 & 11). */
#define APP_REPORT_MIN_INTERVAL_S 5    /* do not report more often than this */
#define APP_REPORT_MAX_INTERVAL_S 300  /* send a report at least this often, even if unchanged */
#define APP_REPORT_DELTA_CENTIDEGREES 20 /* 0.20C - report early if it changes by at least this much */

#ifdef __cplusplus
}
#endif

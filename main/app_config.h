/*
 * Central place for pins, Zigbee endpoint IDs, and control-loop tuning.
 * Values marked TODO are placeholders - adjust for your actual wiring/board.
 */
#pragma once

#include <stdint.h>
#include "driver/gpio.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---------------------------------------------------------------------- */
/* I2C bus (ADS1115)                                                       */
/* ---------------------------------------------------------------------- */

/* TODO: confirm against your board's pinout. GPIO4-7 carry JTAG signals on
 * ESP32-C6 (only relevant if you use on-chip debugging), avoided here to be
 * safe by default. */
#define APP_I2C_PORT       0
#define APP_I2C_SDA_GPIO   GPIO_NUM_2  /* TODO */
#define APP_I2C_SCL_GPIO   GPIO_NUM_3  /* TODO */
#define APP_I2C_FREQ_HZ    400000

#define APP_ADS1115_I2C_ADDR 0x48 /* ADDR pin tied to GND; TODO confirm wiring */
#define APP_ADS1115_CH_THERM1 0  /* AIN0 */
#define APP_ADS1115_CH_THERM2 1  /* AIN1 */

/* ---------------------------------------------------------------------- */
/* Relay / pump                                                           */
/* ---------------------------------------------------------------------- */

#define APP_RELAY_GPIO        GPIO_NUM_5 /* TODO */
#define APP_RELAY_ACTIVE_HIGH 1          /* set to 0 if your relay board is active-low */

/* ---------------------------------------------------------------------- */
/* Zigbee endpoints (HA profile)                                          */
/* ---------------------------------------------------------------------- */

#define APP_EP_THERMISTOR_1 10 /* Temperature Measurement cluster (0x0402) */
#define APP_EP_THERMISTOR_2 11 /* Temperature Measurement cluster (0x0402) */
#define APP_EP_PUMP_RELAY   12 /* On/Off cluster (0x0006)                 */

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
 * TODO: tune for your system - THRESHOLD_ON must be > THRESHOLD_OFF or the
 * relay will chatter. */
#define APP_DELTA_THRESHOLD_ON_CENTIDEGREES  500 /* 5.00C */
#define APP_DELTA_THRESHOLD_OFF_CENTIDEGREES 200 /* 2.00C */

/* How long a manual on/off write from Home Assistant/Z2M suppresses automatic
 * control before the hysteresis loop resumes driving the relay itself. */
#define APP_MANUAL_OVERRIDE_TIMEOUT_MS (30 * 60 * 1000) /* 30 minutes */

/* Zigbee attribute reporting (Temperature Measurement, endpoints 10 & 11). */
#define APP_REPORT_MIN_INTERVAL_S 5    /* do not report more often than this */
#define APP_REPORT_MAX_INTERVAL_S 300  /* send a report at least this often, even if unchanged */
#define APP_REPORT_DELTA_CENTIDEGREES 20 /* 0.20C - report early if it changes by at least this much */

#ifdef __cplusplus
}
#endif

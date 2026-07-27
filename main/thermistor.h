/*
 * NTC thermistor voltage-divider -> temperature conversion (Beta equation).
 * Assumes the fixed resistor is the top leg (toward supply_voltage) and the
 * NTC is the bottom leg (toward GND) - see thermistor.c.
 */
#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    float b_coefficient;   /* Beta value from the datasheet, e.g. 3950 */
    float r25_ohms;        /* Thermistor resistance at 25C, e.g. 10000 */
    float divider_r_ohms;  /* Fixed divider resistor value */
    float supply_voltage;  /* Divider supply voltage, e.g. 3.3 */
    float adc_fullscale_voltage; /* ADS1115 PGA full-scale voltage for the channel used */
} thermistor_params_t;

/* Converts a raw ADS1115 single-ended reading to degrees Celsius. Returns
 * -273.15 (impossible, so it's obvious in logs/attributes) if the reading is
 * out of range for a connected sensor (e.g. open or shorted). */
float thermistor_raw_to_celsius(int16_t adc_raw, const thermistor_params_t *params);

#ifdef __cplusplus
}
#endif

/*
 * NTC thermistor voltage-divider -> temperature conversion (Beta/Steinhart-
 * Hart equation) - STUB, math to be filled in together once you have the
 * datasheet's B-coefficient/R25 and the divider's fixed resistor value.
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

/* Converts a raw ADS1115 single-ended reading to degrees Celsius.
 *
 * TODO (together):
 *   1. raw -> volts:   v = adc_raw * params->adc_fullscale_voltage / 32768.0f
 *   2. volts -> R_ntc:  depends on whether the thermistor is the high or low
 *      leg of the divider (v = Vs * R_ntc/(R_ntc+R_fixed) vs. the inverse)
 *   3. R_ntc -> Kelvin via the Beta equation:
 *      1/T = 1/T25 + (1/B) * ln(R_ntc/R25),  T25 = 298.15 K
 *   4. Kelvin -> Celsius: T - 273.15
 */
float thermistor_raw_to_celsius(int16_t adc_raw, const thermistor_params_t *params);

#ifdef __cplusplus
}
#endif

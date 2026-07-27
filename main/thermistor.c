#include <math.h>

#include "esp_log.h"

#include "thermistor.h"

static const char *TAG = "THERMISTOR";

/* Kelvin<->Celsius reference point for the Beta equation. */
#define THERMISTOR_T25_KELVIN 298.15f
#define THERMISTOR_KELVIN_TO_CELSIUS_OFFSET 273.15f

float thermistor_raw_to_celsius(int16_t adc_raw, const thermistor_params_t *params)
{
    float v = (float)adc_raw * params->adc_fullscale_voltage / 32768.0f;

    if (v <= 0.0f || v >= params->supply_voltage) {
        ESP_LOGW(TAG, "Thermistor reading out of range (v=%.3fV) - check wiring", (double)v);
        return -THERMISTOR_KELVIN_TO_CELSIUS_OFFSET;
    }

    /* Divider here has the fixed resistor on top (3.3V side) and the NTC on
     * the bottom (GND side): v = Vs * R_ntc/(R_ntc + R_fixed)
     *   =>  R_ntc = v * R_fixed / (Vs - v) */
    float r_ntc = v * params->divider_r_ohms / (params->supply_voltage - v);

    float inv_kelvin = 1.0f / THERMISTOR_T25_KELVIN + (1.0f / params->b_coefficient) * logf(r_ntc / params->r25_ohms);
    return (1.0f / inv_kelvin) - THERMISTOR_KELVIN_TO_CELSIUS_OFFSET;
}

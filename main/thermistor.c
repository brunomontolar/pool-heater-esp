#include "esp_log.h"

#include "thermistor.h"

static const char *TAG = "THERMISTOR";

float thermistor_raw_to_celsius(int16_t adc_raw, const thermistor_params_t *params)
{
    /* TODO (together): implement the Beta/Steinhart-Hart conversion - see
     * thermistor.h for the derivation steps. Returning a clearly-invalid
     * sentinel for now so callers/logs make it obvious this isn't wired up
     * yet, rather than silently reporting 0.00C. */
    (void)adc_raw;
    (void)params;
    ESP_LOGW(TAG, "thermistor_raw_to_celsius() not implemented yet");
    return -273.15f;
}

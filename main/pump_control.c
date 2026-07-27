#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "app_config.h"
#include "i2c_bus.h"
#include "ads1115.h"
#include "thermistor.h"
#include "relay.h"
#include "zb_main.h"
#include "pump_control.h"

static const char *TAG = "PUMP_CTRL";

static ads1115_handle_t s_adc;

/* Generic 10k NTC (B=3950), each with a 10k fixed resistor from 3.3V to the
 * ADS1115 input (NTC to GND) - see thermistor.c for the divider topology. */
static const thermistor_params_t s_therm_params = {
    .b_coefficient = 3950.0f,
    .r25_ohms = 10000.0f,
    .divider_r_ohms = 10000.0f,
    .supply_voltage = 3.3f,
    .adc_fullscale_voltage = 4.096f, /* matches ADS1115_CFG_PGA_4_096V */
};

/* Hysteresis state: the last commanded pump state, decided purely from the
 * temperature delta. */
static bool s_pump_commanded_on = false;

static void pump_control_task(void *arg)
{
    for (;;) {
        int16_t raw1 = 0, raw2 = 0;
        esp_err_t err1 = ads1115_read_channel(s_adc, APP_ADS1115_CH_THERM1, &raw1);
        esp_err_t err2 = ads1115_read_channel(s_adc, APP_ADS1115_CH_THERM2, &raw2);

        if (err1 != ESP_OK || err2 != ESP_OK) {
            ESP_LOGW(TAG, "Failed to read thermistors (err1=0x%x err2=0x%x)", err1, err2);
            vTaskDelay(pdMS_TO_TICKS(APP_CONTROL_LOOP_INTERVAL_MS));
            continue;
        }

        float t1_c = thermistor_raw_to_celsius(raw1, &s_therm_params);
        float t2_c = thermistor_raw_to_celsius(raw2, &s_therm_params);
        int16_t t1_centidegrees = (int16_t)(t1_c * 100.0f);
        int16_t t2_centidegrees = (int16_t)(t2_c * 100.0f);

        zb_main_report_temperature(APP_EP_THERMISTOR_1, t1_centidegrees);
        zb_main_report_temperature(APP_EP_THERMISTOR_2, t2_centidegrees);

        int32_t delta_centidegrees = (int32_t)t1_centidegrees - (int32_t)t2_centidegrees;

        bool override_active = zb_main_is_manual_override_active();
        if (!override_active) {
            if (delta_centidegrees > APP_DELTA_THRESHOLD_ON_CENTIDEGREES) {
                s_pump_commanded_on = true;
            } else if (delta_centidegrees < APP_DELTA_THRESHOLD_OFF_CENTIDEGREES) {
                s_pump_commanded_on = false;
            }
            /* Always (re-)apply, even when unchanged: this self-heals the
             * relay/attribute state back to our hysteresis decision after a
             * manual override window ends, at the cost of a harmless
             * redundant write most cycles (same GPIO level = no relay
             * chatter). */
            zb_main_set_pump_state(s_pump_commanded_on);
        }

        ESP_LOGI(TAG, "T1=%.2fC T2=%.2fC delta=%.2fC pump=%s%s", t1_c, t2_c, (double)(t1_c - t2_c),
                 s_pump_commanded_on ? "ON" : "OFF", override_active ? " (manual override active)" : "");

        vTaskDelay(pdMS_TO_TICKS(APP_CONTROL_LOOP_INTERVAL_MS));
    }
}

void pump_control_start(void)
{
    ESP_ERROR_CHECK(i2c_bus_init());
    ESP_ERROR_CHECK(ads1115_init(i2c_bus_get_handle(), &s_adc));
    ESP_ERROR_CHECK(relay_init());
    xTaskCreate(pump_control_task, "pump_control", 4096, NULL, 4, NULL);
}

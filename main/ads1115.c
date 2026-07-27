#include <stdlib.h>

#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "app_config.h"
#include "ads1115.h"

static const char *TAG = "ADS1115";

/* All channels wired here use a 3.3V-supplied divider, so this is the widest
 * PGA range that never clips (see app_config.h / thermistor.c). */
#define ADS1115_PGA_RANGE ADS1115_CFG_PGA_4_096V

/* 128SPS single-shot conversions complete in ~8ms; poll a bit past that
 * before giving up. */
#define ADS1115_CONVERSION_POLL_DELAY_MS 2
#define ADS1115_CONVERSION_POLL_ATTEMPTS 10

struct ads1115_dev_s {
    i2c_master_dev_handle_t i2c_dev;
};

static esp_err_t ads1115_write_reg(ads1115_handle_t handle, uint8_t reg, uint16_t value)
{
    uint8_t buf[3] = { reg, (uint8_t)(value >> 8), (uint8_t)(value & 0xFF) };
    return i2c_master_transmit(handle->i2c_dev, buf, sizeof(buf), pdMS_TO_TICKS(100));
}

static esp_err_t ads1115_read_reg(ads1115_handle_t handle, uint8_t reg, uint16_t *value_out)
{
    uint8_t rx[2];
    ESP_RETURN_ON_ERROR(i2c_master_transmit_receive(handle->i2c_dev, &reg, 1, rx, sizeof(rx), pdMS_TO_TICKS(100)), TAG,
                         "Failed to read register 0x%02x", reg);
    *value_out = ((uint16_t)rx[0] << 8) | rx[1];
    return ESP_OK;
}

esp_err_t ads1115_init(i2c_master_bus_handle_t bus, ads1115_handle_t *out_handle)
{
    struct ads1115_dev_s *dev = calloc(1, sizeof(*dev));
    ESP_RETURN_ON_FALSE(dev != NULL, ESP_ERR_NO_MEM, TAG, "Out of memory");

    const i2c_device_config_t dev_config = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = APP_ADS1115_I2C_ADDR,
        .scl_speed_hz = APP_I2C_FREQ_HZ,
    };
    esp_err_t err = i2c_master_bus_add_device(bus, &dev_config, &dev->i2c_dev);
    if (err != ESP_OK) {
        free(dev);
        return err;
    }

    *out_handle = dev;
    return ESP_OK;
}

esp_err_t ads1115_read_channel(ads1115_handle_t handle, uint8_t channel, int16_t *raw_out)
{
    ESP_RETURN_ON_FALSE(handle != NULL && raw_out != NULL && channel <= 3, ESP_ERR_INVALID_ARG, TAG,
                         "Invalid argument");

    uint16_t config = ADS1115_CFG_OS_SINGLE | ADS1115_CFG_MUX_SINGLE(channel) | ADS1115_PGA_RANGE |
                       ADS1115_CFG_MODE_SINGLE | ADS1115_CFG_DR_128SPS | ADS1115_CFG_COMP_DISABLE;
    ESP_RETURN_ON_ERROR(ads1115_write_reg(handle, ADS1115_REG_CONFIG, config), TAG, "Failed to start conversion");

    uint16_t status = 0;
    for (int attempt = 0; attempt < ADS1115_CONVERSION_POLL_ATTEMPTS; attempt++) {
        vTaskDelay(pdMS_TO_TICKS(ADS1115_CONVERSION_POLL_DELAY_MS));
        ESP_RETURN_ON_ERROR(ads1115_read_reg(handle, ADS1115_REG_CONFIG, &status), TAG, "Failed to poll config register");
        if (status & ADS1115_CFG_OS_SINGLE) {
            break;
        }
    }
    ESP_RETURN_ON_FALSE(status & ADS1115_CFG_OS_SINGLE, ESP_ERR_TIMEOUT, TAG, "Conversion timed out");

    uint16_t raw;
    ESP_RETURN_ON_ERROR(ads1115_read_reg(handle, ADS1115_REG_CONVERSION, &raw), TAG, "Failed to read conversion register");
    *raw_out = (int16_t)raw;
    return ESP_OK;
}

#include <stdlib.h>

#include "esp_check.h"
#include "esp_log.h"

#include "app_config.h"
#include "ads1115.h"

static const char *TAG = "ADS1115";

struct ads1115_dev_s {
    i2c_master_dev_handle_t i2c_dev;
};

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
    /* TODO (together): write config register for `channel` (single-ended,
     * ADS1115_CFG_MUX_SINGLE(channel)), chosen PGA range, single-shot mode;
     * wait for the conversion to complete (poll OS bit or delay per the
     * selected data rate); then read ADS1115_REG_CONVERSION into *raw_out. */
    (void)handle;
    (void)channel;
    (void)raw_out;
    ESP_LOGW(TAG, "ads1115_read_channel() not implemented yet");
    return ESP_ERR_NOT_SUPPORTED;
}

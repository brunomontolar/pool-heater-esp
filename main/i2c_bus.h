/* Shared I2C master bus (ESP-IDF v6 i2c_master driver) for the ADS1115. */
#pragma once

#include "driver/i2c_master.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Initializes the I2C master bus on APP_I2C_PORT/SDA/SCL. Call once, before
 * ads1115_init(). */
esp_err_t i2c_bus_init(void);

/* Returns the bus handle for attaching devices (e.g. via
 * i2c_master_bus_add_device()). Valid only after i2c_bus_init() succeeds. */
i2c_master_bus_handle_t i2c_bus_get_handle(void);

#ifdef __cplusplus
}
#endif

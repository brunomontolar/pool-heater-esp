/*
 * ADS1115 4-channel 16-bit ADC driver - STUB.
 *
 * Register map and config-register bitfields are filled in (fixed by the
 * ADS1115 datasheet); the actual I2C read/convert sequence and gain/PGA
 * selection are left as TODOs to fill in together, since they depend on the
 * voltage divider design for each thermistor channel.
 */
#pragma once

#include <stdint.h>
#include "driver/i2c_master.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Register addresses. */
#define ADS1115_REG_CONVERSION 0x00
#define ADS1115_REG_CONFIG     0x01
#define ADS1115_REG_LO_THRESH  0x02
#define ADS1115_REG_HI_THRESH  0x03

/* Config register bitfields (16-bit, MSB first over I2C). */
#define ADS1115_CFG_OS_SINGLE       (1 << 15) /* Start a single conversion */
#define ADS1115_CFG_MUX_SINGLE(ch)  ((4 + (ch)) << 12) /* AINx vs GND, ch = 0..3 */
#define ADS1115_CFG_PGA_6_144V      (0 << 9)
#define ADS1115_CFG_PGA_4_096V      (1 << 9)
#define ADS1115_CFG_PGA_2_048V      (2 << 9) /* default on power-up */
#define ADS1115_CFG_PGA_1_024V      (3 << 9)
#define ADS1115_CFG_PGA_0_512V      (4 << 9)
#define ADS1115_CFG_PGA_0_256V      (5 << 9)
#define ADS1115_CFG_MODE_SINGLE     (1 << 8)
#define ADS1115_CFG_MODE_CONTINUOUS (0 << 8)
#define ADS1115_CFG_DR_128SPS       (4 << 5) /* default */
#define ADS1115_CFG_COMP_DISABLE    (0x0003) /* disable the ALERT/RDY comparator */

typedef struct ads1115_dev_s *ads1115_handle_t;

/* Attaches an ADS1115 at APP_ADS1115_I2C_ADDR on the given bus. */
esp_err_t ads1115_init(i2c_master_bus_handle_t bus, ads1115_handle_t *out_handle);

/* Reads a single-ended channel (0-3) and returns the raw signed 16-bit ADC
 * code. Convert to volts via (raw * PGA_full_scale_volts / 32768).
 *
 * TODO (together): write the conversion sequence - set the config register
 * for the desired channel/PGA/single-shot mode, wait for OS bit / conversion
 * time, then read ADS1115_REG_CONVERSION. */
esp_err_t ads1115_read_channel(ads1115_handle_t handle, uint8_t channel, int16_t *raw_out);

#ifdef __cplusplus
}
#endif

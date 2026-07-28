/* Onboard user LED - used as a Zigbee pairing-window indicator. */
#pragma once

#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t led_init(void);

/* on = true lights the LED, accounting for APP_LED_ACTIVE_HIGH polarity
 * internally. */
void led_set(bool on);

#ifdef __cplusplus
}
#endif

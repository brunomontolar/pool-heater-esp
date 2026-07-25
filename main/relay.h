/* GPIO-driven relay controlling the pump. */
#pragma once

#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t relay_init(void);

/* on = true energizes the relay (pump running), accounting for
 * APP_RELAY_ACTIVE_HIGH polarity internally. */
void relay_set(bool on);

#ifdef __cplusplus
}
#endif

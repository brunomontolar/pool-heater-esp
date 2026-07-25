#include "driver/gpio.h"

#include "app_config.h"
#include "relay.h"

esp_err_t relay_init(void)
{
    const gpio_config_t cfg = {
        .pin_bit_mask = 1ULL << APP_RELAY_GPIO,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    esp_err_t err = gpio_config(&cfg);
    if (err == ESP_OK) {
        relay_set(false);
    }
    return err;
}

void relay_set(bool on)
{
    int level = on ? APP_RELAY_ACTIVE_HIGH : !APP_RELAY_ACTIVE_HIGH;
    gpio_set_level(APP_RELAY_GPIO, level);
}

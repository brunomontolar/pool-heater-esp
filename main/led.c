#include "driver/gpio.h"

#include "app_config.h"
#include "led.h"

esp_err_t led_init(void)
{
    const gpio_config_t cfg = {
        .pin_bit_mask = 1ULL << APP_LED_GPIO,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    esp_err_t err = gpio_config(&cfg);
    if (err == ESP_OK) {
        led_set(false);
    }
    return err;
}

void led_set(bool on)
{
    int level = on ? APP_LED_ACTIVE_HIGH : !APP_LED_ACTIVE_HIGH;
    gpio_set_level(APP_LED_GPIO, level);
}

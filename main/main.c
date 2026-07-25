#include "esp_log.h"
#include "nvs_flash.h"

#include "zb_main.h"
#include "pump_control.h"

void app_main(void)
{
    ESP_ERROR_CHECK(nvs_flash_init());

    zb_main_start();
    pump_control_start();
}

// Open-drain reset line into the core MCU.

#include "core_reset.h"

#include "driver/gpio.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "narya_pin_assign.h"

#define TAG "core_reset"

static bool s_inited;

void core_reset_init(void)
{
    if (s_inited) return;
    gpio_config_t io = {
        .pin_bit_mask = (1ULL << NARYA_HID_CORE_RESET),
        .mode         = GPIO_MODE_OUTPUT_OD,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    gpio_config(&io);
    gpio_set_level(NARYA_HID_CORE_RESET, 1);    // released (open-drain high-Z)
    s_inited = true;
}

void core_reset_pulse(void)
{
    core_reset_init();
    ESP_LOGI(TAG, "asserting core reset on GPIO%d", NARYA_HID_CORE_RESET);
    gpio_set_level(NARYA_HID_CORE_RESET, 0);
    vTaskDelay(pdMS_TO_TICKS(100));
    gpio_set_level(NARYA_HID_CORE_RESET, 1);
    ESP_LOGI(TAG, "core reset released");
}

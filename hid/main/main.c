// narya_hid - ESP32-S3 USB-HOST gamepad bridge.
//
// Boot sequence:
//   1. Drive NARYA_HID_USB_VBUS_EN HIGH so downstream Vbus is up.
//   2. hid_uart_tx_init(): bring up UART_NUM_1 to the core MCU.
//   3. usb_gamepad_init(): brings up USB host + HID host class, opens any
//      attached gamepad, and forwards each button-edge event over UART.
//   4. Idle, sending a 1 Hz heartbeat for link liveness diagnostics.

#include <stdio.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "driver/gpio.h"

#include "narya_pin_assign.h"
#include "hid_uart_proto.h"
#include "core_reset.h"
#include "usb/usb_gamepad.h"
#include "transport/hid_uart.h"

static const char *TAG = "narya_hid";

static void on_btn(int btn_idx, int pressed, void *user)
{
    (void)user;
    ESP_LOGI(TAG, "btn=%d %s", btn_idx, pressed ? "DOWN" : "UP");
    hid_uart_send_btn(btn_idx, pressed);
}

static void enable_vbus(void)
{
    gpio_config_t io = {
        .pin_bit_mask = (1ULL << NARYA_HID_USB_VBUS_EN),
        .mode         = GPIO_MODE_OUTPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    gpio_config(&io);
    gpio_set_level(NARYA_HID_USB_VBUS_EN, 1);
    vTaskDelay(pdMS_TO_TICKS(50));
}

// Reset the core MCU on hid boot, then wait for its UART RX to be ready.
// The Narya reset button only resets this MCU; we mirror that to the core
// so both come up together.
static void reset_core_at_boot(void)
{
    core_reset_pulse();
    ESP_LOGI(TAG, "waiting for core boot");
    vTaskDelay(pdMS_TO_TICKS(3000));
}

void app_main(void)
{
    ESP_LOGI(TAG, "narya_hid boot on core %d", xPortGetCoreID());
    ESP_LOGI(TAG, "[pins] usb(dm=%d dp=%d vbus_en=%d) uart(tx=%d rx=%d baud=%d)",
             NARYA_HID_USB_DM, NARYA_HID_USB_DP, NARYA_HID_USB_VBUS_EN,
             NARYA_HID_UART_TX, NARYA_HID_UART_RX, NARYA_HID_UART_BAUD);
    ESP_LOGI(TAG, "[proto] sof=0x%02X max_frame=%u", NARYA_HID_SOF, (unsigned)NARYA_HID_MAX_FRAME);

    reset_core_at_boot();
    enable_vbus();

    if (hid_uart_tx_init() != ESP_OK) {
        ESP_LOGE(TAG, "halt: hid_uart_tx_init failed");
        return;
    }

    if (usb_gamepad_init(on_btn, NULL) != ESP_OK) {
        ESP_LOGE(TAG, "halt: usb_gamepad_init failed");
        return;
    }

    int hb = 0;
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        hid_uart_send_heartbeat();
        ESP_LOGI(TAG, "hb=%d", hb++);
    }
}

// narya_hid - ESP32-S3 USB-HOST gamepad bridge skeleton.
// P0/P1: brings up app_main, validates pin/proto headers compile, then idles.
// USB host + UART TX wire in later phases.

#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"

#include "narya_pin_assign.h"
#include "hid_uart_proto.h"

static const char *TAG = "narya_hid";

void app_main(void)
{
    ESP_LOGI(TAG, "narya_hid boot on core %d", xPortGetCoreID());
    ESP_LOGI(TAG, "[pins] usb(dm=%d dp=%d vbus_en=%d) uart(tx=%d rx=%d baud=%d)",
             NARYA_HID_USB_DM, NARYA_HID_USB_DP, NARYA_HID_USB_VBUS_EN,
             NARYA_HID_UART_TX, NARYA_HID_UART_RX, NARYA_HID_UART_BAUD);
    ESP_LOGI(TAG, "[proto] sof=0x%02X max_frame=%u", NARYA_HID_SOF, (unsigned)NARYA_HID_MAX_FRAME);

    while (true) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

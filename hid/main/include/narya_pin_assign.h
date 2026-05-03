// Narya board pin assignments for ESP32-S3 (hid firmware).
// Mirrors fmruby-core's USB-HOST and UART link conventions.

#ifndef NARYA_PIN_ASSIGN_H
#define NARYA_PIN_ASSIGN_H

#include "driver/gpio.h"
#include "hal/uart_types.h"

// USB-HOST: DP/DM are hardware-fixed on ESP32-S3 (GPIO19/GPIO20).
// VBUS enable controls bus power for downstream devices.
#define NARYA_HID_USB_DM            GPIO_NUM_19
#define NARYA_HID_USB_DP            GPIO_NUM_20
#define NARYA_HID_USB_VBUS_EN       GPIO_NUM_1

// HID UART link to ESP32-WROVER (this side: TX-only logically; RX kept for ack/debug).
#define NARYA_HID_UART_PORT         UART_NUM_1
#define NARYA_HID_UART_TX           GPIO_NUM_11
#define NARYA_HID_UART_RX           GPIO_NUM_13
#define NARYA_HID_UART_BAUD         115200

#endif // NARYA_PIN_ASSIGN_H

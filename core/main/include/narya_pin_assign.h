// Narya board pin assignments for ESP32-WROVER (core firmware).
// Mirrors the conventions in fmruby-graphics-audio's fmrb_pin_assign.h
// and fmruby-core's UART link side.

#ifndef NARYA_PIN_ASSIGN_H
#define NARYA_PIN_ASSIGN_H

#include "driver/gpio.h"
#include "hal/uart_types.h"

// Video: NTSC composite via internal DAC1.
#define NARYA_CORE_VIDEO_DAC        GPIO_NUM_25

// Audio: I2S1 standard mode -> external DAC/amp.
#define NARYA_CORE_I2S_BCK          GPIO_NUM_32
#define NARYA_CORE_I2S_WS           GPIO_NUM_33
#define NARYA_CORE_I2S_DOUT         GPIO_NUM_27

// HID UART link from ESP32-S3 (this side: RX-only logically; TX kept for ack/debug).
#define NARYA_CORE_HID_UART_PORT    UART_NUM_1
#define NARYA_CORE_HID_UART_RX      GPIO_NUM_21
#define NARYA_CORE_HID_UART_TX      GPIO_NUM_18
#define NARYA_CORE_HID_UART_BAUD    115200

#endif // NARYA_PIN_ASSIGN_H

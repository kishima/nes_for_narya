// narya_core - ESP32-WROVER core firmware top-level.
// Currently exercises NTSC video (P2) + I2S sine wave audio (P3).
// HID UART and the Nofrendo emulator wire in later phases.

#include <math.h>
#include <stdio.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"

#include "narya_pin_assign.h"
#include "hid_uart_proto.h"
#include "video_out.h"
#include "audio_i2s.h"

static const char *TAG = "narya_core";

// Continuously push a 1 kHz sine into I2S as a P3 hardware bring-up signal.
// One block per call = one NTSC frame's worth (262 samples). The blocking
// i2s_channel_write inside audio_i2s_write_mono paces this loop.
static void audio_test_task(void *arg)
{
    (void)arg;
    static int16_t block[262];
    const float two_pi  = 6.28318530718f;
    const float step    = two_pi * 1000.0f / (float)NARYA_AUDIO_SAMPLE_RATE_HZ;
    float phase = 0.0f;
    while (true) {
        for (size_t i = 0; i < sizeof(block) / sizeof(block[0]); ++i) {
            block[i] = (int16_t)(sinf(phase) * 8000.0f);
            phase += step;
            if (phase > two_pi) phase -= two_pi;
        }
        audio_i2s_write_mono(block, sizeof(block) / sizeof(block[0]));
    }
}

extern "C" void app_main(void)
{
    ESP_LOGI(TAG, "narya_core boot on core %d", xPortGetCoreID());
    ESP_LOGI(TAG, "[pins] video_dac=%d i2s(bck=%d ws=%d dout=%d) uart(rx=%d tx=%d baud=%d)",
             NARYA_CORE_VIDEO_DAC,
             NARYA_CORE_I2S_BCK, NARYA_CORE_I2S_WS, NARYA_CORE_I2S_DOUT,
             NARYA_CORE_HID_UART_RX, NARYA_CORE_HID_UART_TX, NARYA_CORE_HID_UART_BAUD);
    ESP_LOGI(TAG, "[proto] sof=0x%02X max_frame=%u", NARYA_HID_SOF, (unsigned)NARYA_HID_MAX_FRAME);

    // P2: bring up NTSC video chain (I2S0 + APLL + DMA + DAC1).
    video_init(/*samples_per_cc=*/4, /*machine=*/EMU_NES, /*palette=*/nullptr, /*ntsc=*/1);
    video_test_fill();
    ESP_LOGI(TAG, "[video] init ok ntsc=1 samples_per_cc=4 machine=EMU_NES");

    // P3: bring up I2S1 audio and start a 1 kHz sine generator.
    if (audio_i2s_init() == ESP_OK) {
        xTaskCreatePinnedToCore(audio_test_task, "audio_test", 3 * 1024, nullptr, 4, nullptr, 0);
    }

    while (true) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        ESP_LOGI(TAG, "[perf] frame=%d", _frame_counter);
    }
}

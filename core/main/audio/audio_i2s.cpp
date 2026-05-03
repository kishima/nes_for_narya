// I2S1 audio output for the Narya core firmware.
// Modelled after fmruby-graphics-audio's apu_if.cpp (init lines 53-100,
// push lines 102-138) but rewritten for clarity and Narya pinout.

#include "audio_i2s.h"

#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "driver/gpio.h"
#include "driver/i2s_std.h"

#include "narya_pin_assign.h"

static const char *TAG = "audio_i2s";

static i2s_chan_handle_t s_tx = nullptr;
static int16_t          *s_stereo = nullptr;   // scratch interleave buffer (DMA-capable)

esp_err_t audio_i2s_init(void)
{
    if (s_tx) return ESP_OK;

    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_1, I2S_ROLE_MASTER);
    chan_cfg.auto_clear   = true;
    chan_cfg.dma_desc_num = 4;
    chan_cfg.dma_frame_num = NARYA_AUDIO_MAX_MONO_SAMPLES;
    esp_err_t err = i2s_new_channel(&chan_cfg, &s_tx, nullptr);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2s_new_channel failed: %d", err);
        return err;
    }

    i2s_std_config_t std_cfg = {
        .clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG(NARYA_AUDIO_SAMPLE_RATE_HZ),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT,
                                                       I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = NARYA_CORE_I2S_BCK,
            .ws   = NARYA_CORE_I2S_WS,
            .dout = NARYA_CORE_I2S_DOUT,
            .din  = I2S_GPIO_UNUSED,
            .invert_flags = { .mclk_inv = false, .bclk_inv = false, .ws_inv = false },
        },
    };
    err = i2s_channel_init_std_mode(s_tx, &std_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2s_channel_init_std_mode failed: %d", err);
        return err;
    }

    err = i2s_channel_enable(s_tx);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2s_channel_enable failed: %d", err);
        return err;
    }

    // Stereo interleave scratch in DMA-capable internal RAM.
    s_stereo = (int16_t*)heap_caps_malloc(NARYA_AUDIO_MAX_MONO_SAMPLES * 2 * sizeof(int16_t),
                                          MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    if (!s_stereo) {
        ESP_LOGE(TAG, "stereo buffer alloc failed");
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "[i2s] init ok bck=%d ws=%d dout=%d sample_rate=%d",
             NARYA_CORE_I2S_BCK, NARYA_CORE_I2S_WS, NARYA_CORE_I2S_DOUT,
             NARYA_AUDIO_SAMPLE_RATE_HZ);
    return ESP_OK;
}

void audio_i2s_write_mono(const int16_t *samples, size_t n_samples)
{
    if (!s_tx || !s_stereo || !samples || n_samples == 0) return;
    if (n_samples > NARYA_AUDIO_MAX_MONO_SAMPLES) {
        ESP_LOGW(TAG, "n_samples %u > cap %u, truncating",
                 (unsigned)n_samples, (unsigned)NARYA_AUDIO_MAX_MONO_SAMPLES);
        n_samples = NARYA_AUDIO_MAX_MONO_SAMPLES;
    }

    for (size_t i = 0; i < n_samples; ++i) {
        s_stereo[i * 2 + 0] = samples[i];
        s_stereo[i * 2 + 1] = samples[i];
    }

    size_t bytes_written = 0;
    const size_t bytes = n_samples * 2 * sizeof(int16_t);
    esp_err_t err = i2s_channel_write(s_tx, s_stereo, bytes, &bytes_written, portMAX_DELAY);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "i2s_channel_write err=%d written=%u", err, (unsigned)bytes_written);
    }
}

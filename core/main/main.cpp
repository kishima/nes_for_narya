// narya_core - ESP32-WROVER core firmware top-level.
//
// Boot sequence:
//   - Initialize the rom_store (parses the 'roms' partition directory).
//   - Bring up NTSC video and I2S audio.
//   - Show the ROM picker on the TV.
//   - Construct the emulator and insert the chosen ROM.
//
// Task & ISR placement (Anemoia core):
//
//   Core 0  (cache locality with the I2S0 video DMA + flash-XIP front-end)
//     * video_isr                  : I2S0 EOF interrupt, ~15.7 kHz, IRAM
//     * apu_task          (prio 1) : Anemoia Apu2A03::clock() infinite loop;
//                                    blocks on I2S DMA queue when a 128-frame
//                                    block is full -> paces APU to 44.1 kHz.
//     * main_task                  : finishes app_main(), then idles.
//     * IDLE0                      : starves while apu_task is busy.
//
//   Core 1  (no DMA-bound peripherals here)
//     * emu_task          (prio 4) : Bus::clock() per NES frame, drains the
//                                    HID UART event queue, paces to 60 fps.
//     * hid_rx_task       (prio 5) : UART RX framer -> event queue.
//     * perf_task         (prio 2) : 1 Hz [perf]/[emu] log emitter.
//     * IDLE1
//
// Cross-core APU register access: emu_task on core 1 writes CPU/APU
// registers; apu_task on core 0 reads them. ESP32 dual-core keeps DRAM
// coherent so this works without explicit barriers.

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"

#include "narya_pin_assign.h"
#include "hid_uart_proto.h"
#include "video_out.h"
#include "audio_i2s.h"
#include "emu/emu.h"
#include "menu/rom_menu.h"
#include "rom_store.h"
#include "transport/hid_uart.h"

// C entry-point exposed by emu_nofrendo.cpp for injecting decoded button
// events from the UART HID link into the emulator's event_btn() handler.
extern "C" void nofrendo_event_btn(void *emu, int btn_idx, int pressed);

static const char *TAG = "narya_core";

#define NARYA_AUDIO_BUF_SAMPLES    NARYA_AUDIO_MAX_MONO_SAMPLES

static Emu *g_emu = nullptr;

static void emu_task(void *arg)
{
    Emu *emu = (Emu*)arg;
    static int16_t audio_block[NARYA_AUDIO_BUF_SAMPLES];

    // NTSC frame budget; matches Anemoia's 60.098 fps target.
    const int64_t FRAME_US = 16639;
    int64_t next_frame_us = esp_timer_get_time();

    int diag_frames = 0;
    int64_t diag_clock_us_sum = 0;
    int64_t diag_window_start = esp_timer_get_time();

    while (true) {
        int64_t t0 = esp_timer_get_time();
        emu->update();                              // run one NES frame
        int64_t t1 = esp_timer_get_time();
        diag_clock_us_sum += (t1 - t0);
        diag_frames++;

        _lines = emu->video_buffer();               // hand frame to video_isr
        int n = emu->audio_buffer(audio_block, NARYA_AUDIO_BUF_SAMPLES);
        if (n > 0) audio_i2s_write_mono(audio_block, n);

        // 1 Hz: average emu->update() time so we can see if the core is
        // running at real-time (~16 ms) or far slower.
        if (t1 - diag_window_start >= 1000000) {
            ESP_LOGI(TAG, "[emu] frames=%d avg_update=%lldus",
                     diag_frames,
                     diag_frames ? diag_clock_us_sum / diag_frames : 0);
            diag_frames = 0;
            diag_clock_us_sum = 0;
            diag_window_start = t1;
        }

        // Frame pace. Anemoia produces audio out-of-band on apu_task, so
        // audio_buffer() returns 0 here and this loop has no I2S back-
        // pressure of its own. We must yield on every iteration to keep
        // IDLE0 alive even when bus.clock() blew its 16 ms budget.
        next_frame_us += FRAME_US;
        int64_t now = esp_timer_get_time();
        int64_t sleep_us = next_frame_us - now;
        if (sleep_us > 1000) {
            vTaskDelay(pdMS_TO_TICKS(sleep_us / 1000));
        } else {
            vTaskDelay(1);                          // unconditional yield
            if (sleep_us < -2 * FRAME_US) {
                // Fell behind by more than a frame; resync rather than chase.
                next_frame_us = now;
            }
        }
    }
}

static void perf_task(void *arg)
{
    (void)arg;
    int last_frames = 0;
    int64_t last_us = esp_timer_get_time();
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        int64_t now = esp_timer_get_time();
        int frames = _frame_counter;
        int dframes = frames - last_frames;
        int64_t dus = now - last_us;
        last_frames = frames;
        last_us     = now;
        ESP_LOGI(TAG, "[perf] frame=%d (+%d in %lldms)", frames, dframes, dus / 1000);
    }
}

// Drain the UART RX queue, log each decoded event, and forward button edges
// to the emulator. The emulator only consumes the first 8 button indices
// (0..3 = D-pad, 4 = Start, 5 = Select, 6 = A, 7 = B); see narya port
// notes in emu_nofrendo.cpp::EmuNofrendo::event_btn.
static void hid_rx_task(void *arg)
{
    Emu *emu = (Emu*)arg;
    narya_hid_msg_t msg;
    while (true) {
        if (hid_uart_rx_recv(&msg, portMAX_DELAY) != pdTRUE) continue;
        switch (msg.type) {
        case NARYA_EVT_BTN_DOWN:
            ESP_LOGI(TAG, "hid_evt: btn=%u DOWN seq=%u", msg.payload[0], msg.seq);
            if (emu) nofrendo_event_btn(emu, msg.payload[0], 1);
            break;
        case NARYA_EVT_BTN_UP:
            ESP_LOGI(TAG, "hid_evt: btn=%u UP seq=%u", msg.payload[0], msg.seq);
            if (emu) nofrendo_event_btn(emu, msg.payload[0], 0);
            break;
        case NARYA_EVT_HEARTBEAT:
            ESP_LOGI(TAG, "hid_link_alive seq=%u", msg.seq);
            break;
        default:
            ESP_LOGD(TAG, "hid_evt: type=0x%02X len=%u seq=%u", msg.type, msg.len, msg.seq);
            break;
        }
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

    if (rom_store_init() != ESP_OK) {
        ESP_LOGE(TAG, "halt: rom_store_init failed");
        return;
    }
    if (audio_i2s_init() != ESP_OK) {
        ESP_LOGE(TAG, "halt: i2s init failed");
        return;
    }

    // Construct emulator and insert ROM. video_init() must be called with
    // the emulator's NTSC palette so the ISR's blit() produces correct color.
    g_emu = NewNofrendo(/*ntsc=*/1);
    if (!g_emu) {
        ESP_LOGE(TAG, "halt: NewNofrendo failed");
        return;
    }

    // Initialize the video chain BEFORE the menu so the menu can render.
    // We pass the emulator's NES palette since blit() will index into it
    // both for the menu and (later) for emulator frames.
    video_init(g_emu->cc_width, EMU_NES, g_emu->composite_palette(), /*ntsc=*/1);
    ESP_LOGI(TAG, "[video] init ok ntsc=1 samples_per_cc=%d", g_emu->cc_width);

    // The HID RX worker fills its queue independent of any consumer; bring
    // it up so the menu can drain events directly.
    if (hid_uart_rx_init() != ESP_OK) {
        ESP_LOGE(TAG, "halt: hid_uart_rx_init failed");
        return;
    }

    char rom_name[ROM_STORE_NAME_MAX];
    if (rom_menu_run(rom_name, sizeof(rom_name),
                     /*default_timeout_ms=*/0) != ESP_OK) {
        ESP_LOGE(TAG, "halt: rom_menu_run failed");
        return;
    }

    // Free the menu framebuffer (60 KB DRAM) before insert so Nofrendo's
    // primary_buffer can land in internal RAM rather than spilling to
    // PSRAM. PSRAM-backed primary_buffer thrashes the data cache against
    // the PSRAM-resident ROM and visibly breaks MMC3 game timing.
    rom_menu_release();

    if (g_emu->insert(rom_name, 1, 0) != 0) {
        ESP_LOGE(TAG, "halt: insert %s failed", rom_name);
        return;
    }
    ESP_LOGI(TAG, "[emu] rom=%s loaded", rom_name);

    // emu_task moves to core 1 so it does not contend with the Anemoia
    // apu_task (pinned to core 0 with the I2S DMA peripheral and the
    // video ISR). With both on core 0 the higher-priority emu_task
    // starved apu_task and the audio I2S queue underran ('ぶつぶつ').
    xTaskCreatePinnedToCore(emu_task,    "emu_task",    6 * 1024, g_emu, 4, nullptr, 1);
    xTaskCreatePinnedToCore(perf_task,   "perf_task",   3 * 1024, nullptr, 2, nullptr, 1);
    xTaskCreatePinnedToCore(hid_rx_task, "hid_rx_task", 3 * 1024, g_emu, 5, nullptr, 1);
}

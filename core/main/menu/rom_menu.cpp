// ROM picker UI. Renders into a 256x240 NES-indexed framebuffer that we
// hand to the existing video pipeline through the global _lines pointer.

#include "rom_menu.h"

#include <algorithm>
#include <dirent.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <vector>
#include <string>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "video_out.h"
#include "hid_uart.h"
#include "hid_uart_proto.h"
#include "glcdfont.h"

#define TAG "rom_menu"

// Geometry: NES active area is 256x240; with a 5x7 font we use a 6x8 cell
// (one column / one row of spacing). That gives 42 columns x 30 rows.
#define CELL_W              6
#define CELL_H              8
#define COLS                (256 / CELL_W)    // 42
#define ROWS                (240 / CELL_H)    // 30
// NTSC overscan: most TVs trim ~16-24px of each edge. Keep all content in
// the central 200 px so it remains visible on consumer sets. Empirically
// matches fmruby-graphics-audio's 16-px Y safe margin.
#define SAFE_TOP_ROW        3                 // y >= 24 px
#define SAFE_BOT_ROW        27                // y <  216 px (exclusive)
#define MAX_ROMS            32

// NES palette indices. The blit() in video_out.cpp masks src bytes with 0x3F,
// so the visible color comes from nes_4_phase[idx].
//   0x0F = absolute black (super-black)
//   0x30 = light gray / "white"
#define BG_COLOR            0x0F
#define FG_COLOR            0x30
#define HIGHLIGHT_BG        0x30
#define HIGHLIGHT_FG        0x0F

static uint8_t  *s_fb;            // 256*240 indexed framebuffer (PSRAM)
static uint8_t **s_lines;         // 240 row pointers (DRAM, ISR-accessed)

static void fb_clear(uint8_t color)
{
    memset(s_fb, color, 256 * 240);
}

// Draw a single 5x7 glyph at (px, py) with given fg/bg. The font is column-
// major, 5 columns + 1 spacing column = 6 px wide, 8 rows tall.
static void draw_char(int px, int py, char c, uint8_t fg, uint8_t bg)
{
    if (px < 0 || py < 0 || px + CELL_W > 256 || py + CELL_H > 240) return;
    const uint8_t *gly = &font[(uint8_t)c * 5];
    for (int col = 0; col < 5; ++col) {
        uint8_t v = gly[col];
        for (int row = 0; row < 8; ++row) {
            uint8_t color = (v & (1 << row)) ? fg : bg;
            s_fb[(py + row) * 256 + (px + col)] = color;
        }
    }
    // Spacing column.
    for (int row = 0; row < 8; ++row) {
        s_fb[(py + row) * 256 + (px + 5)] = bg;
    }
}

static void draw_str(int x_cell, int y_cell, const char *s, uint8_t fg, uint8_t bg)
{
    int px = x_cell * CELL_W;
    int py = y_cell * CELL_H;
    while (*s && px + CELL_W <= 256) {
        draw_char(px, py, *s++, fg, bg);
        px += CELL_W;
    }
}

static void draw_row_filled(int y_cell, uint8_t bg)
{
    int py = y_cell * CELL_H;
    for (int row = 0; row < CELL_H; ++row) {
        memset(&s_fb[(py + row) * 256], bg, 256);
    }
}

static int scan_roms(const char *dir, std::vector<std::string> &out)
{
    DIR *d = opendir(dir);
    if (!d) {
        ESP_LOGE(TAG, "opendir(%s) failed", dir);
        return -1;
    }
    struct dirent *de;
    while ((de = readdir(d)) != nullptr) {
        const char *name = de->d_name;
        size_t n = strlen(name);
        if (n < 5) continue;
        if (strcasecmp(name + n - 4, ".nes") != 0) continue;
        out.emplace_back(name);
        if (out.size() >= MAX_ROMS) break;
    }
    closedir(d);
    std::sort(out.begin(), out.end());
    return (int)out.size();
}

static void redraw(const std::vector<std::string> &roms, int cursor)
{
    fb_clear(BG_COLOR);
    // All Y coordinates are inside [SAFE_TOP_ROW, SAFE_BOT_ROW) so they
    // survive NTSC overscan on a typical TV.
    const int title_y    = SAFE_TOP_ROW;          // 3
    const int subtitle_y = SAFE_TOP_ROW + 1;      // 4
    const int list_top   = SAFE_TOP_ROW + 3;      // 6
    const int help_y2    = SAFE_BOT_ROW - 1;      // 26
    const int help_y1    = SAFE_BOT_ROW - 2;      // 25
    const int list_max   = help_y1 - list_top - 1;// gap of one row above help

    draw_str(2, title_y,    "narya esp_8_bit", FG_COLOR, BG_COLOR);
    draw_str(2, subtitle_y, "select rom",      FG_COLOR, BG_COLOR);

    if (roms.empty()) {
        draw_str(2, list_top,     "no .nes in /storage", FG_COLOR, BG_COLOR);
        draw_str(2, list_top + 2, "rebuild with rom in", FG_COLOR, BG_COLOR);
        draw_str(2, list_top + 3, "core/data/nofrendo/", FG_COLOR, BG_COLOR);
        return;
    }

    int first = 0;
    if (cursor >= first + list_max) first = cursor - list_max + 1;

    for (int i = 0; i < list_max && first + i < (int)roms.size(); ++i) {
        int idx = first + i;
        bool sel = (idx == cursor);
        if (sel) draw_row_filled(list_top + i, HIGHLIGHT_BG);
        const std::string &name = roms[idx];
        draw_str(2, list_top + i, name.c_str(),
                 sel ? HIGHLIGHT_FG : FG_COLOR,
                 sel ? HIGHLIGHT_BG : BG_COLOR);
    }

    draw_str(2, help_y1, "up/down: move",  FG_COLOR, BG_COLOR);
    draw_str(2, help_y2, "A: start",       FG_COLOR, BG_COLOR);
}

static esp_err_t alloc_buffers(void)
{
    if (s_fb) return ESP_OK;
    // Both the framebuffer and the row-pointer array are dereferenced by the
    // video ISR. The cache is briefly disabled around SPI-flash reads (e.g.
    // when emu->insert() opens the ROM via LittleFS), so any IRAM-resident
    // ISR that touches PSRAM during that window faults with "Cache disabled
    // but cached memory region accessed". Force both into internal DRAM.
    s_fb = (uint8_t*)heap_caps_malloc(256 * 240, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!s_fb) return ESP_ERR_NO_MEM;
    s_lines = (uint8_t**)heap_caps_malloc(240 * sizeof(uint8_t*), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!s_lines) { heap_caps_free(s_fb); s_fb = nullptr; return ESP_ERR_NO_MEM; }
    for (int i = 0; i < 240; ++i) s_lines[i] = &s_fb[i * 256];
    return ESP_OK;
}

static void free_buffers(void)
{
    if (s_fb)    { heap_caps_free(s_fb);    s_fb    = nullptr; }
    if (s_lines) { heap_caps_free(s_lines); s_lines = nullptr; }
}

esp_err_t rom_menu_run(const char *mount_dir,
                       char *out_path, size_t out_cap,
                       uint32_t default_timeout_ms)
{
    if (!mount_dir || !out_path || out_cap < 2) return ESP_ERR_INVALID_ARG;

    std::vector<std::string> roms;
    if (scan_roms(mount_dir, roms) < 0) return ESP_FAIL;

    esp_err_t err = alloc_buffers();
    if (err != ESP_OK) return err;

    int cursor = 0;
    redraw(roms, cursor);
    _lines = s_lines;       // ISR now renders our framebuffer

    if (roms.empty()) {
        // No ROMs -- we cannot proceed. Hold the screen so the user sees it.
        ESP_LOGE(TAG, "no roms found under %s", mount_dir);
        while (true) vTaskDelay(pdMS_TO_TICKS(1000));
    }

    ESP_LOGI(TAG, "%d rom(s) found; awaiting selection", (int)roms.size());

    const TickType_t poll = pdMS_TO_TICKS(50);
    TickType_t elapsed = 0;
    int selected = -1;

    while (selected < 0) {
        narya_hid_msg_t msg;
        if (hid_uart_rx_recv(&msg, poll) == pdTRUE) {
            elapsed = 0;        // user is interacting; reset idle timer
            if (msg.type != NARYA_EVT_BTN_DOWN) continue;
            switch (msg.payload[0]) {
            case 0:  // up
                cursor = (cursor - 1 + (int)roms.size()) % (int)roms.size();
                redraw(roms, cursor);
                break;
            case 1:  // down
                cursor = (cursor + 1) % (int)roms.size();
                redraw(roms, cursor);
                break;
            case 6:  // A -> select
            case 4:  // Start -> select
                selected = cursor;
                break;
            default:
                break;
            }
        } else if (default_timeout_ms > 0) {
            elapsed += pdTICKS_TO_MS(poll);
            if (elapsed >= default_timeout_ms) {
                ESP_LOGI(TAG, "no input for %lums; auto-selecting first rom",
                         (unsigned long)default_timeout_ms);
                selected = 0;
            }
        }
    }

    int written = snprintf(out_path, out_cap, "%s/%s",
                           mount_dir, roms[selected].c_str());
    if (written < 0 || (size_t)written >= out_cap) {
        free_buffers();
        return ESP_ERR_INVALID_SIZE;
    }
    ESP_LOGI(TAG, "selected: %s", out_path);
    return ESP_OK;
}

void rom_menu_release(void)
{
    // Park the ISR before reclaiming the buffers it points at.
    _lines = nullptr;
    free_buffers();
}

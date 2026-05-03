// Minimal USB-HOST gamepad driver for the Narya hid firmware.
//
// Strategy: rely on espressif/usb_host_hid for class transfer plumbing and
// bolt on a tiny "best-effort" decoder for generic gamepads. We intentionally
// avoid the deep VID/PID-specific paths from fmruby-core's usb_task.c since
// the only end consumer here is a NES emulator with eight buttons and a
// D-pad.

#include "usb_gamepad.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include "usb/usb_host.h"
#include "usb/hid_host.h"
#include "usb/hid_usage_keyboard.h"

#include "core_reset.h"

#define TAG "usb_gamepad"

#define USB_HOST_TASK_PRIORITY     5
#define HID_EVENT_TASK_PRIORITY    5
#define USB_HOST_TASK_STACK_BYTES  4096
#define HID_EVENT_TASK_STACK_BYTES 4096

#define MAX_REPORT_LEN             16
#define HAT_CENTER                 0x0F

// HAT switch -> NES D-pad mapping. 8-way HAT collapsed to 4 cardinal buttons.
// Index = HAT value (0..7). Bitmask returned: bit 0=up, 1=down, 2=left, 3=right.
static const uint8_t HAT_TO_DPAD[8] = {
    /* 0: N  */ 0x01,
    /* 1: NE */ 0x01 | 0x08,
    /* 2: E  */ 0x08,
    /* 3: SE */ 0x02 | 0x08,
    /* 4: S  */ 0x02,
    /* 5: SW */ 0x02 | 0x04,
    /* 6: W  */ 0x04,
    /* 7: NW */ 0x01 | 0x04,
};

typedef struct {
    enum {
        APP_EVENT_HID_DRIVER = 0,
        APP_EVENT_HID_HOST,
    } kind;
    hid_host_device_handle_t handle;
    union {
        hid_host_driver_event_t driver_event;
        hid_host_interface_event_t iface_event;
    };
} app_event_t;

typedef struct {
    hid_host_device_handle_t handle;
    bool                     opened;
    uint16_t                 vid;
    uint16_t                 pid;
    // Decoded button bitmask we last emitted to the consumer. Bits map to
    // narya_hid btn_idx (bit i = button i). Diff against new state to find
    // edges.
    uint8_t                  prev_btns;
    uint8_t                  prev_hat;
    uint8_t                  dump_count;   // raw-hex dumps emitted so far
    bool                     reset_combo_held;  // last-seen state of Select+R2
} gamepad_t;

// We accept up to two simultaneous gamepad interfaces; only the first is
// forwarded as buttons (NES has one player by design in this port).
static gamepad_t            s_pads[2];
static size_t               s_pad_count;
static QueueHandle_t        s_event_q;
static usb_gamepad_btn_cb_t s_cb;
static void                *s_cb_user;

// ---------- Helpers ----------------------------------------------------------

static gamepad_t* find_pad(hid_host_device_handle_t h)
{
    for (size_t i = 0; i < s_pad_count; ++i) {
        if (s_pads[i].handle == h) return &s_pads[i];
    }
    return NULL;
}

static gamepad_t* alloc_pad(hid_host_device_handle_t h)
{
    if (s_pad_count >= sizeof(s_pads) / sizeof(s_pads[0])) return NULL;
    gamepad_t *p = &s_pads[s_pad_count++];
    memset(p, 0, sizeof(*p));
    p->handle    = h;
    p->prev_hat  = HAT_CENTER;
    return p;
}

static void emit(int btn_idx, int pressed)
{
    ESP_LOGI(TAG, "btn=%d %s", btn_idx, pressed ? "DOWN" : "UP");
    if (s_cb) s_cb(btn_idx, pressed, s_cb_user);
}

// Diagnostic: dump the first N raw reports per device so the user can see the
// actual layout in case our decoder picks the wrong offsets.
#define REPORT_DUMP_COUNT 20

// Reset combo: pressing Share (Select) + R2 triggers a hardware reset of
// the core MCU via the open-drain EN line.
#define RESET_COMBO_B0_MASK (1u << 7)   // R2 in byte 0
#define RESET_COMBO_B1_MASK (1u << 0)   // Share/Select in byte 1

static void dump_report(const gamepad_t *pad, const uint8_t *data, size_t len)
{
    char buf[3 * MAX_REPORT_LEN + 1];
    size_t n = (len > MAX_REPORT_LEN) ? MAX_REPORT_LEN : len;
    for (size_t i = 0; i < n; ++i) {
        snprintf(&buf[i * 3], 4, "%02X ", data[i]);
    }
    buf[n * 3] = '\0';
    ESP_LOGI(TAG, "raw[%u]: %s (len=%u VID=%04X PID=%04X)",
             pad->dump_count, buf, (unsigned)len, pad->vid, pad->pid);
}

// Decode a HORI / PS3-style HID gamepad report (no report-ID prefix).
// This layout is used by HORI HORIPAD FPS PLUS (VID 0x0F0D PID 0x0009),
// most PC-compatible PS3-style clones, and many generic 16-button
// joypads. Layout:
//   data[0] : face & shoulder buttons
//             bit0=Square  bit1=Cross  bit2=Circle  bit3=Triangle
//             bit4=L1      bit5=R1     bit6=L2      bit7=R2
//   data[1] : menu / stick buttons
//             bit0=Share   bit1=Options bit2=L3     bit3=R3
//   data[2] : HAT switch (low nibble; 0..7 = direction, >=8 = center)
//   data[3..6] : Lx, Ly, Rx, Ry axes (0x80 resting)
//
// Mapping to NES button indices (see hid_uart_proto.h NARYA_EVT_*):
//   D-pad  -> 0..3 (up/down/left/right)
//   Circle -> 6 (A)   - right face button feels natural for NES "A"
//   Cross  -> 7 (B)   - bottom face button
//   Options -> 4 (Start)
//   Share   -> 5 (Select)
static void decode_hori(gamepad_t *pad, const uint8_t *data, size_t len)
{
    if (len < 3) return;

    uint8_t b0 = data[0];
    uint8_t b1 = (len > 1) ? data[1] : 0;
    uint8_t hat = data[2] & 0x0F;

    // Reset combo: rising edge of (R2 && Share) triggers a core reset.
    bool combo_now = (b0 & RESET_COMBO_B0_MASK) && (b1 & RESET_COMBO_B1_MASK);
    if (combo_now && !pad->reset_combo_held) {
        ESP_LOGI(TAG, "reset combo (Select+R2) detected -> pulsing core reset");
        core_reset_pulse();
    }
    pad->reset_combo_held = combo_now;

    uint8_t new_btns = 0;
    if (hat < 8) new_btns |= HAT_TO_DPAD[hat];

    if (b0 & (1 << 2)) new_btns |= (1 << 6);  // Circle -> A
    if (b0 & (1 << 1)) new_btns |= (1 << 7);  // Cross  -> B

    if (b1 & (1 << 0)) new_btns |= (1 << 5);  // Share   -> Select
    if (b1 & (1 << 1)) new_btns |= (1 << 4);  // Options -> Start

    uint8_t diff = new_btns ^ pad->prev_btns;
    for (int b = 0; b < 8; ++b) {
        if (diff & (1 << b)) emit(b, (new_btns >> b) & 1);
    }
    pad->prev_btns = new_btns;
    pad->prev_hat  = hat;
}

// ---------- HID-host interface event -----------------------------------------

static void on_hid_iface_event(hid_host_device_handle_t handle,
                               const hid_host_interface_event_t event,
                               void *arg)
{
    (void)arg;
    uint8_t buf[MAX_REPORT_LEN];
    size_t  got = 0;

    switch (event) {
    case HID_HOST_INTERFACE_EVENT_INPUT_REPORT: {
        gamepad_t *pad = find_pad(handle);
        if (!pad) return;
        esp_err_t err = hid_host_device_get_raw_input_report_data(handle, buf, sizeof(buf), &got);
        if (err == ESP_OK && got > 0) {
            if (pad->dump_count < REPORT_DUMP_COUNT) {
                dump_report(pad, buf, got);
                pad->dump_count++;
            }
            decode_hori(pad, buf, got);
        }
        break;
    }
    case HID_HOST_INTERFACE_EVENT_DISCONNECTED:
        ESP_LOGI(TAG, "device disconnected handle=%p", (void*)handle);
        hid_host_device_close(handle);
        // Compact the pad table by overwriting the gone entry with the last.
        for (size_t i = 0; i < s_pad_count; ++i) {
            if (s_pads[i].handle == handle) {
                s_pads[i] = s_pads[--s_pad_count];
                break;
            }
        }
        break;
    case HID_HOST_INTERFACE_EVENT_TRANSFER_ERROR:
        ESP_LOGW(TAG, "transfer error handle=%p", (void*)handle);
        break;
    default:
        break;
    }
}

// ---------- HID-host driver event (connect) ----------------------------------

static void on_hid_driver_event(hid_host_device_handle_t handle,
                                const hid_host_driver_event_t event,
                                void *arg)
{
    (void)arg;
    if (event != HID_HOST_DRIVER_EVENT_CONNECTED) return;

    hid_host_dev_params_t params = {0};
    if (hid_host_device_get_params(handle, &params) != ESP_OK) {
        ESP_LOGW(TAG, "get_params failed for handle=%p", (void*)handle);
        return;
    }

    hid_host_dev_info_t info = {0};
    hid_host_get_device_info(handle, &info);
    ESP_LOGI(TAG, "connected addr=%u iface=%u sub=%u proto=%u VID=0x%04X PID=0x%04X",
             params.addr, params.iface_num, params.sub_class, params.proto,
             info.VID, info.PID);

    gamepad_t *pad = alloc_pad(handle);
    if (!pad) {
        ESP_LOGW(TAG, "no slot for new device");
        return;
    }
    pad->vid = info.VID;
    pad->pid = info.PID;

    const hid_host_device_config_t cfg = {
        .callback     = on_hid_iface_event,
        .callback_arg = NULL,
    };
    if (hid_host_device_open(handle, &cfg) != ESP_OK) {
        ESP_LOGE(TAG, "hid_host_device_open failed");
        return;
    }
    pad->opened = true;
    if (hid_host_device_start(handle) != ESP_OK) {
        ESP_LOGE(TAG, "hid_host_device_start failed");
    }
}

// ---------- Background tasks -------------------------------------------------

static void usb_host_task(void *arg)
{
    (void)arg;
    while (true) {
        uint32_t flags = 0;
        usb_host_lib_handle_events(portMAX_DELAY, &flags);
        if (flags & USB_HOST_LIB_EVENT_FLAGS_NO_CLIENTS) {
            usb_host_device_free_all();
        }
        if (flags & USB_HOST_LIB_EVENT_FLAGS_ALL_FREE) {
            // No devices left; keep looping (we never uninstall here).
        }
    }
}

static void hid_event_task(void *arg)
{
    (void)arg;
    while (true) {
        // Block forever waiting on HID events; the class driver pumps both
        // driver-level (connect) and interface-level (report) events through
        // this single call.
        hid_host_handle_events(portMAX_DELAY);
    }
}

// ---------- Public init ------------------------------------------------------

esp_err_t usb_gamepad_init(usb_gamepad_btn_cb_t cb, void *user)
{
    s_cb      = cb;
    s_cb_user = user;

    s_event_q = xQueueCreate(8, sizeof(app_event_t));
    if (!s_event_q) return ESP_ERR_NO_MEM;

    const usb_host_config_t host_cfg = {
        .skip_phy_setup    = false,
        .intr_flags        = ESP_INTR_FLAG_LEVEL1,
    };
    esp_err_t err = usb_host_install(&host_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "usb_host_install: %s", esp_err_to_name(err));
        return err;
    }

    BaseType_t ok = xTaskCreatePinnedToCore(usb_host_task, "usb_host",
                                            USB_HOST_TASK_STACK_BYTES, NULL,
                                            USB_HOST_TASK_PRIORITY, NULL, 0);
    if (ok != pdPASS) return ESP_FAIL;

    const hid_host_driver_config_t hid_cfg = {
        .create_background_task = false,    // we run hid_event_task ourselves
        .task_priority          = HID_EVENT_TASK_PRIORITY,
        .stack_size             = HID_EVENT_TASK_STACK_BYTES,
        .core_id                = 0,
        .callback               = on_hid_driver_event,
        .callback_arg           = NULL,
    };
    err = hid_host_install(&hid_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "hid_host_install: %s", esp_err_to_name(err));
        return err;
    }

    ok = xTaskCreatePinnedToCore(hid_event_task, "hid_event",
                                 HID_EVENT_TASK_STACK_BYTES, NULL,
                                 HID_EVENT_TASK_PRIORITY, NULL, 0);
    if (ok != pdPASS) return ESP_FAIL;

    ESP_LOGI(TAG, "USB host + HID host up");
    return ESP_OK;
}

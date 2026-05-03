// HID UART wire protocol shared by hid (ESP32-S3, TX) and core (ESP32-WROVER, RX).
//
// Wire format (variable length, header 3B + payload + 1B CRC):
//   [0]                 SOF             = 0xAA
//   [1]                 type            = narya_hid_evt_t
//   [2]                 high nibble = seq (0..15), low nibble = payload length (0..15)
//   [3 .. 3+len-1]      payload (per type, see narya_hid_evt_t comments)
//   [3+len]             CRC8 over bytes [1 .. 3+len-1]   (poly=0x07, init=0x00)
//
// Why no ACK / no retransmit: HID events are state-driven. A missed press is
// always followed by a release on next change, and idle drift is corrected by
// HEARTBEAT. The receiver is allowed to drop frames whose CRC fails.
//
// IMPORTANT: keep this header identical between core/main/include/ and
// hid/main/include/. Symlinks are forbidden by project policy.

#ifndef NARYA_HID_UART_PROTO_H
#define NARYA_HID_UART_PROTO_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define NARYA_HID_SOF              0xAAu
#define NARYA_HID_MAX_PAYLOAD      8u
#define NARYA_HID_MAX_FRAME        (1u + 1u + 1u + NARYA_HID_MAX_PAYLOAD + 1u) // 12

typedef enum {
    NARYA_EVT_BTN_DOWN  = 0x01,  // payload[0] = button index (0..255)
    NARYA_EVT_BTN_UP    = 0x02,  // payload[0] = button index (0..255)
    NARYA_EVT_AXIS      = 0x03,  // payload[0] = axis index, payload[1] = signed value (int8 reinterpreted)
    NARYA_EVT_HEARTBEAT = 0xF0,  // no payload
} narya_hid_evt_t;

typedef struct {
    uint8_t  type;     // narya_hid_evt_t
    uint8_t  seq;      // 0..15, monotonically increasing modulo 16
    uint8_t  len;      // 0..NARYA_HID_MAX_PAYLOAD
    uint8_t  payload[NARYA_HID_MAX_PAYLOAD];
} narya_hid_msg_t;

// CRC-8 / CCITT (poly=0x07, init=0x00). Table-free; called once per frame.
static inline uint8_t narya_hid_crc8(const uint8_t *data, size_t len) {
    uint8_t crc = 0x00;
    for (size_t i = 0; i < len; ++i) {
        crc ^= data[i];
        for (int j = 0; j < 8; ++j) {
            crc = (uint8_t)((crc & 0x80u) ? ((crc << 1) ^ 0x07u) : (crc << 1));
        }
    }
    return crc;
}

// Encode msg into out. Returns total bytes written, or 0 on error
// (null args, oversized payload, oversized seq, or insufficient out_cap).
static inline size_t narya_hid_encode(const narya_hid_msg_t *msg, uint8_t *out, size_t out_cap) {
    if (msg == NULL || out == NULL) return 0;
    if (msg->len > NARYA_HID_MAX_PAYLOAD) return 0;
    if (msg->seq > 0x0Fu) return 0;
    const size_t need = (size_t)(3u + msg->len + 1u);
    if (out_cap < need) return 0;

    out[0] = NARYA_HID_SOF;
    out[1] = msg->type;
    out[2] = (uint8_t)(((msg->seq & 0x0Fu) << 4) | (msg->len & 0x0Fu));
    for (uint8_t i = 0; i < msg->len; ++i) {
        out[3u + i] = msg->payload[i];
    }
    out[3u + msg->len] = narya_hid_crc8(&out[1], (size_t)(2u + msg->len));
    return need;
}

// Decode a complete in-buffer frame (single shot). Returns 1 on success,
// 0 on bad SOF/CRC/length. For streaming UART input use the framer in hid_uart.c.
static inline int narya_hid_decode(const uint8_t *in, size_t in_len, narya_hid_msg_t *out) {
    if (in == NULL || out == NULL || in_len < 4u) return 0;
    if (in[0] != NARYA_HID_SOF) return 0;
    const uint8_t seq = (uint8_t)((in[2] >> 4) & 0x0Fu);
    const uint8_t len = (uint8_t)(in[2] & 0x0Fu);
    if (len > NARYA_HID_MAX_PAYLOAD) return 0;
    if (in_len < (size_t)(3u + len + 1u)) return 0;
    const uint8_t crc = narya_hid_crc8(&in[1], (size_t)(2u + len));
    if (crc != in[3u + len]) return 0;

    out->type = in[1];
    out->seq  = seq;
    out->len  = len;
    for (uint8_t i = 0; i < len; ++i) {
        out->payload[i] = in[3u + i];
    }
    return 1;
}

#ifdef __cplusplus
}
#endif

#endif // NARYA_HID_UART_PROTO_H

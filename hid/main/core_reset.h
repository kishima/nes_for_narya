// Open-drain reset line into the core MCU's EN pin.
// External pull-up keeps the line HIGH so the core normally runs;
// core_reset_pulse() drives it LOW briefly then releases (high-Z) to
// trigger a hardware reset on the WROVER.

#ifndef NARYA_CORE_RESET_H
#define NARYA_CORE_RESET_H

#ifdef __cplusplus
extern "C" {
#endif

// Configure NARYA_HID_CORE_RESET as open-drain output and park the line
// in its released (high-Z) state. Idempotent.
void core_reset_init(void);

// Hold the EN pin LOW for 100 ms then release. Non-blocking afterwards;
// callers that need the core to be ready (UART RX up etc.) should add
// their own delay. Safe to call from any task context.
void core_reset_pulse(void);

#ifdef __cplusplus
}
#endif

#endif // NARYA_CORE_RESET_H

// Diagnostic instrumentation toggle for the Narya core firmware.
//
// When NARYA_DIAG is defined, the revertable [mapper001-diag] and
// [ppu-diag] log lines (and their underlying counters / snapshot
// fields) are compiled in. Default is off so production builds stay
// quiet. Search the source for `diag (revertable)` markers to find
// the wrapped blocks; each block is gated by `#ifdef NARYA_DIAG`.
//
// To re-enable, uncomment the line below or pass -DNARYA_DIAG=1 via
// the build system. The instrumentation lives in:
//   core/main/main.cpp                      (1 Hz emu_task log)
//   core/main/emu/anemoia/mappers/mapper001.cpp (MMC1 register writes)
//   core/main/emu/anemoia/ppu2C02.cpp       (PPU vblank + $2002 reads)
//
// Background on what this is used for is in
// doc/mmc1-post-config-hang.md and doc/apu-irq-investigation.md.

#ifndef NARYA_DIAG_H
#define NARYA_DIAG_H

// #define NARYA_DIAG 1

#endif

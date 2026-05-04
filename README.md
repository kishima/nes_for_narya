# nes_for_narya

A two-MCU NES emulator for the [Narya board](https://github.com/family-mruby/narya-board) : an ESP32-WROVER drives
NTSC composite video and I2S audio while running the
[Anemoia-ESP32](https://github.com/Shim06/Anemoia-ESP32) NES core; an
ESP32-S3 hosts a USB HID gamepad and forwards button events over UART.

The project began as a Narya port of Peter Barrett's
[`esp_8_bit`](https://github.com/rossumur/esp_8_bit) (the NTSC pipeline
was lifted from there), then dropped Atari / SMS support and swapped
the original Nofrendo NES core for Anemoia for higher per-mapper
fidelity and full 44.1 kHz APU.

See [doc/porting_plan.md](doc/porting_plan.md) for the early
implementation plan.

## Topology

```
USB Gamepad ─► ESP32-S3 (hid)
                ├─ Vbus enable
                ├─ EN reset (open-drain) ──► ESP32-WROVER EN
                └─ UART1 TX ──────────────► ESP32-WROVER UART1 RX
                                              │
                                  ESP32-WROVER (core)
                                  ├─ NTSC composite (DAC1 / GPIO25) ─► TV
                                  ├─ I2S (BCK/WS/DOUT = 32/33/27) ──► amp
                                  └─ Anemoia-ESP32 NES emulator
```

Pin maps live in `core/main/include/narya_pin_assign.h` and
`hid/main/include/narya_pin_assign.h`.

## Layout

| Path | MCU | Role |
|------|-----|------|
| [core/](core/) | ESP32-WROVER | NTSC, I2S audio, ROM store, Anemoia, UART RX |
| [hid/](hid/) | ESP32-S3 | USB-Host, HID decoder, UART TX, core reset GPIO |
| [doc/](doc/) | — | Design notes |

Both firmwares are pure ESP-IDF v5.x and built inside Docker.

## Build & flash

Prerequisite: Docker. The default toolchain image is
`ghcr.io/family-mruby/fmruby-esp32-build:latest`; override with
`DOCKER_IMAGE` if needed.

Place NES ROMs (any `*.nes`) under `core/data/nofrendo/` (the
directory name is preserved from the project's earlier life). They
are gitignored and concatenated into a custom raw `roms` partition at
build time.

```sh
rake build         # build core then hid
cd core && rake check-port && rake flash
cd hid  && rake check-port && rake flash
```

Use `rake monitor` (per subproject) for the serial console.

## Controls

The HID decoder targets PS-style HID gamepads without a report-ID byte
(D-pad on byte 2 hat, face buttons on byte 0, menu buttons on byte 1).

| Pad | NES |
|-----|-----|
| D-pad | D-pad |
| Cross / Circle | B / A |
| Share / Options | Select / Start |
| **Share + R2** | **Reset core (back to ROM picker)** |

The first 20 raw HID reports per device are logged in hex on the hid
serial console so a different layout can be added in
`hid/main/usb/usb_gamepad.c::decode_hori`.

## Compatibility

Anemoia-ESP32 implements mappers 0 / 1 / 2 / 3 / 4 / 69 covering ~79 %
of the NES catalogue. Tested titles in this port:

- NROM (mapper 0)
- MMC1 (mapper 1) with CHR-ROM
- MMC3 (mapper 4) with CHR-ROM
- MMC3 (mapper 4) with CHR-RAM + 512 KB PRG (NES-TGROM) — required a
  Narya-side fix to `mapper004_ppuWrite` for CHR-RAM stores
- MMC1 (mapper 1) with CHR-RAM saves: boot and create-save flow works,
  but save persistence is currently in-memory only (see "known limits")
- Mapper 7 (AxROM) and beyond: not implemented in Anemoia

The task watchdog is set to log-only, so a stuck game keeps the
firmware alive. Press **Share + R2** on the pad to soft-reset the core
back to the ROM picker.

### Known limits

- Battery PRG-RAM (used by RPGs for save data) lives in DRAM only. It
  survives within one play session but is lost on reset / ROM swap /
  power-off. A flash-backed save slot scheme is on the to-do list.

## Architecture highlights

- **NTSC out**: I2S0 + APLL + DMA + DAC1, ported from esp_8_bit with
  all Arduino API removed.
- **Audio**: 44.1 kHz / 16-bit mono I2S; Anemoia's APU drives a
  separate apu_task pinned to core 0.
- **NES core**: Anemoia-ESP32 vendored under
  [`core/main/emu/anemoia/`](core/main/emu/anemoia/) plus a thin
  `EmuAnemoia` glue (`core/main/emu/emu_anemoia.cpp`) that exposes
  Anemoia's scanline + audio output as the existing `Emu` interface.
- **Bank cache bypass**: the upstream Anemoia software bank cache
  (~170 KB DRAM) is replaced with direct `flash-XIP` access through
  `Cartridge::prgRomPtr / chrRomPtr` so we keep ~180 KB free heap for
  the rest of the firmware.
- **ROM store**: a custom raw partition (subtype 0x40) holds a
  4 KB directory plus 4 KB-aligned ROM blobs;
  `tools/build_roms.py` generates the image at build time and
  `esp_partition_mmap` hands the emulator a flash-XIP pointer at
  runtime (no PSRAM round-trip).
- **ROM picker**: pre-emulator menu rendered into a 256x240 NES
  framebuffer with the bundled 5x7 font; the buffer is freed before
  `Emu::insert` so PRG bank allocations land in DRAM.
- **HID UART link**: 5-byte minimum frame (SOF / type / seq+len /
  payload / CRC8). Stateless, no ACK.
- **Coordinated reset**: the hid firmware drives an open-drain line
  into the core's EN pin, so a press of the hid reset button or the
  Share+R2 combo brings both MCUs back together.

## License

Licensed as a whole under **GPL v3 or later**; see [LICENSE](LICENSE).
Per-component attribution and compatibility reasoning are in
[THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).

NES ROM copyrights are out of scope of this repository; bring your own.
</content>
</invoke>
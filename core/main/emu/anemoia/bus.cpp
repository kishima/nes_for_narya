#include "bus.h"

// Diagnostic counters / state samples. Main's emu_task drains and logs
// these once per second to see what the emulator is actually doing.
extern "C" volatile uint32_t g_strobe_writes      = 0;  // CPU writes to $4016
extern "C" volatile uint32_t g_controller_reads   = 0;  // CPU reads of $4016
extern "C" volatile uint32_t g_render_clocks      = 0;  // bus.clock with frame_latch=false
extern "C" volatile uint32_t g_skip_clocks        = 0;  // bus.clock with frame_latch=true
// OR-masks aggregated over the 1 Hz window so brief register pokes do not
// fall between log lines. mask_or / ctrl_or capture every value the CPU
// wrote to PPUMASK ($2001) / PPUCTRL ($2000) within the window, so a game
// that toggles render_sprite mid-frame still shows up. controller_or =
// what hid_rx_task pushed onto bus->controller, strobe_or = what the
// game latched at $4016 strobe writes. If controller_or has bits the
// game never sees, the strobe is racing the input path; if both are 0
// while pressing, hid->bus is broken; if both have bits, the input is
// reaching the game.
extern "C" volatile uint8_t  g_mask_or            = 0;
extern "C" volatile uint8_t  g_ctrl_or            = 0;
extern "C" volatile uint8_t  g_controller_or      = 0;
extern "C" volatile uint8_t  g_strobe_latched_or  = 0;
// 6502 PC sampled at the start of every bus.clock(). g_pc_last is the
// most recent value, g_pc_min / g_pc_max record the [min, max] range
// across the 1 Hz log window. A tight init-stuck loop shows a narrow
// range (a few bytes of PRG); a healthy main loop ranges across hundreds
// of bytes. Reset each second by main when the diag line is emitted.
extern "C" volatile uint16_t g_pc_last             = 0;
extern "C" volatile uint16_t g_pc_min              = 0xFFFF;
extern "C" volatile uint16_t g_pc_max              = 0x0000;

Bus::Bus()
{
    memset(RAM, 0, sizeof(RAM));
    cpu.connectBus(this);
    cpu.apu.connectBus(this);
    ppu.connectBus(this);
}

Bus::~Bus()
{
}

IRAM_ATTR void Bus::cpuWrite(uint16_t addr, uint8_t data)
{
    if (cart->cpuWrite(addr, data)) {}
    else if ((addr & 0xE000) == 0x0000) { RAM[addr & 0x07FF] = data; }
    else if ((addr & 0xE000) == 0x2000) {
        // Snoop PPUCTRL / PPUMASK writes for the diagnostic OR-masks
        // before forwarding so we capture every poke the game made within
        // the 1 Hz window.
        uint8_t reg = addr & 0x0007;
        if (reg == 0x00) g_ctrl_or = (uint8_t)(g_ctrl_or | data);
        if (reg == 0x01) g_mask_or = (uint8_t)(g_mask_or | data);
        ppu.cpuWrite(reg, data);
    }
    else if ((addr & 0xF000) == 0x4000 && (addr <= 0x4013 || addr == 0x4015 || addr == 0x4017))
    {
        cpu.apuWrite(addr, data);
    }
    else if (addr == 0x4014) { cpu.OAM_DMA(data); }
    else if (addr == 0x4016)
    {
        g_strobe_writes++;
        controller_strobe = data & 1;
        if (controller_strobe) {
            controller_state = controller;
            g_strobe_latched_or = (uint8_t)(g_strobe_latched_or | controller);
        }
    }
}

IRAM_ATTR uint8_t Bus::cpuRead(uint16_t addr)
{
    uint8_t data = 0x00;

    if (cart->cpuRead(addr, data)) {}
    else if ((addr & 0xE000) == 0x0000) { data = RAM[addr & 0x07FF]; }
    else if ((addr & 0xE000) == 0x2000) { data = ppu.cpuRead(addr & 0x0007); }
    else if (addr == 0x4016)
    {
        g_controller_reads++;
        uint8_t value = controller_state & 1;
        if (!controller_strobe) controller_state >>= 1;
        data = value | 0x40;
    }
    return data;
}

void Bus::reset()
{
    // Narya port: no TFT to clear; the NTSC pipeline takes over once
    // emu_task starts publishing scanlines.
    for (auto& i : RAM) i = 0x00;
    cart->reset();
    cpu.reset();
    cpu.apu.reset();
    ppu.reset();
}

IRAM_ATTR void Bus::clock()
{
    // Capture bus->controller OR-mask so a held button shows up reliably
    // in the 1 Hz log even if it does not coincide with a strobe write.
    // PPUMASK / PPUCTRL OR-masks are updated inline in cpuWrite, no need
    // to sample them here.
    g_controller_or = (uint8_t)(g_controller_or | controller);
    if (frame_latch) g_skip_clocks++; else g_render_clocks++;

    // Snapshot the 6502 PC at the start of each frame. Only one sample
    // per frame so this is cheap; widening to a per-instruction trace
    // would need cpu6502 hooks. The min/max bracket aggregates across
    // every bus.clock() in the 1 Hz window and is what tells init-stuck
    // games (small range) apart from running ones (large range).
    uint16_t pc = cpu.PC;
    g_pc_last = pc;
    if (pc < g_pc_min) g_pc_min = pc;
    if (pc > g_pc_max) g_pc_max = pc;

    // 1 frame == 341 dots * 261 scanlines
    // Visible scanlines 0-239

    // Rendering 3 scanlines at a time because 1 CPU clock == 3 PPU clocks
    // and there's only 341 ppu clocks (dots) in a scanline, which is not divisible by 3.
    // Using a counter/for loop with += 341 & -= 3 is too big of a performance hit.
    // 1 scanline == ~113.67 CPU clocks, so for every 3 scanlines, two scanlines will have an extra
    // CPU clock
    if (!frame_latch)
    {
        for (ppu_scanline = 0; ppu_scanline < 240; ppu_scanline += 3)
        {
            cpu.clock(113);
            ppu.renderScanline(ppu_scanline);

            cpu.clock(114);
            ppu.renderScanline(ppu_scanline + 1);

            cpu.clock(114);
            ppu.renderScanline(ppu_scanline + 2);
        }
    }
    else
    {
        for (ppu_scanline = 0; ppu_scanline < 240; ppu_scanline += 3)
        {
            cpu.clock(113);
            ppu.fakeSpriteHit(ppu_scanline);

            cpu.clock(114);
            ppu.fakeSpriteHit(ppu_scanline + 1);

            cpu.clock(114);
            ppu.fakeSpriteHit(ppu_scanline + 2);
        }
    }

    // Setup for the next frame
    // Same reason as scanlines 0-239, 2/3 of scanlines will have an extra CPU clock.
    // Scanline 240
    cpu.clock(113);

    // Scanline 241-261
    ppu.setVBlank();
    cpu.clock(2501);

    ppu.clearVBlank();
    cpu.clock(114);

#ifdef FRAMESKIP
    frame_latch = !frame_latch;
#endif
}

IRAM_ATTR void Bus::setPPUMirrorMode(Cartridge::MIRROR mirror)
{
    ppu.setMirror(mirror);
}

Cartridge::MIRROR Bus::getPPUMirrorMode()
{
    return ppu.getMirror();
}

IRAM_ATTR void Bus::OAM_Write(uint8_t addr, uint8_t data)
{
    ppu.ptr_sprite[addr] = data;
}

void Bus::insertCartridge(Cartridge* cartridge)
{
    cart = cartridge;
    cpu.connectCartridge(cartridge);
    ppu.connectCartridge(cartridge);
    cart->connectBus(this);
}

void Bus::connectScreen(TFT_eSPI* screen)
{
    // Narya port: no TFT panel; the framebuffer goes to the NTSC composite
    // pipeline through narya_anemoia_publish_scanlines instead.
    (void)screen;
    ptr_screen = nullptr;
}

IRAM_ATTR void Bus::renderImage(uint16_t scanline)
{
    // Anemoia calls renderImage with the scanline index of the *first* line
    // in the just-completed SCANLINES_PER_BUFFER chunk.
    narya_anemoia_publish_scanlines(ppu.ptr_display, (int)scanline,
                                    SCANLINES_PER_BUFFER);
}

IRAM_ATTR void Bus::IRQ()
{
    cpu.IRQ();
}

IRAM_ATTR void Bus::NMI()
{
    cpu.NMI();
}

// Narya port: no SD card, save state is not wired up. Stubbed so the
// existing call sites (and Bus class API) keep compiling.
void Bus::saveState() {}
void Bus::loadState() {}
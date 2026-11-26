// SPDX-License-Identifier: GPL-3.0-or-later
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include "GearboyCore.h"
#include "EverdriveOSMemoryRule.h"

bool g_mcp_stdio_mode = true;

static void Check(bool ok, const char* message)
{
    if (!ok)
    {
        fprintf(stderr, "FAIL: %s\n", message);
        exit(1);
    }
}

static void LoadProbe(GearboyCore& core, const u8* program, size_t size, bool cgb = false)
{
    u8 rom[0x8000] = {};
    const u8 entry[] = { 0xC3, 0x50, 0x01 }; // jp $0150
    memcpy(rom + 0x100, entry, sizeof(entry));
    memcpy(rom + 0x134, "GBXOS", 5);
    memcpy(rom + 0x13F, "KRKZ", 4);
    rom[0x143] = cgb ? 0x80 : 0;
    memcpy(rom + 0x150, program, size);
    core.Init();
    Check(core.LoadROMFromBuffer(rom, sizeof(rom), false), "load RTC probe ROM");
    Check(core.GetMapperType() == Cartridge::CartridgeEverdriveOS, "probe uses the EverDrive OS rule");
}

static void RunFrames(GearboyCore& core, unsigned count, bool linked_dispatch = false)
{
    u16 frame[SGB_SCREEN_WIDTH * SGB_SCREEN_HEIGHT];
    s16 audio[AUDIO_BUFFER_SIZE];
    int samples = 0;
    for (unsigned i = 0; i < count; ++i)
    {
        if (linked_dispatch)
        {
            // Linked libretro drives each core directly and finishes its audio frame separately.
            unsigned int clocks, total_clocks = 0;
            while (!core.RunCycle(frame, clocks))
            {
                total_clocks += clocks;
                Check(total_clocks <= GAMEBOY_CLOCKS_SAFE_LIMIT, "direct core dispatch reaches VBlank");
            }
            core.EndFrame(audio, &samples);
        }
        else
            core.RunToVBlank(frame, audio, &samples);
    }
}

static void LatchSeconds(Memory& memory, u8& reference, u8& game)
{
    memory.Write(0xBD12, 0);
    memory.Write(0xBD12, 1);
    memory.Write(0xBD13, 0);
    reference = memory.Read(0xBD10);
    game = memory.Read(0xBD11);
}

static void TestVBlankSplit(bool linked_dispatch)
{
    const u8 program[] =
    {
        0xF3,                   // di
        0x3E, 0xA5,             // ld a,$a5
        0xEA, 0x0A, 0xBD,       // ld [$bd0a],a   ; unlock cartridge
        0x3E, 0x04,             // ld a,4
        0xEA, 0x13, 0xBD,       // ld [$bd13],a   ; select day-high/halt
        0x3E, 0x40,             // ld a,$40
        0xEA, 0x10, 0xBD,       // ld [$bd10],a   ; halt reference
        0xEA, 0x11, 0xBD,       // ld [$bd11],a   ; halt game
        0xAF,                   // xor a
        0xEA, 0x13, 0xBD,       // ld [$bd13],a   ; select seconds
        0xEA, 0x10, 0xBD,       // ld [$bd10],a
        0xEA, 0x11, 0xBD,       // ld [$bd11],a   ; reset both fractions
        0xEA, 0x00, 0xC0,       // ld [$c000],a   ; frame-boundary marker
        0x3E, 0x04,             // ld a,4
        0xEA, 0x13, 0xBD,       // ld [$bd13],a
        0xF0, 0x44,             // ldh a,[$44]
        0xFE, 150,              // cp 150         ; wait for the first VBlank
        0x20, 0xFA,             // jr nz,-6
        0xF0, 0x44,             // ldh a,[$44]
        0xFE, 143,              // cp 143         ; just before the next VBlank
        0x20, 0xFA,             // jr nz,-6
        0xAF,                   // xor a
        0xEA, 0x10, 0xBD,       // ld [$bd10],a   ; resume reference
        0x3C,                   // inc a
        0xEA, 0x00, 0xC0,       // ld [$c000],a
        0xF0, 0x44,             // ldh a,[$44]
        0xFE, 145,              // cp 145         ; just after VBlank
        0x20, 0xFA,             // jr nz,-6
        0xAF,                   // xor a
        0xEA, 0x11, 0xBD,       // ld [$bd11],a   ; resume game
        0x3E, 0x02,             // ld a,2
        0xEA, 0x00, 0xC0,       // ld [$c000],a
        0x18, 0xFE,             // jr -2
    };
    GearboyCore core;
    LoadProbe(core, program, sizeof(program));
    Memory& memory = *core.GetMemory();
    for (unsigned i = 0; i < 8 && memory.Read(0xC000) != 1; ++i)
        RunFrames(core, 1, linked_dispatch);
    Check(memory.Read(0xC000) == 1, "VBlank separates the two resume writes");

    u8 reference, game;
    RunFrames(core, 59, linked_dispatch);
    LatchSeconds(memory, reference, game);
    Check(reference == 0 && game == 0, "neither counter receives time before its resume write");
    RunFrames(core, 1, linked_dispatch);
    LatchSeconds(memory, reference, game);
    Check(reference == 1 && game == 1, "both counters advance after a full second");
}

static void TestShortPulses()
{
    const u8 program[] =
    {
        0xF3,                   // di
        0x3E, 0xA5,             // ld a,$a5
        0xEA, 0x0A, 0xBD,       // ld [$bd0a],a   ; unlock cartridge
        0xAF,                   // xor a
        0xEA, 0x13, 0xBD,       // ld [$bd13],a   ; select seconds
        0xEA, 0x11, 0xBD,       // ld [$bd11],a   ; reset fraction
        0x3E, 0x04,             // ld a,4
        0xEA, 0x13, 0xBD,       // ld [$bd13],a   ; select day-high/halt
        0x21, 0x11, 0xBD,       // ld hl,$bd11
        0x06, 0x40,             // ld b,$40
        0xAF,                   // xor a
        0x77,                   // ld [hl],a     ; resume for 8 clocks
        0x70,                   // ld [hl],b     ; halt for the next 68 clocks
        0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, // 12 nops
        0x18, 0xF0,             // jr -16        ; total loop: 76 clocks
    };
    GearboyCore core;
    LoadProbe(core, program, sizeof(program));
    RunFrames(core, 600);
    u8 reference, game;
    LatchSeconds(*core.GetMemory(), reference, game);
    Check(reference == 10 && game == 1, "a batch counts time between two halt writes");
}

static void TestGDMA()
{
    const u8 program[] =
    {
        0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, // 13 nops
        0x3E, 0x0F,             // ld a,15
        0xE0, 0x55,             // ldh [$55],a    ; 516-clock DMA from the SRAM shadow
        0xFA, 0xF0, 0xBF,       // ld a,[$bff0]   ; reference seconds after DMA
        0xEA, 0x00, 0xC0,       // ld [$c000],a
        0x18, 0xFE,             // jr -2
    };
    GearboyCore core;
    LoadProbe(core, program, sizeof(program), true);
    Memory& memory = *core.GetMemory();
    memory.Write(0xBD0A, 0xA5);
    memory.Write(0xBD02, 0xFF);
    memory.Write(0xBD06, 0x06);
    memory.Write(0xBD13, 6);
    memory.Write(0xBD10, 1);
    memory.Write(0xFF51, 0xBF);
    memory.Write(0xFF52, 0xF0);
    memory.Write(0xFF53, 0);
    memory.Write(0xFF54, 0);
    auto* cart = static_cast<EverdriveOSMemoryRule*>(memory.GetCurrentRule());
    // The jump, NOPs, load and DMA total 604 clocks, including the stall.
    cart->AdvanceCycles(GEARBOY_MASTER_CLOCK_RATE - 604);
    RunFrames(core, 1);
    Check(memory.Read(0xC000) == 1, "RTC shadow advances through a DMA stall inside the batch");
    u8 reference, game;
    LatchSeconds(memory, reference, game);
    Check(reference == 1 && game == 1, "DMA does not overflow either counter's batch time");
}

int main()
{
    TestVBlankSplit(false);
    TestVBlankSplit(true);
    TestShortPulses();
    TestGDMA();
    puts("EverDrive RTC timing tests passed");
}

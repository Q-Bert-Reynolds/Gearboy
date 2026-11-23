// SPDX-License-Identifier: GPL-3.0-or-later
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sstream>
#include "Cartridge.h"
#include "EverdriveOSMemoryRule.h"

bool g_mcp_stdio_mode = true;

static const unsigned int kSecond = GEARBOY_MASTER_CLOCK_RATE;
static const u16 kReference = 0xBD10;
static const u16 kGame = 0xBD11;
static const u16 kLatch = 0xBD12;
static const u16 kIndex = 0xBD13;

static void Check(bool ok, const char* message)
{
    if (!ok)
    {
        fprintf(stderr, "FAIL: %s\n", message);
        exit(1);
    }
}

static void WriteField(EverdriveOSMemoryRule& cart, u16 port, u8 field, u8 value)
{
    cart.PerformWrite(kIndex, field);
    cart.PerformWrite(port, value);
}

static u8 ReadField(EverdriveOSMemoryRule& cart, u16 port, u8 field)
{
    cart.PerformWrite(kIndex, field);
    return cart.PerformRead(port);
}

static void Latch(EverdriveOSMemoryRule& cart)
{
    cart.PerformWrite(kLatch, 0);
    cart.PerformWrite(kLatch, 1);
}

static void TestCounters(Cartridge& rom)
{
    EverdriveOSMemoryRule cart(NULL, &rom);
    cart.PerformWrite(0xBD0A, 0xA5);
    const u8 day511[] = {59, 59, 23, 255, 1};
    const u8 halted[] = {17, 15, 4, 83, 0x40};
    for (u8 i = 0; i < 5; ++i)
    {
        WriteField(cart, kReference, i, day511[i]);
        WriteField(cart, kGame, i, halted[i]);
    }

    cart.AdvanceCycles(kSecond - 1);
    Latch(cart);
    Check(ReadField(cart, kReference, 0) == 59, "counter retains a fractional second");
    cart.AdvanceCycles(1);
    cart.PerformWrite(kLatch, 1);
    Check(ReadField(cart, kReference, 0) == 59, "repeated latch write keeps the snapshot");
    Latch(cart);
    for (u8 i = 0; i < 5; ++i)
    {
        Check(ReadField(cart, kReference, i) == (i == 4 ? 0x80 : 0), "day 511 rolls over with carry");
        Check(ReadField(cart, kGame, i) == halted[i], "game clock halts independently");
    }

    u8 native[5];
    cart.GetGameRTC(native);
    Check(!memcmp(native, halted, sizeof(native)), "$BD11 supplies the native game clock");
    WriteField(cart, kGame, 4, 0);
    WriteField(cart, kReference, 6, 1);
    cart.AdvanceCycles(2 * kSecond);
    Latch(cart);
    Check(ReadField(cart, kReference, 4) == 0x80, "day carry stays set");
    Check(ReadField(cart, kGame, 0) == 19, "game clock resumes");
    Check(cart.GetSaveRAM()[0x1FFF0] == 2 && cart.GetSaveRAM()[0x1FFF1] == 19,
        "SRAM mirrors reference and game seconds in port order");

    const u8 returned[] = {37, 31, 17, 99, 1};
    cart.SetGameRTC(returned);
    Latch(cart);
    for (u8 i = 0; i < 5; ++i)
        Check(ReadField(cart, kGame, i) == returned[i], "native game returns its clock to $BD11");
}

static void TestSaveState(Cartridge& rom)
{
    EverdriveOSMemoryRule cart(NULL, &rom);
    cart.PerformWrite(0xBD0A, 0xA5);
    WriteField(cart, kReference, 6, 1);
    WriteField(cart, kGame, 0, 5);
    WriteField(cart, kReference, 6, 0);
    Latch(cart);
    cart.AdvanceCycles(2 * kSecond + kSecond / 2);
    cart.PerformWrite(kIndex, 3);

    std::stringstream state;
    cart.SaveState(state);
    state.put('!');
    WriteField(cart, kReference, 6, 1);
    cart.AdvanceCycles(3 * kSecond);
    cart.PerformWrite(kLatch, 0);
    state.seekg(0);
    cart.LoadState(state);
    Check(state.get() == '!', "read only the RTC state payload");
    Check(cart.PerformRead(kIndex) == 3, "restore the selected RTC register");
    cart.PerformWrite(kLatch, 1);
    Check(ReadField(cart, kGame, 0) == 5, "restore the latched value and latch input");
    Latch(cart);
    Check(ReadField(cart, kGame, 0) == 7, "restore the live clock from the time of the save");
    cart.AdvanceCycles(kSecond / 2);
    Latch(cart);
    Check(ReadField(cart, kGame, 0) == 8, "restore the fractional second");
    Check(cart.GetSaveRAM()[0x1FFF1] == 5, "restore the released SRAM mirror");

    const size_t legacy_size = 0x20000 + EverdriveOSMemoryRule::k_save_size + sizeof(bool) + 5;
    std::istringstream legacy(state.str().substr(0, legacy_size) + '!');
    cart.LoadState(legacy, GB_SAVESTATE_EVERDRIVE_RTC_VERSION - 1);
    Check(legacy.get() == '!', "old save states have no RTC payload");
    Latch(cart);
    Check(ReadField(cart, kGame, 0) == 5, "old save states still restore the SRAM copy");
}

int main()
{
    u8 image[0x8000] = {};
    memcpy(image + 0x134, "GBXOS", 5);
    memcpy(image + 0x13F, "KRKZ", 4);
    Cartridge rom;
    rom.Init();
    Check(rom.LoadFromBuffer(image, sizeof(image)), "load synthetic EverDrive OS header");
    TestCounters(rom);
    TestSaveState(rom);
    puts("EverDrive RTC tests passed");
}

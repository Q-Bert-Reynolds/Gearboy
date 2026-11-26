//
//  EverdriveOSMemoryRule.cpp
//  by Nolan Baker (Q*Bert Reynolds)
//

#include <cstdlib>
#include "EverdriveOSMemoryRule.h"
#include "Memory.h"
#include "Cartridge.h"
#include <cstring>

EverdriveOSMemoryRule::EverdriveOSMemoryRule(Memory* pMemory, Cartridge* pCartridge) : MemoryRule(pMemory, pCartridge)
{
    m_pOS = new u8[0x20000];
    m_pPSRAM = new u8[k_psram_size];
    memset(m_pPSRAM, 0xFF, k_psram_size);
    m_bUnlocked = false;
    m_Block[0] = m_Block[1] = 0;
    m_bLatchHigh = getenv("GEARBOY_EVERDRIVE_LATCH_BD03") != NULL;
    m_PendingHigh = 0;
    m_Masks[0] = m_Masks[1] = 0;
    m_Config = 0x04;
    m_Mapper = 0;
    m_Page = 0x04;
    m_bLaunch = false;
    m_Switch = 0;
    memset(m_GameMBC, 0, sizeof(m_GameMBC));
    memset(m_GameRTC, 0, sizeof(m_GameRTC));
    memset(m_LatchedRTC, 0, sizeof(m_LatchedRTC));
    memset(m_RTCCycles, 0, sizeof(m_RTCCycles));
    m_pClockBatchCycles = NULL;
    m_AccountedClockCycles = 0;
    m_ShadowEnabled = false;
    m_RTCIndex = m_RTCLatch = 0;
    m_pSave = new u8[k_save_size];
    memset(m_pSave, 0xFF, k_save_size);
    Reset(false);
}

EverdriveOSMemoryRule::~EverdriveOSMemoryRule()
{
    SafeDeleteArray(m_pOS);
    SafeDeleteArray(m_pPSRAM);
    SafeDeleteArray(m_pSave);
}

void EverdriveOSMemoryRule::Reset(bool bCGB)
{
    u8* rom = m_pCartridge->GetTheROM();
    int size = m_pCartridge->GetTotalSize();
    if (!rom || size < 0x150 || memcmp(rom + 0x134, "GBXOS", 5))
        return; // a game it launched: the image, PSRAM and save RAM stay for the in-game menu and the next boot
    m_bCGB = bCGB;
    m_bUnlocked = false;
    m_Block[0] = 0;
    m_Block[1] = 0;
    m_PendingHigh = 0;
    m_Config = 0x04; // what the bootloader leaves
    m_Mapper = 0;
    m_Page = 0x04;
    m_bLaunch = false;
    m_Switch = 0;
    m_Masks[0] = m_Masks[1] = 0;
    memset(m_pOS, 0xFF, 0x20000); // PSRAM keeps what it had, like the cart's across a reset
    if (size > 0x20000)
        Log("EverdriveOS: the X7's bootloader only takes a 128KB GBCOS.BIN (a 256KB one got 16KB), using the first 128KB");
    memcpy(m_pOS, rom, size < 0x20000 ? size : 0x20000);
    m_File.assign(m_pOS, m_pOS + 0x20000);
    m_Everdrive.Reset(m_pCartridge->GetFileDirectory());
}

// the byte the $BE00 window shows
u8* EverdriveOSMemoryRule::WindowByte(u16 address)
{
    u32 offset = ((m_Block[1] << 8 | m_Block[0]) << 9) + (address - 0xBE00);
    if (m_Config & 0x02)
    {
        const u32 ram_offset = offset & (k_save_size - 1);
        if (m_ShadowEnabled && ram_offset >= 0x1FFF0 && ram_offset < 0x1FFFA)
            SyncClockBatch();
        return &m_pSave[ram_offset];
    }
    return &m_pPSRAM[offset & (k_psram_size - 1)];
}

u8 EverdriveOSMemoryRule::RegisterRead(u16 address)
{
    if (!m_bUnlocked)
        return 0x00;
    u8 value;
    if (m_Everdrive.Read(address, &value))
        return value;
    switch (address)
    {
        case 0xBD02: return m_Block[0];
        case 0xBD03: return m_bLatchHigh ? m_PendingHigh : m_Block[1];
        case 0xBD06: return m_Config;
        case 0xBD07: return m_Mapper;
        case 0xBD08: return m_Page;
        case 0xBD10: case 0xBD11:
            return m_RTCIndex < 5 ? m_LatchedRTC[address - 0xBD10][m_RTCIndex] : 0;
        case 0xBD13: return m_RTCIndex;
        case 0xBD14: case 0xBD15: case 0xBD16: case 0xBD17: return m_GameMBC[address - 0xBD14]; // the game's mapper state
        default: return 0x00;
    }
}

u8 EverdriveOSMemoryRule::PerformRead(u16 address)
{
    if (address < 0x8000)
        return m_pOS[address];
    if (address >= 0xA000 && address < 0xC000)
    {
        if (address >= 0xBE00)
            return *WindowByte(address);
        if ((address >= 0xBD00 && address < 0xBD10) ||
            (address >= 0xBD10 && address <= 0xBD17))
            return RegisterRead(address);
        return m_pOS[(m_Page << 13) + (address - 0xA000)];
    }
    return m_pMemory->Retrieve(address);
}

u8 EverdriveOSMemoryRule::DebugRead(u16 address)
{
    if (address >= 0xBD00 && address < 0xBD10)
        return 0x00; // reading $BD00 can clock the SD card
    return PerformRead(address);
}

void EverdriveOSMemoryRule::PerformWrite(u16 address, u8 value)
{
    if (address < 0x8000)
    {
        if (!(m_Config & 0x04)) // mapper writes made by the resident resume stub
        {
            if (address < 0x2000) m_GameMBC[0] = value;
            else if (address == 0x2000 || address == 0x2100) m_GameMBC[3] = value;
            else if (address == 0x3000) m_GameMBC[1] = (m_GameMBC[1] & 1) | ((value & 1) << 1);
            else if (address == 0x4000) m_GameMBC[2] = value;
            else if (address == 0x6000) m_GameMBC[1] = value;
        }
        m_pOS[address] = value; // RAM, writing $2000 doesn't switch banks
        return;
    }
    if (address >= 0xA000 && address < 0xC000)
    {
        if (address >= 0xBE00)
        {
            u8 streamed;
            if ((m_Config & 0x01) && m_Everdrive.Read(EverdriveX::k_reg_spi_data, &streamed))
                value = streamed; // streaming ($BD06 bit 0, with auto read): a write stores the card's next byte
            *WindowByte(address) = value;
            return;
        }
        if ((address >= 0xBD00 && address < 0xBD10) ||
            (address >= 0xBD10 && address <= 0xBD17))
        {
            if (address == EverdriveX::k_reg_key)
            {
                m_bUnlocked = (value == EverdriveX::k_key_unlock);
                m_Everdrive.Write(address, value);
                return;
            }
            if (!m_bUnlocked || m_Everdrive.Write(address, value))
                return;
            switch (address)
            {
                case 0xBD02: m_Block[0] = value; if (m_bLatchHigh) m_Block[1] = m_PendingHigh; break;
                case 0xBD03: if (m_bLatchHigh) m_PendingHigh = value; else m_Block[1] = value; break;
                case 0xBD04: m_Masks[0] = value; break;
                case 0xBD05: m_Masks[1] = value; break;
                case 0xBD06: // bit 4: reboot into the OS (the EverDrive OS's "Back to menu", in-game)
                    m_Config = value;
                    if (value & 0x10)
                        m_Switch = 2;
                    break;
                case 0xBD07:
                    m_Mapper = value;
                    if (value)
                    {
                        Log("EverdriveOS: game launch, mapper %d, $BD04 %02X $BD05 %02X $BD06 %02X", value, m_Masks[0], m_Masks[1], m_Config);
                        m_bLaunch = true;
                    }
                    break;
                case 0xBD08: m_Page = value & 0x0F; break;
                case 0xBD13: m_RTCIndex = value; break;
                case 0xBD12:
                    if (m_RTCLatch == 0 && value == 1)
                    {
                        SyncClockBatch();
                        memcpy(m_LatchedRTC, m_GameRTC, sizeof(m_GameRTC));
                    }
                    m_RTCLatch = value;
                    break;
                case 0xBD10: case 0xBD11:
                    SyncClockBatch();
                    if (m_RTCIndex < 5)
                    {
                        const unsigned port = address - 0xBD10;
                        static const u8 masks[] = {0x3F, 0x3F, 0x1F, 0xFF, 0xC1};
                        m_GameRTC[port][m_RTCIndex] = value & masks[m_RTCIndex];
                        if (m_RTCIndex == 0)
                            m_RTCCycles[port] = 0;
                        if (m_ShadowEnabled)
                            CaptureRTCShadow();
                    }
                    else if (address == 0xBD10 && m_RTCIndex == 6)
                    {
                        m_ShadowEnabled = (value & 1) != 0;
                        if (m_ShadowEnabled)
                            CaptureRTCShadow();
                    }
                    break;
                case 0xBD09: // bit 1: resume at $0040, bit 2: select OS without resetting the CPU
                    if (value & 0x02)
                        m_Switch = 1;
                    else if (value & 0x04)
                        m_Switch = 3;
                    break;
                default: break;
            }
            return;
        }
        m_pOS[(m_Page << 13) + (address - 0xA000)] = value;
        return;
    }
    m_pMemory->Load(address, value);
}

void EverdriveOSMemoryRule::SaveRam(std::ostream &file)
{
    file.write(reinterpret_cast<const char*>(m_pSave), k_save_size);
}

bool EverdriveOSMemoryRule::LoadRam(std::istream &file, s32 fileSize)
{
    if ((fileSize > 0) && (fileSize != (s32)k_save_size))
    {
        Log("EverdriveOS: save RAM file is %d bytes, expected %d", fileSize, k_save_size);
        return false;
    }
    file.read(reinterpret_cast<char*>(m_pSave), k_save_size);
    return true;
}

size_t EverdriveOSMemoryRule::GetRamSize()
{
    return k_save_size;
}

u8* EverdriveOSMemoryRule::GetRamBanks()
{
    return m_pSave;
}

u8* EverdriveOSMemoryRule::GetCurrentRamBank()
{
    return m_pOS + (m_Page << 13);
}

int EverdriveOSMemoryRule::GetCurrentRamBankIndex()
{
    return m_Page;
}

u8* EverdriveOSMemoryRule::GetRomBank0()
{
    return m_pOS;
}

int EverdriveOSMemoryRule::GetCurrentRomBank0Index()
{
    return 0;
}

u8* EverdriveOSMemoryRule::GetCurrentRomBank1()
{
    return m_pOS + 0x4000;
}

int EverdriveOSMemoryRule::GetCurrentRomBank1Index()
{
    return 1;
}

void EverdriveOSMemoryRule::SaveState(std::ostream& stream)
{
    stream.write(reinterpret_cast<const char*>(m_pOS), 0x20000); // the image, PSRAM isn't saved
    stream.write(reinterpret_cast<const char*>(m_pSave), k_save_size);
    stream.write(reinterpret_cast<const char*>(&m_bUnlocked), sizeof(m_bUnlocked));
    stream.write(reinterpret_cast<const char*>(m_Block), sizeof(m_Block));
    stream.write(reinterpret_cast<const char*>(&m_Config), sizeof(m_Config));
    stream.write(reinterpret_cast<const char*>(&m_Mapper), sizeof(m_Mapper));
    stream.write(reinterpret_cast<const char*>(&m_Page), sizeof(m_Page));
    stream.write(reinterpret_cast<const char*>(m_GameRTC), sizeof(m_GameRTC));
    stream.write(reinterpret_cast<const char*>(m_LatchedRTC), sizeof(m_LatchedRTC));
    stream.write(reinterpret_cast<const char*>(m_RTCCycles), sizeof(m_RTCCycles));
    stream.write(reinterpret_cast<const char*>(&m_RTCIndex), sizeof(m_RTCIndex));
    stream.write(reinterpret_cast<const char*>(&m_RTCLatch), sizeof(m_RTCLatch));
    stream.write(reinterpret_cast<const char*>(&m_ShadowEnabled), sizeof(m_ShadowEnabled));
}

void EverdriveOSMemoryRule::LoadState(std::istream& stream, u32 version)
{
    stream.read(reinterpret_cast<char*>(m_pOS), 0x20000);
    stream.read(reinterpret_cast<char*>(m_pSave), k_save_size);
    stream.read(reinterpret_cast<char*>(&m_bUnlocked), sizeof(m_bUnlocked));
    stream.read(reinterpret_cast<char*>(m_Block), sizeof(m_Block));
    stream.read(reinterpret_cast<char*>(&m_Config), sizeof(m_Config));
    stream.read(reinterpret_cast<char*>(&m_Mapper), sizeof(m_Mapper));
    stream.read(reinterpret_cast<char*>(&m_Page), sizeof(m_Page));

    if (version >= GB_SAVESTATE_EVERDRIVE_RTC_VERSION)
    {
        stream.read(reinterpret_cast<char*>(m_GameRTC), sizeof(m_GameRTC));
        stream.read(reinterpret_cast<char*>(m_LatchedRTC), sizeof(m_LatchedRTC));
        stream.read(reinterpret_cast<char*>(m_RTCCycles), sizeof(m_RTCCycles));
        stream.read(reinterpret_cast<char*>(&m_RTCIndex), sizeof(m_RTCIndex));
        stream.read(reinterpret_cast<char*>(&m_RTCLatch), sizeof(m_RTCLatch));
        stream.read(reinterpret_cast<char*>(&m_ShadowEnabled), sizeof(m_ShadowEnabled));
        return;
    }

    // Older states stored only the RTC copy in cartridge SRAM.
    for (int i = 0; i < 5; i++)
    {
        m_GameRTC[0][i] = m_pSave[0x1FFF0 + i * 2];
        m_GameRTC[1][i] = m_pSave[0x1FFF1 + i * 2];
    }
    memcpy(m_LatchedRTC, m_GameRTC, sizeof(m_GameRTC));
    memset(m_RTCCycles, 0, sizeof(m_RTCCycles));
    m_ShadowEnabled = false;
    m_RTCIndex = m_RTCLatch = 0;
}

void EverdriveOSMemoryRule::SetEverdriveEnabled(bool enabled)
{
    m_Everdrive.SetEnabled(enabled);
}

void EverdriveOSMemoryRule::SetEverdriveFolder(const char* folder)
{
    m_Everdrive.SetFolder(folder);
}

bool EverdriveOSMemoryRule::LaunchRequested()
{
    return m_bLaunch;
}

// The FPGA's ROM mask gates address lines A15-A22. The game header does not
// configure those lines: a valid header can still launch with incorrect banking.
void EverdriveOSMemoryRule::TakeLaunch(std::vector<u8>& rom, std::vector<u8>& save)
{
    m_bLaunch = false;
    u32 banks = 1;
    while (banks <= m_Masks[0])
        banks <<= 1;
    rom.resize(banks * 0x8000);
    for (u32 bank = 0; bank < banks; bank++)
        memcpy(rom.data() + bank * 0x8000, m_pPSRAM + (bank & m_Masks[0]) * 0x8000, 0x8000);
    save.assign(m_pSave, m_pSave + k_save_size);
}

// the in-game menu: the cart shows the image again at the game's next VBlank vector fetch, $BD14-$BD17 read
// back the game's mapper state (RAM enable, mode, RAM bank, ROM bank)
void EverdriveOSMemoryRule::EnterFromGame(u8 ramg, u8 mode, u8 ramb, u8 romb)
{
    m_GameMBC[0] = ramg;
    m_GameMBC[1] = mode;
    m_GameMBC[2] = ramb;
    m_GameMBC[3] = romb;
    m_Switch = 0;
    m_bUnlocked = false;
}

int EverdriveOSMemoryRule::TakeSwitch()
{
    int s = m_Switch;
    m_Switch = 0;
    return s;
}

int EverdriveOSMemoryRule::PeekSwitch()
{
    return m_Switch;
}

const std::vector<u8>& EverdriveOSMemoryRule::GetImage()
{
    return m_File;
}

u8* EverdriveOSMemoryRule::GetSaveRAM()
{
    return m_pSave;
}

u8 EverdriveOSMemoryRule::GetCartridgeFeatures() const
{
    return m_Everdrive.GetCartridgeFeatures();
}

// A six-bit seconds/minutes or five-bit hours field carries at its normal
// terminal count. A value written above that limit wraps at its bit width.
static bool IncrementField(u8& value, u8 last, u8 mask)
{
    bool carry = (value == last);
    value = carry ? 0 : (value + 1) & mask;
    return carry;
}

// The paired MBC3 counters are independent of the calendar I2C device.
void EverdriveOSMemoryRule::AdvanceCycles(unsigned int cycles)
{
    for (unsigned port = 0; port < 2; ++port)
    {
        u8* rtc = m_GameRTC[port];
        if (rtc[4] & 0x40)
            continue;
        m_RTCCycles[port] += cycles;
        while (m_RTCCycles[port] >= GEARBOY_MASTER_CLOCK_RATE)
        {
            m_RTCCycles[port] -= GEARBOY_MASTER_CLOCK_RATE;
            if (IncrementField(rtc[0], 59, 0x3F) &&
                IncrementField(rtc[1], 59, 0x3F) &&
                IncrementField(rtc[2], 23, 0x1F) && ++rtc[3] == 0)
            {
                if (rtc[4] & 1)
                    rtc[4] |= 0x80;
                rtc[4] ^= 1;
            }
            if (m_ShadowEnabled)
                CaptureRTCShadow();
        }
    }
}

void EverdriveOSMemoryRule::BeginClockBatch(const unsigned int& cpu_clocks)
{
    m_pClockBatchCycles = &cpu_clocks;
    m_AccountedClockCycles = 0;
}

void EverdriveOSMemoryRule::SyncClockBatch()
{
    if (!m_pClockBatchCycles)
        return;

    const unsigned int elapsed = *m_pClockBatchCycles;
    const unsigned int cycles = elapsed - m_AccountedClockCycles;
    if (cycles != 0)
    {
        AdvanceCycles(cycles);
        m_AccountedClockCycles = elapsed;
    }
}

void EverdriveOSMemoryRule::EndClockBatch()
{
    SyncClockBatch();
    m_pClockBatchCycles = NULL;
}

void EverdriveOSMemoryRule::GetGameRTC(u8* registers)
{
    memcpy(registers, m_GameRTC[kGame], 5);
}

void EverdriveOSMemoryRule::SetGameRTC(const u8* registers)
{
    memcpy(m_GameRTC[kGame], registers, 5);
    u8 calendar[16];
    if (m_Everdrive.GetClockRegisters(calendar))
    {
        int fields[7];
        for (int i = 0; i < 7; i++)
            fields[i] = (calendar[i] & 15) + (calendar[i] >> 4) * 10;
        static const int month_days[] = {31,28,31,30,31,30,31,31,30,31,30,31};
        int days = 0;
        for (int year = 0; year < fields[6]; year++)
            days += 365 + ((year & 3) == 0);
        for (int month = 1; month < fields[5] && month <= 12; month++)
            days += month_days[month - 1] + ((month == 2) && ((fields[6] & 3) == 0));
        days += fields[4] - 1;
        m_GameRTC[kReference][0] = (u8)fields[0];
        m_GameRTC[kReference][1] = (u8)fields[1];
        m_GameRTC[kReference][2] = (u8)fields[2];
        m_GameRTC[kReference][3] = (u8)days;
        m_GameRTC[kReference][4] = (u8)((days >> 8) & 1);
    }
    CaptureRTCShadow();
}

void EverdriveOSMemoryRule::CaptureRTCShadow()
{
    for (int i = 0; i < 5; i++)
    {
        m_pSave[0x1FFF0 + i * 2] = m_GameRTC[0][i];
        m_pSave[0x1FFF1 + i * 2] = m_GameRTC[1][i];
    }
}

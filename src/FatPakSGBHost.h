// Opt-in FatPak terminal reference host. This models the protocol, not a SNES CPU.
#pragma once
#include <array>
#include <bitset>
#include <cstdint>
#include <deque>
#include <string>
#include <vector>
#include "FatPakSGBProtocol.h"
class FatPakSGBHost {
public:
    enum Profile { Disabled, TerminalV1, BenchV0 };
    struct Row {
        uint8_t font=0,flags=0,start=0,end=0;
        std::vector<uint8_t> chars,attrs;
    };
    struct Model {
        std::array<Row,56> rows; // 28, or 56 in TEXT 1
        std::array<Row,28> boxLines;
        std::array<Row,2> bars;
        std::array<uint16_t,16> palette;
        uint8_t top=0,textRows=28,barFlags=0,oskRows=0;
        uint8_t cursorRow=0,cursorX=0,cursorLook=0;
        uint8_t boxX=0,boxTop=0,boxRows=0,boxHi=255;
        uint16_t boxWidth=0;
        uint8_t oskLayer=0,oskKey=255;
        bool busy=false,launch=false;
        bool exit=false; // EXIT applied: back to the BIOS once its ACK is echoed (snes/bios.s on the real SNES)
        uint32_t loaded=0,total=0;
        // A module's background (SGBFS_CMD_MAP*): 0 the text, 1 the map under the bars, 2 full screen,
        // | SGBFS_MAP_HIRES for 512x448 (drawn here at half size)
        uint8_t mapMode=0;
        uint16_t mapX=0,mapY=0;
        unsigned codeBytes=0; // CODE bytes seen: the kernel extension sent before a map or TEXT 1
        uint8_t text=0; // TEXT: 1 512x448 character tiles, 56 rows of 64 columns (drawn here at half size)
        unsigned Rows() const { return text?56u:28u; }
        std::array<uint8_t,4096> mapTiles{}; // 256 GB 2bpp tiles
        std::array<uint16_t,32> mapColors{}; // 8 palettes of 4
        std::array<uint16_t,8192> mapCells{}; // 128x64 (256x224 uses 64x32 of it): tile | palette << 10
        Model();
    };
    FatPakSGBHost();
    void Configure(Profile profile);
    void Reset();
    Profile GetProfile() const { return m_profile; }
    bool Active() const { return m_active; }
    void Packet(const uint8_t* packet);
    void Capture(const uint8_t* pixels);
    bool ReadMailbox(int player,uint8_t& byte) const;
    bool Inject(uint8_t event,uint8_t a,uint8_t b);
    bool Click(uint8_t row,uint8_t x,uint8_t buttons);
    bool SetRecording(const std::string& path);
    bool Replay(const std::string& path);
    bool VerifyDataFile(const std::string& path) const;
    bool WritePPM(const std::string& path) const;
    void Render(uint16_t* pixels) const; // 256x224 BGR555
    bool Page(unsigned slot,const uint8_t* bytes);
    static uint16_t CRC(const uint8_t* bytes,unsigned size);
    const Model& GetModel() const { return m_model; }
    const std::array<uint8_t,131072>& GetWRAM() const { return m_wram; }
    const std::array<uint8_t,4>& GetMailbox() const { return m_mailbox; }
    uint32_t Frames() const { return m_frames; }
    uint32_t Good() const { return m_good; }
    uint32_t Bad() const { return m_bad; }
    uint32_t Duplicates() const { return m_duplicates; }
    uint32_t UploadBytes() const { return m_uploadBytes; }
    uint32_t Entry() const { return m_entry; }
    const std::string& LastError() const { return m_error; }
private:
    struct Event { uint8_t type,a,b; };
    Profile m_profile;
    bool m_active;
    std::array<uint8_t,131072> m_wram;
    std::bitset<131072> m_written;
    std::array<uint8_t,4> m_mailbox;
    std::array<uint8_t,2> m_last,m_pageAcked;
    Model m_model;
    std::deque<Event> m_events;
    uint8_t m_expected,m_eventSeq;
    int m_transferDelay;
    uint32_t m_transferAddress,m_entry,m_frames,m_good,m_bad,m_duplicates,m_uploadBytes;
    std::string m_error,m_recording;
    bool Commands(Model& model,const uint8_t* bytes,unsigned size);
    bool Queue(uint8_t type,uint8_t a,uint8_t b,bool control);
    void Publish();
    void Status(const uint8_t* bytes);
    void Record(uint8_t type,const uint8_t* bytes,unsigned size);
    static void DecodeTiles(const uint8_t* pixels,uint8_t* bytes,unsigned tiles);
};

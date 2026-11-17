#include "FatPakSGBHost.h"
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <fstream>
using namespace sgbfs;
static void Check(bool b,const char* what) { if(!b) { fprintf(stderr,"FAIL: %s\n",what); std::exit(1); } }
static void TLV(std::vector<uint8_t>& p,unsigned op,std::initializer_list<uint8_t> b) {
    p.push_back(op); p.push_back(b.size()); p.push_back(b.size()>>8); p.insert(p.end(),b);
}
static std::array<uint8_t,1920> Page(unsigned seq,const std::vector<uint8_t>& b) {
    std::array<uint8_t,1920> p={}; p[0]=seq; p[1]=b.size(); p[2]=b.size()>>8;
    std::copy(b.begin(),b.end(),p.begin()+3); unsigned c=FatPakSGBHost::CRC(p.data(),3+b.size());
    p[3+b.size()]=c; p[4+b.size()]=c>>8; return p;
}
static void Pixels(const uint8_t* data,uint8_t* pixels,unsigned tiles) {
    for(unsigned t=0;t<tiles;++t) for(unsigned y=0;y<8;++y) for(unsigned x=0;x<8;++x)
        pixels[((t/20)*8+y)*160+(t%20)*8+x]=((data[t*16+y*2]>>(7-x))&1)|(((data[t*16+y*2+1]>>(7-x))&1)<<1);
}
static void Start(FatPakSGBHost& h,const char* recording=nullptr) {
    h.Configure(FatPakSGBHost::TerminalV1);
    if(recording) Check(h.SetRecording(recording),"start v1 recording");
    uint8_t packet[16]={0x81,0,0,0x7F},pixels[160*144]={};
    h.Packet(packet); for(int i=0;i<4;++i) h.Capture(pixels);
    packet[0]=0x91; h.Packet(packet); Check(h.Active(),"terminal JUMP activates after upload");
}
static void Echo(FatPakSGBHost& h) {
    uint8_t bytes[4096]={},pixels[160*144]={}; bytes[3840]=83; bytes[3841]=1; bytes[3844]=h.GetMailbox()[0];
    unsigned c=FatPakSGBHost::CRC(bytes+3840,14); bytes[3854]=c; bytes[3855]=c>>8;
    Pixels(bytes,pixels,256); h.Capture(pixels);
}
int main(int argc,char** argv) {
    if(argc==2) {
        std::ifstream f(argv[1],std::ios::binary); Check(f.good(),"open Python vectors"); unsigned count=0;
        for(;;) {
            int valid=f.get(); if(valid<0) break;
            std::array<uint8_t,1920> page; f.read(reinterpret_cast<char*>(page.data()),page.size()); Check(f.good(),"whole vector");
            FatPakSGBHost h; Start(h); bool actual=h.Page(0,page.data());
            if(actual!=bool(valid)) { fprintf(stderr,"vector %u expected=%d actual=%d\n",count,valid,actual); return 1; }
            ++count;
        }
        printf("%u independent Python/C++ protocol vectors passed\n",count); return 0;
    }
    const uint8_t check[]="123456789"; Check(FatPakSGBHost::CRC(check,9)==0x29B1,"CCITT check vector");
    FatPakSGBHost h; uint8_t packet[16]={0x91,0,0,0x7F}; h.Packet(packet); Check(!h.Active(),"ordinary SGB unaffected");
    h.Configure(FatPakSGBHost::TerminalV1); h.Packet(packet); Check(!h.Active(),"JUMP before upload rejected");
    Start(h); Check(h.GetMailbox()[1]==5,"HELLO published");
    Check(!h.Inject(2,28|32,0) && !h.Inject(1,0,0),"invalid input rejected");
    Check(h.Inject(1,'A',1),"key queues behind HELLO");
    auto hello=h.GetMailbox(); uint8_t empty[160*144]={}; h.Capture(empty); Check(h.GetMailbox()==hello,"invalid status cannot release mailbox");
    Echo(h); Check(h.GetMailbox()[1]==1 && h.GetMailbox()[2]=='A',"valid echo releases next event"); Echo(h);
    std::vector<uint8_t> b; TLV(b,4,{1,25,3,0}); TLV(b,1,{1,2,1,0,4,1,1,'A',1,4}); TLV(b,2,{2,1}); TLV(b,5,{2,12,1});
    auto p=Page(1,b); Check(h.Page(0,p.data()),"valid page commits"); Check(h.GetModel().rows[2].chars[0]=='A',"ROW_SAME");
    auto before=h.GetModel().rows[1].chars;
    b.clear(); TLV(b,1,{1,2,1,0,0,1,1,'Z',1,4}); TLV(b,99,{}); p=Page(2,b);
    Check(!h.Page(1,p.data()) && h.GetModel().rows[1].chars==before,"invalid trailing TLV rolls back whole transaction");
    b.clear(); TLV(b,3,{1,0}); p=Page(2,b); auto corrupt=p; corrupt[100]=1;
    Check(h.Page(1,corrupt.data()),"padding ignored"); Check(h.GetModel().rows[1].chars==before && h.GetModel().rows[2].chars.empty(),"SCROLL text ring");
    Check(h.Page(1,p.data()) && h.GetModel().rows[1].chars==before,"duplicate SCROLL idempotent");
    corrupt=p; corrupt[3]^=1; Check(!h.Page(1,corrupt.data()),"corrupt duplicate not ACKed");
    b.clear(); TLV(b,6,{16,80,0,3,3}); TLV(b,7,{1,2,1,'X'}); TLV(b,8,{1}); TLV(b,9,{1,2,1,'B'});
    TLV(b,11,{4,1,31,0}); TLV(b,12,{1}); TLV(b,13,{5,0,0,0,10,0,0,0}); p=Page(3,b);
    Check(h.Page(0,p.data()),"overlay state"); Check(h.GetModel().rows[1].chars==before,"overlay preserves text");
    std::array<uint16_t,256*224> image; h.Render(image.data());
    Check(image[3*2048+16]==h.GetModel().palette[3],"box border");
    Check(image[223*256]==h.GetModel().palette[1] && image[223*256+128]==h.GetModel().palette[0],"progress proportion");
    Check(h.WritePPM("/tmp/gearboy-fatpak-reference.ppm"),"image export");
    b.clear(); TLV(b,7,{3,2,1,'X'}); p=Page(4,b); Check(!h.Page(1,p.data()),"BOX_LINE bounds");
    b.clear(); TLV(b,14,{1}); p=Page(4,b); Check(h.Page(1,p.data()),"launch accepted");
    bool ready=false; for(int i=0;i<12;++i) { ready|=h.GetMailbox()[1]==7; Echo(h); } Check(ready,"LAUNCH_READY");
    for(unsigned seq=5;seq<=260;++seq) { p=Page((seq-1)%255+1,{}); Check(h.Page(seq&1,p.data()),"255->1 global sequence wrap"); Echo(h); }
    Start(h); unsigned count=0; while(h.Inject(2,32,30)) ++count; Check(count==61,"reserve three control queue slots");
    Check(!h.Click(0,0,1),"click cannot strand a pressed edge at queue limit");
    h.Reset(); Check(!h.Active() && !h.Good(),"reset clears host");
    // LCD captures exercise global B-before-A ordering in the v1 transport.
    Start(h); uint8_t bytes[4096]={},pixels[160*144]={}; auto a=Page(2,{}),bb=Page(1,{});
    std::copy(a.begin(),a.end(),bytes); std::copy(bb.begin(),bb.end(),bytes+1920); Pixels(bytes,pixels,256); h.Capture(pixels);
    Check(h.Good()==2,"both in-flight pages apply in global sequence order");
    Start(h); b.clear(); TLV(b,1,{0,2,1,1,2,2,1,'A','B',2,1}); p=Page(1,b);
    Check(h.Page(0,p.data()),"overlapping glyph transaction");
    Check(h.WritePPM("/tmp/gearboy-fatpak-or.ppm"),"OR overhang reference");
    Start(h,"/tmp/gearboy-fatpak-v1.fph");
    a=Page(2,{}); bb=Page(1,{});
    std::copy(a.begin(),a.end(),bytes); std::copy(bb.begin(),bb.end(),bytes+1920);
    Pixels(bytes,pixels,256); h.Capture(pixels);
    Check(h.Inject(1,'A',0) && h.Click(4,120,1),"record input edges");
    for(int i=0;i<8;++i) Echo(h);
    for(int i=0;i<130;++i) { Check(h.Inject(1,'B',0),"repeat injection"); Echo(h); }
    Check(h.SetRecording(""),"close recording");
    FatPakSGBHost replay;
    Check(replay.Replay("/tmp/gearboy-fatpak-v1.fph"),"v1 stream replay");
    Check(replay.Good()==h.Good() && replay.GetMailbox()==h.GetMailbox() && replay.GetWRAM()==h.GetWRAM(),"v1 replay matches input echo and event sequence wrap");
    std::ifstream in("/tmp/gearboy-fatpak-v1.fph",std::ios::binary);
    std::vector<char> file((std::istreambuf_iterator<char>(in)),{});
    std::ofstream out("/tmp/gearboy-fatpak-truncated.fph",std::ios::binary);
    out.write(file.data(),file.size()-1); out.close();
    Check(!replay.Replay("/tmp/gearboy-fatpak-truncated.fph"),"truncated replay rejected");
    // DATA_SND must capture the last byte of WRAM without crossing its boundary.
    uint8_t snd[16]={0x79,0xFF,0xFF,0x7F,1,0xA7}; h.Packet(snd);
    Check(h.GetWRAM().back()==0xA7,"DATA_SND at WRAM boundary");
    snd[4]=2; snd[5]=0; h.Packet(snd);
    Check(h.GetWRAM().back()==0xA7,"DATA_SND bounds rejection");
    puts("FatPak host protocol/model/mailbox/reference tests passed");
}

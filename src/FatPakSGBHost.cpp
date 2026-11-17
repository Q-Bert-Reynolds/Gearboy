#include "FatPakSGBHost.h"
#include <algorithm>
#include <cstring>
#include <fstream>
#include <iterator>
#include "FatPakSGBFonts.inl"
using namespace sgbfs;
namespace {
unsigned LE16(const uint8_t* p) { return p[0]|unsigned(p[1])<<8; }
uint32_t LE24(const uint8_t* p) { return LE16(p)|uint32_t(p[2])<<16; }
uint32_t LE32(const uint8_t* p) { return LE16(p)|uint32_t(LE16(p+2))<<16; }
uint8_t Next(uint8_t s) { return s==255?1:s+1; }
}
FatPakSGBHost::Model::Model() {
    palette.fill(0);
    palette[0]=29|29<<5|27<<10; palette[1]=20|25<<5|31<<10;
    palette[2]=14|17<<5|14<<10; palette[3]=2|2<<5|3<<10;
    palette[4]=4|9<<5|24<<10; palette[5]=19|4<<5|4<<10;
    palette[6]=15|5<<5|20<<10; palette[7]=21|10<<5;
}
FatPakSGBHost::FatPakSGBHost():m_profile(Disabled) { Reset(); }
void FatPakSGBHost::Configure(Profile p) { m_recording.clear(); m_profile=p; Reset(); }
void FatPakSGBHost::Reset() {
    Record(4,nullptr,0);
    m_active=false; m_wram.fill(0); m_written.reset(); m_mailbox.fill(0); m_last.fill(0); m_pageAcked.fill(0);
    m_model=Model(); m_events.clear(); m_expected=1; m_eventSeq=0; m_transferDelay=0;
    m_transferAddress=m_entry=m_frames=m_good=m_bad=m_duplicates=m_uploadBytes=0; m_error.clear();
}
uint16_t FatPakSGBHost::CRC(const uint8_t* p,unsigned n) {
    uint16_t c=SGBFS_CRC_INIT;
    while(n--) { c^=uint16_t(*p++)<<8; for(int i=0;i<8;++i) c=(c<<1)^((c&0x8000)?SGBFS_CRC_POLY:0); }
    return c;
}
void FatPakSGBHost::DecodeTiles(const uint8_t* p,uint8_t* out,unsigned tiles) {
    for(unsigned t=0;t<tiles;++t) for(unsigned y=0;y<8;++y) {
        unsigned lo=0,hi=0;
        for(unsigned x=0;x<8;++x) {
            unsigned c=p[((t/20)*8+y)*160+(t%20)*8+x]&3;
            lo|=(c&1)<<(7-x); hi|=((c>>1)&1)<<(7-x);
        }
        *out++=lo; *out++=hi;
    }
}
void FatPakSGBHost::Packet(const uint8_t* p) {
    if(m_profile==Disabled) return;
    unsigned op=p[0]>>3,addr=LE24(p+1);
    if(op!=0x0F && op!=0x10 && op!=0x12) return;
    Record(1,p,16);
    if(op==0x0F) {
        if(addr>=0x7E0000 && p[4]<=11 && addr<=0x800000u-p[4]) {
            std::memcpy(m_wram.data()+addr-0x7E0000,p+5,p[4]); m_uploadBytes+=p[4];
            for(unsigned i=0;i<p[4];++i) m_written.set(addr-0x7E0000+i);
        } else m_error="DATA_SND outside WRAM or oversized payload";
    } else if(op==0x10) {
        if(addr>=0x7E0000 && addr<=0x7FF000 && !(addr&4095)) {
            m_transferAddress=addr; m_transferDelay=4;
        } else m_error="DATA_TRN outside aligned WRAM";
    } else {
        if(addr<0x7E0000 || addr>=0x800000 || m_transferDelay || !m_written.test(addr-0x7E0000)) {
            m_error="JUMP before completed WRAM upload"; return;
        }
        m_entry=addr; m_active=true; m_last.fill(0); m_pageAcked.fill(0); m_expected=1;
        m_model=Model(); m_events.clear(); m_mailbox.fill(0); m_eventSeq=0;
        if(m_profile==BenchV0) m_mailbox={{0xA5,0,0,0}};
        else { Queue(SGBFS_EVENT_HELLO,1,7,true); Publish(); }
        m_error.clear();
    }
}
void FatPakSGBHost::Capture(const uint8_t* pixels) {
    if(m_profile==Disabled) return;
    Record(2,pixels,160*144); ++m_frames;
    if(m_transferDelay && !--m_transferDelay) {
        uint8_t blob[4096]; DecodeTiles(pixels,blob,256);
        std::memcpy(m_wram.data()+m_transferAddress-0x7E0000,blob,4096); m_uploadBytes+=4096;
        for(unsigned i=0;i<4096;++i) m_written.set(m_transferAddress-0x7E0000+i);
    }
    if(!m_active) return;
    uint8_t bytes[4096]; DecodeTiles(pixels,bytes,256);
    if(m_profile==TerminalV1) Status(bytes+3840);
    // Process the expected page first, whether it is A or B.
    unsigned first=bytes[1920]==m_expected?1:0;
    Page(first,bytes+first*1920); Page(1-first,bytes+(1-first)*1920); Publish();
}
bool FatPakSGBHost::ReadMailbox(int player,uint8_t& byte) const {
    if(!m_active || player<0 || player>3) return false;
    byte=m_mailbox[player]; return true;
}
bool FatPakSGBHost::Queue(uint8_t type,uint8_t a,uint8_t b,bool control) {
    if(!control && (type==3 || type==8) && !m_events.empty() && m_events.back().type==type &&
       (m_events.back().a>>5)==(a>>5)) { m_events.back().a=a; m_events.back().b=b; return true; }
    if(m_events.size()>=64 || (!control && m_events.size()>=61)) return false;
    Event e={type,a,b};
    if(control) {
        auto i=m_events.begin(); while(i!=m_events.end() && (i->type==5 || i->type==6 || i->type==7)) ++i;
        m_events.insert(i,e);
    } else m_events.push_back(e);
    return true;
}
void FatPakSGBHost::Publish() {
    if(m_profile!=TerminalV1 || m_mailbox[0] || m_events.empty()) return;
    Event e=m_events.front(); m_events.pop_front(); m_eventSeq=m_eventSeq%127+1;
    m_mailbox={{m_eventSeq,e.type,e.a,e.b}};
}
bool FatPakSGBHost::Inject(uint8_t type,uint8_t a,uint8_t b) {
    if(!m_active || m_profile!=TerminalV1) return false;
    if(type==1) { if(!a || b>7) return false; }
    else if(type==2 || type==3 || type==4 || type==8) {
        if((a&128) || (a&31)>=28 || (type==8 && (a>>5)) || ((type==2 || type==3) && !(a>>5))) return false;
    } else return false;
    bool ok=Queue(type,a,b,false);
    if(ok) { uint8_t event[3]={type,a,b}; Record(3,event,3); Publish(); }
    return ok;
}
bool FatPakSGBHost::Click(uint8_t row,uint8_t x,uint8_t buttons) {
    if(row>=28 || buttons<1 || buttons>3 || m_events.size()>59) return false;
    return Inject(2,uint8_t(row|buttons<<5),x) && Inject(4,row,x);
}
void FatPakSGBHost::Status(const uint8_t* p) {
    if(p[0]!=83 || p[1]!=1 || p[4]>127 || p[5]>1 || CRC(p,14)!=LE16(p+14)) return;
    for(int i=8;i<14;++i) if(p[i]) return;
    if(m_mailbox[0] && p[4]==m_mailbox[0]) {
        if(m_mailbox[1]==6) m_pageAcked[m_mailbox[2]]=m_mailbox[3];
        m_mailbox.fill(0);
    }
}
bool FatPakSGBHost::Page(unsigned slot,const uint8_t* p) {
    if(slot>1 || !m_active || !p[0]) return false;
    uint8_t seq=p[0];
    if(m_profile==BenchV0) {
        unsigned sum=0; for(unsigned i=0;i<1918;i+=2) sum+=LE16(p+i);
        bool ok=LE16(p+2)==1914 && uint16_t(sum)==LE16(p+1918);
        if(ok && m_last[slot]==seq) { ++m_duplicates; return true; }
        if(ok) { ++m_good; m_last[slot]=seq; } else ++m_bad;
        m_mailbox={{0xA5,seq,uint8_t(slot),uint8_t(ok?1:2)}}; return ok;
    }
    unsigned n=LE16(p+1);
    if(n>1915 || CRC(p,3+n)!=LE16(p+3+n)) { ++m_bad; m_error="page size/CRC"; return false; }
    if(seq!=m_expected && m_last[slot]==seq) {
        ++m_duplicates;
        if(m_pageAcked[slot]==seq) return true;
        bool pending=m_mailbox[0] && m_mailbox[1]==6 && m_mailbox[2]==slot && m_mailbox[3]==seq;
        for(const Event& e:m_events) pending|=e.type==6 && e.a==slot && e.b==seq;
        if(!pending) Queue(6,uint8_t(slot),seq,true);
        return true;
    }
    if(seq!=m_expected) return false;
    Model candidate=m_model;
    if(!Commands(candidate,p+3,n)) { ++m_bad; m_error="invalid terminal transaction"; return false; }
    bool launch=!m_model.launch && candidate.launch;
    if(m_events.size()+1+unsigned(launch)>64) { m_error="control queue full; page deferred"; return false; }
    m_model=candidate; m_last[slot]=seq; m_pageAcked[slot]=0; m_expected=Next(seq); ++m_good; m_error.clear();
    Queue(6,uint8_t(slot),seq,true); if(launch) Queue(7,1,0,true); return true;
}
bool FatPakSGBHost::Commands(Model& m,const uint8_t* p,unsigned n) {
    unsigned at=0;
    while(at<n) {
        if(n-at<3) return false;
        unsigned op=p[at],len=LE16(p+at+1); at+=3;
        if(len>n-at) return false;
        const uint8_t* b=p+at; at+=len;
        switch(op) {
        case SGBFS_CMD_ROW: {
            if(len<7 || b[0]>=28 || b[1]>6 || b[2]>1 || b[3]>248 || (b[4]!=255 && (b[4]>248 || b[4]<b[3])) ||
               b[5]>64 || len!=7u+b[5]+2u*b[6]) return false;
            Row r; r.font=b[1]; r.flags=b[2]; r.start=b[3]; r.end=b[4]; r.chars.assign(b+7,b+7+b[5]);
            unsigned width=0;
            for(unsigned i=7+b[5];i<len;i+=2) {
                unsigned count=b[i],attr=b[i+1];
                if(!count || !(attr&15) || (attr>>4)>14 || r.attrs.size()+count>b[5]) return false;
                r.attrs.insert(r.attrs.end(),count,uint8_t(attr)); width+=count*(attr&15);
            }
            if(r.attrs.size()!=b[5] || width>248) return false;
            m.rows[b[0]]=r; break;
        }
        case SGBFS_CMD_ROW_SAME:
            if(len!=2 || b[0]>=28 || b[1]>=28) return false;
            m.rows[b[0]]=m.rows[b[1]]; break;
        case SGBFS_CMD_SCROLL: {
            int d=len?int(int8_t(b[0])):0;
            if(len!=2 || b[1]>1 || !d || d<=-28 || d>=28 || unsigned(d<0?-d:d)>m.textRows) return false;
            auto old=m.rows;
            for(unsigned y=0;y<m.textRows;++y) {
                int from=int(y)+d; m.rows[m.top+y]=from>=0 && from<m.textRows?old[m.top+from]:Row();
            } break;
        }
        case SGBFS_CMD_LAYOUT:
            if(len!=4 || b[0]>=28 || !b[1] || b[1]>28 || b[2]>3 || (b[3]!=0 && b[3]!=5) ||
               b[0]+b[1]+b[3]+bool(b[2]&2)>28) return false;
            m.top=b[0]; m.textRows=b[1]; m.barFlags=b[2]; m.oskRows=b[3]; break;
        case SGBFS_CMD_CURSOR:
            if(len!=3 || b[0]>=28 || b[2]>2) return false;
            m.cursorRow=b[0]; m.cursorX=b[1]; m.cursorLook=b[2]; break;
        case SGBFS_CMD_BOX:
            if(len!=5) return false;
            if(!LE16(b+1)) { for(unsigned i=0;i<5;++i) if(b[i]) return false; }
            else if(LE16(b+1)<8 || b[0]+LE16(b+1)>256 || !b[4] || b[3]+b[4]>28) return false;
            m.boxX=b[0]; m.boxWidth=LE16(b+1); m.boxTop=b[3]; m.boxRows=b[4]; m.boxHi=255;
            m.boxLines=std::array<Row,28>(); break;
        case SGBFS_CMD_BOX_LINE: case SGBFS_CMD_BAR: {
            if(len<3 || b[1]>6 || b[2]>64 || len!=3u+b[2] || b[0]>=(op==7?m.boxRows:2)) return false;
            Row r; r.font=b[1]; r.chars.assign(b+3,b+len);
            if(op==7) m.boxLines[b[0]]=r; else m.bars[b[0]]=r; break;
        }
        case SGBFS_CMD_BOX_HI:
            if(len!=1 || (b[0]!=255 && b[0]>=m.boxRows)) return false;
            m.boxHi=b[0]; break;
        case SGBFS_CMD_OSK:
            if(len!=2 || b[0]>3 || (b[1]>=50 && b[1]!=255)) return false;
            m.oskLayer=b[0]; m.oskKey=b[1]; break;
        case SGBFS_CMD_PALETTE:
            if(len<4 || !b[1] || b[1]>16 || b[0]+b[1]>16 || len!=2u+2u*b[1]) return false;
            for(unsigned i=0;i<b[1];++i) { unsigned c=LE16(b+2+2*i); if(c&0x8000) return false; m.palette[b[0]+i]=c; } break;
        case SGBFS_CMD_BUSY:
            if(len!=1 || b[0]>1) return false;
            m.busy=b[0]; break;
        case SGBFS_CMD_PROGRESS:
            if(len!=8 || LE32(b)>LE32(b+4)) return false;
            m.loaded=LE32(b); m.total=LE32(b+4); break;
        case SGBFS_CMD_LAUNCH:
            if(len!=1 || b[0]!=1) return false;
            m.launch=true; break;
        case SGBFS_CMD_HELLO: case SGBFS_CMD_RESET:
            if(len!=2 || b[0]!=1 || b[1]!=0) return false;
            m=Model(); break;
        default: return false;
        }
    }
    return true;
}
void FatPakSGBHost::Render(uint16_t* pixels) const {
    const Model& m=m_model;
    std::fill(pixels,pixels+256*224,m.palette[0]);
    auto line=[&](const Row& r,unsigned row,unsigned left,unsigned right,bool selected) {
        if(row>=28 || left>=right || right>256) return;
        uint8_t ink[8][256]={},dim[8][256]={},accent[8][256]={};
        unsigned x=left;
        for(unsigned i=0;i<r.chars.size();++i) {
            unsigned c=r.chars[i],font=r.font,style=(r.flags && i<r.attrs.size())?r.attrs[i]>>4:0;
            bool bold=style==1 || style==3 || style==7 || style==13,italic=style==2 || style==3;
            if(r.flags && font<3 && style==5) font=2;
            else if(font<3 && (bold || italic)) font=(font==2?7:10)+(bold&&italic?2:italic?1:0);
            unsigned color=(style==4 || style==8)?2:3;
            if(style==5 || style==11) color=5;
            else if(style==6 || style==7 || style==9 || style==10) color=4;
            else if(style==12) color=6;
            else if(style==13 || style==14) color=7;
            int icon=c>=1 && c<=8?int(c-1):c>=129 && c<=131?int(c-121):-1;
            for(unsigned y=0;y<8;++y) for(unsigned bit=0;bit<8 && x+bit<right;++bit) {
                unsigned shade=(!r.flags && icon>=0)?kFatPakIcons[icon][y*8+bit]:
                    (kFatPakGlyphs[font][c*8+y]&(128>>bit)?color:0);
                if(shade) {
                    ink[y][x+bit]=1;
                    if(shade==2) dim[y][x+bit]=1;
                    else if(shade!=3) accent[y][x+bit]=shade;
                }
            }
            x+=(r.flags && i<r.attrs.size())?r.attrs[i]&15:kFatPakWidths[r.font][c];
            if(x>=right) break;
        }
        for(unsigned y=0;y<8;++y) for(unsigned px=left;px<right;++px) {
            bool hi=selected || (r.start!=r.end && px>=8u+r.start && (r.end==255 || px<8u+r.end));
            unsigned a=accent[y][px];
            unsigned color=ink[y][px]?(dim[y][px]?2:(a && (!hi || a==1)?a:3)):(hi?1:0);
            pixels[(row*8+y)*256+px]=m.palette[color];
        }
    };
    for(unsigned y=0;y<28;++y) line(m.rows[y],y,8,256,false);
    if(m.barFlags&1) line(m.bars[0],0,8,256,false);
    if(m.barFlags&2) line(m.bars[1],27,8,256,false);
    if(m.oskRows) {
        static const char* keys[]={"1234567890qwertyuiopasdfghjkl;zxcvbnm,./",
            "!@#$%^&*()QWERTYUIOPASDFGHJKL:ZXCVBNM<>?",
            "`~-_=+[]{}\\|'\"\xA1\xBF\xE1\xE9\xED\xF3\xFA\xF1\xFC"};
        const char* k=keys[m.oskLayer==3?1:m.oskLayer]; unsigned length=unsigned(std::strlen(k));
        for(unsigned y=0;y<5;++y) {
            Row r; r.chars.assign(20,' ');
            for(unsigned x=0;x<10;++x) if(y<4 && y*10+x<length) r.chars[x*2]=uint8_t(k[y*10+x]);
            if(y==4) {
                const uint8_t b[20]={29,' ','#',' ',31,' ','_','_','_','_','_','_','_','_',15,' ',16,' ',30,' '};
                r.chars.assign(b,b+20); if(m.oskLayer==3) r.chars[0]=25;
            }
            line(r,m.top+m.textRows+y,4,256,false);
            if(m.oskKey!=255 && m.oskKey/10==y) {
                unsigned x=4+(m.oskKey%10)*16;
                for(unsigned yy=0;yy<8;++yy) for(unsigned xx=x;xx<x+8;++xx)
                    pixels[(m.top+m.textRows+y)*2048+yy*256+xx]^=0x7FFF;
            }
        }
    }
    if(m.boxWidth) {
        for(unsigned y=0;y<m.boxRows;++y) {
            line(m.boxLines[y],m.boxTop+y,m.boxX,m.boxX+m.boxWidth,y==m.boxHi);
            for(unsigned yy=0;yy<8;++yy) {
                pixels[(m.boxTop+y)*2048+yy*256+m.boxX]=m.palette[3];
                pixels[(m.boxTop+y)*2048+yy*256+m.boxX+m.boxWidth-1]=m.palette[3];
            }
        }
        for(unsigned x=m.boxX;x<m.boxX+m.boxWidth;++x) {
            pixels[m.boxTop*2048+x]=m.palette[3]; pixels[(m.boxTop+m.boxRows)*2048-256+x]=m.palette[3];
        }
    }
    if(m.cursorLook) for(unsigned y=0;y<8;++y)
        pixels[(m.cursorRow*8+y)*256+m.cursorX]=m.cursorLook==2?0x7FFF:m.palette[3];
    if(m.busy) for(unsigned y=0;y<4;++y) for(unsigned x=248;x<252;++x) pixels[y*256+x]=m.palette[3];
    if(m.total) for(unsigned x=0;x<uint64_t(m.loaded)*256/m.total;++x) pixels[223*256+x]=m.palette[1];
}
bool FatPakSGBHost::WritePPM(const std::string& path) const {
    std::ofstream f(path.c_str(),std::ios::binary);
    uint16_t pixels[256*224]; Render(pixels); f<<"P6\n256 224\n255\n";
    for(unsigned i=0;i<256*224;++i) for(unsigned shift=0;shift<15;shift+=5) {
        unsigned v=(pixels[i]>>shift)&31; f.put(char((v<<3)|(v>>2)));
    }
    return f.good();
}
bool FatPakSGBHost::SetRecording(const std::string& path) {
    if(path.empty()) { m_recording.clear(); return true; }
    if(m_frames || m_active || m_profile==Disabled) return false;
    std::ofstream f(path.c_str(),std::ios::binary|std::ios::trunc); f.write("FPH1",4); f.put(char(m_profile));
    if(!f.good()) return false;
    m_recording=path; return true;
}
void FatPakSGBHost::Record(uint8_t type,const uint8_t* bytes,unsigned size) {
    if(m_recording.empty()) return;
    std::ofstream f(m_recording.c_str(),std::ios::binary|std::ios::app);
    f.put(char(type)); for(unsigned i=0;i<4;++i) f.put(char(size>>(i*8)));
    if(size) f.write(reinterpret_cast<const char*>(bytes),size);
    if(!f.good()) { m_error="capture recorder write failed"; m_recording.clear(); }
}
bool FatPakSGBHost::Replay(const std::string& path) {
    std::ifstream f(path.c_str(),std::ios::binary); char magic[4]={}; f.read(magic,4);
    int profile=f.get();
    if(!f || std::memcmp(magic,"FPH1",4) || profile<1 || profile>2) return false;
    m_recording.clear(); Configure(Profile(profile)); bool ok=true;
    for(;;) {
        int type=f.get(); if(type<0) { ok=f.eof(); break; }
        uint8_t header[4]={}; f.read(reinterpret_cast<char*>(header),4);
        unsigned size=LE32(header);
        if(!f || (type==1?size!=16:type==2?size!=160*144:type==3?size!=3:type==4?size!=0:true)) { ok=false; break; }
        std::vector<uint8_t> bytes(size); if(size) f.read(reinterpret_cast<char*>(bytes.data()),size);
        if(!f) { ok=false; break; }
        if(type==1) Packet(bytes.data()); else if(type==2) Capture(bytes.data()); else if(type==4) Reset();
        else if(!Inject(bytes[0],bytes[1],bytes[2])) { ok=false; break; }
    }
    if(!ok) m_error="invalid or truncated host recording";
    return ok;
}
bool FatPakSGBHost::VerifyDataFile(const std::string& path) const {
    std::ifstream f(path.c_str(),std::ios::binary|std::ios::ate);
    std::streamoff size=f.tellg(); if(size<32 || size>32+65536+7424) return false;
    f.seekg(0); std::vector<uint8_t> b(size); f.read(reinterpret_cast<char*>(b.data()),size);
    if(!f || std::memcmp(b.data(),"SGB1\1\40",6) || b[6] || b[7]) return false;
    unsigned checksum=LE16(b.data()+26); b[26]=b[27]=0;
    if(CRC(b.data(),unsigned(b.size()))!=checksum) return false;
    uint32_t load=LE24(b.data()+8),entry=LE24(b.data()+11),program=LE32(b.data()+14),fonts=LE32(b.data()+18),overlay=LE32(b.data()+22);
    if(!program || load<0x7F0000 || load>=0x800000 || (load&4095) || program>0x800000-load ||
       fonts>0x800000-load-program || entry<load || entry>=load+program || entry!=m_entry ||
       uint64_t(program)+fonts+overlay+32!=b.size() || (overlay && overlay!=7424)) return false;
    if(overlay) { if(b[30]!=1 || b[31]!=1) return false; }
    else if(b[28] || b[29] || b[30] || b[31]) return false;
    for(unsigned i=0;i<program+fonts;++i) if(!m_written.test(load-0x7E0000+i)) return false;
    return std::equal(b.begin()+32,b.begin()+32+program+fonts,m_wram.begin()+load-0x7E0000);
}

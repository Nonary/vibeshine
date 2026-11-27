#pragma once
#include <windows.h>
#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <span>
#include <ostream>
#include <stdexcept>
#include <string>
#include <vector>
#include "src/vibe_input_protocol.h"

namespace usb_signature {
constexpr std::array<unsigned,4> offsets{0,59,119,239};
constexpr unsigned header_bytes=80, metadata_bytes=64;
constexpr unsigned row_bytes(unsigned channels){return metadata_bytes+4u*4u*channels;}
using Header=std::array<std::uint8_t,header_bytes>;
using Row=std::array<std::uint8_t,row_bytes(vibe_input::MaximumChannels)>;
static_assert(sizeof(float)==4 && std::numeric_limits<float>::is_iec559);
inline void check(bool ok,const char* text){if(!ok)throw std::runtime_error(text);}
inline void put32(std::uint8_t* p,std::uint32_t v){for(unsigned i=0;i<4;++i)p[i]=static_cast<std::uint8_t>(v>>(8*i));}
inline void put64(std::uint8_t* p,std::uint64_t v){for(unsigned i=0;i<8;++i)p[i]=static_cast<std::uint8_t>(v>>(8*i));}

inline Header make_header(const vibe_input::Profile& p,std::uint32_t maximum,std::uint64_t frequency){
    check(vibe_input::ValidProfile(p)&&maximum&&maximum<=721000&&frequency,"Invalid signature profile/capacity/clock");
    Header h{};std::memcpy(h.data(),"VIBESIG1",8);
    put32(&h[8],1);put32(&h[12],header_bytes);put32(&h[16],row_bytes(p.channels));
    put32(&h[20],p.channels);put32(&h[24],p.sampleRate);put32(&h[28],p.frames);put32(&h[32],p.channelMask);
    put32(&h[36],3);put32(&h[40],p.nativeContainerBits);put32(&h[44],p.nativeValidBits);
    put32(&h[48],4);for(unsigned i=0;i<offsets.size();++i)put32(&h[52+4*i],offsets[i]);
    put32(&h[68],maximum);put64(&h[72],frequency);return h;
}
inline void make_row(Row& row,const vibe_input::Profile& p,const vibe_input::AudioHeader& m,
                     std::uint64_t global,std::uint64_t session,std::uint64_t encoder_qpc,std::span<const float> pcm){
    check(vibe_input::ValidProfile(p)&&vibe_input::SameProfile(p,m.profile)&&m.epoch&&m.epoch<=UINT32_MAX&&encoder_qpc&&encoder_qpc>=m.sendQpc&&
          m.sourceKind==vibe_input::UserModeUsb&&pcm.size()==static_cast<std::size_t>(p.channels)*p.frames,
          "Signature metadata or PCM geometry differs");
    put64(&row[0],global);put64(&row[8],session);put64(&row[16],m.blockSequence);put64(&row[24],m.firstFrame);
    put32(&row[32],static_cast<std::uint32_t>(m.epoch));put32(&row[36],0);put64(&row[40],m.firstUrbSequence);put64(&row[48],m.lastUrbSequence);put64(&row[56],encoder_qpc);
    unsigned at=metadata_bytes;
    for(const auto frame:offsets)for(unsigned ch=0;ch<p.channels;++ch){
        const float value=pcm[frame*p.channels+ch];check(std::isfinite(value),"Nonfinite pre-Opus signature");
        put32(&row[at],std::bit_cast<std::uint32_t>(value));at+=4;
    }
}
class writer {
    HANDLE file_=INVALID_HANDLE_VALUE;
    vibe_input::Profile profile_{};
    std::uint64_t frequency_=0,rows_=0;
    std::uint32_t maximum_=0;
    void write(const std::uint8_t* bytes,DWORD size){DWORD done=0;check(WriteFile(file_,bytes,size,&done,nullptr)&&done==size,"Pre-Opus signature write failed");}
public:
    writer(const std::wstring& path,const vibe_input::Profile& profile,std::uint32_t maximum,std::uint64_t hz):
        profile_(profile),frequency_(hz),maximum_(maximum){
        const auto h=make_header(profile,maximum,hz);
        file_=CreateFileW(path.c_str(),GENERIC_WRITE,FILE_SHARE_READ,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr);
        check(file_!=INVALID_HANDLE_VALUE,"Cannot create a new pre-Opus signature file");
        try{write(h.data(),header_bytes);}catch(...){CloseHandle(file_);file_=INVALID_HANDLE_VALUE;throw;}
    }
    writer(const writer&)=delete;writer& operator=(const writer&)=delete;
    ~writer(){if(file_!=INVALID_HANDLE_VALUE)CloseHandle(file_);}
    void append(const vibe_input::AudioHeader& m,std::uint64_t global,std::uint64_t session,std::uint64_t encoder_qpc,std::span<const float> pcm){
        check(file_!=INVALID_HANDLE_VALUE && global==rows_ && rows_<maximum_ && m.qpcFrequency==frequency_,"Missing, duplicate or excess pre-Opus signature row");
        Row row;make_row(row,profile_,m,global,session,encoder_qpc,pcm);write(row.data(),row_bytes(profile_.channels));++rows_;
    }
    std::uint64_t count()const noexcept{return rows_;}
    std::uint64_t bytes()const noexcept{return header_bytes+rows_*row_bytes(profile_.channels);}
    void append_stored(std::span<const std::uint8_t> data,std::uint32_t records){
        const auto expected=std::uint64_t(records)*row_bytes(profile_.channels);
        check(file_!=INVALID_HANDLE_VALUE&&!rows_&&records<=maximum_&&data.size()==expected,"Stored signature size mismatch");
        constexpr std::size_t chunk=1024*1024;
        for(std::size_t offset=0;offset<data.size();){
            const auto n=static_cast<DWORD>(data.size()-offset>chunk?chunk:data.size()-offset);
            write(data.data()+offset,n);offset+=n;
        }
        rows_=records;
    }
    void finish(){
        LARGE_INTEGER actual{};check(file_!=INVALID_HANDLE_VALUE&&FlushFileBuffers(file_)&&GetFileSizeEx(file_,&actual)&&
            static_cast<std::uint64_t>(actual.QuadPart)==bytes(),"Pre-Opus signature flush/length mismatch");
        check(CloseHandle(file_)!=FALSE,"Pre-Opus signature close failed");file_=INVALID_HANDLE_VALUE;
    }
};

using stage_row=std::array<std::uint64_t,23>;
inline void write_stage(std::ostream& out,const stage_row& row){
    for(std::size_t i=0;i<row.size();++i){if(i)out.put(',');out<<row[i];}
    out.put('\n');
}

// Bounded diagnostic history, never a PCM queue. Both arrays are initialized
// and every backing page is touched before source readiness is signaled.
class history {
    vibe_input::Profile profile_;
    std::uint64_t frequency_;
    std::uint32_t capacity_;
    std::uint32_t used_=0;
    std::vector<std::uint8_t> samples_;
    std::vector<stage_row> stages_;
    template<class T> static void touch(std::vector<T>& storage){
        auto* bytes=reinterpret_cast<volatile std::uint8_t*>(storage.data());
        const auto size=storage.size()*sizeof(T);
        for(std::size_t i=0;i<size;i+=4096)bytes[i]=0;
        if(size)bytes[size-1]=0;
    }
public:
    history(const vibe_input::Profile& profile,std::uint32_t capacity,std::uint64_t frequency):
        profile_(profile),frequency_(frequency),capacity_(capacity){
        (void)make_header(profile,capacity,frequency);
        samples_.resize(static_cast<std::size_t>(capacity)*row_bytes(profile.channels));
        stages_.resize(capacity);
        touch(samples_);touch(stages_);
    }
    void record(const vibe_input::AudioHeader& m,const stage_row& stage,std::span<const float> pcm){
        check(used_<capacity_&&stage[1]==used_&&stage[2]==m.blockSequence&&stage[3]==m.epoch&&stage[4]==m.firstFrame&&
              stage[9]==frequency_&&m.qpcFrequency==frequency_,"Missing, excess or inconsistent diagnostic history row");
        Row scratch;
        make_row(scratch,profile_,m,used_,stage[0],stage[16],pcm);
        const auto stride=row_bytes(profile_.channels);
        std::memcpy(samples_.data()+static_cast<std::size_t>(used_)*stride,scratch.data(),stride);
        stages_[used_]=stage;
        ++used_;
    }
    std::uint32_t count()const noexcept{return used_;}
    std::uint64_t bytes()const noexcept{return header_bytes+std::uint64_t(used_)*row_bytes(profile_.channels);}
    void save(const std::wstring& signature_path,std::ostream& csv)const{
        writer output(signature_path,profile_,capacity_,frequency_);
        output.append_stored(std::span<const std::uint8_t>(samples_.data(),static_cast<std::size_t>(used_)*row_bytes(profile_.channels)),used_);
        output.finish();
        for(std::uint32_t i=0;i<used_;++i)write_stage(csv,stages_[i]);
        csv.flush();check(csv.good(),"Deferred timing CSV flush failed");
    }
};
}

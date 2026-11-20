#include "support/usb_preopus_signature.h"
#include "support/usb_short_evidence.h"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <vector>

namespace {
unsigned checks=0;
void test(bool ok){++checks;if(!ok)throw std::runtime_error("Pre-Opus file regression at check "+std::to_string(checks));}
template<class F>void reject(F f){bool failed=false;try{f();}catch(const std::runtime_error&){failed=true;}test(failed);}
std::uint32_t u32(const std::uint8_t* b){return b[0]|(std::uint32_t(b[1])<<8)|(std::uint32_t(b[2])<<16)|(std::uint32_t(b[3])<<24);}
std::uint64_t u64(const std::uint8_t* b){return u32(b)|(std::uint64_t(u32(b+4))<<32);}
struct directory {
    std::filesystem::path path;
    directory(){std::array<wchar_t,MAX_PATH+1> base{};const auto n=GetTempPathW(MAX_PATH,base.data());test(n&&n<MAX_PATH);
        LARGE_INTEGER q{};test(QueryPerformanceCounter(&q)!=FALSE);path=std::filesystem::path(base.data())/(L"VibeSignatureTest-"+std::to_wstring(GetCurrentProcessId())+L"-"+std::to_wstring(q.QuadPart));
        test(CreateDirectoryW(path.c_str(),nullptr)!=FALSE);}
    ~directory(){RemoveDirectoryW(path.c_str());}
};
void profile(const std::filesystem::path& dir,const vibe_input::Profile& p,unsigned index){
    constexpr std::uint64_t hz=10000000,first=0x1234567890ull,qpc=0x3456789012ull;
    std::vector<float> pcm(static_cast<std::size_t>(240)*p.channels);
    for(std::size_t i=0;i<pcm.size();++i)pcm[i]=static_cast<float>(i+1)/8192.0f;
    pcm[0]=std::bit_cast<float>(0x80000000u);
    vibe_input::AudioHeader m{};m.profile=p;m.sourceKind=vibe_input::UserModeUsb;m.epoch=9;m.firstFrame=first;m.firstUrbSequence=789;m.lastUrbSequence=790;m.sendQpc=qpc-1;m.qpcFrequency=hz;
    const auto path=dir/(std::to_string(index)+".bin");
    usb_signature::writer w(path.wstring(),p,2,hz);
    struct remove_owned {const std::filesystem::path& path;~remove_owned(){DeleteFileW(path.c_str());}} cleanup{path};
    reject([&]{usb_signature::writer duplicate(path.wstring(),p,2,hz);});
    reject([&]{w.append(m,1,0,qpc,pcm);});
    auto bad=m;bad.qpcFrequency=1;reject([&]{w.append(bad,0,0,qpc,pcm);});
    bad=m;bad.epoch=std::uint64_t{1}<<32;reject([&]{w.append(bad,0,0,qpc,pcm);});
    reject([&]{w.append(m,0,0,qpc,std::span<const float>(pcm.data(),pcm.size()-1));});
    const auto saved=pcm[59*p.channels];pcm[59*p.channels]=std::numeric_limits<float>::quiet_NaN();
    reject([&]{w.append(m,0,0,qpc,pcm);});pcm[59*p.channels]=saved;
    w.append(m,0,0,qpc,pcm);++m.blockSequence;m.firstFrame+=240;w.append(m,1,0,qpc+1,pcm);
    reject([&]{w.append(m,2,0,qpc+2,pcm);});w.finish();test(w.count()==2);
    std::ifstream input(path,std::ios::binary);std::vector<std::uint8_t> data((std::istreambuf_iterator<char>(input)),{});
    const auto bytes=64+16*p.channels;
    test(data.size()==80+2u*bytes&&w.bytes()==data.size());test(std::memcmp(data.data(),"VIBESIG1",8)==0);
    const std::array<std::uint32_t,16> expected{1,80,static_cast<std::uint32_t>(bytes),p.channels,48000,240,p.channelMask,3,p.nativeContainerBits,p.nativeValidBits,4,0,59,119,239,2};
    for(std::size_t i=0;i<expected.size();++i)test(u32(&data[8+4*i])==expected[i]);test(u64(&data[72])==hz);
    for(unsigned i=0;i<2;++i){const auto* r=&data[80+i*bytes];
        test(u64(r)==i&&u64(r+8)==0&&u64(r+16)==i&&u64(r+24)==first+i*240&&u32(r+32)==9&&u32(r+36)==0);
        test(u64(r+40)==789&&u64(r+48)==790&&u64(r+56)==qpc+i);
        unsigned at=64;for(unsigned frame:{0u,59u,119u,239u})for(unsigned ch=0;ch<p.channels;++ch){
            test(u32(r+at)==std::bit_cast<std::uint32_t>(pcm[frame*p.channels+ch]));at+=4;}
    }
    const auto deferred=dir/(std::to_string(index)+"-deferred.bin");
    usb_signature::history history(p,2,hz);
    m.blockSequence=0;m.firstFrame=first;
    std::ostringstream csv;
    for(unsigned i=0;i<2;++i){
        usb_signature::stage_row stage{};
        for(unsigned j=0;j<stage.size();++j)stage[j]=300+j;
        stage[0]=0;stage[1]=i;stage[2]=m.blockSequence;stage[3]=m.epoch;stage[4]=m.firstFrame;
        stage[9]=hz;stage[16]=qpc+i;
        if(i==0){auto bad=stage;++bad[1];reject([&]{history.record(m,bad,pcm);});}
        history.record(m,stage,pcm);++m.blockSequence;m.firstFrame+=240;
        test(!std::filesystem::exists(deferred)&&csv.str().empty());
    }
    usb_signature::stage_row excess{};reject([&]{history.record(m,excess,pcm);});
    history.save(deferred.wstring(),csv);
    struct remove_owned deferred_cleanup{deferred};
    std::ifstream copy(deferred,std::ios::binary);
    const std::vector<std::uint8_t> committed((std::istreambuf_iterator<char>(copy)),{});
    test(committed==data&&history.bytes()==data.size()&&history.count()==2);
    std::ostringstream expected_csv;
    for(unsigned i=0;i<2;++i){
        for(unsigned j=0;j<23;++j){
            if(j)expected_csv<<',';
            const std::uint64_t v=j==0?0:j==1||j==2?i:j==3?9:j==4?first+240*i:j==9?hz:j==16?qpc+i:300+j;
            expected_csv<<v;
        }
        expected_csv<<'\n';
    }
    test(csv.str()==expected_csv.str());
    reject([&]{history.save(deferred.wstring(),csv);});
}
}
int main(){try{
    directory dir;unsigned index=0;
    struct test_trace {
        std::uint64_t output_block=0;
        std::array<float,240*12> pcm{};
        std::array<std::uint32_t,2> datagram_bytes{};
        std::array<std::array<std::uint8_t,1400>,2> datagrams{};
        std::uint32_t datagram_count=0;
    };
    usb_short::history<test_trace> short_run(2,12);
    test_trace tr;tr.pcm[239*12+11]=-0.25f;tr.datagram_count=2;tr.datagram_bytes={1400,13};
    for(unsigned i=0;i<tr.datagram_count;++i)for(unsigned j=0;j<tr.datagram_bytes[i];++j)tr.datagrams[i][j]=static_cast<std::uint8_t>(i+j);
    auto invalid=tr;invalid.datagram_bytes[1]=1401;
    reject([&]{short_run.append(invalid,100,3,1.0);});
    short_run.append(tr,100,3,1.0);tr.output_block=1;tr.datagram_count=0;
    reject([&]{short_run.append(tr,200,2,1.0);});
    short_run.append(tr,200,4,1.5);tr.output_block=2;
    reject([&]{short_run.append(tr,300,4,1.0);});
    test(short_run.count()==2&&short_run.wire_bytes()==1421);
    const auto& stored=*short_run.begin();
    test(stored.trace.pcm==tr.pcm&&stored.trace.datagrams==tr.datagrams&&stored.dequeue==100&&stored.control_prefix==3);
    test((short_run.end()-1)->trace.datagram_count==0);
    reject([&]{usb_short::history<test_trace> invalid_duration(28001,12);});
    reject([&]{usb_short::history<test_trace> invalid_channels(2,0);});
    for(auto layout:std::array<std::array<unsigned,2>,5>{{{2,3},{6,0x3f},{6,0x60f},{8,0x63f},{12,0x2d63f}}})for(bool pcm24:{false,true}){
        const vibe_input::Profile p{48000,static_cast<std::uint16_t>(layout[0]),240,layout[1],static_cast<std::uint16_t>(pcm24?32:16),static_cast<std::uint16_t>(pcm24?24:16)};
        profile(dir.path,p,++index);
    }
    std::cout<<"Pre-Opus signature files PASS: "<<checks<<" checks, ten exact profiles, no audio\n";return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}

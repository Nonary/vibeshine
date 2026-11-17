#include "src/audio_bounded_trace.h"
#include "src/audio_packet.h"
#include "src/audio_encoder.h"
#include "src/audio_transport.h"
#include "src/thread_safe.h"
#include <openssl/crypto.h>
extern "C" {
#include "src/rswrapper.h"
}
#include <atomic>
#include <cstdlib>
#include <iostream>
#include <new>
#include <thread>

namespace {
  thread_local bool count_allocations = false;
  thread_local std::uint64_t cpp_allocations = 0, crypto_allocations = 0;
  unsigned assertions = 0;
  void check(bool ok, const char *message) { ++assertions; if(!ok) throw std::runtime_error(message); }
  void *crypto_malloc(std::size_t size, const char *, int) {
    if(count_allocations) ++crypto_allocations;
    return std::malloc(size);
  }
  void *crypto_realloc(void *p, std::size_t size, const char *, int) {
    if(count_allocations) ++crypto_allocations;
    return std::realloc(p,size);
  }
  void crypto_free(void *p, const char *, int) { std::free(p); }
}
void *operator new(std::size_t size) {
  if(count_allocations) ++cpp_allocations;
  if(auto p=std::malloc(size ? size : 1)) return p;
  throw std::bad_alloc();
}
void *operator new[](std::size_t size) { return ::operator new(size); }
void operator delete(void *p) noexcept { std::free(p); }
void operator delete[](void *p) noexcept { std::free(p); }
void operator delete(void *p,std::size_t) noexcept { std::free(p); }
void operator delete[](void *p,std::size_t) noexcept { std::free(p); }

int main() {
  try {
    check(CRYPTO_set_mem_functions(crypto_malloc,crypto_realloc,crypto_free)==1,"OpenSSL allocation hooks must precede initialization");
    audio::bounded_trace_t<std::uint64_t,8> trace;
    check(!trace.front(),"trace initially nonempty");
    for(unsigned i=0;i<8;++i){auto p=trace.reserve();check(p!=nullptr,"trace capacity too small");*p=i;trace.publish();}
    check(!trace.reserve(),"full trace silently overwrites");
    for(unsigned i=0;i<8;++i){check(*trace.front()==i,"trace order changed");trace.pop();}
    std::atomic<bool> corrupt{false};
    std::thread consumer([&]{for(std::uint64_t i=0;i<100000;++i){const std::uint64_t *p;while(!(p=trace.front()))std::this_thread::yield();if(*p!=i)corrupt=true;trace.pop();}});
    for(std::uint64_t i=0;i<100000;++i){std::uint64_t *p;while(!(p=trace.reserve()))std::this_thread::yield();*p=i;trace.publish();}
    consumer.join();check(!corrupt&&!trace.front()&&trace.completed()==trace.published(),"concurrent wrap-around loses or corrupts diagnostics");

    safe::queue_t<audio::packet_t> queue(4);queue.preallocate();
    const auto allocation=queue.unsafe().data();const auto capacity=queue.unsafe().capacity();
    auto delivery=std::make_shared<audio::delivery_state_t>();
    std::array<std::uint8_t,1400> encoded{};
    for(int i=0;i<4;++i)check(queue.try_raise(nullptr,std::span<const std::uint8_t>(encoded.data(),375),delivery),"preallocated packet capacity incorrect");
    check(!queue.try_raise(nullptr,std::span<const std::uint8_t>(encoded.data(),375),delivery),"full packet queue accepted overwrite");
    queue.reset();check(queue.unsafe().data()==allocation&&queue.unsafe().capacity()==capacity,"queue reset freed reserved storage");

    reed_solomon_init();
    audio::session_profile_t profile;profile.bitrate=600000;profile.source_kind="project_atmos_pcm714";
    audio::opus_encoder_t encoder;std::string error;
    check(encoder.initialize(profile,error),"production Opus initialization failed");
    audio::transport::config_t config;config.discrete_714=true;config.encrypted=true;config.key_id=0x71412001;
    audio::transport::packetizer_t sender(config);
    std::uint64_t datagrams=0,bytes=0;
    const audio::transport::packetizer_t::emit_t emit=[&](auto header,auto payload){++datagrams;bytes+=header.size()+payload.size();};
    std::array<float,2880> pcm{};for(std::size_t i=0;i<pcm.size();++i)pcm[i]=static_cast<float>(static_cast<int>(i%97)-48)/300.0f;
    // Count only accepted steady blocks. Initialization, fault diagnostics and
    // test assertion reporting are outside this interval. Opus C malloc is not
    // intercepted by these C++/OpenSSL hooks; do not call this process-wide.
    bool good=true;
    count_allocations=true;
    for(unsigned i=0;i<1000;++i){
      const auto n=encoder.encode(pcm,encoded,error);
      if(n<=0||!queue.try_raise(nullptr,std::span<const std::uint8_t>(encoded.data(),n),delivery)){good=false;break;}
      auto packet=queue.try_pop();
      if(!packet||!packet->is_inline()||!sender.push(packet->payload(),emit)){good=false;break;}
      delivery->observe_queue_age(std::chrono::steady_clock::now()-packet->queued_at);
      delivery->acknowledge();
    }
    count_allocations=false;
    check(good&&delivery->completed()==1000&&datagrams==1500&&bytes>0,"production ingress/encoder/transport failed");
    check(encoder.packet_budget_verified()&&encoder.effective_packet_bitrate()==600000&&encoder.lsb_depth()==24,"actual Atmos packet budget or input precision was not verified");
    check(cpp_allocations==0,"steady production packet path allocated C++ storage");
    check(crypto_allocations==0,"steady production packet path allocated OpenSSL storage");
    check(queue.unsafe().data()==allocation&&queue.unsafe().capacity()==capacity,"queue grew during production ingress");
    // Legacy byte ownership remains exact through the same queue and sender API.
    audio::buffer_t legacy(7);for(unsigned i=0;i<7;++i)legacy[i]=static_cast<std::uint8_t>(i+1);
    check(queue.try_raise(nullptr,std::move(legacy)),"legacy queue rejected");auto old=queue.try_pop();
    check(old&&!old->is_inline()&&old->payload().size()==7&&old->payload()[6]==7,"legacy packet bytes changed");
    std::cout<<"Ingress PASS assertions="<<assertions<<" blocks=1000 datagrams="<<datagrams<<" C++ allocations="<<cpp_allocations<<" OpenSSL allocations="<<crypto_allocations<<"\n";
    return 0;
  }catch(const std::exception &e){count_allocations=false;std::cerr<<e.what()<<"\n";return 1;}
}

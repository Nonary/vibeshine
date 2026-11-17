#include "src/audio_atmos_device.h"
#include "src/audio_encoder.h"
#include <iostream>
#include <stdexcept>
#include <thread>
using namespace audio;
namespace {
  void check_at(bool v,const char *expression,int line){if(!v)throw std::runtime_error("Atmos format lease regression at line "+std::to_string(line)+": "+expression);}
#define check(value) check_at((value),#value,__LINE__)
  struct state {atmos_device_state_t current{{1},{2}};bool identity=true,partial_fail=false,readback_fail=false,partial_readback=false,public_readback_fail=false;unsigned sets=0,settle_mode=0;};
  struct backend:atmos_device_backend_t {
    state &s;explicit backend(state &s):s(s){}
    atmos_setter_diagnostic_t setter_log;
    atmos_setter_diagnostic_t last_setter_diagnostic()override{return setter_log;}
    void verify()override{if(!s.identity)throw std::runtime_error("binding changed");}
    atmos_device_state_t read()override{return s.current;}
    std::vector<unsigned char> read_public_mix()override{if(s.public_readback_fail)throw std::runtime_error("public mix diagnostic failed");return {4};}
    atmos_format_settle_t settle(const atmos_device_state_t &target,const atmos_device_state_t &initial,const std::vector<unsigned char> &)override {
      atmos_format_settle_t r;if(!s.settle_mode)return r;
      r.qpc_frequency=1;r.attempted=initial!=target;r.maximum_wait_ms=2000;r.reads=1;
      if(s.settle_mode==1)s.current=target;
      r.final_readback=s.current;r.final_public_mix=s.current.mix_format;
      if(s.settle_mode==2)r.final_public_mix={9};
      r.target_matched=r.final_readback==target&&r.final_public_mix==target.mix_format;
      r.timed_out=!r.target_matched;return r;
    }
    atmos_device_state_t target(const atmos_format_t &)override{return {{3},{4}};}
    void set(const atmos_device_state_t &value)override{
      setter_log={};setter_log.called=true;setter_log.device_before=value.device_format;setter_log.mix_before=value.mix_format;
      setter_log.device_after=value.device_format;setter_log.mix_after=value.mix_format;
      ++s.sets;s.current=value;
      if(s.partial_fail&&s.sets==1){s.current.mix_format={2};setter_log.mix_after={2};setter_log.hresult=0x80004005;throw std::runtime_error("partial setter failure");}
      if(s.readback_fail&&s.sets==1)s.current={{9},{9}};
      if(s.partial_readback&&s.sets==1)s.current.mix_format={2};
    }
  };
  struct affinity {std::thread::id owner;bool correct=true;unsigned calls=0;};
  struct affine_probe:backend {
    affinity &a;
    affine_probe(state &s,affinity &a):backend(s),a(a){a.owner=std::this_thread::get_id();}
    void thread_check(){a.correct=a.correct&&a.owner==std::this_thread::get_id();++a.calls;}
    ~affine_probe()override{thread_check();}
    void verify()override{thread_check();backend::verify();}
    atmos_device_state_t read()override{thread_check();return backend::read();}
    std::vector<unsigned char> read_public_mix()override{thread_check();return backend::read_public_mix();}
    atmos_format_settle_t settle(const atmos_device_state_t &t,const atmos_device_state_t &i,const std::vector<unsigned char> &m)override{thread_check();return backend::settle(t,i,m);}
    atmos_device_state_t target(const atmos_format_t &f)override{thread_check();return backend::target(f);}
    void set(const atmos_device_state_t &v)override{thread_check();backend::set(v);}
  };
}
int main(){try{
  unsigned profiles=0;
  for(unsigned ch:{2,6,8,12})for(bool pcm16:{false,true}){
    atmos_format_t f{ch,atmos_default_mask(ch),static_cast<std::uint16_t>(pcm16?16:32),static_cast<std::uint16_t>(pcm16?16:24)};
    check(f.valid()&&f.samples()==ch*240&&f.block_align()==ch*(pcm16?2:4));++profiles;
  }
  check((atmos_format_t{6,0x60f,32,24}.valid()));check(!(atmos_format_t{6,0x63f,32,24}.valid()));
  check(!(atmos_format_t{12,0x2d63f,32,16}.valid()));
  state normal;
  {atmos_device_lease_t lease(std::make_unique<backend>(normal));check(lease.inspect()&&normal.sets==0);check(lease.apply({}));check(lease.report().active);check(lease.restore()&&lease.report().restore==atmos_restore_e::restored);check(lease.restore());}
  check(normal.current==(atmos_device_state_t{{1},{2}})&&normal.sets==2);
  state partial_readback;partial_readback.partial_readback=true;
  {atmos_device_lease_t lease(std::make_unique<backend>(partial_readback));
   check(!lease.apply({}));check(lease.report().apply_readback_valid);
   check(lease.report().apply_readback==(atmos_device_state_t{{3},{2}}));
   check(lease.report().apply_public_mix_readback==std::vector<unsigned char>{4});
   check(lease.report().installed==(atmos_device_state_t{{3},{4}}));
   check(lease.report().after==lease.report().before&&lease.report().restore==atmos_restore_e::restored);
   check(lease.restore()&&lease.report().apply_readback==(atmos_device_state_t{{3},{2}}));}
  check(partial_readback.sets==2);
  state settled;settled.partial_readback=true;settled.settle_mode=1;
  {atmos_device_lease_t lease(std::make_unique<backend>(settled));check(lease.apply({}));
   check(lease.report().apply_readback==(atmos_device_state_t{{3},{2}}));
   check(lease.report().apply_settle.target_matched&&lease.report().after==lease.report().installed);
   check(lease.restore()&&lease.report().restore_settle.target_matched);
   check(lease.report().apply_readback==(atmos_device_state_t{{3},{2}})&&lease.report().after==lease.report().before);}
  state no_settle;no_settle.partial_readback=true;no_settle.settle_mode=3;
  {atmos_device_lease_t lease(std::make_unique<backend>(no_settle));check(!lease.apply({}));
   check(lease.report().apply_settle.timed_out&&!lease.report().apply_settle.target_matched);
   check(lease.report().restore==atmos_restore_e::restored);}
  state public_disagrees;public_disagrees.settle_mode=2;
  {atmos_device_lease_t lease(std::make_unique<backend>(public_disagrees));check(!lease.apply({}));
   check(lease.report().apply_settle.final_readback==lease.report().installed&&!lease.report().apply_settle.target_matched);
   check(lease.report().restore==atmos_restore_e::failed);}
  state early_restore;
  {atmos_device_lease_t lease(std::make_unique<backend>(early_restore));check(lease.apply({}));
   early_restore.current=lease.report().before;early_restore.settle_mode=2;
   check(!lease.restore()&&lease.report().restore==atmos_restore_e::failed);
   check(lease.report().restore_settle.final_readback==lease.report().before&&!lease.report().restore_settle.target_matched);
   check(early_restore.sets==1);}
  state public_failure;public_failure.public_readback_fail=true;
  {atmos_device_lease_t lease(std::make_unique<backend>(public_failure));check(lease.apply({}));
   check(lease.report().apply_public_mix_readback.empty()&&lease.report().apply_public_mix_readback_error=="public mix diagnostic failed");
   check(lease.restore()&&lease.report().restore==atmos_restore_e::restored);}
  state unwind;
  {atmos_device_lease_t lease(std::make_unique<backend>(unwind));check(lease.apply({}));}
  check(unwind.sets==2&&unwind.current==(atmos_device_state_t{{1},{2}}));
  state conflict;
  {atmos_device_lease_t lease(std::make_unique<backend>(conflict));check(lease.apply({}));conflict.current={{7},{8}};check(lease.restore());check(lease.report().restore==atmos_restore_e::conflict);}
  check(conflict.sets==1&&conflict.current==(atmos_device_state_t{{7},{8}}));
  state partial;partial.partial_fail=true;
  {atmos_device_lease_t lease(std::make_unique<backend>(partial));check(!lease.apply({}));check(!lease.report().apply_readback_valid);check(lease.report().restore==atmos_restore_e::restored);
   check(lease.report().apply_setter.called&&lease.report().apply_setter.hresult==0x80004005);
   check(lease.report().apply_setter.mix_before==std::vector<unsigned char>{4}&&lease.report().apply_setter.mix_after==std::vector<unsigned char>{2});
   check(lease.report().restore_setter.called&&lease.report().restore_setter.hresult==0&&lease.report().restore_setter.device_before==std::vector<unsigned char>{1});}
  check(partial.sets==2&&partial.current==(atmos_device_state_t{{1},{2}}));
  state changed;
  {atmos_device_lease_t lease(std::make_unique<backend>(changed));check(lease.apply({}));changed.identity=false;check(!lease.restore());check(lease.report().restore==atmos_restore_e::failed);}
  check(changed.sets==1);
  state readback;readback.readback_fail=true;
  {atmos_device_lease_t lease(std::make_unique<backend>(readback));check(!lease.apply({}));check(lease.report().restore==atmos_restore_e::conflict);}
  check(readback.sets==1);
  state threaded;affinity owner;
  auto lease=std::make_unique<atmos_device_lease_t>(make_affine_atmos_backend([&]{return std::make_unique<affine_probe>(threaded,owner);}));
  check(lease->apply({}));
  std::thread other([lease=std::move(lease)]()mutable{lease.reset();});other.join();
  check(owner.correct&&owner.calls>=8&&owner.owner!=std::this_thread::get_id()&&threaded.sets==2);
  bool failed_factory=false;
  try{make_affine_atmos_backend([]()->std::unique_ptr<atmos_device_backend_t>{throw std::runtime_error("factory failure");});}
  catch(const std::runtime_error&){failed_factory=true;}
  check(failed_factory);
  for(int channels:{2,6,8,12}){
    session_profile_t profile;check(standard_opus_profile(channels,false,profile));profile.source_kind="project_atmos_pcm714";
    opus_encoder_t encoder;std::string error;
    if(!encoder.initialize(profile,error))throw std::runtime_error("Opus initialization for "+std::to_string(channels)+" channels: "+error);
    check(encoder.lsb_depth()==24&&encoder.floating_point_build()&&encoder.child_bitrate()>0);
    check(!encoder.packet_budget_verified()&&encoder.effective_packet_bitrate()==0);
    std::array<float,2880> pcm{};std::array<std::uint8_t,1400> packet{};
    for(int block=0;block<16;++block){
      for(int i=0;i<channels*240;++i)pcm[i]=static_cast<float>((i*17+block*13)%199-99)/8192;
      const auto bytes=encoder.encode(std::span<const float>(pcm.data(),channels*240),packet,error);
      if(bytes<0)throw std::runtime_error("Actual Opus encode for "+std::to_string(channels)+" channels: "+error);
      check(bytes==profile.bitrate/1600&&encoder.packet_budget_verified());
      check(encoder.effective_packet_bitrate()==bytes*1600&&encoder.child_bitrate()>0);
    }
    std::cout<<channels<<"ch requested="<<profile.bitrate<<" child_codec_rate="<<encoder.child_bitrate()<<" effective_packet_rate="<<encoder.effective_packet_bitrate()<<" LSB="<<encoder.lsb_depth()<<'\n';
  }
  {session_profile_t profile;check(standard_opus_profile(2,false,profile));profile.source_kind="project_atmos_pcm714";profile.frame_size=120;
   opus_encoder_t encoder;std::string error;check(!encoder.initialize(profile,error));}
  {session_profile_t profile;check(standard_opus_profile(2,false,profile));profile.source_kind="project_atmos_pcm714";
   opus_encoder_t encoder;std::string error;check(encoder.initialize(profile,error));
   std::array<float,480> pcm{};std::array<std::uint8_t,59> undersized{};
   check(encoder.encode(pcm,undersized,error)<0&&!error.empty());
   check(!encoder.packet_budget_verified()&&encoder.effective_packet_bitrate()==0);}
  std::cout<<"Atmos product lease/layout/float Opus tests PASS; "<<profiles<<" standard native profiles plus side-5.1; no device mutation\n";return 0;
}catch(const std::exception &e){std::cerr<<e.what()<<'\n';return 1;}}

#include "src/audio_usb_validation.h"
#include <array>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <vector>
namespace {
unsigned checks=0;
void need(bool value,const char* text){++checks;if(!value)throw std::runtime_error(text);}
using namespace vibe_input;
WelcomeMessage session(Profile p) {
 constexpr std::uint64_t nonce[2]={1,2};WelcomeMessage s{};s.header=MakeHeader(Welcome,sizeof(s),nonce);
 s.profile=p;s.binding.vendor=0x1209;s.binding.product=VibeUsbProduct(p.channels,p.channelMask,p.nativeContainerBits,p.nativeValidBits);
 s.qpcFrequency=10000000;s.queueBlocks=QueueBlocks;s.maximumAgeMs=MaximumAgeMs;return s;
}
AudioHeader first(const WelcomeMessage& s) {
 AudioHeader h{};h.header=MakeHeader(Audio,AudioWireBytes(s.profile.channels),s.header.nonce);
 h.sourceKind=UserModeUsb;h.epoch=3;h.firstFrame=480;h.firstUrbSequence=h.lastUrbSequence=20;
 h.firstReceiveQpc=h.lastReceiveQpc=10000;h.blockReadyQpc=11000;h.enqueueQpc=12000;h.sendQpc=13000;
 h.qpcFrequency=s.qpcFrequency;h.profile=s.profile;h.queueDepthBlocks=1;h.queueCapacityBlocks=QueueBlocks;
 h.payloadBytes=AudioPayloadBytes(s.profile.channels);h.lastUrbFrameOffset=239;return h;
}
}
int main(){try {
 // Independent expected PID pairs: PCM16 then PCM24.
 for(auto layout:std::array<std::array<unsigned,4>,5>{{{2,3,2,1},{6,0x3f,4,3},{6,0x60f,6,5},{8,0x63f,8,7},{12,0x2d63f,9,14}}})for(bool pcm24:{false,true}) {
  const Profile p{48000,static_cast<std::uint16_t>(layout[0]),240,layout[1],static_cast<std::uint16_t>(pcm24?32:16),static_cast<std::uint16_t>(pcm24?24:16)};
  const auto s=session(p);const auto h=first(s);std::vector<float> pcm(240*layout[0]);
  {
   auto old=s;old.sourceKind=UserModeUsb;old.serverPid=23;old.clientPid=25;old.serverCreationTime=33;old.clientCreationTime=35;old.windowsSessionId=1;
   auto next=old;++next.header.nonce[0];
   need(audio::usb_same_producer_new_session(old,next),"Authenticated AS0 successor rejected");
   for(unsigned i=0;i<12;++i){
    auto bad=next;
    switch(i){case 0:bad=old;break;case 1:++bad.serverPid;break;case 2:++bad.clientPid;break;
     case 3:++bad.serverCreationTime;break;case 4:++bad.clientCreationTime;break;case 5:++bad.windowsSessionId;break;
     case 6:++bad.qpcFrequency;break;case 7:++bad.binding.port;break;case 8:++bad.profile.channels;break;
     case 9:bad.sourceKind=0;break;case 10:++bad.header.magic;break;case 11:++bad.binding.endpointId[0];break;}
    need(!audio::usb_same_producer_new_session(old,bad),"Substituted AS0 successor accepted");
   }
   auto restarted=h;restarted.epoch=5;restarted.firstReceiveQpc=h.lastReceiveQpc+1;
   need(audio::usb_fresh_native_epoch(h.epoch,h.lastReceiveQpc,restarted),"Fresh native interval rejected");
   restarted.epoch=h.epoch;need(!audio::usb_fresh_native_epoch(h.epoch,h.lastReceiveQpc,restarted),"Reused native epoch accepted");
   restarted.epoch=5;restarted.firstReceiveQpc=h.lastReceiveQpc;
   need(!audio::usb_fresh_native_epoch(h.epoch,h.lastReceiveQpc,restarted),"Stale native callback time accepted");
  }
  need(s.binding.product==0x7140,"All private native profiles must use the requested common USB product");
  {
   auto last=h;last.epoch=4;last.firstFrame=480;last.lastUrbSequence=250;last.lastReceiveQpc=1000;
   auto next=h;next.epoch=4;next.firstFrame=1200;next.blockSequence=0;next.firstUrbSequence=251;next.firstReceiveQpc=1001;
   need(audio::usb_overflow_successor(last,next),"Fresh same-epoch overflow successor rejected");
   for(unsigned i=0;i<6;++i){
    auto bad=next;
    if(i==0)bad.epoch=5;if(i==1)bad.firstFrame=720;if(i==2)bad.firstFrame=719;
    if(i==3)bad.firstUrbSequence=250;if(i==4)bad.firstReceiveQpc=1000;if(i==5)bad.blockSequence=1;
    need(!audio::usb_overflow_successor(last,bad),"Invalid USB overflow successor accepted");
   }
   audio::usb_recovery_limit_t limit;
   need(limit.admit(1000,100),"First confirmed overflow refused");
   need(!limit.admit(1499,100),"Rapid repeated overflow accepted");
   need(limit.admit(1500,100),"Second separated overflow refused");
   need(!limit.admit(2000,100),"Third overflow accepted");
  }
  for(unsigned pid=0;pid<=15;++pid) {
   auto other=s;other.binding.product=static_cast<std::uint16_t>(pid);
   audio::usb_block_validator_t binding_gate;
   need((binding_gate.accept(h,other,pcm,14000)==nullptr)==(pid==s.binding.product),"Wrong profile USB product accepted");
  }
  {auto other=s;other.binding.vendor=0x1208;audio::usb_block_validator_t binding_gate;
   need(binding_gate.accept(h,other,pcm,14000)!=nullptr,"Different USB vendor accepted");}
  pcm[0]=pcm24?1.0f/8388608.0f:1.0f/32768.0f;
  audio::usb_block_validator_t gate;need(!gate.accept(h,s,pcm,14000),"Valid first USB block rejected");
  auto second=h;second.blockSequence=1;second.firstFrame+=240;second.firstUrbFrameOffset=240;second.lastUrbFrameOffset=479;
  need(!gate.accept(second,s,pcm,14000),"Second half of same URB rejected");
  auto next=second;next.blockSequence=2;next.firstFrame+=240;next.firstUrbSequence=next.lastUrbSequence=29;
  next.firstUrbFrameOffset=0;next.lastUrbFrameOffset=239;need(!gate.accept(next,s,pcm,14000),"Intervening control URB IDs wrongly treated as audio loss");
  for(unsigned kind=0;kind<18;++kind) {
   auto bad=h;
   switch(kind) {
    case 0:bad.header.magic^=1;break;case 1:bad.header.nonce[0]++;break;case 2:bad.sourceKind=0;break;
    case 3:bad.profile.channelMask^=1;break;case 4:bad.epoch=0;break;case 5:bad.blockSequence=1;break;
    case 6:bad.inputDroppedFrames=1;break;case 7:bad.inputRejectedUrbs=1;break;case 8:bad.queueOverruns=1;break;
    case 9:bad.blockReadyQpc=9999;break;case 10:bad.sendQpc=11999;break;case 11:bad.sendQpc=14001;break;
    case 12:bad.queueDepthBlocks=5;break;case 13:bad.queueCapacityBlocks=32;break;case 14:bad.lastUrbFrameOffset=240;break;
    case 15:bad.qpcFrequency=1;break;case 16:bad.reserved=1;break;case 17:bad.firstFrame=UINT64_MAX;break;
   }
   audio::usb_block_validator_t reject;need(reject.accept(bad,s,pcm,14000)!=nullptr,"Forged USB metadata accepted");
  }
  for(unsigned kind=0;kind<4;++kind) {
   audio::usb_block_validator_t reject;need(!reject.accept(h,s,pcm,14000),"Initial block failed");auto bad=second;
   if(kind==0)++bad.epoch;if(kind==1)++bad.firstFrame;if(kind==2)bad.firstUrbFrameOffset=0;if(kind==3)bad.firstReceiveQpc=9999;
   need(reject.accept(bad,s,pcm,14000)!=nullptr,"Cross-block discontinuity accepted");
  }
  for(float value:{std::numeric_limits<float>::quiet_NaN(),1.0f,pcm24?1.0f/16777216.0f:1.0f/65536.0f}) {
   audio::usb_block_validator_t reject;pcm[0]=value;need(reject.accept(h,s,pcm,14000)!=nullptr,"Non-native PCM accepted");
  }
 }
 std::cout<<"USB source validation PASS "<<checks<<" checks; no audio/device activity\n";return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<"\n";return 1;}}

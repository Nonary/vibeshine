#pragma once
#include "vibe_input_protocol.h"
#include "vibe_profile_ids.h"
#include <cstdint>
#include <cmath>
#include <cstring>
#include <span>
namespace audio {
inline bool usb_binding_matches_profile(const vibe_input::UsbBinding& binding,const vibe_input::Profile& profile) noexcept {
  const auto product=VibeLabUsbProduct(profile.channels,profile.channelMask,profile.nativeContainerBits,profile.nativeValidBits);
  return product!=0 && binding.vendor==0x1209 && binding.product==product;
}
inline bool usb_same_producer_new_session(const vibe_input::WelcomeMessage& old,const vibe_input::WelcomeMessage& next) noexcept {
  using namespace vibe_input;
  return ValidHeader(old.header,Welcome,sizeof(old)) && ValidHeader(next.header,Welcome,sizeof(next)) &&
         old.sourceKind==UserModeUsb && next.sourceKind==UserModeUsb &&
         old.serverPid==next.serverPid && old.serverCreationTime==next.serverCreationTime &&
         old.clientPid==next.clientPid && old.clientCreationTime==next.clientCreationTime &&
         old.windowsSessionId==next.windowsSessionId && old.qpcFrequency==next.qpcFrequency &&
         SameProfile(old.profile,next.profile) && !std::memcmp(&old.binding,&next.binding,sizeof(old.binding)) &&
         !SameNonce(old.header.nonce,next.header.nonce);
}
inline bool usb_fresh_native_epoch(std::uint32_t prior_epoch,std::uint64_t prior_receive,const vibe_input::AudioHeader& next) noexcept {
  return prior_epoch && next.epoch>prior_epoch && next.firstReceiveQpc>prior_receive;
}
inline bool usb_overflow_successor(const vibe_input::AudioHeader& last,const vibe_input::AudioHeader& first) noexcept {
  return last.epoch && first.epoch==last.epoch && first.blockSequence==0 &&
         last.firstFrame<=UINT64_MAX-vibe_input::Frames && first.firstFrame>last.firstFrame+vibe_input::Frames &&
         first.firstReceiveQpc>last.lastReceiveQpc && first.firstUrbSequence>last.lastUrbSequence;
}
class usb_recovery_limit_t {
  std::uint64_t previous_qpc_=0;
  unsigned attempts_=0;
public:
  bool admit(std::uint64_t qpc,std::uint64_t hz){
    if(!qpc||!hz||attempts_>=2 || (attempts_&&(qpc<=previous_qpc_||(qpc-previous_qpc_)/hz<5)))return false;
    previous_qpc_=qpc;++attempts_;return true;
  }
};
// Pure strict USB-specific gate, shared by the actual client and offline tests.
// There is deliberately no kernel state, RingReady or fake AtmosRecord here.
class usb_block_validator_t {
 bool previous_valid_=false;
 vibe_input::AudioHeader previous_{};
 public:
 const char* accept(const vibe_input::AudioHeader& h,const vibe_input::WelcomeMessage& s,
                   std::span<const float> pcm,std::uint64_t received) {
  using namespace vibe_input;
  if(!ValidProfile(s.profile)||!usb_binding_matches_profile(s.binding,s.profile)||!SameProfile(h.profile,s.profile)||h.sourceKind!=UserModeUsb||h.reserved||h.reserved2||
     !ValidHeader(h.header,Audio,AudioWireBytes(s.profile.channels))||!SameNonce(h.header.nonce,s.header.nonce)||
     h.payloadBytes!=AudioPayloadBytes(s.profile.channels)||pcm.size()!=Frames*s.profile.channels)
   return "USB profile/session mismatch";
  if(!h.epoch||h.blockSequence!=(previous_valid_?previous_.blockSequence+1:0)||
     h.firstFrame>UINT64_MAX-Frames||h.queueOverruns||h.inputDroppedFrames||h.inputRejectedUrbs)
   return "USB sequence/position/loss contract mismatch";
  if(!h.firstReceiveQpc||h.lastReceiveQpc<h.firstReceiveQpc||h.blockReadyQpc<h.lastReceiveQpc||
     h.enqueueQpc<h.blockReadyQpc||h.sendQpc<h.enqueueQpc||received<h.sendQpc||
     !h.qpcFrequency||h.qpcFrequency!=s.qpcFrequency||h.firstUrbSequence>h.lastUrbSequence||
     h.firstUrbFrameOffset>=65536||h.lastUrbFrameOffset>=65536||
     (h.firstUrbSequence==h.lastUrbSequence&&h.lastUrbFrameOffset!=h.firstUrbFrameOffset+Frames-1)||
     h.queueCapacityBlocks!=s.queueBlocks||!h.queueDepthBlocks||h.queueDepthBlocks>h.queueCapacityBlocks)
   return "USB provenance/timing/queue contract mismatch";
  if(previous_valid_&&(h.epoch!=previous_.epoch||h.firstFrame!=previous_.firstFrame+Frames||
     h.firstUrbSequence<previous_.firstUrbSequence||h.lastUrbSequence<previous_.lastUrbSequence||
     h.firstReceiveQpc<previous_.firstReceiveQpc||h.lastReceiveQpc<previous_.lastReceiveQpc||
     h.blockReadyQpc<previous_.blockReadyQpc||h.enqueueQpc<previous_.enqueueQpc||h.sendQpc<previous_.sendQpc||
     (h.firstUrbSequence==previous_.lastUrbSequence&&h.firstUrbFrameOffset!=previous_.lastUrbFrameOffset+1)))
   return "USB epoch/frame/URB continuity changed";
  const float scale=h.profile.nativeValidBits==24?8388608.0f:32768.0f;
  for(float x:pcm)if(!std::isfinite(x)||x< -1.0f||x>=1.0f||std::trunc(x*scale)!=x*scale)
   return "USB float PCM does not preserve native integer precision";
  previous_=h;previous_valid_=true;return nullptr;
 }
};
}

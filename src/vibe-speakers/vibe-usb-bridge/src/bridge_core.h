// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "vibe_input_protocol.h"
#include "vibe_usb_audio.h"
#include <array>
#include <atomic>
#include <cstring>
#include <limits>
#include <stdexcept>

namespace vibe_bridge {
using namespace vibe_input;
namespace usb = vibe::usb_audio;
struct Failure : std::runtime_error {
 ErrorCode code; std::uint32_t systemError;
 Failure(ErrorCode c,const char* text,std::uint32_t system=0):std::runtime_error(text),code(c),systemError(system){}
};
inline void Require(bool yes,ErrorCode code,const char* text){if(!yes)throw Failure(code,text);}
inline std::uint64_t AgeBudgetTicks(std::uint64_t frequency)noexcept{return(frequency/1000)*MaximumAgeMs+(frequency%1000)*MaximumAgeMs/1000;}
inline bool Fresh(std::uint64_t now,std::uint64_t ready,std::uint64_t frequency)noexcept{return frequency&&now>=ready&&now-ready<=AgeBudgetTicks(frequency);}
constexpr bool ValidDuration(std::uint32_t seconds)noexcept{return seconds<=86400;}

// Test builds may request a finite run. Product callers use zero/INFINITE.
constexpr std::uint32_t DurationWait(std::uint32_t seconds) noexcept {
 return seconds ? seconds * 1000u : UINT32_MAX;
}

template<class Wait, class Signal>
void AwaitSessionStop(std::uint32_t seconds,Wait&& wait,Signal&& signal){
 if(wait(DurationWait(seconds))==258u)signal(); // WAIT_TIMEOUT
}

// Exactly one producer and one consumer. Storage and indices survive reconnect;
// discard/reset is permitted only after the producer has been disarmed/retired.
#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable:4324)
#endif
template<class T,std::size_t Capacity> class Spsc {
 static_assert(Capacity>0&&Capacity<=UINT32_MAX);
 static_assert(std::atomic<std::uint64_t>::is_always_lock_free);
 std::array<T,Capacity> entries_{};
 alignas(64) std::atomic<std::uint64_t> written_{0};
 alignas(64) std::atomic<std::uint64_t> read_{0};
 alignas(64) std::atomic<std::uint32_t> occupied_{0};
 std::uint32_t maximum_=0;
public:
 template<class Fill> bool Push(Fill&& fill) noexcept {
  const auto w=written_.load(std::memory_order_relaxed);
  if(w-read_.load(std::memory_order_acquire)==Capacity)return false;
  const auto depth=occupied_.fetch_add(1,std::memory_order_acq_rel)+1;
  fill(entries_[static_cast<std::size_t>(w%Capacity)],depth);
  if(depth>maximum_)maximum_=depth;
  written_.store(w+1,std::memory_order_release);return true;
 }
 bool Pop(T& out) noexcept {
  const auto r=read_.load(std::memory_order_relaxed);
  if(r==written_.load(std::memory_order_acquire))return false;
  out=entries_[static_cast<std::size_t>(r%Capacity)];
  occupied_.fetch_sub(1,std::memory_order_acq_rel);
  read_.store(r+1,std::memory_order_release);return true;
 }
 std::uint64_t DiscardAfterProducerStopped() noexcept {
  const auto w=written_.load(std::memory_order_acquire),r=read_.load(std::memory_order_relaxed);
  occupied_.store(0,std::memory_order_relaxed);read_.store(w,std::memory_order_release);return w-r;
 }
 std::uint32_t MaximumAfterProducerStopped()const noexcept{return maximum_;}
 void ResetMaximumAfterProducerStopped()noexcept{maximum_=0;}
};
#ifdef _MSC_VER
#pragma warning(pop)
#endif

inline float NativeSample(const std::uint8_t* p,std::uint16_t bits) noexcept {
 if(bits==16){std::int16_t n;std::memcpy(&n,p,2);return static_cast<float>(n)/32768.0f;}
 std::int32_t n;std::memcpy(&n,p,4);return static_cast<float>(n)/2147483648.0f;
}

// Provenance comes from actual validated USB OUT requests, never invented kernel
// records. A packet's USB buffer offset can differ from its concatenated PCM
// position. Both first/last offsets below are measured in concatenated PCM frames.
class BlockAssembler {
 Profile profile_{};
 AudioMessage pending_{};
 std::uint32_t filled_=0;
 std::uint64_t nextFrame_=0,lastUrb_=0,lastReceive_=0,epoch_=0,sequence_=0,frequency_=0;
 bool started_=false;
 std::uint64_t nonce_[2]{};
public:
 void Reset(const Profile& p,const std::uint64_t(&nonce)[2]) noexcept {
  profile_=p;pending_={};filled_=0;nextFrame_=lastUrb_=lastReceive_=epoch_=sequence_=frequency_=0;started_=false;
  nonce_[0]=nonce[0];nonce_[1]=nonce[1];
 }
 std::uint32_t PartialFrames()const noexcept{return filled_;}
 std::uint64_t Blocks()const noexcept{return sequence_;}
 template<class Clock,class Emit>
 ErrorCode Append(const usb::pcm_metadata& m,std::span<const usb::iso_packet> packets,
                  std::span<const std::uint8_t> bytes,Clock&& qpc,Emit&& emit) noexcept {
  if(!ValidProfile(profile_)||!m.epoch||m.packet_count!=packets.size()||
     m.channels!=profile_.channels||m.channel_mask!=profile_.channelMask||
     m.native_container_bits!=profile_.nativeContainerBits||m.native_valid_bits!=profile_.nativeValidBits||
     !m.receive_qpc||!m.ready_qpc||m.ready_qpc<m.receive_qpc||!m.qpc_frequency||
     m.first_frame>UINT64_MAX-m.frames)return InvalidRecord;
  const auto align=profile_.channels*(profile_.nativeContainerBits/8u);
  if(m.frames>usb::max_transfer/align||m.native_bytes!=m.frames*align)return InvalidRecord;
  std::uint64_t total=0;std::size_t end=0;
  for(const auto& p:packets){
   if(p.offset<end||p.offset>bytes.size()||p.length>bytes.size()-p.offset||p.length%align)return InvalidRecord;
   end=static_cast<std::size_t>(p.offset)+p.length;total+=p.length;
   if(profile_.nativeValidBits==24)for(std::size_t i=p.offset;i<end;i+=4)if(bytes[i])return InvalidRecord;
  }
  if(total!=m.native_bytes)return InvalidRecord;
  if(!m.frames)return NoError; // A legitimate empty ISO request is not PCM.
  if(started_){
   if(m.epoch!=epoch_)return EpochChanged;
   if(m.first_frame!=nextFrame_||m.sequence<=lastUrb_)return SequenceGap;
   if(m.receive_qpc<lastReceive_||m.qpc_frequency!=frequency_)return InvalidRecord;
  }else{epoch_=m.epoch;frequency_=m.qpc_frequency;started_=true;}
  std::uint32_t urbFrame=0;
  for(const auto& p:packets){
   for(std::uint32_t offset=0;offset<p.length;offset+=align,++urbFrame){
    if(!filled_){
     auto& h=pending_.info;h={};h.header=MakeHeader(Audio,AudioWireBytes(profile_.channels),nonce_);
     h.sourceKind=UserModeUsb;h.blockSequence=sequence_;h.epoch=m.epoch;h.firstFrame=m.first_frame+urbFrame;
     h.firstUrbSequence=m.sequence;h.firstReceiveQpc=m.receive_qpc;h.firstUrbFrameOffset=urbFrame;
     h.qpcFrequency=m.qpc_frequency;h.profile=profile_;h.payloadBytes=AudioPayloadBytes(profile_.channels);
     h.queueCapacityBlocks=QueueBlocks;
    }
    for(std::uint32_t c=0;c<profile_.channels;++c)
     pending_.samples[filled_*profile_.channels+c]=NativeSample(bytes.data()+p.offset+offset+c*(profile_.nativeContainerBits/8u),profile_.nativeContainerBits);
    ++filled_;
    if(filled_==Frames){
     auto& h=pending_.info;h.lastUrbSequence=m.sequence;h.lastReceiveQpc=m.receive_qpc;h.lastUrbFrameOffset=urbFrame;
     h.blockReadyQpc=qpc(); // last actual sample above has now been converted/copied
     if(h.blockReadyQpc<m.ready_qpc)return InvalidRecord;
     if(!emit(pending_))return QueueOverflow;
     ++sequence_;filled_=0;
    }
   }
  }
  nextFrame_=m.first_frame+m.frames;lastUrb_=m.sequence;lastReceive_=m.receive_qpc;return NoError;
 }
};
} // namespace vibe_bridge

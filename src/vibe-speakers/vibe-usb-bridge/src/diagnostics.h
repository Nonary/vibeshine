// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "windows_support.h"
#include <fstream>
#include <thread>

namespace vibe_bridge {
// Independent original-native record format. It is deliberately NOT .atmoscap.
// All little endian; header then packetCount*(offset,length) then ORIGINAL USB
// buffer (including any gaps between packets). Packed PCM is never fabricated.
#pragma pack(push,1)
struct RawHeader {
 std::uint64_t magic=0x3157415253554256ull; // bytes VBUSRAW1
 std::uint32_t version=1,headerBytes=sizeof(RawHeader);
 std::uint32_t sequence=0,requestedStartFrame=0,epoch=0,packetCount=0;
 std::uint64_t firstFrame=0,frames=0,receiveQpc=0,validatedQpc=0,qpcFrequency=0;
 std::uint32_t nativeBytes=0,channels=0,channelMask=0,payloadBytes=0;
 std::uint16_t containerBits=0,validBits=0;
};
#pragma pack(pop)
static_assert(sizeof(RawHeader)==92);
struct NativeDiagnostic {
 RawHeader header;
 std::array<usb::iso_packet,usb::max_iso_packets> packets;
 std::array<std::uint8_t,usb::max_transfer> payload;
};
struct BlockDiagnostic { AudioHeader header;std::uint64_t writeCompleteQpc=0; };
inline Handle CreateOutput(const std::wstring& path){
 Handle h(CreateFileW(path.c_str(),GENERIC_WRITE,FILE_SHARE_READ,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr));
 WinCheck(h.Valid(),InternalFailure,"Create new evidence file");return h;
}
inline void WriteBytes(HANDLE file,const void* bytes,std::uint32_t size){
 DWORD written=0;WinCheck(WriteFile(file,bytes,size,&written,nullptr)&&written==size,InternalFailure,"Write diagnostic evidence");
}
inline void WriteText(HANDLE file,const std::string& text){WriteBytes(file,text.data(),static_cast<std::uint32_t>(text.size()));}

class Diagnostics {
 std::unique_ptr<Spsc<NativeDiagnostic,32>> native_{std::make_unique<Spsc<NativeDiagnostic,32>>()};
 Spsc<BlockDiagnostic,256> blocks_;
 Handle wake_{CreateEventW(nullptr,FALSE,FALSE,nullptr)},stop_{CreateEventW(nullptr,TRUE,FALSE,nullptr)};
 Handle raw_,nativeLog_,blockLog_;
 std::thread writer_;
 std::atomic<bool> failed_{false};
 std::atomic<std::uint64_t> nativeRejected_{0},blocksRejected_{0};
 std::uint64_t nativeWritten_=0,blocksWritten_=0;
 Profile profile_{};
 bool rawEnabled_=false,detailed_=true;
#ifdef VIBE_BRIDGE_TESTING
 std::function<void()> emptyBeforeCompletion_;
#endif
 void Run() noexcept {
  try{
   auto native=std::make_unique<NativeDiagnostic>();BlockDiagnostic block;
   for(;;){
    const bool stopping=WaitForSingleObject(stop_.Get(),0)==WAIT_OBJECT_0;
    bool progress=false;
    while(native_->Pop(*native)){
     progress=true;const auto& h=native->header;
     WriteBytes(raw_.Get(),&h,sizeof(h));
     WriteBytes(raw_.Get(),native->packets.data(),h.packetCount*sizeof(usb::iso_packet));
     WriteBytes(raw_.Get(),native->payload.data(),h.payloadBytes);
     WriteText(nativeLog_.Get(),"{\"event\":\"native\",\"sequence\":"+std::to_string(h.sequence)+",\"epoch\":"+std::to_string(h.epoch)+
      ",\"first_frame\":"+std::to_string(h.firstFrame)+",\"frames\":"+std::to_string(h.frames)+",\"receive_qpc\":"+std::to_string(h.receiveQpc)+
      ",\"validated_qpc\":"+std::to_string(h.validatedQpc)+",\"qpc_frequency\":"+std::to_string(h.qpcFrequency)+",\"native_bytes\":"+std::to_string(h.nativeBytes)+
      ",\"channels\":"+std::to_string(h.channels)+",\"channel_mask\":"+std::to_string(h.channelMask)+",\"container_bits\":"+std::to_string(h.containerBits)+
      ",\"valid_bits\":"+std::to_string(h.validBits)+",\"packet_count\":"+std::to_string(h.packetCount)+",\"payload_bytes\":"+std::to_string(h.payloadBytes)+"}\n");++nativeWritten_;
    }
    while(blocks_.Pop(block)){
     progress=true;const auto& h=block.header;
     WriteText(blockLog_.Get(),"{\"event\":\"block_written\",\"nonce\":"+Json(NonceText(h.header.nonce))+",\"sequence\":"+std::to_string(h.blockSequence)+
      ",\"epoch\":"+std::to_string(h.epoch)+",\"first_frame\":"+std::to_string(h.firstFrame)+",\"first_urb_sequence\":"+std::to_string(h.firstUrbSequence)+
      ",\"last_urb_sequence\":"+std::to_string(h.lastUrbSequence)+",\"first_urb_frame_offset\":"+std::to_string(h.firstUrbFrameOffset)+
      ",\"last_urb_frame_offset\":"+std::to_string(h.lastUrbFrameOffset)+",\"first_receive_qpc\":"+std::to_string(h.firstReceiveQpc)+
      ",\"last_receive_qpc\":"+std::to_string(h.lastReceiveQpc)+",\"block_ready_qpc\":"+std::to_string(h.blockReadyQpc)+",\"enqueue_qpc\":"+std::to_string(h.enqueueQpc)+
      ",\"send_qpc\":"+std::to_string(h.sendQpc)+",\"write_complete_qpc\":"+std::to_string(block.writeCompleteQpc)+",\"qpc_frequency\":"+std::to_string(h.qpcFrequency)+
      ",\"queue_depth_blocks\":"+std::to_string(h.queueDepthBlocks)+",\"queue_capacity_blocks\":"+std::to_string(h.queueCapacityBlocks)+"}\n");++blocksWritten_;
    }
#ifdef VIBE_BRIDGE_TESTING
    if(!progress&&emptyBeforeCompletion_)emptyBeforeCompletion_();
#endif
    if(stopping&&!progress)break;
    if(!progress){HANDLE handles[]={stop_.Get(),wake_.Get()};WaitForMultipleObjects(2,handles,FALSE,INFINITE);}
   }
  }catch(...){failed_.store(true,std::memory_order_release);}
 }
public:
 Diagnostics(const std::wstring& prefix,Profile p,bool raw,bool detailed
#ifdef VIBE_BRIDGE_TESTING
 ,std::function<void()> emptyBeforeCompletion={}
#endif
 ):profile_(p),rawEnabled_(raw),detailed_(detailed){
#ifdef VIBE_BRIDGE_TESTING
  emptyBeforeCompletion_=std::move(emptyBeforeCompletion);
#endif
  WinCheck(wake_.Valid()&&stop_.Valid(),InternalFailure,"Create diagnostics events");
  if(raw){raw_=CreateOutput(prefix+L".vusbraw");nativeLog_=CreateOutput(prefix+L".native.jsonl");}
  if(detailed)blockLog_=CreateOutput(prefix+L".blocks.jsonl");
  writer_=std::thread([this]{Run();});
 }
 ~Diagnostics(){Stop();}
 bool Native(const usb::pcm_metadata& m,std::span<const usb::iso_packet> packets,std::span<const std::uint8_t> bytes)noexcept{
  if(!rawEnabled_)return !Failed();
  if(Failed()||packets.size()>usb::max_iso_packets||bytes.size()>usb::max_transfer){++nativeRejected_;return false;}
  const bool result=native_->Push([&](NativeDiagnostic& d,std::uint32_t){
   d.header={};auto& h=d.header;h.sequence=m.sequence;h.requestedStartFrame=m.start_frame;h.epoch=m.epoch;h.packetCount=m.packet_count;
   h.firstFrame=m.first_frame;h.frames=m.frames;h.receiveQpc=m.receive_qpc;h.validatedQpc=m.ready_qpc;h.qpcFrequency=m.qpc_frequency;
   h.nativeBytes=m.native_bytes;h.channels=m.channels;h.channelMask=m.channel_mask;h.payloadBytes=static_cast<std::uint32_t>(bytes.size());
   h.containerBits=profile_.nativeContainerBits;h.validBits=profile_.nativeValidBits;
   std::memcpy(d.packets.data(),packets.data(),packets.size_bytes());std::memcpy(d.payload.data(),bytes.data(),bytes.size());
  });
  if(!result)++nativeRejected_;else SetEvent(wake_.Get());return result;
 }
 bool Block(const AudioHeader& h,std::uint64_t complete)noexcept{
  if(!detailed_)return !Failed();
  const bool result=!Failed()&&blocks_.Push([&](BlockDiagnostic& d,std::uint32_t){d={h,complete};});
  if(!result)++blocksRejected_;else SetEvent(wake_.Get());return result;
 }
 bool Failed()const noexcept{return failed_.load(std::memory_order_acquire);}
#ifdef VIBE_BRIDGE_TESTING
 void RequestStopForTest()noexcept{SetEvent(stop_.Get());}
#endif
 void Stop()noexcept{SetEvent(stop_.Get());if(writer_.joinable())writer_.join();}
 std::uint64_t NativeRejected()const noexcept{return nativeRejected_.load();}
 std::uint64_t BlocksRejected()const noexcept{return blocksRejected_.load();}
 std::uint64_t NativeWrittenAfterStop()const noexcept{return nativeWritten_;}
 std::uint64_t BlocksWrittenAfterStop()const noexcept{return blocksWritten_;}
};
} // namespace vibe_bridge

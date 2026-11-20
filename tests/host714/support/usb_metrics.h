#pragma once
// Test-build-only instrumentation; never compiled into a normal installed host.
// Reuse the tested bounded, pre-touched history. No I/O or allocations per block.
#if defined(_WIN32) && defined(VIBE_AUDIO_TEST_INSTRUMENTATION)
#include <filesystem>
#include <fstream>
#include "usb_preopus_signature.h"
namespace audio {
class usb_lab_metrics {
  std::wstring prefix_;
  std::unique_ptr<usb_signature::history> history_;
  std::uint64_t rows_=0,section_=0;
public:
  static std::uint64_t now(){LARGE_INTEGER q{};QueryPerformanceCounter(&q);return q.QuadPart;}
  explicit usb_lab_metrics(const atmos_format_t& format) noexcept {
   try {
    std::array<wchar_t,32768> path{};
    const auto n=GetEnvironmentVariableW(L"VIBE_USB_LAB_METRICS_PREFIX",path.data(),DWORD(path.size()));
    if(!n)return;
    if(n>=path.size())throw std::runtime_error("Laboratory metrics path too long");
    prefix_=path.data();
    if(!std::filesystem::path(prefix_).is_absolute()||std::filesystem::exists(prefix_+L".stages.csv")||std::filesystem::exists(prefix_+L".preopus-signatures.bin"))throw std::runtime_error("Metrics require a fresh absolute prefix");
    LARGE_INTEGER hz{};QueryPerformanceFrequency(&hz);
    const vibe_input::Profile profile{48000,static_cast<std::uint16_t>(format.channels),240,format.channel_mask,format.container_bits,format.valid_bits};
    history_=std::make_unique<usb_signature::history>(profile,400000,hz.QuadPart);
   } catch(...) { history_.reset();prefix_.clear(); }
  }
  ~usb_lab_metrics(){if(history_)try{std::ofstream csv(std::filesystem::path(prefix_+L".stages.csv"));csv<<"session,output_block,bridge_sequence,epoch,first_frame,first_urb_sequence,last_urb_sequence,first_urb_frame_offset,last_urb_frame_offset,qpc_frequency,first_receive_qpc,last_receive_qpc,block_ready_qpc,enqueue_qpc,send_qpc,host_receive_qpc,encoder_begin_qpc,encoder_end_qpc,transport_end_qpc,bridge_queue_depth,bridge_queue_capacity,diagnostic_queue_depth,diagnostic_dequeue_qpc\n";history_->save(prefix_+L".preopus-signatures.bin",csv);}catch(const std::exception& e){BOOST_LOG(error)<<"Laboratory audio metrics save failed: "<<e.what();}}
  bool enabled()const{return bool(history_);}
  void next_section(){++section_;}
  void record(const vibe_input::AudioHeader& m,std::uint64_t receive,std::uint64_t begin,std::uint64_t end,std::uint64_t queued,std::span<const float> pcm){
    if(!history_)return;
    history_->record(m,{section_,rows_,m.blockSequence,m.epoch,m.firstFrame,m.firstUrbSequence,m.lastUrbSequence,m.firstUrbFrameOffset,m.lastUrbFrameOffset,m.qpcFrequency,m.firstReceiveQpc,m.lastReceiveQpc,m.blockReadyQpc,m.enqueueQpc,m.sendQpc,receive,begin,end,queued,m.queueDepthBlocks,m.queueCapacityBlocks,0,queued},pcm);++rows_;
  }
};
}
#endif

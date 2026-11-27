// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <algorithm>
#include <array>
#include <cstdint>
#include <string>

namespace vibe::usb_audio {
struct clock_observation {
  std::uint64_t id=0,first_frame=0,receive_qpc=0,return_queued_qpc=0,first_send_qpc=0,complete_send_qpc=0;
  std::uint64_t tx_begin=0,tx_end=0,send_batch=0;
  std::int64_t receive_ns=0,prior_cursor_ns=0,start_ns=0,due_ns=0,poll_ns=0,complete_send_ns=0,admission_gap_ns=0;
  std::uint32_t epoch=0,sequence=0,endpoint=0,flags=0,interval=0,requested_start=0,returned_start=0,packets=0;
  std::uint32_t minimum_bytes=0,maximum_bytes=0,actual_bytes=0,anchor=0,pending_at_admission=0,feedback_16_16=0;
  std::int32_t status=0;
};
struct control_transition {std::uint32_t old_epoch=0,new_epoch=0,sequence=0,reason=0;std::uint64_t qpc=0;};

// Fixed storage, one USB worker. No locks, formatting or I/O in the clock path.
// Marker capacity bounds diagnostics only. Overflow is counted explicitly.
class clock_flight {
  static constexpr std::size_t marker_capacity=4096;
  struct tx_marker {std::uint64_t id=0,begin=0,end=0;};
  std::array<tx_marker,marker_capacity> markers_{};
  std::uint64_t marker_read_=0,marker_write_=0;
  std::array<std::int64_t,2> last_receive_{};
  template<class Edit> void update(std::uint64_t id,Edit edit) noexcept {
    if(!id)return;
    auto& row=tail[(id-1)%tail.size()];
    if(row.id!=id){++expired_updates;return;}
    edit(row);
    if(id<=prefix.size())prefix[id-1]=row;
  }
public:
  std::array<clock_observation,32> prefix{};
  std::array<clock_observation,4096> tail{};
  std::array<clock_observation,128> notable{};
  std::array<control_transition,128> resets{};
  std::array<std::uint64_t,2> admissions{},late_polls{},late_reanchors{},socket_completed{};
  std::array<std::int64_t,2> max_admission_gap_ns{},max_poll_late_ns{},max_send_late_ns{};
  std::uint64_t observations=0,notable_count=0,reset_count=0,expired_updates=0,marker_overflow=0,send_batches=0;
  std::uint64_t epoch_qpc=0,qpc_frequency=0;bool spaced_expired_out=false,synchronous_out=false;
  bool boot_frame_origin=false;
  std::uint64_t boot_epoch_100ns=0,boot_calibration_bracket_ns=0;
  std::uint64_t admitted(clock_observation row) noexcept {
    row.id=++observations;const unsigned ep=row.endpoint==1?0u:1u;
    if(admissions[ep]){row.admission_gap_ns=row.receive_ns-last_receive_[ep];max_admission_gap_ns[ep]=std::max(max_admission_gap_ns[ep],row.admission_gap_ns);}
    last_receive_[ep]=row.receive_ns;++admissions[ep];if(row.anchor==2)++late_reanchors[ep];
    tail[(row.id-1)%tail.size()]=row;if(row.id<=prefix.size())prefix[row.id-1]=row;
    return row.id;
  }
  void polled(std::uint64_t id,std::int64_t ns) noexcept {
    update(id,[&](auto& row){row.poll_ns=ns;const unsigned ep=row.endpoint==1?0u:1u;const auto late=ns-row.due_ns;
      if(late>2000000)++late_polls[ep];max_poll_late_ns[ep]=std::max(max_poll_late_ns[ep],late);});
  }
  void returned(std::uint64_t id,std::int32_t status,std::uint64_t qpc,std::uint64_t begin,std::uint64_t end) noexcept {
    if(!id)return;
    update(id,[&](auto& row){row.status=status;row.return_queued_qpc=qpc;row.tx_begin=begin;row.tx_end=end;});
    if(marker_write_-marker_read_==marker_capacity){++marker_overflow;return;}
    markers_[marker_write_++%marker_capacity]={id,begin,end};
  }
  void socket_accepted(std::uint64_t total,std::uint64_t qpc,std::int64_t ns) noexcept {
    ++send_batches;
    while(marker_read_!=marker_write_){
      const auto marker=markers_[marker_read_%marker_capacity];if(total<=marker.begin)break;
      update(marker.id,[&](auto& row){if(!row.first_send_qpc)row.first_send_qpc=qpc;});
      if(total<marker.end)break;
      update(marker.id,[&](auto& row){row.complete_send_qpc=qpc;row.complete_send_ns=ns;row.send_batch=send_batches;
        const unsigned ep=row.endpoint==1?0u:1u;++socket_completed[ep];const auto late=ns-row.due_ns;
        if(row.status==0)max_send_late_ns[ep]=std::max(max_send_late_ns[ep],late);
        if(row.anchor==2||row.admission_gap_ns>=20000000||row.poll_ns-row.due_ns>=5000000||late>=5000000)
          notable[notable_count++%notable.size()]=row;});
      ++marker_read_;
    }
  }
  void reset(control_transition event) noexcept {resets[reset_count++%resets.size()]=event;}
  std::uint64_t pending_send_markers()const noexcept{return marker_write_-marker_read_;}
};
void write_clock_evidence(const std::wstring& prefix,const clock_flight& clock);
}

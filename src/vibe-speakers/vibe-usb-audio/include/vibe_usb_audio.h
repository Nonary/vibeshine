// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "vibe_profile_ids.h"
#include "clock_flight.h"
#include <array>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace vibe::usb_audio {
struct profile {
  std::uint8_t channels = 12;
  std::uint32_t channel_mask = 0x2d63f;
  std::uint16_t native_container_bits = 32;
  std::uint16_t native_valid_bits = 24;
  static profile from_channels(unsigned channels);
  static profile from_channels(unsigned channels, std::uint32_t mask, std::uint16_t container_bits=32, std::uint16_t valid_bits=24);
  bool valid() const noexcept;
  std::uint16_t product_id() const noexcept { return VibeLabUsbProduct(channels,channel_mask,native_container_bits,native_valid_bits); }
  std::uint32_t frame_bytes() const noexcept { return channels * (native_container_bits/8u); }
};
// pid.codes/1209/0001..0009 and 000E: private in-house testing only. NOT an
// assigned Vibe Speakers identity and NEVER for distribution or production.
inline constexpr std::uint16_t lab_vendor = 0x1209;
inline constexpr std::uint32_t sample_rate = 48000;
inline constexpr std::size_t max_transfer = 256 * 1024;
inline constexpr std::size_t max_iso_packets = 1024;
inline constexpr std::size_t max_pending = 64;
// USB USBDI QueryBusTime is a wrapping DWORD of milliseconds. Keep the
// fractional boot millisecond until both terms have been added.
constexpr std::uint32_t usb_start_frame(std::uint64_t epoch_100ns,std::uint64_t elapsed_ns) noexcept {
  return static_cast<std::uint32_t>((epoch_100ns+elapsed_ns/100u)/10000u);
}
struct identity { std::string busid; std::string serial; profile format; };
struct iso_packet { std::uint32_t offset, length; };
struct pcm_metadata {
  std::uint32_t sequence, start_frame, epoch, packet_count;
  std::uint64_t first_frame, frames, receive_qpc, ready_qpc, qpc_frequency;
  std::uint32_t native_bytes, channels, channel_mask;
  std::uint16_t native_container_bits, native_valid_bits;
};
struct urb_trace {
  std::uint32_t phase,sequence,endpoint,direction,flags,requested_start_frame,returned_start_frame,wire_interval;
  std::uint32_t epoch,packet_count,transfer_bytes,packet_min,packet_max,packet_total;
  std::uint64_t qpc,receive_qpc,qpc_frequency;
  std::uint32_t scheduled_duration_us;
};
struct callbacks {
  // Borrowed until return. Bytes and packet offsets are exactly USB/IP OUT
  // payload, no scaling, float conversion, reordering or padding fabrication.
  // Called exactly once at full validated OUT admission, BEFORE paced USB
  // completion. Later UNLINK cancels the ACK, not the already received data;
  // its captured frame count is exposed separately. MUST not block.
  std::function<bool(const pcm_metadata&, std::span<const iso_packet>, std::span<const std::uint8_t>)> pcm;
  std::function<std::uint64_t()> qpc;
  std::uint64_t qpc_frequency = 0;
  std::function<void(const std::string&)> diagnostic;
  // Bounded first16 OUT/feedback URBs per epoch. No formatting on USBworker.
  std::function<void(const urb_trace&)> trace;
  // Same USB worker, after epoch changes, before another PCM admission.
  // Only a direct valid AS SET_INTERFACE(1,0) is a terminal normal stop.
  // Config0, port reset and AS1 are discontinuities. Must not block/throw.
  std::function<void(std::uint32_t epoch,bool terminal_as0)> stream_reset;
  bool record_clock=false;
  bool space_expired_out=false; // explicit diagnostics-only comparative policy
  bool synchronous_out=false; // isolated USB SOF-locked descriptor/clock experiment
  std::function<std::uint64_t()> boot_interrupt_time_100ns; // D only; sampled at startup
};
struct counters {
  std::uint64_t submits = 0, completed = 0, canceled = 0, stalls = 0;
  std::uint64_t audio_urbs = 0, audio_frames = 0, feedback_urbs = 0;
  std::uint64_t low_byte_nonzero = 0, callback_failures = 0, late_urbs = 0;
  std::uint64_t protocol_failures = 0, pending_on_close = 0, audio_frames_discarded_on_close = 0;
  std::uint64_t audio_frames_unlinked_after_capture = 0;
  std::size_t pending_high_water = 0;
  std::uint32_t worker_thread_id=0, mmcss_task_index=0, mmcss_register_error=0, mmcss_revert_error=0;
  bool mmcss_registered=false, mmcss_reverted=false, worker_failed=false;
};

// Portable, single-worker USB/IP/UAC2 protocol. Socket ingress is bounded;
// each whole URB receives its original admission QPC before async diagnostics.
class session {
public:
  explicit session(identity id, callbacks cb = {});
  bool feed(std::span<const std::uint8_t> bytes, std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now());
  void poll(std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now());
  std::vector<std::uint8_t> take_output();
  std::span<const std::uint8_t> output() const noexcept { return transmit_; }
  void consume_output(std::size_t bytes,std::chrono::steady_clock::time_point now=std::chrono::steady_clock::now());
  std::chrono::steady_clock::time_point next_deadline() const noexcept;
  bool imported() const noexcept { return imported_; }
  bool failed() const noexcept { return failed_; }
  bool finished() const noexcept { return finished_; }
  const counters& stats() const noexcept { return stats_; }
  const clock_flight* clock_trace()const noexcept{return clock_.get();}
  void close(); // after worker join only; report, never fabricate pending audio
  std::vector<std::uint8_t> configuration_descriptor() const;
  std::vector<std::uint8_t> device_descriptor() const;
private:
  struct urb {
    std::uint32_t sequence=0, direction=0, endpoint=0, length=0, start_frame=0, packet_count=0;
    std::array<std::uint8_t,8> setup{};
    std::array<iso_packet,max_iso_packets> packets{};
    std::array<std::uint8_t,max_transfer> data{};
    bool used=false, captured=false, traced=false;
    std::uint32_t flags=0,wire_interval=0,requested_start_frame=0;
    std::uint64_t receive_qpc=0;
    std::uint64_t clock_id=0;
    std::chrono::steady_clock::time_point due{};
  };
  bool parse_one();
  void submit(urb* request);
  void control(const urb& request);
  void reply(const urb& request, std::int32_t status, std::span<const std::uint8_t> data={}, std::uint32_t out_length=0);
  void reset_stream(bool terminal_as0=false,std::uint32_t sequence=0,std::uint32_t reason=0);
  void fail(const char* reason);
  void header(std::uint16_t operation, std::uint32_t status=0);
  void device_record();
  void diagnostic(const std::string& message);
  std::uint64_t qpc() const;
  std::chrono::steady_clock::time_point effective_deadline(const urb&)const noexcept;
  std::int64_t bus_ns(std::chrono::steady_clock::time_point value)const noexcept;
  void trace(const urb& request,unsigned phase) const;
  static std::span<const iso_packet> packets(const urb& u) noexcept { return {u.packets.data(),u.packet_count==0xffffffffu?0:u.packet_count}; }
  static std::span<const std::uint8_t> payload(const urb& u) noexcept { return {u.data.data(),u.direction?0:u.length}; }
  identity identity_;
  callbacks callbacks_;
  counters stats_;
  std::unique_ptr<clock_flight> clock_;
  std::uint64_t socket_bytes_=0,last_audio_return_end_=0;
  std::uint64_t boot_epoch_100ns_=0;
  std::chrono::steady_clock::time_point last_audio_send_{};
  bool imported_=false, failed_=false, finished_=false;
  std::uint8_t configuration_=0, alternate_=0;
  bool alternate_explicit_=false;
  std::uint32_t epoch_=1;
  std::uint64_t frame_position_=0;
  std::vector<std::uint8_t> receive_, transmit_;
  std::unique_ptr<urb[]> pool_;
  std::vector<urb*> pending_;
  std::chrono::steady_clock::time_point next_audio_{}, next_feedback_{}, receive_time_{};
  std::chrono::steady_clock::time_point bus_epoch_{std::chrono::steady_clock::now()};
  unsigned audio_trace_count_=0,feedback_trace_count_=0;
};

struct binding {
  std::string busid, serial, service;
  std::uint16_t vendor=0, product=0; // set only after exact imported tuple readback
  int port=0;
  std::uint32_t worker_thread_id=0, mmcss_task_index=0;
  bool mmcss_registered=false;
  bool worker_pro_audio=false;
  std::wstring host_instance, usb_instance, endpoint_id;
  std::uint32_t mix_channels=0, mix_rate=0, mix_bits=0, mix_mask=0;
  std::int32_t mix_hresult=1; // S_FALSE until IAudioClient format read is attempted.
};
bool available(const std::wstring& controller_instance); // exact controller, read-only released VHCI ABI probe
class controller {
public:
  ~controller();
  controller(const controller&)=delete;
  controller& operator=(const controller&)=delete;
  bool connected() const noexcept;
  binding inspect(); // exact owned USB port ancestry + endpoint container
  counters stop(); // exact owned detach, join, return final stable counters
  const clock_flight* clock_after_stop()const noexcept;
private:
  struct impl;
  explicit controller(std::shared_ptr<impl> state);
  std::shared_ptr<impl> state_;
  friend std::unique_ptr<controller> create(profile, callbacks, const std::wstring&, bool);
};
// Managed authorization is supplied only after retained owner-process validation.
std::unique_ptr<controller> create(profile format, callbacks cb, const std::wstring& controller_instance, bool verified_managed_owner=false);
} // namespace vibe::usb_audio

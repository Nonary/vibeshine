#pragma once
#include "audio_policy.h"
#include "audio_atmos_format.h"
#include "vibe_input_protocol.h"
#include <memory>
#include <functional>
#include <span>
#include <string>
namespace audio {
enum class usb_end_reason_e { none, terminal_as0, controlled_stop, confirmed_queue_overflow };
struct usb_source_config_t {
 std::string pipe {R"(\\.\pipe\VibeSpeakersUsb)"};
 std::string bridge_executable,bridge_sha256,endpoint,usb_instance,controller_instance,serial,service;
 unsigned port=0;
 atmos_format_t format {};
 std::string sample_format {"pcm24"};
};
struct usb_source_statistics_t {
 std::uint64_t blocks=0,read_timeouts=0,protocol_errors=0,stale_blocks=0,disconnects=0;
 double maximum_age_ms=0;
};
class usb_source_t {
 public:
 explicit usb_source_t(usb_source_config_t);
 ~usb_source_t();
 usb_source_t(const usb_source_t&)=delete;
 usb_source_t& operator=(const usb_source_t&)=delete;
 bool connect();
 // Fresh client only; permit a successor solely after an authenticated,
 // loss-free terminal AS0 from the same still-owned USB producer.
 bool connect_after_as0(const usb_source_t &ended, const std::function<bool()> &stopping);
 bool connect_after_overflow(const usb_source_t &ended, const std::function<bool()> &stopping);
 std::uint64_t lost_native_frames() const;
 usb_end_reason_e end_reason() const;
 policy::sample_status_e sample(std::span<float>);
 void close();
 const std::string& error() const;
 const usb_source_statistics_t& statistics() const;
 const vibe_input::AudioHeader& metadata() const;
 const vibe_input::WelcomeMessage& session() const;
 std::uint64_t receive_qpc() const;
 static bool available(const usb_source_config_t&,std::string&);
 // Initialization-only public API/USBIP query; no default/format/audio mutation.
 static bool inspect_binding(const usb_source_config_t&,std::string&);
 private:
 bool connect_successor(const usb_source_t&,const std::function<bool()>&,usb_end_reason_e);
 struct impl_t; std::unique_ptr<impl_t> impl_;
};
}

#pragma once
#include "audio_usb_source.h"
#include <functional>
#include <memory>
#include <string>

namespace audio {
struct usb_launch_config_t {
  std::string bridge_executable, bridge_sha256, controller_instance, evidence_root;
  unsigned seconds=14400;
  bool detailed=false, raw=false;
  // Only the explicit laboratory harness supplies this pair.
  unsigned lab_stall_writer_after_blocks=0, lab_stall_writer_ms=0;
  std::string setup_error;
};
// Initialization/teardown only. The caller closes usb_source_t before stop().
// No audio read, format setter, driver installation or forced process kill.
class usb_session_t {
public:
  explicit usb_session_t(usb_launch_config_t);
  ~usb_session_t();
  usb_session_t(const usb_session_t&)=delete;
  usb_session_t& operator=(const usb_session_t&)=delete;
  static bool configured(const usb_launch_config_t&,std::string& error);
  static usb_launch_config_t bundled_configuration(usb_launch_config_t);
  bool start(atmos_format_t,const std::function<bool()>& cancelled={});
  bool stop() noexcept;
  const usb_source_config_t& source() const;
  const std::string& error() const;
  std::string evidence_json() const;
private:
  struct impl_t;
  std::unique_ptr<impl_t> impl_;
};
// Pure guards/serialization regression. No COM, endpoint or child process.
void test_usb_session_guards();
}

#pragma once
#include "audio_atmos_format.h"
#include <memory>
#include <functional>
#include <string>
#include <vector>
#include <array>
namespace platf { class audio_control_t; }

namespace audio {
  enum class atmos_mix_argument_e { target, zero, null_pointer };
  struct atmos_device_request_t {
    std::string endpoint, device_instance;
    atmos_format_t format;
    atmos_mix_argument_e diagnostic_mix_argument=atmos_mix_argument_e::target;
  };
  struct atmos_device_state_t {
    std::vector<unsigned char> device_format, mix_format;
    bool operator==(const atmos_device_state_t &) const = default;
  };
  struct atmos_format_notification_t {
    std::uint64_t qpc=0;
    std::uint32_t kind=0, property_id=0;
    std::array<unsigned char,16> property_set{};
  };
  struct atmos_format_settle_t {
    bool attempted=false, target_matched=false, timed_out=false, observation_limit=false;
    std::uint32_t maximum_wait_ms=0, reads=0, wakes=0, notifications_pending=0;
    std::uint64_t qpc_frequency=0, set_begin_qpc=0, set_end_qpc=0, initial_read_qpc=0, final_read_qpc=0, notification_count=0;
    atmos_device_state_t final_readback;
    std::vector<unsigned char> final_public_mix;
    std::vector<atmos_format_notification_t> notifications;
  };
  struct atmos_setter_diagnostic_t {
    bool called=false;
    atmos_mix_argument_e mix_argument=atmos_mix_argument_e::target;
    std::uint32_t hresult=0;
    std::uint64_t begin_qpc=0,end_qpc=0;
    std::vector<unsigned char> device_before,device_after,mix_before,mix_after;
  };
  // Initialization/teardown only. Backend is shared by the host and real-device
  // probe; fake backends exercise rollback without touching Windows devices.
  class atmos_device_backend_t {
  public:
    virtual ~atmos_device_backend_t() = default;
    virtual void verify() = 0;
    virtual atmos_device_state_t read() = 0;
    // Diagnostic only: an independent public-client view, never a replacement
    // for the exact policy snapshot used by the ownership/restore contract.
    virtual std::vector<unsigned char> read_public_mix() { return {}; }
    virtual atmos_format_settle_t settle(const atmos_device_state_t &, const atmos_device_state_t &, const std::vector<unsigned char> &) { return {}; }
    virtual atmos_setter_diagnostic_t last_setter_diagnostic() { return {}; }
    virtual atmos_device_state_t target(const atmos_format_t &) = 0;
    virtual void set(const atmos_device_state_t &) = 0;
  };
  enum class atmos_restore_e { untouched, restored, conflict, failed };
  struct atmos_device_report_t {
    bool applied = false, active = false, apply_readback_valid = false;
    atmos_restore_e restore = atmos_restore_e::untouched;
    // Preserve the exact first post-set read independently of rollback/final
    // readback. A failed apply must not erase the evidence that rejected it.
    atmos_device_state_t before, installed, apply_readback, after;
    std::vector<unsigned char> apply_public_mix_readback;
    std::string apply_public_mix_readback_error;
    atmos_format_settle_t apply_settle, restore_settle;
    atmos_setter_diagnostic_t apply_setter,restore_setter;
    std::string error;
  };
  class atmos_device_lease_t {
  public:
    explicit atmos_device_lease_t(std::unique_ptr<atmos_device_backend_t>);
    ~atmos_device_lease_t();
    atmos_device_lease_t(const atmos_device_lease_t &) = delete;
    atmos_device_lease_t &operator=(const atmos_device_lease_t &) = delete;
    bool inspect();
    bool apply(const atmos_format_t &);
    bool restore() noexcept;
    const atmos_device_report_t &report() const { return report_; }
  private:
    std::unique_ptr<atmos_device_backend_t> backend_;
    atmos_device_report_t report_;
    void set_recorded(const atmos_device_state_t &,atmos_setter_diagnostic_t &);
  };
  struct usb_source_config_t;
  std::unique_ptr<atmos_device_backend_t> make_owned_usb_format_backend(const usb_source_config_t&);
  std::unique_ptr<atmos_device_backend_t> make_affine_atmos_backend(std::function<std::unique_ptr<atmos_device_backend_t>()>);
  void test_atmos_format_notifications(); // Local callback/event unit test; no endpoint/COM registration.
}

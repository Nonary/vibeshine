#pragma once

#include "terminal_session_hdr_policy.h"
#include "terminal_session_runtime.h"

#include <memory>
#include <string_view>

#ifdef _WIN32
namespace platf::dxgi { class INamedPipe; }
#endif

namespace terminal_session::worker {
  // The worker process and its primary thread are created with this
  // descriptor. The broker retains its own returned handles; the seat user
  // needs no external real handle to the worker because the worker uses its
  // pseudo-handle for self-inspection and self-management.
  inline constexpr std::wstring_view worker_process_security_sddl = L"O:SYG:SYD:P(A;;GA;;;SY)";

  /** Real process boundary used by the service; provider resources remain injectable. */
  class process_t final: public seat_worker_t {
  public:
    process_t() = default;
    ~process_t() override;
    std::optional<route_t> start(const worker_request_t &, std::string &error) override;
    std::optional<route_t> resume(const worker_request_t &, std::string &error) override;
    bool park(const route_t &) noexcept override;
    bool stop(const route_t &) noexcept override;
    bool cleanup_needed() const noexcept override;
  private:
#ifdef _WIN32
    void *process_ {};
    void *job_ {};
    std::string pipe_name_;
    std::unique_ptr<platf::dxgi::INamedPipe> pipe_;
    provider_resource_t resource_;
    // The service cannot query the worker's WTS display topology itself. Bind
    // the first exact sole-target attestation and require it for this worker
    // until full stop.
    std::optional<terminal_session::hdr::target_binding_t> hdr_target_binding_;
#endif
    std::uint32_t pid_ {};
  };
}

#pragma once

#include "terminal_session_hdr_policy.h"
#include "terminal_session_runtime.h"

#include <memory>
#include <array>
#include <atomic>
#include <cstdint>
#include <optional>
#include <thread>
#include <string_view>

#include "steam_offline_manager.h"

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
    void run_display_broker();
    bool validate_display_client(platf::dxgi::INamedPipe &pipe) const;
    void *process_ {};
    void *job_ {};
    std::string pipe_name_;
    std::string display_pipe_name_;
    std::unique_ptr<platf::dxgi::INamedPipe> pipe_;
    provider_resource_t resource_;
    std::uint64_t generation_ {};
    std::atomic_bool display_broker_running_ {false};
    std::jthread display_broker_thread_;
    // The service cannot query the worker's WTS display topology itself. Bind
    // the first exact sole-target attestation and require it for this worker
    // until full stop.
    std::optional<terminal_session::hdr::target_binding_t> hdr_target_binding_;
    struct snapshot_record {
      std::uint64_t sequence {};
      std::uint64_t display_id {};
      std::array<std::uint8_t, 32> digest {};
      std::array<std::uint8_t, 32> tag {};
    };
    struct snapshot_tier_state {
      std::uint64_t next_sequence {1};
      std::optional<snapshot_record> pending;
      std::optional<snapshot_record> committed;
    };
    // Never sent over the helper pipe: only this SYSTEM broker can seal or
    // verify generation-bound snapshot digests.
    std::array<std::uint8_t, 32> snapshot_auth_key_ {};
    std::array<snapshot_tier_state, 3> snapshot_tiers_ {};
    steam_offline::manager_t steam_offline_manager_;
    steam_offline::preparation_t steam_offline_preparation_;
    bool steam_offline_isolation_ {};
    std::atomic_bool cleanup_pending_ {false};
    std::atomic<bool> steam_offline_monitor_stop_ {false};
    std::atomic<bool> steam_offline_poisoned_ {false};
    std::thread steam_offline_monitor_;
    std::uint64_t worker_generation_ {};
    std::uint64_t worker_creation_time_ {};
#endif
    std::uint32_t pid_ {};
  };
}

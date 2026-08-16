/**
 * @file src/terminal_session_runtime.h
 * @brief Transactional, provider-neutral terminal seat orchestration.
 */
#pragma once

#include "terminal_session_broker.h"
#include "terminal_session_protocol.h"

#include <memory>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>

namespace terminal_session {
  enum class seat_state_e : std::uint8_t { idle, preparing, ready, running, retained, stopping, failed };

  struct provider_capability_t {
    bool supported {};
    bool concurrent_sessions {};
    bool remote_display {};
    bool audio_endpoint {};
    bool token_launch {};
    std::string error;
  };

  struct provider_request_t {
    std::string client_uuid;
    std::uint64_t generation {};
    std::uint32_t launch_id {};
    std::uint16_t width {};
    std::uint16_t height {};
  };

  struct provider_resource_t {
    std::uint32_t windows_session_id {};
    std::string seat_id;
    std::uint64_t opaque_id {};
    std::uint16_t rtsp_port {};
    std::uint16_t control_port {};
    std::uint16_t video_port {};
    std::uint16_t audio_port {};
    // Provider-owned primary token; zero is deliberately unsupported.
    std::uintptr_t launch_token {};
    // Provider-owned interactive desktop name/handle contract.
    std::string desktop_name;
    // SID read from the provider-owned launch token; an empty value is not
    // sufficient to admit a worker process.
    std::string user_sid;
  };

  class seat_provider_t {
  public:
    virtual ~seat_provider_t() = default;
    [[nodiscard]] virtual provider_capability_t preflight() = 0;
    [[nodiscard]] virtual std::optional<provider_resource_t> allocate(const provider_request_t &, std::string &error) = 0;
    virtual void release(const provider_resource_t &) noexcept = 0;
    virtual bool release_checked(const provider_resource_t &resource) noexcept { release(resource); return true; }
    virtual bool release_checked(const provider_resource_t &resource, protocol::release_mode) noexcept { return release_checked(resource); }
  };

  struct worker_request_t {
    provider_request_t admission;
    provider_resource_t resource;
    protocol::ticket_t ticket;
    std::string config_root;
    std::string state_root;
    std::string log_root;
    std::vector<std::uint8_t> launch_payload;
    // Only the SYSTEM worker may consume this opt-in; it is not a public-pipe
    // command and is independently revalidated against the launch material.
    bool steam_offline_isolation {};
    std::function<bool(const protocol::request_t &)> ticket_validator;
  };

  class seat_worker_t {
  public:
    virtual ~seat_worker_t() = default;
    [[nodiscard]] virtual std::optional<route_t> start(const worker_request_t &, std::string &error) = 0;
    [[nodiscard]] virtual std::optional<route_t> resume(const worker_request_t &, std::string &error) {
      error = "The private worker does not support reconnect admission.";
      return std::nullopt;
    }
    /** Leave the worker and its launched applications alive while WTS disconnects. */
    [[nodiscard]] virtual bool park(const route_t &) noexcept { return true; }
    virtual bool stop(const route_t &) noexcept = 0;
    /** True while this worker still owns process/pipe/job resources that stop() must release. */
    [[nodiscard]] virtual bool cleanup_needed() const noexcept { return false; }
  };

  class runtime_t {
  public:
    using worker_factory_t = std::function<std::unique_ptr<seat_worker_t>()>;
    runtime_t(std::unique_ptr<seat_provider_t> provider, std::unique_ptr<seat_worker_t> worker);
    runtime_t(std::unique_ptr<seat_provider_t> provider, worker_factory_t worker_factory);
    ~runtime_t();

    [[nodiscard]] route_t prepare(request_t request);
    [[nodiscard]] state_t snapshot(std::string_view client_uuid) const;
    [[nodiscard]] bool disconnect(std::string_view client_uuid, std::string_view reason,
                                  protocol::release_mode mode = protocol::release_mode::retain);
    void unpair(std::string_view client_uuid);
    void shutdown();

  private:
    struct seat_t {
      seat_state_e state {seat_state_e::idle};
      std::uint64_t generation {};
      std::uint32_t launch_id {};
      route_t route;
      provider_resource_t resource;
      std::unique_ptr<seat_worker_t> worker_owner;
      seat_worker_t *worker {};
    };
    route_t reject(bool retryable, std::string error) const;
    [[nodiscard]] worker_request_t make_worker_request(const request_t &request, const provider_request_t &provider_request,
                                                       const provider_resource_t &resource, std::string &error);
    bool release_locked(std::string_view client_uuid, std::string_view reason, protocol::release_mode mode);

    std::unique_ptr<seat_provider_t> provider_;
    std::unique_ptr<seat_worker_t> worker_;
    worker_factory_t worker_factory_;
    mutable std::mutex mutex_;
    protocol::admission_authority authority_;
    std::unordered_map<std::string, seat_t> seats_;
  };

  /** Registers the fail-closed production runtime used by nvhttp. */
  void register_production_runtime();
  /** Stops and unregisters the production runtime. */
  void unregister_production_runtime();
}

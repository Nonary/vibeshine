/**
 * @file src/terminal_session_runtime.h
 * @brief Transactional, provider-neutral terminal seat orchestration.
 */
#pragma once

#include "terminal_session_broker.h"
#include "terminal_session_protocol.h"

#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>

namespace terminal_session {
  enum class seat_state_e : std::uint8_t { idle, preparing, ready, running, stopping, failed };

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
  };

  struct provider_resource_t {
    std::uint32_t windows_session_id {};
    std::string seat_id;
    std::uint64_t opaque_id {};
    std::uint16_t rtsp_port {};
    std::uint16_t control_port {};
    std::uint16_t video_port {};
    std::uint16_t audio_port {};
  };

  class seat_provider_t {
  public:
    virtual ~seat_provider_t() = default;
    [[nodiscard]] virtual provider_capability_t preflight() = 0;
    [[nodiscard]] virtual std::optional<provider_resource_t> allocate(const provider_request_t &, std::string &error) = 0;
    virtual void release(const provider_resource_t &) noexcept = 0;
  };

  struct worker_request_t {
    provider_request_t admission;
    provider_resource_t resource;
    protocol::ticket_t ticket;
    std::string config_root;
    std::string state_root;
    std::string log_root;
  };

  class seat_worker_t {
  public:
    virtual ~seat_worker_t() = default;
    [[nodiscard]] virtual std::optional<route_t> start(const worker_request_t &, std::string &error) = 0;
    virtual bool stop(const route_t &) noexcept = 0;
  };

  class runtime_t {
  public:
    runtime_t(std::unique_ptr<seat_provider_t> provider, std::unique_ptr<seat_worker_t> worker);
    ~runtime_t();

    [[nodiscard]] route_t prepare(request_t request);
    [[nodiscard]] state_t snapshot(std::string_view client_uuid) const;
    [[nodiscard]] bool disconnect(std::string_view client_uuid, std::string_view reason);
    void unpair(std::string_view client_uuid);
    void shutdown();

  private:
    struct seat_t {
      seat_state_e state {seat_state_e::idle};
      std::uint64_t generation {};
      std::uint32_t launch_id {};
      route_t route;
      provider_resource_t resource;
    };
    route_t reject(bool retryable, std::string error) const;
    bool release_locked(std::string_view client_uuid, std::string_view reason);

    std::unique_ptr<seat_provider_t> provider_;
    std::unique_ptr<seat_worker_t> worker_;
    mutable std::mutex mutex_;
    protocol::admission_authority authority_;
    std::unordered_map<std::string, seat_t> seats_;
  };

  /** Registers the fail-closed production runtime used by nvhttp. */
  void register_production_runtime();
  /** Stops and unregisters the production runtime. */
  void unregister_production_runtime();
}

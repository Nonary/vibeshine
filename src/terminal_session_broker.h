/**
 * @file src/terminal_session_broker.h
 * @brief Product boundary between the paired GameStream host and private Windows seats.
 */
#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>

namespace rtsp_stream {
  struct launch_session_t;
}

namespace terminal_session {
  enum class operation_e : std::uint8_t {
    launch,
    resume,
  };

  /**
   * The authenticated launch material that the main Vibeshine instance hands to
   * a private seat. The client UUID comes from its paired TLS certificate; the
   * Moonlight-supplied uniqueid is never an authorization input.
   */
  struct request_t {
    operation_e operation {operation_e::launch};
    std::shared_ptr<rtsp_stream::launch_session_t> launch_session;
    std::unordered_map<std::string, std::string> runtime_config_overrides;
  };

  /**
   * A broker-owned seat route. rtsp_port is an already allocated host-order
   * listener port, not a Sunshine base-port offset.
   */
  struct route_t {
    bool accepted {};
    bool ready {};
    bool retryable {};
    std::uint16_t rtsp_port {};
    // The worker owns one complete media/control allocation. A route is ready
    // only when every required port is known; no arbitrary RTSP-only fallback
    // is permitted for terminal sessions.
    std::uint16_t control_port {};
    std::uint16_t video_port {};
    std::uint16_t audio_port {};
    std::uint32_t windows_session_id {};
    std::string seat_id;
    std::string error;
  };

  /**
   * Per-client state projected through the main paired host for serverinfo,
   * app-list, reconnect, and Web UI status.
   */
  struct state_t {
    bool exists {};
    bool ready {};
    bool connected {};
    std::int32_t app_id {};
    std::string app_uuid;
    std::string app_name;
    std::uint32_t windows_session_id {};
    std::string seat_id;
  };

  enum class snapshot_status_e : std::uint8_t {
    present,
    absent,
    unavailable,
  };

  struct snapshot_result_t {
    snapshot_status_e status {snapshot_status_e::unavailable};
    state_t state;
  };

  enum class route_mode_e : std::uint8_t {
    terminal,
    console,
    unavailable,
  };

  [[nodiscard]] constexpr route_mode_e route_mode(const bool force_terminal, const snapshot_status_e status) noexcept {
    if (force_terminal || status == snapshot_status_e::present) return route_mode_e::terminal;
    return status == snapshot_status_e::absent ? route_mode_e::console : route_mode_e::unavailable;
  }

  struct runtime_hooks_t {
    std::function<route_t(request_t)> prepare;
    std::function<snapshot_result_t(std::string_view client_uuid)> snapshot;
    // True means the exact client's worker/session teardown is complete. A
    // timeout or partial cleanup must return false so the caller stays closed.
    std::function<bool(std::string_view client_uuid, std::string_view reason)> disconnect;
    std::function<void(std::string_view client_uuid)> unpair;
    std::function<void()> shutdown;
  };

  void register_runtime_hooks(runtime_hooks_t hooks);
  [[nodiscard]] bool runtime_available();
  /** True only on a host that exposes the terminal-session integration. */
  [[nodiscard]] bool supported();
  [[nodiscard]] route_t prepare(request_t request);
  [[nodiscard]] snapshot_result_t snapshot_result(std::string_view client_uuid);
  /** Compatibility projection for callers that only need an authoritative seat state. */
  [[nodiscard]] state_t snapshot(std::string_view client_uuid);
  [[nodiscard]] bool disconnect(std::string_view client_uuid, std::string_view reason);
  void notify_unpair(std::string_view client_uuid);
  void notify_shutdown();
}  // namespace terminal_session

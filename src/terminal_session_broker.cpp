#include "terminal_session_broker.h"

#include <exception>
#include <mutex>
#include <utility>

#include "rtsp.h"

namespace terminal_session {
  namespace {
    std::mutex runtime_hooks_mutex;
    runtime_hooks_t runtime_hooks;

    route_t invalid_request(std::string error) {
      return route_t {
        .accepted = false,
        .ready = false,
        .retryable = false,
        .error = std::move(error),
      };
    }
  }  // namespace

  void register_runtime_hooks(runtime_hooks_t hooks) {
    std::lock_guard lock {runtime_hooks_mutex};
    runtime_hooks = std::move(hooks);
  }

  bool runtime_available() {
    std::lock_guard lock {runtime_hooks_mutex};
    return static_cast<bool>(runtime_hooks.prepare);
  }

  bool supported() {
#ifdef _WIN32
    return runtime_available();
#else
    return false;
#endif
  }

  route_t prepare(request_t request) {
    if (!request.launch_session) {
      return invalid_request("Terminal session launch material is missing.");
    }
    if (request.launch_session->id == 0) {
      return invalid_request("Terminal session launch ID is missing.");
    }
    if (request.launch_session->client_uuid.empty()) {
      return invalid_request("An authenticated paired client is required for a terminal session.");
    }
    if (!request.launch_session->terminal_session_requested) {
      return invalid_request("Terminal session routing was not enabled for this paired client.");
    }
    if (request.launch_session->role != remote_session::role_e::game) {
      return invalid_request("Only configured application streams may use a terminal session.");
    }

    std::function<route_t(request_t)> prepare_hook;
    {
      std::lock_guard lock {runtime_hooks_mutex};
      prepare_hook = runtime_hooks.prepare;
    }
    if (!prepare_hook) {
      return route_t {
        .accepted = false,
        .ready = false,
        .retryable = true,
        .error = "Terminal session broker is not available.",
      };
    }

    route_t route;
    try {
      route = prepare_hook(std::move(request));
    } catch (const std::exception &error) {
      return route_t {
        .accepted = false,
        .ready = false,
        .retryable = true,
        .error = "Terminal session broker failed: " + std::string {error.what()},
      };
    } catch (...) {
      return route_t {
        .accepted = false,
        .ready = false,
        .retryable = true,
        .error = "Terminal session broker failed.",
      };
    }
    if (route.ready && (!route.accepted || route.rtsp_port == 0 || route.control_port == 0 || route.video_port == 0 || route.audio_port == 0)) {
      return invalid_request("Terminal session broker returned an incomplete media route.");
    }
    if (!route.ready && route.error.empty()) {
      route.error = "Terminal session did not become ready.";
    }
    return route;
  }

  snapshot_result_t snapshot_result(const std::string_view client_uuid) {
    if (client_uuid.empty()) {
      return {};
    }
    std::function<snapshot_result_t(std::string_view)> snapshot_hook;
    {
      std::lock_guard lock {runtime_hooks_mutex};
      snapshot_hook = runtime_hooks.snapshot;
    }
    if (!snapshot_hook) {
      return {};
    }
    try {
      auto result = snapshot_hook(client_uuid);
      if (result.status == snapshot_status_e::present && !result.state.exists) return {};
      if (result.status != snapshot_status_e::present) result.state = {};
      return result;
    } catch (...) {
      return {};
    }
  }

  state_t snapshot(const std::string_view client_uuid) {
    const auto result = snapshot_result(client_uuid);
    return result.status == snapshot_status_e::present ? result.state : state_t {};
  }

  bool disconnect(const std::string_view client_uuid, const std::string_view reason) {
    if (client_uuid.empty()) {
      return false;
    }
    std::function<bool(std::string_view, std::string_view)> disconnect_hook;
    {
      std::lock_guard lock {runtime_hooks_mutex};
      disconnect_hook = runtime_hooks.disconnect;
    }
    if (!disconnect_hook) {
      return false;
    }
    try {
      return disconnect_hook(client_uuid, reason);
    } catch (...) {
      return false;
    }
  }

  void notify_unpair(const std::string_view client_uuid) {
    std::function<void(std::string_view)> unpair_hook;
    {
      std::lock_guard lock {runtime_hooks_mutex};
      unpair_hook = runtime_hooks.unpair;
    }
    if (unpair_hook && !client_uuid.empty()) {
      try {
        unpair_hook(client_uuid);
      } catch (...) {
      }
    }
  }

  void notify_shutdown() {
    std::function<void()> shutdown_hook;
    {
      std::lock_guard lock {runtime_hooks_mutex};
      shutdown_hook = runtime_hooks.shutdown;
    }
    if (shutdown_hook) {
      try {
        shutdown_hook();
      } catch (...) {
      }
    }
  }
}  // namespace terminal_session

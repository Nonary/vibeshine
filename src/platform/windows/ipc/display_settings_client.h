/**
 * @file src/platform/windows/ipc/display_settings_client.h
 * @brief Client helper to send display apply/revert commands to the helper process.
 */
#pragma once

#ifdef _WIN32

  #include <cstdint>
  #include <optional>
  #include <string>

namespace platf::display_helper_client {
  struct ApplyResult {
    bool succeeded = false;
    bool acknowledged = false;
    std::uint64_t request_id = 0;

    explicit operator bool() const {
      return succeeded;
    }
  };

  enum class DisarmResult {
    Disarmed,
    Busy,
    Unavailable,
  };

  // Send APPLY with JSON payload (SingleDisplayConfiguration)
  ApplyResult send_apply_json(const std::string &json, int result_timeout_ms = 5000);

  // Wait for helper verification result after APPLY (v2 engine only).
  // Returns nullopt on timeout/unavailable.
  std::optional<bool> wait_for_verification_result(std::uint64_t request_id, int timeout_ms);

  // Send REVERT with optional JSON payload.
  bool send_revert(const std::string &json_payload = {});

  // Deadline-bounded REVERT send used only to hand an older external helper
  // back to its own strict restore/exit path during an upgrade handoff.
  bool send_revert_fast(const std::string &json_payload, int timeout_ms);

  // Update helper log level to match Sunshine's minimum log level (v2 engine only).
  bool send_log_level(int min_log_level);

  // Export current OS display settings as a golden restore snapshot
  bool send_export_golden(const std::string &json_payload = {});

  // Best-effort cancel of any pending restore/watchdog activity on the helper
  bool send_disarm_restore();

  // Correlated DISARM bounded by timeout_ms. Disarmed means the helper confirmed
  // cancellation before display mutation; Busy means an unconfirmed mutation must
  // be allowed to drain; Unavailable means no matching acknowledgement arrived.
  DisarmResult send_disarm_restore_fast(int timeout_ms);

  // Save the current OS display state to session_current (rotate current->previous) without applying config.
  // Waits briefly for the helper to confirm whether the snapshot was actually saved,
  // then fails open so an unusually slow display stack cannot stall stream startup.
  bool send_snapshot_current(const std::string &json_payload = {});

  // Reset helper-side persistence/state (best-effort)
  bool send_reset();

  // Request helper process to terminate gracefully.
  bool send_stop();

  // Lightweight liveness probe; returns true if a Ping frame was sent.
  // This does not wait for a reply; it only validates a healthy send path.
  bool send_ping();

  // Fast liveness probe bounded by timeout_ms for connect/send.
  bool send_ping_fast(int timeout_ms);

  // Reset the cached connection so the next send will reconnect.
  void reset_connection();
}  // namespace platf::display_helper_client

#endif

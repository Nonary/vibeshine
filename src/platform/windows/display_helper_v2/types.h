#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <display_device/json.h>
#include <display_device/types.h>
#include <display_device/windows/types.h>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace display_helper::v2 {
  enum class ApplyAction {
    Apply,
    Revert,
    Disarm,
    ExportGolden,
    SnapshotCurrent,
    Reset,
    Ping,
    Stop,
  };

  enum class ApplyStatus {
    Ok,
    HelperUnavailable,
    InvalidRequest,
    VerificationFailed,
    NeedsVirtualDisplayReset,
    Retryable,
    Expired,
    Fatal,
  };

  enum class SnapshotTier {
    Current,
    Previous,
    Golden,
  };

  enum class PolicyDecision {
    Proceed,
    Retry,
    ResetVirtualDisplay,
    SkipToNextTier,
  };

  enum class WatchdogStatus {
    Healthy,
    MissedPing,
    TimedOut,
  };

  enum class State {
    Waiting,
    InProgress,
    Verification,
    Recovery,
    RecoveryValidation,
    EventLoop,
    VirtualDisplayMonitoring,  // Monitors virtual display for crashes during active session
  };

  enum class DisplayEvent {
    DisplayChange,
    PowerResume,
    DeviceArrival,
    DeviceRemoval,
  };

  enum class HelperEvent {
    HeartbeatTimeout,
  };

  using ActiveTopology = display_device::ActiveTopology;
  using EnumeratedDeviceList = display_device::EnumeratedDeviceList;
  using Snapshot = display_device::DisplaySettingsSnapshot;
  using SingleDisplayConfiguration = display_device::SingleDisplayConfiguration;

  struct ApplyRequest {
    std::optional<SingleDisplayConfiguration> configuration;
    std::optional<ActiveTopology> topology;
    std::vector<std::pair<std::string, display_device::Point>> monitor_positions;
    /// Restore physical monitor refresh rates (device_id -> num/den) after virtual
    /// display creation resets them.
    std::vector<std::pair<std::string, std::pair<unsigned int, unsigned int>>> refresh_rate_overrides;
    bool hdr_blank = false;
    bool prefer_golden_first = false;
    /// When false, a broken Sunshine connection must not autonomously restore
    /// (stream is intentionally pause-retained).
    bool restore_on_disconnect = true;
    std::optional<std::string> virtual_layout;
    std::optional<std::chrono::steady_clock::time_point> expires_at;
    // Internal: a VD reset or another Apply stage already crossed the absolute
    // deadline gate. The remaining stages must drain to a stable result.
    bool deadline_committed = false;
    // Shared by every queued retry/restart of one logical Apply. It is set at
    // the actual worker mutation boundary, so a stale completion cannot make a
    // later retry forget that the deadline and baseline are already committed.
    std::shared_ptr<std::atomic<bool>> mutation_committed;
    // Apply policy/exclusions are staged until the same mutation boundary.
    std::optional<std::vector<std::string>> staged_exclusions;
  };

  struct SnapshotCommandPayload {
    std::vector<std::string> exclude_devices;
    /// True when the payload carried an exclusion list (even an empty one, which clears).
    bool update_exclusions = false;
  };

  struct ApplyCommand {
    ApplyRequest request;
    std::uint64_t generation = 0;
    std::uint64_t request_id = 0;
    std::uint64_t connection_epoch = 0;
    std::optional<std::set<std::string>> snapshot_blacklist;
  };

  struct RevertCommand {
    std::uint64_t generation = 0;
    /// Prefer golden over previous when the current session snapshot is unavailable.
    bool prefer_golden_if_current_missing = true;
    /// Optional override of the golden-first strategy carried in the REVERT payload.
    std::optional<bool> always_restore_from_golden;
    /// Skip the 5s grace window before the first restore attempt (--restore mode).
    bool immediate = false;
    /// True when triggered by a broken connection / heartbeat loss rather than an
    /// explicit client REVERT; honors the restore-on-disconnect policy.
    bool from_disconnect = false;
    // Set only for an explicit IPC REVERT. Internal restore-mode,
    // heartbeat, and disconnect safety reverts deliberately leave this unset
    // so reconnect purge cannot discard required recovery work.
    std::optional<std::uint64_t> client_connection_epoch;
  };

  struct DisarmCommand {
    std::uint64_t generation = 0;
    std::uint64_t request_id = 0;
    std::uint64_t connection_epoch = 0;
    std::optional<std::chrono::steady_clock::time_point> expires_at;
  };

  struct ExportGoldenCommand {
    SnapshotCommandPayload payload;
    std::uint64_t generation = 0;
    std::uint64_t connection_epoch = 0;
  };

  struct SnapshotCurrentCommand {
    SnapshotCommandPayload payload;
    std::uint64_t generation = 0;
    std::uint64_t request_id = 0;
    std::uint64_t connection_epoch = 0;
    std::optional<std::chrono::steady_clock::time_point> expires_at;
  };

  struct ResetCommand {
    std::uint64_t generation = 0;
    std::uint64_t connection_epoch = 0;
  };

  struct PingCommand {
    std::uint64_t generation = 0;
    std::uint64_t connection_epoch = 0;
  };

  struct StopCommand {
    std::uint64_t generation = 0;
    std::uint64_t connection_epoch = 0;
  };

  struct ApplyCompleted {
    ApplyStatus status = ApplyStatus::Fatal;
    std::optional<ActiveTopology> expected_topology;
    bool virtual_display_requested = false;
    bool deadline_committed = false;
    std::uint64_t generation = 0;
  };

  struct VerificationCompleted {
    bool success = false;
    std::uint64_t generation = 0;
  };

  struct RecoveryCompleted {
    bool success = false;
    std::optional<Snapshot> snapshot;
    std::uint64_t generation = 0;
  };

  struct RecoveryValidationCompleted {
    bool success = false;
    std::uint64_t generation = 0;
  };

  struct DisplayEventMessage {
    DisplayEvent event = DisplayEvent::DisplayChange;
    std::uint64_t generation = 0;
  };

  struct HelperEventMessage {
    HelperEvent event = HelperEvent::HeartbeatTimeout;
    std::uint64_t generation = 0;
  };

  using Message = std::variant<
    ApplyCommand,
    RevertCommand,
    DisarmCommand,
    ExportGoldenCommand,
    SnapshotCurrentCommand,
    ResetCommand,
    PingCommand,
    StopCommand,
    ApplyCompleted,
    VerificationCompleted,
    RecoveryCompleted,
    RecoveryValidationCompleted,
    DisplayEventMessage,
    HelperEventMessage>;

  inline std::optional<std::uint64_t> connection_bound_epoch(const Message &message) {
    return std::visit([](const auto &value) -> std::optional<std::uint64_t> {
      using T = std::decay_t<decltype(value)>;
      if constexpr (std::is_same_v<T, ApplyCommand> ||
                    std::is_same_v<T, DisarmCommand> ||
                    std::is_same_v<T, ExportGoldenCommand> ||
                    std::is_same_v<T, SnapshotCurrentCommand> ||
                    std::is_same_v<T, ResetCommand> ||
                    std::is_same_v<T, PingCommand> ||
                    std::is_same_v<T, StopCommand>) {
        return value.connection_epoch;
      } else if constexpr (std::is_same_v<T, RevertCommand>) {
        return value.client_connection_epoch;
      }
      return std::nullopt;
    },
                      message);
  }

  inline bool is_connection_bound_command(const Message &message) {
    return connection_bound_epoch(message).has_value();
  }
}  // namespace display_helper::v2

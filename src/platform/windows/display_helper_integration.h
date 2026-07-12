/**
 * @file src/platform/windows/display_helper_integration.h
 * @brief High-level wrappers to use the display helper from Sunshine start/stop events.
 */
#pragma once

#include "src/config.h"
#include "src/display_helper_builder.h"
#include "src/rtsp.h"

#include <chrono>
#include <cstdint>
#include <display_device/types.h>
#include <functional>
#include <memory>
#include <optional>
#include <vector>

namespace display_helper_integration {
#ifdef _WIN32
  class DisplayTeardownLease;

  class DisplayStartReservation {
  public:
    ~DisplayStartReservation();
    DisplayStartReservation(DisplayStartReservation &&) noexcept;
    DisplayStartReservation &operator=(DisplayStartReservation &&) noexcept;
    DisplayStartReservation(const DisplayStartReservation &) = delete;
    DisplayStartReservation &operator=(const DisplayStartReservation &) = delete;

    // Publish that the corresponding stream/capture is now visible in its
    // active counter. This is idempotent and may run on a different thread
    // from the one which acquired the reservation.
    void publish_active();

    // Atomically turn an unpublished start into an exclusive teardown window.
    // Abort paths use this before rolling back VD/APPLY work so unrelated
    // cleanup cannot race the abandoned start.
    // Abort paths commonly run from noexcept scope guards and timer callbacks.
    // Allocation failure leaves the reservation live for its ordinary release,
    // rather than terminating the process during rollback.
    std::shared_ptr<DisplayTeardownLease> begin_abort_cleanup() noexcept;

  private:
    struct Impl;
    explicit DisplayStartReservation(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;

    friend std::shared_ptr<DisplayStartReservation> acquire_display_start_reservation(
      std::chrono::milliseconds timeout
    );
  };

  // Serialize the zero-active-session decision across RTSP and WebRTC. The
  // reservation is intentionally logical (rather than a thread-owned mutex)
  // because RTSP publishes activity from its later ANNOUNCE worker.
  std::shared_ptr<DisplayStartReservation> acquire_display_start_reservation(
    std::chrono::milliseconds timeout
  );

  // True while a start is queued behind final teardown. Cleanup uses this as
  // a pre-mutation yield signal so normal stream switches do not inherit the
  // full display-removal timeout.
  bool display_start_waiting();
  bool display_start_in_progress();

  class DisplayTeardownLease {
  public:
    ~DisplayTeardownLease();
    DisplayTeardownLease(DisplayTeardownLease &&) noexcept;
    DisplayTeardownLease &operator=(DisplayTeardownLease &&) noexcept;
    DisplayTeardownLease(const DisplayTeardownLease &) = delete;
    DisplayTeardownLease &operator=(const DisplayTeardownLease &) = delete;

  private:
    struct Impl;
    explicit DisplayTeardownLease(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;

    friend std::shared_ptr<DisplayTeardownLease> try_acquire_display_teardown(
      std::function<bool()> still_last_capture_user,
      std::function<void()> retry_if_start_abandoned
    );
    friend class DisplayStartReservation;
  };

  // Claim the final-capture teardown window if no stream start is pending and
  // the caller is still the final capture user. While held, new starts wait.
  std::shared_ptr<DisplayTeardownLease> try_acquire_display_teardown(
    std::function<bool()> still_last_capture_user,
    std::function<void()> retry_if_start_abandoned = {}
  );

  // Cross-protocol ownership for non-idempotent platform streaming hooks.
  // A token is bound to one contiguous platform-streaming lifetime. Every
  // start, policy update, and final release must present that same token so a
  // deferred action from an old session cannot affect a later session.
  struct PlatformStreamingClaim {
    std::uint64_t generation {0};
    bool start_required {false};
  };

  // Returns the current lifecycle token when the caller still owns an active
  // capture. start_required is true until the platform start hook succeeds.
  std::optional<PlatformStreamingClaim> claim_platform_streaming_lifecycle(
    std::function<bool()> still_needed = {}
  );
  bool run_platform_streaming_start(
    std::uint64_t generation,
    std::function<void()> start_hook,
    // Rechecked under the platform-hook serialization immediately before the
    // callback. This closes the claim-to-start window for a just-ended stream.
    std::function<bool()> still_needed = {},
    // Invoked when start_hook throws after a caller-owned side effect. It must
    // clean only work that the callback actually began; the lifecycle remains
    // unstarted so final release will not perform an unmatched platform stop.
    std::function<void()> rollback_hook = {}
  );
  bool run_platform_streaming_update(
    std::uint64_t generation,
    std::function<void()> update_hook
  );
  // The callback always executes for an accepted release under the hook lock;
  // platform_started distinguishes a deferred-but-never-started lifecycle from
  // one that owns frame-limiter/streaming state.
  bool release_platform_streaming_lifecycle(
    std::uint64_t generation,
    std::function<void(bool platform_started)> stop_hook
  );

  // Final teardown holds the display teardown lease, which blocks a new
  // claim. It can therefore snapshot this token and pass it to release.
  std::optional<std::uint64_t> current_platform_streaming_generation();

  // Managed executor for deferred display work. It is intentionally separate
  // from the latency-sensitive global task pool: cleanup can wait for display
  // handoff/settle bounds without delaying input work. Submission never throws
  // and becomes a no-op once shutdown starts.
  void start_display_cleanup_dispatcher() noexcept;
  void stop_display_cleanup_dispatcher() noexcept;
  bool enqueue_display_cleanup_task(std::function<void()> task) noexcept;
  bool enqueue_delayed_display_cleanup_task(
    std::chrono::milliseconds delay,
    std::function<void()> task
  ) noexcept;

  class DisplayHandoffLease {
  public:
    ~DisplayHandoffLease();
    DisplayHandoffLease(DisplayHandoffLease &&) noexcept;
    DisplayHandoffLease &operator=(DisplayHandoffLease &&) noexcept;
    DisplayHandoffLease(const DisplayHandoffLease &) = delete;
    DisplayHandoffLease &operator=(const DisplayHandoffLease &) = delete;

  private:
    struct Impl;
    explicit DisplayHandoffLease(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;

    friend std::shared_ptr<DisplayHandoffLease> acquire_safe_display_handoff(
      std::chrono::milliseconds timeout,
      std::function<bool()> still_allowed
    );
  };

  // Acquire a generation-safe lease that prevents a new REVERT from being
  // dispatched until the caller's first display transaction is complete.
  std::shared_ptr<DisplayHandoffLease> acquire_safe_display_handoff(
    std::chrono::milliseconds timeout,
    std::function<bool()> still_allowed = {}
  );
#endif

  // Launch the helper (if needed) and process the provided builder request.
  // Returns true if the helper accepted the command; false to allow fallback.
  bool apply(const DisplayApplyRequest &request);

#ifdef _WIN32
  struct ApplyDispatchResult {
    bool accepted = false;
    bool handoff_safe = true;
    std::optional<std::uint64_t> verification_token;

    explicit operator bool() const {
      return accepted;
    }
  };

  // Apply and return the exact v2 request token whose verification result the
  // caller may await. Legacy/in-process applies have no verification token.
  ApplyDispatchResult apply_with_verification(const DisplayApplyRequest &request);
#endif

  // Returns true if a deferred APPLY request is currently queued.
  bool has_pending_apply();

  // Retry a deferred APPLY request once a user session is available.
  bool apply_pending_if_ready();

  // Clear any deferred APPLY request (used when sessions end).
  void clear_pending_apply();

  // Launch the helper (if needed) and send REVERT.
  // Returns true if the helper accepted the command; false to allow fallback.
  bool revert(bool prefer_golden_if_current_missing = true);

  // Attempt to cancel any pending restore/revert requests on a running helper.
  // Returns true only when the helper confirms cancellation was safe.
  bool disarm_pending_restore();

  // Stream-start barrier for the rare restore handoff path. Returns true when
  // display enumeration/mutation is safe, or false when an unconfirmed restore
  // remains after the bounded wait.
  bool wait_for_safe_display_handoff(std::chrono::milliseconds timeout);

  // Read-only startup diagnostic. Unlike acquire_safe_display_handoff(), this
  // never sends DISARM to an independently restoring helper.
  bool helper_or_restore_active();

  // Request the helper to export current OS settings as golden restore snapshot.
  bool export_golden_restore();

  // Request the helper to reset its persistence/state.
  bool reset_persistence();

  // Ask the helper to capture the current display snapshot without applying changes.
  bool snapshot_current_display_state();

  // Enumerate display devices via helper (or return nullopt on failure).
  std::optional<display_device::EnumeratedDeviceList> enumerate_devices(
    display_device::DeviceEnumerationDetail detail = display_device::DeviceEnumerationDetail::Minimal
  );

  // Enumerate display devices and return JSON payload for API.
  std::string enumerate_devices_json(
    display_device::DeviceEnumerationDetail detail = display_device::DeviceEnumerationDetail::Minimal
  );

  // Capture the currently active topology before applying changes.
  std::optional<std::vector<std::vector<std::string>>> capture_current_topology();

#ifdef _WIN32
  enum class ApplyVerificationStatus {
    Verified,
    Failed,
    Unknown
  };

  // Wait for helper verification to finish after APPLY (v2 engine only).
  // Returns Unknown on timeout/unavailable/legacy engine.
  ApplyVerificationStatus wait_for_apply_verification(
    std::uint64_t verification_token,
    std::chrono::milliseconds timeout
  );
#endif

#ifdef _WIN32
  struct FramegenEdidTargetSupport {
    int hz {0};
    std::optional<bool> supported;
    std::string method;
  };

  struct FramegenEdidSupportResult {
    std::string device_id;
    std::string device_label;
    bool edid_present {false};
    std::optional<int> max_vertical_hz;
    std::optional<double> max_timing_hz;
    std::vector<FramegenEdidTargetSupport> targets;
  };

  // Read EDID for a specific device and evaluate refresh support for requested targets.
  std::optional<FramegenEdidSupportResult> framegen_edid_refresh_support(
    const std::string &device_hint,
    const std::vector<int> &targets_hz
  );
#endif

  // Returns milliseconds since the last successful display-helper APPLY completed.
  // Returns a very large value if no apply has ever been performed.
  int64_t ms_since_last_apply();

  // Start a lightweight watchdog during active streams that pings the helper periodically
  // and restarts/re-handshakes if it crashes. No-ops if already running.
  void start_watchdog();

  // Stop the helper watchdog when no streams are active.
  void stop_watchdog();

}  // namespace display_helper_integration

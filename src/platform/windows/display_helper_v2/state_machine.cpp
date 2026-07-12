#include "src/platform/windows/display_helper_v2/state_machine.h"

#include "src/logging.h"

#include <boost/algorithm/string/predicate.hpp>
#include <type_traits>
#include <utility>

namespace display_helper::v2 {
  namespace {
    const char *state_to_string(State state) {
      switch (state) {
        case State::Waiting:
          return "Waiting";
        case State::InProgress:
          return "InProgress";
        case State::Verification:
          return "Verification";
        case State::Recovery:
          return "Recovery";
        case State::RecoveryValidation:
          return "RecoveryValidation";
        case State::EventLoop:
          return "EventLoop";
        case State::VirtualDisplayMonitoring:
          return "VirtualDisplayMonitoring";
        default:
          return "Unknown";
      }
    }

    const char *action_to_string(ApplyAction action) {
      switch (action) {
        case ApplyAction::Apply:
          return "Apply";
        case ApplyAction::Revert:
          return "Revert";
        case ApplyAction::Disarm:
          return "Disarm";
        case ApplyAction::ExportGolden:
          return "ExportGolden";
        case ApplyAction::SnapshotCurrent:
          return "SnapshotCurrent";
        case ApplyAction::Reset:
          return "Reset";
        case ApplyAction::Ping:
          return "Ping";
        case ApplyAction::Stop:
          return "Stop";
        default:
          return "Unknown";
      }
    }

    const char *display_event_to_string(DisplayEvent event) {
      switch (event) {
        case DisplayEvent::DisplayChange:
          return "DisplayChange";
        case DisplayEvent::PowerResume:
          return "PowerResume";
        case DisplayEvent::DeviceArrival:
          return "DeviceArrival";
        case DisplayEvent::DeviceRemoval:
          return "DeviceRemoval";
        default:
          return "Unknown";
      }
    }

    const char *apply_status_to_string(ApplyStatus status) {
      switch (status) {
        case ApplyStatus::Ok:
          return "Ok";
        case ApplyStatus::HelperUnavailable:
          return "HelperUnavailable";
        case ApplyStatus::InvalidRequest:
          return "InvalidRequest";
        case ApplyStatus::VerificationFailed:
          return "VerificationFailed";
        case ApplyStatus::NeedsVirtualDisplayReset:
          return "NeedsVirtualDisplayReset";
        case ApplyStatus::Retryable:
          return "Retryable";
        case ApplyStatus::Expired:
          return "Expired";
        case ApplyStatus::Fatal:
          return "Fatal";
        default:
          return "Unknown";
      }
    }
  }  // namespace

  std::optional<std::pair<Snapshot, codec::layout_rotation_map_t>> SnapshotLedger::capture_filtered(const std::vector<std::string> &exclusions, const char *reason) {
    auto snap = service_.capture();
    if (!service_.topology_is_valid(snap.m_topology)) {
      BOOST_LOG(warning) << "Skipping display snapshot save (" << (reason ? reason : "snapshot")
                         << "); topology is invalid or empty.";
      return std::nullopt;
    }

    std::string reject_reason;
    auto filtered = codec::filter_snapshot_for_save(std::move(snap), service_.enumerate(), exclusions, reject_reason);
    if (!filtered) {
      BOOST_LOG(warning) << "Skipping display snapshot save (" << (reason ? reason : "snapshot")
                         << "); " << reject_reason << ".";
      return std::nullopt;
    }

    const auto layout_ids_vec = codec::flatten_topology_device_ids(filtered->m_topology);
    const std::set<std::string> layout_ids(layout_ids_vec.begin(), layout_ids_vec.end());
    auto layouts = service_.capture_layouts(layout_ids);
    return std::make_pair(std::move(*filtered), std::move(layouts));
  }

  bool SnapshotLedger::capture_filtered_and_save(SnapshotTier tier, const std::vector<std::string> &exclusions, const char *reason) {
    const char *why = reason ? reason : "snapshot";
    constexpr int kMaxAttempts = 3;
    for (int attempt = 1; attempt <= kMaxAttempts; ++attempt) {
      if (auto captured = capture_filtered(exclusions, why)) {
        if (persistence_.storage().save(tier, captured->first, captured->second)) {
          if (attempt > 1) {
            BOOST_LOG(info) << "Display snapshot save succeeded on retry #" << attempt << " (" << why << ").";
          }
          return true;
        }
      }
      if (attempt < kMaxAttempts) {
        BOOST_LOG(info) << "Display snapshot save retry #" << (attempt + 1) << " scheduled (" << why << ").";
        clock_.sleep_for(std::chrono::milliseconds(50));
      }
    }
    return false;
  }

  bool SnapshotLedger::refresh_current_preserving_previous(
    const std::vector<std::string> &exclusions,
    std::optional<std::chrono::steady_clock::time_point> expires_at
  ) {
    const auto expired = [&]() {
      return expires_at.has_value() && clock_.now() >= *expires_at;
    };

    if (expired()) {
      BOOST_LOG(info) << "Current session snapshot lease expired before capture began.";
      return false;
    }

    // Capture first; a failed capture must never destroy the existing baseline
    // chain (53cd8b4c / 62839421).
    auto captured = capture_filtered(exclusions, "snapshot-only");
    if (!captured) {
      BOOST_LOG(info) << "Refreshed current session snapshot (snapshot-only): false";
      return false;
    }

    // The host is free to begin display enumeration once the lease expires.
    // Never rotate or replace the baseline with a capture that crossed that
    // boundary, even when the Windows capture call itself could not be canceled.
    if (expired()) {
      BOOST_LOG(info) << "Current session snapshot lease expired during capture; discarding candidate.";
      return false;
    }

    auto &storage = persistence_.storage();
    bool history_rotation_committed = false;
    auto previous_before_rotation = storage.load_with_metadata(SnapshotTier::Previous);
    const auto roll_back_history = [&]() {
      if (!history_rotation_committed) {
        return true;
      }
      return previous_before_rotation ?
               storage.save(
                 SnapshotTier::Previous,
                 previous_before_rotation->snapshot,
                 previous_before_rotation->layout_rotations
               ) :
               storage.remove(SnapshotTier::Previous);
    };
    if (auto current = storage.load_with_metadata(SnapshotTier::Current)) {
      if (!storage.save(SnapshotTier::Previous, current->snapshot, current->layout_rotations)) {
        BOOST_LOG(warning) << "Failed to refresh session snapshot history (snapshot-only): current->previous copy failed.";
        return false;
      }
      history_rotation_committed = true;
    }

    if (expired()) {
      if (!roll_back_history()) {
        BOOST_LOG(error) << "Failed to roll back Previous after the snapshot commit lease expired.";
      }
      BOOST_LOG(info) << "Current session snapshot lease expired before baseline commit; discarded candidate and rolled back history.";
      return false;
    }

    const bool replaced = storage.save(SnapshotTier::Current, captured->first, captured->second);
    if (!replaced && history_rotation_committed) {
      if (!roll_back_history()) {
        BOOST_LOG(error) << "Failed to roll back Previous after Current snapshot replacement failed.";
      }
    }
    BOOST_LOG(info) << "Refreshed current session snapshot (snapshot-only): " << (replaced ? "true" : "false");
    return replaced;
  }

  StateMachine::StateMachine(
    ApplyPipeline &apply,
    RecoveryPipeline &recovery,
    SnapshotLedger &snapshots,
    SystemPorts &system,
    IVirtualDisplayDriver &virtual_display,
    GoldenHealth &golden_health,
    RestoreState &restore_state
  ):
      apply_(apply),
      recovery_(recovery),
      snapshots_(snapshots),
      system_(system),
      virtual_display_(virtual_display),
      golden_health_(golden_health),
      restore_state_(restore_state) {}

  std::vector<std::string> StateMachine::exclusions_vector() const {
    return restore_state_.exclusions();
  }

  void StateMachine::update_blacklist(const std::vector<std::string> &exclude_devices) {
    std::vector<std::string> exclusions;
    exclusions.reserve(exclude_devices.size());
    for (const auto &id : exclude_devices) {
      if (!id.empty()) {
        exclusions.push_back(id);
      }
    }
    restore_state_.set_exclusions(std::move(exclusions));
  }

  void StateMachine::start_recovery(std::chrono::milliseconds delay, ApplyAction trigger) {
    // Recovery must never overlap a delayed post-Apply workaround. Cancellation
    // joins any mutation that already crossed its own gate.
    system_.cancel_pending_display_mutations();
    transition(State::Recovery, trigger);
    recovery_.dispatch_recovery(delay);
  }

  bool StateMachine::process_pending_disconnect_if_apply_drained() {
    if (!pending_disconnect_revert_ ||
        restore_state_.apply_workers_active.load(std::memory_order_acquire) != 0) {
      return false;
    }

    auto pending = *pending_disconnect_revert_;
    pending_disconnect_revert_.reset();
    pending.generation = system_.current_generation();
    BOOST_LOG(info) << "Display helper: Apply drained; resolving deferred disconnect restore policy.";
    const bool skip_disconnect_restore =
      pending.from_disconnect &&
      !restore_state_.restore_on_disconnect.load(std::memory_order_acquire) &&
      !restore_pending();
    if (skip_disconnect_restore) {
      // restore_on_disconnect=false intentionally retains the committed display
      // state, but the canceled Apply completion must still leave InProgress.
      transition(State::Waiting, ApplyAction::Apply);
      apply_result_sent_ = true;
    } else {
      // Do not infer policy handling from state equality: restarting an already
      // active Recovery legitimately leaves state_ unchanged.
      handle_revert_command(pending);
    }
    return true;
  }

  void StateMachine::retarget_virtual_display_device_id_if_needed() {
    if (!current_request_.virtual_layout.has_value()) {
      return;
    }
    if (!current_request_.configuration) {
      return;
    }

    const std::string resolved = virtual_display_.device_id();
    if (resolved.empty()) {
      return;
    }

    auto &cfg = *current_request_.configuration;
    const std::string previous = cfg.m_device_id;
    if (!previous.empty() && boost::iequals(previous, resolved)) {
      return;
    }

    BOOST_LOG(info) << "Display helper: retargeting virtual display device_id from '"
                    << (previous.empty() ? std::string("(empty)") : previous)
                    << "' to '" << resolved << "' for monitoring re-apply.";

    cfg.m_device_id = resolved;

    if (current_request_.topology) {
      for (auto &group : *current_request_.topology) {
        for (auto &device_id : group) {
          if (previous.empty()) {
            continue;
          }
          if (boost::iequals(device_id, previous)) {
            device_id = resolved;
          }
        }
      }
    }

    for (auto &entry : current_request_.monitor_positions) {
      if (!previous.empty() && boost::iequals(entry.first, previous)) {
        entry.first = resolved;
      }
    }
  }

  void StateMachine::set_state_observer(StateObserver observer) {
    observer_ = std::move(observer);
  }

  void StateMachine::set_apply_result_callback(std::function<void(ApplyStatus, std::uint64_t, std::uint64_t)> callback) {
    apply_result_callback_ = std::move(callback);
  }

  void StateMachine::set_verification_result_callback(std::function<void(bool, std::uint64_t, std::uint64_t)> callback) {
    verification_result_callback_ = std::move(callback);
  }

  void StateMachine::set_snapshot_result_callback(std::function<void(std::uint64_t, std::uint64_t, bool)> callback) {
    snapshot_result_callback_ = std::move(callback);
  }

  void StateMachine::set_disarm_result_callback(std::function<void(std::uint64_t, std::uint64_t, bool)> callback) {
    disarm_result_callback_ = std::move(callback);
  }

  void StateMachine::set_exit_callback(std::function<void(int)> callback) {
    exit_callback_ = std::move(callback);
  }

  void StateMachine::set_snapshot_blacklist(std::set<std::string> blacklist) {
    restore_state_.set_exclusions({blacklist.begin(), blacklist.end()});
  }

  State StateMachine::state() const {
    return state_;
  }

  bool StateMachine::recovery_armed() const {
    return recovery_armed_;
  }

  void StateMachine::handle_message(const Message &message) {
    std::visit([this](const auto &payload) {
      using T = std::decay_t<decltype(payload)>;
      if constexpr (std::is_same_v<T, ApplyCommand>) {
        handle_apply_command(payload);
      } else if constexpr (std::is_same_v<T, RevertCommand>) {
        handle_revert_command(payload);
      } else if constexpr (std::is_same_v<T, DisarmCommand>) {
        handle_disarm_command(payload);
      } else if constexpr (std::is_same_v<T, ExportGoldenCommand>) {
        handle_export_golden(payload);
      } else if constexpr (std::is_same_v<T, SnapshotCurrentCommand>) {
        handle_snapshot_current(payload);
      } else if constexpr (std::is_same_v<T, ResetCommand>) {
        handle_reset_command(payload);
      } else if constexpr (std::is_same_v<T, PingCommand>) {
        handle_ping_command(payload);
      } else if constexpr (std::is_same_v<T, StopCommand>) {
        handle_stop_command(payload);
      } else if constexpr (std::is_same_v<T, ApplyCompleted>) {
        handle_apply_completed(payload);
      } else if constexpr (std::is_same_v<T, VerificationCompleted>) {
        handle_verification_completed(payload);
      } else if constexpr (std::is_same_v<T, RecoveryCompleted>) {
        handle_recovery_completed(payload);
      } else if constexpr (std::is_same_v<T, RecoveryValidationCompleted>) {
        handle_recovery_validation_completed(payload);
      } else if constexpr (std::is_same_v<T, DisplayEventMessage>) {
        handle_display_event(payload);
      } else if constexpr (std::is_same_v<T, HelperEventMessage>) {
        handle_helper_event(payload);
      }
    },
               message);
  }

  void StateMachine::handle_apply_command(const ApplyCommand &command) {
    BOOST_LOG(info) << "Display helper: received Apply command"
                    << (command.request.configuration ? " with configuration" : " without configuration")
                    << ", prefer_golden_first=" << (command.request.prefer_golden_first ? "true" : "false")
                    << (command.request.virtual_layout ? ", virtual_layout=" + *command.request.virtual_layout : "");

    if (command.request.expires_at && system_.now() >= *command.request.expires_at) {
      BOOST_LOG(info) << "Display helper: correlated APPLY expired before state-machine processing.";
      if (apply_result_callback_) {
        apply_result_callback_(ApplyStatus::Expired, command.request_id, command.connection_epoch);
      }
      return;
    }

    // A live replacement Apply supersedes a disconnect decision retained while
    // an older worker drained.
    pending_disconnect_revert_.reset();

    system_.cancel_pending_display_mutations();

    // Linearize cancellation with the recovery worker's entry into
    // SetDisplayConfig. If mutation won, preserve the existing snapshot chain;
    // if cancellation won during grace/read-only work, fallback capture is safe.
    const bool superseding_apply_worker =
      restore_state_.apply_workers_active.load(std::memory_order_acquire) != 0;
    const bool superseding_unconfirmed_restore =
      restore_state_.mutation_guard.supersede_for_apply([&]() {
        system_.cancel_operations();
      });
    const bool baseline_capture_unsafe = superseding_apply_worker || superseding_unconfirmed_restore;

    // A new APPLY supersedes any pending restore via IPC instead of forcing a
    // helper restart (72b0d996). Disarm the scheduler after fencing its worker.
    scheduler_.disarm();
    restore_state_.reset_request_progress();

    apply_attempt_ = 1;
    apply_result_sent_ = false;
    current_apply_reports_results_ = true;
    current_request_ = command.request;
    current_request_.mutation_committed = std::make_shared<std::atomic<bool>>(command.request.deadline_committed);
    current_apply_request_id_ = command.request_id;
    current_apply_connection_epoch_ = command.connection_epoch;
    if (command.snapshot_blacklist) {
      current_request_.staged_exclusions = std::vector<std::string> {
        command.snapshot_blacklist->begin(),
        command.snapshot_blacklist->end()
      };
    }
    expected_topology_.reset();

    // The session baseline is normally captured earlier via SnapshotCurrent. A
    // helper restart or rejected capture can still leave no current tier, which
    // used to leave REVERT with nothing to restore and strand the user on the
    // session-only display layout (f3841ad8). Capture the pre-apply state here as
    // a fallback whenever no baseline exists yet and no restore handoff is active.
    if (!snapshots_.tier_exists(SnapshotTier::Current) && baseline_capture_unsafe) {
      BOOST_LOG(info) << "Display helper: skipping pre-apply baseline capture while superseded display work drains; preserving previous/golden snapshots.";
    } else if (!snapshots_.tier_exists(SnapshotTier::Current)) {
      auto capture_lease = restore_state_.mutation_guard.try_begin_capture();
      if (!capture_lease) {
        BOOST_LOG(info) << "Display helper: skipping pre-apply baseline capture because restore mutation safety changed.";
      } else {
        BOOST_LOG(warning) << "Display helper: no session baseline present at APPLY; capturing pre-apply baseline now.";
        const auto capture_exclusions = current_request_.staged_exclusions.value_or(exclusions_vector());
        if (!snapshots_.refresh_current_preserving_previous(capture_exclusions, current_request_.expires_at)) {
          BOOST_LOG(warning) << "Display helper: pre-apply baseline capture failed; REVERT may have nothing to restore.";
        }
      }
    }

    transition(State::InProgress, ApplyAction::Apply);
    apply_.dispatch_apply(current_request_, std::chrono::milliseconds(0), false);
  }

  void StateMachine::handle_revert_command(const RevertCommand &command) {
    if (command.from_disconnect &&
        restore_state_.apply_workers_active.load(std::memory_order_acquire) != 0) {
      // The request policy is published at the exact mutation boundary. Do not
      // read the previous session's policy while a new Apply can still commit;
      // cancel preflight and resolve after every Apply worker has drained.
      pending_disconnect_revert_ = command;
      system_.cancel_operations();
      BOOST_LOG(info) << "Display helper: deferring disconnect restore decision until Apply drains.";
      return;
    }
    // Disconnect-triggered reverts honor the restore-on-disconnect policy: a
    // paused stream with revert_on_disconnect=false must preserve its display
    // state (3b7a52c4 / 0add1f80). Explicit client REVERTs always run.
    if (command.from_disconnect &&
        !restore_state_.restore_on_disconnect.load(std::memory_order_acquire) &&
        !restore_pending()) {
      BOOST_LOG(info) << "Display helper: disconnect with restore-on-disconnect disabled; not restoring.";
      return;
    }

    // If there is nothing to restore from, exit rather than spinning (legacy
    // restore_poll_proc early-exit; keeps --restore boot tasks from hanging).
    if (!snapshots_.tier_exists(SnapshotTier::Current) &&
        !snapshots_.tier_exists(SnapshotTier::Previous) &&
        !snapshots_.tier_exists(SnapshotTier::Golden)) {
      BOOST_LOG(info) << "Restore: no session/previous or golden snapshot present; nothing to restore.";
      if (exit_callback_) {
        exit_callback_(0);
      }
      return;
    }

    BOOST_LOG(info) << "Display helper: received Revert command, initiating recovery"
                    << (command.prefer_golden_if_current_missing ? " (prefer golden if current missing)." : ".");

    restore_state_.prefer_golden_if_current_missing.store(command.prefer_golden_if_current_missing, std::memory_order_release);
    if (command.always_restore_from_golden.has_value()) {
      restore_state_.always_restore_from_golden.store(*command.always_restore_from_golden, std::memory_order_release);
    }
    golden_health_.reset_request_tracking();

    system_.cancel_operations();
    recovery_armed_ = true;
    system_.arm_heartbeat();

    // Give Sunshine a short window to immediately start a new session and DISARM,
    // avoiding costly restore/apply thrash during fast client switching. The boot
    // --restore path runs immediately.
    const auto grace = command.immediate ? std::chrono::milliseconds(0) : std::chrono::milliseconds(5000);
    scheduler_.arm_primary(system_.now(), grace);
    start_recovery(grace, ApplyAction::Revert);
  }

  void StateMachine::handle_disarm_command(const DisarmCommand &command) {
    if (command.expires_at.has_value() && system_.now() >= *command.expires_at) {
      BOOST_LOG(info) << "DISARM request expired before state-machine processing.";
      if (command.request_id != 0 && disarm_result_callback_) {
        disarm_result_callback_(command.request_id, command.connection_epoch, false);
      }
      return;
    }

    // DISARM is a restore command, not permission to race a still-running APPLY.
    // The dispatcher cannot interrupt a blocking display apply either, so report
    // busy and let the stream-start barrier retry after the worker completes.
    if (state_ == State::InProgress ||
        restore_state_.apply_workers_active.load(std::memory_order_acquire) != 0) {
      BOOST_LOG(info) << "DISARM command deferred because display APPLY work is still in progress.";
      if (command.request_id != 0 && disarm_result_callback_) {
        disarm_result_callback_(command.request_id, command.connection_epoch, false);
      }
      return;
    }

    // The shared mutation gate makes this decision atomic with the worker's
    // entry into SetDisplayConfig. Busy means DISARM was not applied.
    if (!restore_state_.mutation_guard.try_disarm([&]() {
          system_.cancel_operations();
        })) {
      BOOST_LOG(info) << "DISARM command ignored because an unconfirmed restore attempt is still pending.";
      if (command.request_id != 0 && disarm_result_callback_) {
        disarm_result_callback_(command.request_id, command.connection_epoch, false);
      }
      return;
    }

    // The ACK is also a barrier for post-Apply HDR/display work. If that work
    // already entered Windows, wait for it to finish before declaring safety.
    system_.cancel_pending_display_mutations();

    BOOST_LOG(info) << "Display helper: received Disarm command, resetting state";

    scheduler_.disarm();
    pending_disconnect_revert_.reset();
    restore_state_.reset_request_progress();
    recovery_armed_ = false;
    system_.disarm_heartbeat();
    system_.delete_restore_task();
    apply_attempt_ = 0;
    apply_result_sent_ = false;
    expected_topology_.reset();
    recovery_snapshot_.reset();
    transition(State::Waiting, ApplyAction::Disarm);
    if (command.request_id != 0 && disarm_result_callback_) {
      disarm_result_callback_(command.request_id, command.connection_epoch, true);
    }
  }

  void StateMachine::handle_export_golden(const ExportGoldenCommand &command) {
    auto capture_lease = restore_state_.mutation_guard.try_begin_capture();
    if (state_ != State::Waiting || restore_pending() ||
        restore_state_.apply_workers_active.load(std::memory_order_acquire) != 0 ||
        !capture_lease) {
      BOOST_LOG(info) << "Skipping golden snapshot export while restore or restore handoff is pending.";
      return;
    }

    if (command.payload.update_exclusions || !command.payload.exclude_devices.empty()) {
      update_blacklist(command.payload.exclude_devices);
    }

    const bool saved = snapshots_.capture_filtered_and_save(SnapshotTier::Golden, exclusions_vector(), "export-golden");
    if (saved) {
      golden_health_.clear_status("snapshot exported");
    }
    BOOST_LOG(info) << "Export golden restore snapshot result=" << (saved ? "true" : "false");
  }

  void StateMachine::handle_snapshot_current(const SnapshotCurrentCommand &command) {
    if (command.expires_at && system_.now() >= *command.expires_at) {
      BOOST_LOG(info) << "Current session snapshot request expired before state-machine processing.";
      if (command.request_id != 0 && snapshot_result_callback_) {
        snapshot_result_callback_(command.request_id, command.connection_epoch, false);
      }
      return;
    }

    // Never overwrite the restore baseline while a restore is being worked on
    // (72b0d996), or after APPLY has superseded a blocking restore that may still
    // be draining: the snapshot would capture a transitional state.
    auto capture_lease = restore_state_.mutation_guard.try_begin_capture();
    if (state_ != State::Waiting || restore_pending() ||
        restore_state_.apply_workers_active.load(std::memory_order_acquire) != 0 ||
        !capture_lease) {
      BOOST_LOG(info) << "Skipping current session snapshot refresh while restore or restore handoff is pending.";
      if (command.request_id != 0 && snapshot_result_callback_) {
        snapshot_result_callback_(command.request_id, command.connection_epoch, false);
      }
      return;
    }

    if (command.payload.update_exclusions || !command.payload.exclude_devices.empty()) {
      update_blacklist(command.payload.exclude_devices);
    }

    const bool saved = snapshots_.refresh_current_preserving_previous(exclusions_vector(), command.expires_at);
    if (command.request_id != 0 && snapshot_result_callback_) {
      snapshot_result_callback_(command.request_id, command.connection_epoch, saved);
    }
  }

  void StateMachine::handle_reset_command(const ResetCommand &) {
    // Deprecated: no-op.
  }

  void StateMachine::handle_ping_command(const PingCommand &) {
    system_.record_ping();
  }

  void StateMachine::handle_stop_command(const StopCommand &) {
    BOOST_LOG(info) << "Display helper: received STOP command, exiting gracefully.";
    system_.cancel_pending_display_mutations();
    if (exit_callback_) {
      exit_callback_(0);
    }
  }

  void StateMachine::handle_apply_completed(const ApplyCompleted &completed) {
    // Keep the worker visible until its completion reaches the serialized FSM
    // queue. Commands queued ahead of this message must continue to treat the
    // Apply as active; stale completions still retire their worker here.
    const auto active_workers = restore_state_.apply_workers_active.load(std::memory_order_acquire);
    if (active_workers != 0) {
      restore_state_.apply_workers_active.fetch_sub(1, std::memory_order_acq_rel);
    }
    if (process_pending_disconnect_if_apply_drained()) {
      return;
    }
    if (is_stale(completed.generation)) {
      return;
    }

    expected_topology_ = completed.expected_topology;
    if (completed.deadline_committed) {
      current_request_.deadline_committed = true;
      restore_state_.mutation_guard.mark_apply_started();
    }

    if (completed.status == ApplyStatus::Ok) {
      if (current_apply_reports_results_ && !apply_result_sent_ && apply_result_callback_) {
        apply_result_callback_(completed.status, current_apply_request_id_, current_apply_connection_epoch_);
        apply_result_sent_ = true;
      }
      transition(State::Verification, ApplyAction::Apply, completed.status);
      apply_.dispatch_verification(current_request_, expected_topology_);
      return;
    }

    if (completed.status == ApplyStatus::NeedsVirtualDisplayReset) {
      const auto decision = apply_.maybe_reset_virtual_display(
        completed.status,
        completed.virtual_display_requested
      );
      if (decision == PolicyDecision::ResetVirtualDisplay) {
        apply_.dispatch_apply(current_request_, std::chrono::milliseconds(0), true);
        return;
      }
    }

    if (completed.status == ApplyStatus::Retryable || completed.status == ApplyStatus::VerificationFailed) {
      if (apply_.can_retry(apply_attempt_)) {
        const auto delay = apply_.retry_delay(apply_attempt_);
        ++apply_attempt_;
        apply_.dispatch_apply(current_request_, delay, false);
        return;
      }
    }

    if (current_apply_reports_results_ && !apply_result_sent_ && apply_result_callback_) {
      apply_result_callback_(completed.status, current_apply_request_id_, current_apply_connection_epoch_);
      apply_result_sent_ = true;
    }

    transition(State::Waiting, ApplyAction::Apply, completed.status);
  }

  void StateMachine::handle_verification_completed(const VerificationCompleted &completed) {
    if (is_stale(completed.generation)) {
      return;
    }

    if (current_apply_reports_results_ && verification_result_callback_) {
      verification_result_callback_(completed.success, current_apply_request_id_, current_apply_connection_epoch_);
    }

    if (completed.success) {
      restore_state_.mutation_guard.mark_superseding_apply_confirmed();
      recovery_armed_ = true;
      system_.arm_heartbeat();
      system_.refresh_shell();
      system_.blank_hdr_states(std::chrono::milliseconds(1000));

      // For virtual displays, enter monitoring state to handle device crashes
      if (current_request_.virtual_layout.has_value()) {
        transition(State::VirtualDisplayMonitoring, ApplyAction::Apply, ApplyStatus::Ok);
        return;
      }
    }

    transition(State::Waiting, ApplyAction::Apply, completed.success ? std::make_optional(ApplyStatus::Ok) : std::nullopt);
  }

  void StateMachine::handle_recovery_completed(const RecoveryCompleted &completed) {
    if (is_stale(completed.generation)) {
      return;
    }

    BOOST_LOG(info) << "Display helper: recovery operation completed, success=" << (completed.success ? "true" : "false")
                    << ", has_snapshot=" << (completed.snapshot ? "true" : "false");

    if (completed.success && completed.snapshot) {
      recovery_snapshot_ = completed.snapshot;
      transition(State::RecoveryValidation, ApplyAction::Revert);
      recovery_.dispatch_recovery_validation(*recovery_snapshot_);
      return;
    }

    BOOST_LOG(warning) << "Display helper: recovery failed or no valid snapshot found, entering event loop";
    scheduler_.on_attempt_failed(system_.now());
    transition(State::EventLoop, ApplyAction::Revert);
  }

  void StateMachine::handle_recovery_validation_completed(const RecoveryValidationCompleted &completed) {
    if (is_stale(completed.generation)) {
      return;
    }

    if (completed.success) {
      BOOST_LOG(info) << "Display helper: recovery validation succeeded, display settings restored.";
      restore_state_.mutation_guard.mark_topology_confirmed();
      recovery_armed_ = false;
      scheduler_.disarm();
      restore_state_.reset_request_progress();
      system_.disarm_heartbeat();
      system_.refresh_shell();
      system_.delete_restore_task();
      // Return to Waiting before signalling completion: the host may keep the
      // helper alive when a newer connection is active (72b0d996).
      transition(State::Waiting, ApplyAction::Revert, ApplyStatus::Ok);
      if (exit_callback_) {
        exit_callback_(0);
      }
      return;
    }

    BOOST_LOG(warning) << "Display helper: recovery validation failed, entering event loop for retry.";
    scheduler_.on_attempt_failed(system_.now());
    transition(State::EventLoop, ApplyAction::Revert);
  }

  void StateMachine::handle_display_event(const DisplayEventMessage &event) {
    if (is_stale(event.generation)) {
      BOOST_LOG(debug) << "Display helper: ignoring stale display event " << display_event_to_string(event.event);
      return;
    }

    BOOST_LOG(info) << "Display helper: received display event '" << display_event_to_string(event.event)
                    << "' in state " << state_to_string(state_);

    // Virtual display monitoring: re-apply configuration when device crashes/recovers
    if (state_ == State::VirtualDisplayMonitoring) {
      BOOST_LOG(info) << "Display helper: display event while monitoring virtual display, re-applying configuration.";
      system_.cancel_pending_display_mutations();
      retarget_virtual_display_device_id_if_needed();
      apply_attempt_ = 1;
      apply_result_sent_ = true;
      current_apply_reports_results_ = false;
      transition(State::InProgress, ApplyAction::Apply);
      apply_.dispatch_apply(current_request_, std::chrono::milliseconds(0), false);
      return;
    }

    // During active apply with virtual display, restart the apply operation
    if ((state_ == State::InProgress || state_ == State::Verification) &&
        current_request_.virtual_layout.has_value()) {
      if (current_request_.configuration) {
        const std::string resolved = virtual_display_.device_id();
        if (!resolved.empty() && boost::iequals(current_request_.configuration->m_device_id, resolved)) {
          // Applying modes/HDR to an IDD can generate display events that do not require a full restart.
          // Only restart when the virtual display device_id changes (e.g. device crash/recreate).
          BOOST_LOG(debug) << "Display helper: display event during virtual display apply ignored (device id unchanged).";
          return;
        }
      }

      static constexpr auto kDebounce = std::chrono::milliseconds(250);
      static constexpr auto kRestartDelay = std::chrono::milliseconds(100);

      const auto now = system_.now();
      if (last_virtual_apply_display_event_restart_.time_since_epoch().count() != 0) {
        const auto elapsed = now - last_virtual_apply_display_event_restart_;
        if (elapsed < kDebounce) {
          BOOST_LOG(debug) << "Display helper: coalescing display event during virtual display apply.";
          return;
        }
      }
      last_virtual_apply_display_event_restart_ = now;

      BOOST_LOG(info) << "Display helper: display event during virtual display apply, restarting apply.";

      // Cancel in-flight apply/verification work so their completions become stale.
      system_.cancel_pending_display_mutations();
      system_.cancel_operations();
      expected_topology_.reset();
      retarget_virtual_display_device_id_if_needed();
      transition(State::InProgress, ApplyAction::Apply);
      apply_.dispatch_apply(current_request_, kRestartDelay, false);
      return;
    }

    // Standard recovery from EventLoop state
    if (state_ != State::EventLoop) {
      return;
    }
    if (!recovery_armed_) {
      return;
    }

    // Display events reset the backoff and (re)open the event restore window;
    // the actual attempt fires immediately when allowed (legacy signal_restore_event).
    scheduler_.on_display_event(system_.now());
    if (scheduler_.should_attempt(system_.now())) {
      start_recovery(std::chrono::milliseconds(0), ApplyAction::Revert);
    }
  }

  void StateMachine::handle_helper_event(const HelperEventMessage &event) {
    if (is_stale(event.generation)) {
      return;
    }
    if (event.event != HelperEvent::HeartbeatTimeout) {
      return;
    }

    if (restore_state_.apply_workers_active.load(std::memory_order_acquire) != 0) {
      RevertCommand disconnect;
      disconnect.generation = event.generation;
      disconnect.from_disconnect = true;
      handle_revert_command(disconnect);
      return;
    }

    BOOST_LOG(warning) << "Display helper: heartbeat timeout detected in state " << state_to_string(state_)
                       << ", recovery_armed=" << (recovery_armed_ ? "true" : "false");

    if (!recovery_armed_) {
      return;
    }

    // Heartbeat loss means Sunshine crashed/hung: honor the restore-on-disconnect
    // policy the same way a broken pipe would (3b7a52c4).
    if (!restore_state_.restore_on_disconnect.load(std::memory_order_acquire) && !restore_pending()) {
      BOOST_LOG(info) << "Display helper: heartbeat lost with restore-on-disconnect disabled; not restoring.";
      return;
    }

    BOOST_LOG(info) << "Display helper: initiating recovery due to heartbeat timeout";
    golden_health_.reset_request_tracking();
    scheduler_.arm_primary(system_.now(), std::chrono::milliseconds(5000));
    start_recovery(std::chrono::milliseconds(5000), ApplyAction::Revert);
  }

  void StateMachine::handle_tick() {
    if (state_ != State::EventLoop || !recovery_armed_) {
      return;
    }

    const auto now = system_.now();
    if (scheduler_.window_just_expired(now)) {
      BOOST_LOG(info) << "Restore polling: window exhausted; pausing attempts until next event.";
      golden_health_.register_unresolved("restore window exhausted");
      return;
    }
    if (scheduler_.should_attempt(now)) {
      start_recovery(std::chrono::milliseconds(0), ApplyAction::Revert);
    }
  }

  bool StateMachine::restore_pending() const {
    return state_ == State::Recovery || state_ == State::RecoveryValidation || state_ == State::EventLoop;
  }

  void StateMachine::transition(State next, ApplyAction trigger, std::optional<ApplyStatus> status) {
    if (next == state_) {
      return;
    }

    if (status) {
      BOOST_LOG(info) << "Display helper: state transition " << state_to_string(state_)
                      << " -> " << state_to_string(next)
                      << " (trigger: " << action_to_string(trigger)
                      << ", status: " << apply_status_to_string(*status) << ")";
    } else {
      BOOST_LOG(info) << "Display helper: state transition " << state_to_string(state_)
                      << " -> " << state_to_string(next)
                      << " (trigger: " << action_to_string(trigger) << ")";
    }

    if (observer_) {
      observer_(StateTransition {
        .from = state_,
        .to = next,
        .trigger = trigger,
        .result_status = status,
        .timestamp = system_.now(),
      });
    }
    state_ = next;
  }

  bool StateMachine::is_stale(std::uint64_t generation) const {
    return generation != system_.current_generation();
  }
}  // namespace display_helper::v2

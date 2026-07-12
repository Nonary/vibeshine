/**
 * @file tests/unit/test_display_helper_v2_state_machine.cpp
 * @brief Unit tests for display helper v2 state machine and operations.
 */
#ifdef _WIN32

  #include "../tests_common.h"
  #include "src/platform/windows/display_helper_v2/async_dispatcher.h"
  #include "src/platform/windows/display_helper_v2/operations.h"
  #include "src/platform/windows/display_helper_v2/runtime_support.h"
  #include "src/platform/windows/display_helper_v2/snapshot.h"
  #include "src/platform/windows/display_helper_v2/state_machine.h"

  #include <deque>
  #include <map>
  #include <set>

namespace {
  class FakeClock final: public display_helper::v2::IClock {
  public:
    std::chrono::steady_clock::time_point now() override {
      return now_;
    }

    void sleep_for(std::chrono::milliseconds duration) override {
      now_ += duration;
    }

    void advance(std::chrono::milliseconds duration) {
      now_ += duration;
    }

  private:
    std::chrono::steady_clock::time_point now_ {std::chrono::steady_clock::now()};
  };

  class FakeDispatcher final: public display_helper::v2::IAsyncDispatcher {
  public:
    void dispatch_apply(
      const display_helper::v2::ApplyRequest &request,
      const display_helper::v2::CancellationToken &,
      std::chrono::milliseconds delay,
      bool reset_virtual_display,
      std::function<void()> mutation_commit,
      std::function<void(const display_helper::v2::ApplyOutcome &)> completion
    ) override {
      apply_request = request;
      apply_delay = delay;
      apply_reset_virtual_display = reset_virtual_display;
      apply_completion_queue.push_back(PendingApply {
        .mutation_commit = std::move(mutation_commit),
        .completion = std::move(completion),
      });
      apply_completion = [this](const display_helper::v2::ApplyOutcome &outcome) {
        if (apply_completion_queue.empty()) {
          return;
        }
        auto next = std::move(apply_completion_queue.front());
        apply_completion_queue.pop_front();
        if (outcome.deadline_committed && next.mutation_commit) {
          next.mutation_commit();
        }
        next.completion(outcome);
      };
      apply_dispatch_count += 1;
    }

    void dispatch_verification(
      const display_helper::v2::ApplyRequest &request,
      const std::optional<display_helper::v2::ActiveTopology> &expected_topology,
      const display_helper::v2::CancellationToken &,
      std::function<void(bool)> completion
    ) override {
      verification_request = request;
      verification_topology = expected_topology;
      verification_completion = std::move(completion);
      verification_dispatch_count += 1;
    }

    void dispatch_recovery(
      const display_helper::v2::CancellationToken &,
      std::chrono::milliseconds delay,
      std::function<void(const display_helper::v2::RecoveryOutcome &)> completion
    ) override {
      recovery_delay = delay;
      recovery_completion = std::move(completion);
      recovery_dispatch_count += 1;
    }

    void dispatch_recovery_validation(
      const display_helper::v2::Snapshot &snapshot,
      const display_helper::v2::CancellationToken &,
      std::function<void(bool)> completion
    ) override {
      recovery_validation_snapshot = snapshot;
      recovery_validation_completion = std::move(completion);
      recovery_validation_dispatch_count += 1;
    }

    display_helper::v2::ApplyRequest apply_request;
    std::chrono::milliseconds apply_delay {0};
    bool apply_reset_virtual_display = false;
    int apply_dispatch_count = 0;
    std::function<void(const display_helper::v2::ApplyOutcome &)> apply_completion;

    struct PendingApply {
      std::function<void()> mutation_commit;
      std::function<void(const display_helper::v2::ApplyOutcome &)> completion;
    };

    std::deque<PendingApply> apply_completion_queue;

    display_helper::v2::ApplyRequest verification_request;
    std::optional<display_helper::v2::ActiveTopology> verification_topology;
    int verification_dispatch_count = 0;
    std::function<void(bool)> verification_completion;

    int recovery_dispatch_count = 0;
    std::chrono::milliseconds recovery_delay {0};
    std::function<void(const display_helper::v2::RecoveryOutcome &)> recovery_completion;

    display_helper::v2::Snapshot recovery_validation_snapshot;
    int recovery_validation_dispatch_count = 0;
    std::function<void(bool)> recovery_validation_completion;
  };

  class FakeTaskManager final: public display_helper::v2::IScheduledTaskManager {
  public:
    bool create_restore_task(const std::wstring &) override {
      created += 1;
      return true;
    }

    bool delete_restore_task() override {
      deleted += 1;
      return true;
    }

    bool is_task_present() override {
      return created > deleted;
    }

    int created = 0;
    int deleted = 0;
  };

  class FakeWorkarounds final: public display_helper::v2::IPlatformWorkarounds {
  public:
    void blank_hdr_states(std::chrono::milliseconds delay) override {
      blank_calls += 1;
      last_delay = delay;
    }

    void cancel_pending_display_mutations() override {
      cancel_calls += 1;
    }

    void refresh_shell() override {
      refresh_calls += 1;
    }

    int blank_calls = 0;
    int cancel_calls = 0;
    int refresh_calls = 0;
    std::chrono::milliseconds last_delay {0};
  };

  class FakeVirtualDisplayDriver final: public display_helper::v2::IVirtualDisplayDriver {
  public:
    bool disable() override {
      disabled = true;
      return true;
    }

    bool enable() override {
      enabled = true;
      return true;
    }

    bool is_available() override {
      return available;
    }

    std::string device_id() override {
      return current_device_id;
    }

    bool available = true;
    bool disabled = false;
    bool enabled = false;
    std::string current_device_id {"virtual"};
  };

  class FakeDisplaySettings final: public display_helper::v2::IDisplaySettings {
  public:
    display_helper::v2::ApplyStatus apply(const display_device::SingleDisplayConfiguration &) override {
      return apply_status;
    }

    display_helper::v2::ApplyStatus apply_topology(const display_device::ActiveTopology &) override {
      return topology_status;
    }

    display_device::EnumeratedDeviceList enumerate(display_device::DeviceEnumerationDetail) override {
      return devices;
    }

    display_device::ActiveTopology capture_topology() override {
      return topology;
    }

    bool validate_topology(const display_device::ActiveTopology &topology_input) override {
      return valid_topology_ids.count(extract_id(topology_input)) > 0;
    }

    display_device::DisplaySettingsSnapshot capture_snapshot() override {
      if (capture_callback) {
        capture_callback();
      }
      return snapshot;
    }

    bool apply_snapshot(const display_device::DisplaySettingsSnapshot &snapshot_input) override {
      apply_snapshot_calls += 1;
      return apply_snapshot_ids.count(extract_id(snapshot_input.m_topology)) > 0;
    }

    bool snapshot_matches_current(const display_device::DisplaySettingsSnapshot &snapshot_input) override {
      const auto id = extract_id(snapshot_input.m_topology);
      auto &calls = match_calls[id];
      if (calls < static_cast<int>(match_sequence[id].size())) {
        return match_sequence[id][calls++];
      }
      return false;
    }

    bool configuration_matches(const display_device::SingleDisplayConfiguration &) override {
      return configuration_matches_result;
    }

    bool set_display_origin(const std::string &, const display_device::Point &) override {
      return true;
    }

    std::optional<display_device::ActiveTopology> compute_expected_topology(
      const display_device::SingleDisplayConfiguration &,
      const std::optional<display_device::ActiveTopology> &
    ) override {
      return expected_topology;
    }

    bool is_topology_same(const display_device::ActiveTopology &, const display_device::ActiveTopology &) override {
      return topology_same_result;
    }

    static std::string extract_id(const display_device::ActiveTopology &topology_input) {
      if (!topology_input.empty() && !topology_input.front().empty()) {
        return topology_input.front().front();
      }
      return {};
    }

    display_helper::v2::ApplyStatus apply_status = display_helper::v2::ApplyStatus::Ok;
    display_helper::v2::ApplyStatus topology_status = display_helper::v2::ApplyStatus::Ok;
    display_device::EnumeratedDeviceList devices;
    display_device::ActiveTopology topology;
    std::set<std::string> valid_topology_ids;
    std::set<std::string> apply_snapshot_ids;
    std::map<std::string, std::vector<bool>> match_sequence;
    std::map<std::string, int> match_calls;
    display_device::DisplaySettingsSnapshot snapshot;
    bool configuration_matches_result = true;
    std::optional<display_device::ActiveTopology> expected_topology;
    bool topology_same_result = true;
    int apply_snapshot_calls = 0;
    std::function<void()> capture_callback;
  };

  display_device::DisplaySettingsSnapshot make_snapshot(const std::string &id) {
    display_device::DisplaySettingsSnapshot snapshot;
    snapshot.m_topology.push_back({id});
    snapshot.m_modes[id] = display_device::DisplayMode {};
    snapshot.m_hdr_states[id] = std::nullopt;
    return snapshot;
  }

  class ControlledSnapshotStorage final: public display_helper::v2::InMemorySnapshotStorage {
  public:
    bool save(
      display_helper::v2::SnapshotTier tier,
      const display_helper::v2::Snapshot &snapshot
    ) override {
      if (tier == display_helper::v2::SnapshotTier::Current && fail_current_save) {
        return false;
      }
      const bool saved = InMemorySnapshotStorage::save(tier, snapshot);
      if (saved &&
          tier == display_helper::v2::SnapshotTier::Previous &&
          expire_after_next_previous_save && clock) {
        expire_after_next_previous_save = false;
        clock->advance(std::chrono::seconds(1));
      }
      return saved;
    }

    FakeClock *clock = nullptr;
    bool fail_current_save = false;
    bool expire_after_next_previous_save = false;
  };

  struct StateMachineHarness {
    FakeClock clock;
    display_helper::v2::ApplyPolicy policy {clock};
    FakeDispatcher dispatcher;
    FakeVirtualDisplayDriver virtual_display;
    FakeDisplaySettings display_settings;
    display_helper::v2::SnapshotService snapshot_service {display_settings};
    display_helper::v2::InMemorySnapshotStorage storage;
    display_helper::v2::SnapshotPersistence snapshot_persistence {storage};
    FakeWorkarounds workarounds;
    FakeTaskManager task_manager;
    display_helper::v2::HeartbeatMonitor heartbeat {clock};
    display_helper::v2::CancellationSource cancellation;
    display_helper::v2::GoldenHealth golden_health {std::filesystem::path {}};
    display_helper::v2::RestoreState restore_state;
    std::deque<display_helper::v2::Message> messages;
    std::optional<display_helper::v2::ApplyStatus> apply_result;
    std::uint64_t apply_result_request_id = 0;
    std::uint64_t apply_result_epoch = 0;
    std::optional<bool> verification_result;
    std::uint64_t verification_result_request_id = 0;
    std::uint64_t verification_result_epoch = 0;
    std::optional<bool> snapshot_result;
    std::optional<bool> disarm_result;
    std::optional<int> exit_code;

    display_helper::v2::SystemPorts system_ports {workarounds, task_manager, heartbeat, clock, cancellation};
    display_helper::v2::ApplyPipeline apply_pipeline {
      dispatcher,
      policy,
      system_ports,
      restore_state,
      [this](display_helper::v2::Message message) {
        messages.push_back(std::move(message));
      }
    };
    display_helper::v2::RecoveryPipeline recovery_pipeline {
      dispatcher,
      system_ports,
      [this](display_helper::v2::Message message) {
        messages.push_back(std::move(message));
      }
    };
    display_helper::v2::SnapshotLedger snapshot_ledger {snapshot_service, snapshot_persistence, clock};

    display_helper::v2::StateMachine state_machine {
      apply_pipeline,
      recovery_pipeline,
      snapshot_ledger,
      system_ports,
      virtual_display,
      golden_health,
      restore_state
    };

    void seed_current_snapshot(const std::string &id = "seed") {
      display_device::DisplaySettingsSnapshot snapshot;
      snapshot.m_topology.push_back({id});
      snapshot.m_modes[id] = display_device::DisplayMode {};
      snapshot.m_hdr_states[id] = std::nullopt;
      ASSERT_TRUE(storage.save(display_helper::v2::SnapshotTier::Current, snapshot));
    }

    void add_active_device(const std::string &id) {
      display_device::EnumeratedDevice device;
      device.m_device_id = id;
      device.m_display_name = "\\\\.\\DISPLAY_" + id;
      device.m_friendly_name = "Fake Monitor";
      device.m_info = display_device::EnumeratedDevice::Info {};
      display_settings.devices.push_back(std::move(device));
    }

    StateMachineHarness() {
      state_machine.set_apply_result_callback([this](display_helper::v2::ApplyStatus status, std::uint64_t request_id, std::uint64_t epoch) {
        apply_result = status;
        apply_result_request_id = request_id;
        apply_result_epoch = epoch;
      });
      state_machine.set_verification_result_callback([this](bool success, std::uint64_t request_id, std::uint64_t epoch) {
        verification_result = success;
        verification_result_request_id = request_id;
        verification_result_epoch = epoch;
      });
      state_machine.set_snapshot_result_callback([this](std::uint64_t, std::uint64_t, bool success) {
        snapshot_result = success;
      });
      state_machine.set_disarm_result_callback([this](std::uint64_t, std::uint64_t, bool accepted) {
        disarm_result = accepted;
      });
      state_machine.set_exit_callback([this](int code) {
        exit_code = code;
      });
    }

    void drain_messages() {
      while (!messages.empty()) {
        auto message = std::move(messages.front());
        messages.pop_front();
        state_machine.handle_message(message);
      }
    }
  };
}  // namespace

TEST(DisplayHelperV2Debounce, CoalescesUntilDelay) {
  display_helper::v2::DebouncedTrigger debouncer(std::chrono::milliseconds(500));
  auto start = std::chrono::steady_clock::now();

  debouncer.notify(start);
  EXPECT_FALSE(debouncer.should_fire(start));
  EXPECT_FALSE(debouncer.should_fire(start + std::chrono::milliseconds(250)));
  EXPECT_TRUE(debouncer.should_fire(start + std::chrono::milliseconds(500)));
  EXPECT_FALSE(debouncer.pending());
}

TEST(DisplayHelperV2Heartbeat, TriggersTimeoutWhenArmed) {
  FakeClock clock;
  display_helper::v2::HeartbeatMonitor monitor(clock);

  monitor.arm();
  clock.advance(std::chrono::seconds(31));
  EXPECT_TRUE(monitor.check_timeout());
  EXPECT_FALSE(monitor.check_timeout());
}

TEST(DisplayHelperV2Heartbeat, IgnoresTimeoutWhenDisarmed) {
  FakeClock clock;
  display_helper::v2::HeartbeatMonitor monitor(clock);

  clock.advance(std::chrono::seconds(40));
  EXPECT_FALSE(monitor.check_timeout());
}

TEST(DisplayHelperV2MessageQueue, SelectivePurgePreservesCleanupAndCompletions) {
  display_helper::v2::MessageQueue<display_helper::v2::Message> queue;
  display_helper::v2::ApplyRequest request;
  request.configuration = display_device::SingleDisplayConfiguration {};
  queue.push(display_helper::v2::ApplyCommand {request, 1});
  display_helper::v2::RevertCommand client_revert {.generation = 1};
  client_revert.client_connection_epoch = 7;
  queue.push(client_revert);
  display_helper::v2::RevertCommand disconnect_revert {.generation = 1};
  disconnect_revert.from_disconnect = true;
  queue.push(disconnect_revert);
  queue.push(display_helper::v2::RevertCommand {.generation = 1});
  queue.push(display_helper::v2::ApplyCompleted {
    .status = display_helper::v2::ApplyStatus::Fatal,
    .generation = 1,
  });
  queue.push(display_helper::v2::PingCommand {1});
  queue.push(display_helper::v2::StopCommand {1});

  const auto client_revert_epoch =
    display_helper::v2::connection_bound_epoch(display_helper::v2::Message {client_revert});
  ASSERT_TRUE(client_revert_epoch.has_value());
  EXPECT_EQ(*client_revert_epoch, 7u);
  EXPECT_FALSE(display_helper::v2::connection_bound_epoch(display_helper::v2::Message {disconnect_revert}));
  EXPECT_FALSE(display_helper::v2::connection_bound_epoch(display_helper::v2::Message {display_helper::v2::RevertCommand {.generation = 1}}));
  const auto stop_epoch = display_helper::v2::connection_bound_epoch(
    display_helper::v2::Message {display_helper::v2::StopCommand {.generation = 1}}
  );
  ASSERT_TRUE(stop_epoch.has_value());
  EXPECT_EQ(*stop_epoch, 0u);

  EXPECT_EQ(queue.erase_if(display_helper::v2::is_connection_bound_command), 4u);
  ASSERT_EQ(queue.size(), 3u);
  auto first = queue.try_pop();
  auto second = queue.try_pop();
  auto third = queue.try_pop();
  ASSERT_TRUE(first.has_value());
  ASSERT_TRUE(second.has_value());
  ASSERT_TRUE(third.has_value());
  EXPECT_TRUE(std::holds_alternative<display_helper::v2::RevertCommand>(*first));
  EXPECT_TRUE(std::get<display_helper::v2::RevertCommand>(*first).from_disconnect);
  EXPECT_TRUE(std::holds_alternative<display_helper::v2::RevertCommand>(*second));
  EXPECT_FALSE(std::get<display_helper::v2::RevertCommand>(*second).client_connection_epoch);
  EXPECT_TRUE(std::holds_alternative<display_helper::v2::ApplyCompleted>(*third));
}

TEST(DisplayHelperV2ApplyOperation, ArmsMutationCallbackOnlyAfterCommit) {
  FakeClock clock;
  FakeDisplaySettings display;
  int mutation_commits = 0;
  display_helper::v2::ApplyOperation operation(
    display,
    clock,
    [&]() {
      ++mutation_commits;
    }
  );

  display_helper::v2::CancellationSource cancellation;
  display_helper::v2::ApplyRequest expired;
  expired.configuration = display_device::SingleDisplayConfiguration {};
  expired.expires_at = clock.now();
  const auto expired_outcome = operation.run(expired, cancellation.token());
  EXPECT_EQ(expired_outcome.status, display_helper::v2::ApplyStatus::Expired);
  EXPECT_FALSE(expired_outcome.deadline_committed);
  EXPECT_EQ(mutation_commits, 0);

  display_helper::v2::ApplyRequest valid;
  valid.configuration = display_device::SingleDisplayConfiguration {};
  valid.expires_at = clock.now() + std::chrono::seconds(1);
  const auto valid_outcome = operation.run(valid, cancellation.token());
  EXPECT_TRUE(valid_outcome.deadline_committed);
  EXPECT_EQ(mutation_commits, 1);
}

TEST(DisplayHelperV2StateMachine, ApplyTransitionsAndVerifies) {
  StateMachineHarness harness;
  display_helper::v2::ApplyRequest request;
  request.configuration = display_device::SingleDisplayConfiguration {};

  display_helper::v2::ApplyCommand command {request, harness.cancellation.current_generation()};
  command.request_id = 42;
  command.connection_epoch = 7;
  harness.state_machine.handle_message(command);
  EXPECT_EQ(harness.state_machine.state(), display_helper::v2::State::InProgress);
  EXPECT_EQ(harness.task_manager.created, 0);
  ASSERT_TRUE(harness.dispatcher.apply_completion);

  display_helper::v2::ApplyOutcome outcome;
  outcome.status = display_helper::v2::ApplyStatus::Ok;
  outcome.deadline_committed = true;
  harness.dispatcher.apply_completion(outcome);
  harness.drain_messages();

  EXPECT_EQ(harness.state_machine.state(), display_helper::v2::State::Verification);
  ASSERT_TRUE(harness.dispatcher.verification_completion);

  harness.dispatcher.verification_completion(true);
  harness.drain_messages();

  EXPECT_EQ(harness.state_machine.state(), display_helper::v2::State::Waiting);
  ASSERT_TRUE(harness.verification_result.has_value());
  EXPECT_TRUE(harness.verification_result.value());
  EXPECT_TRUE(harness.state_machine.recovery_armed());
  EXPECT_EQ(harness.workarounds.refresh_calls, 1);
  EXPECT_EQ(harness.workarounds.blank_calls, 1);
  EXPECT_EQ(harness.workarounds.last_delay, std::chrono::milliseconds(1000));
  ASSERT_TRUE(harness.apply_result.has_value());
  EXPECT_EQ(harness.apply_result, display_helper::v2::ApplyStatus::Ok);
  EXPECT_EQ(harness.apply_result_request_id, 42u);
  EXPECT_EQ(harness.apply_result_epoch, 7u);
  EXPECT_EQ(harness.verification_result_request_id, 42u);
  EXPECT_EQ(harness.verification_result_epoch, 7u);
}

TEST(DisplayHelperV2StateMachine, ApplyRetriesOnRetryable) {
  StateMachineHarness harness;
  display_helper::v2::ApplyRequest request;
  request.configuration = display_device::SingleDisplayConfiguration {};

  harness.state_machine.handle_message(display_helper::v2::ApplyCommand {request, harness.cancellation.current_generation()});

  display_helper::v2::ApplyOutcome outcome;
  outcome.status = display_helper::v2::ApplyStatus::Retryable;
  harness.dispatcher.apply_completion(outcome);
  harness.drain_messages();

  EXPECT_EQ(harness.state_machine.state(), display_helper::v2::State::InProgress);
  EXPECT_EQ(harness.dispatcher.apply_delay, std::chrono::milliseconds(300));
  EXPECT_FALSE(harness.apply_result.has_value());
}

TEST(DisplayHelperV2StateMachine, ApplyStopsAfterMaxRetries) {
  StateMachineHarness harness;
  display_helper::v2::ApplyRequest request;
  request.configuration = display_device::SingleDisplayConfiguration {};

  harness.state_machine.handle_message(display_helper::v2::ApplyCommand {request, harness.cancellation.current_generation()});

  display_helper::v2::ApplyOutcome outcome;
  outcome.status = display_helper::v2::ApplyStatus::Retryable;

  for (int i = 0; i < 3; ++i) {
    harness.dispatcher.apply_completion(outcome);
    harness.drain_messages();
  }

  EXPECT_EQ(harness.state_machine.state(), display_helper::v2::State::Waiting);
  ASSERT_TRUE(harness.apply_result.has_value());
  EXPECT_EQ(harness.apply_result, display_helper::v2::ApplyStatus::Retryable);
}

TEST(DisplayHelperV2StateMachine, VirtualDisplayResetTriggersDispatch) {
  StateMachineHarness harness;
  display_helper::v2::ApplyRequest request;
  request.configuration = display_device::SingleDisplayConfiguration {};
  request.virtual_layout = "extended";

  harness.state_machine.handle_message(display_helper::v2::ApplyCommand {request, harness.cancellation.current_generation()});

  display_helper::v2::ApplyOutcome outcome;
  outcome.status = display_helper::v2::ApplyStatus::NeedsVirtualDisplayReset;
  outcome.virtual_display_requested = true;

  harness.dispatcher.apply_completion(outcome);
  harness.drain_messages();

  EXPECT_TRUE(harness.dispatcher.apply_reset_virtual_display);
}

// Test: Display event during APPLY with virtual display triggers re-apply.
// When a virtual display device crashes/reappears during an active apply operation,
// the state machine should restart the apply.
TEST(DisplayHelperV2StateMachine, DisplayEventDuringApplyWithVirtualDisplayTriggersReapply) {
  StateMachineHarness harness;
  harness.virtual_display.current_device_id = "virtual_new";
  display_helper::v2::ApplyRequest request;
  request.configuration = display_device::SingleDisplayConfiguration {};
  request.configuration->m_device_id = "virtual_old";
  request.topology = display_device::ActiveTopology {{request.configuration->m_device_id}};
  request.monitor_positions.emplace_back(request.configuration->m_device_id, display_device::Point {0, 0});
  request.virtual_layout = "extended";  // Virtual display requested

  harness.state_machine.handle_message(display_helper::v2::ApplyCommand {request, harness.cancellation.current_generation()});
  EXPECT_EQ(harness.state_machine.state(), display_helper::v2::State::InProgress);

  const int apply_dispatches_before = harness.dispatcher.apply_dispatch_count;

  // Simulate display device removal/arrival (virtual display crash and recovery)
  harness.state_machine.handle_message(display_helper::v2::DisplayEventMessage {display_helper::v2::DisplayEvent::DeviceArrival, harness.cancellation.current_generation()});

  // State machine should restart the apply for virtual displays
  EXPECT_EQ(harness.state_machine.state(), display_helper::v2::State::InProgress);
  EXPECT_EQ(harness.dispatcher.apply_dispatch_count, apply_dispatches_before + 1);
  ASSERT_TRUE(harness.dispatcher.apply_request.configuration.has_value());
  EXPECT_EQ(harness.dispatcher.apply_request.configuration->m_device_id, "virtual_new");
  ASSERT_TRUE(harness.dispatcher.apply_request.topology.has_value());
  ASSERT_FALSE(harness.dispatcher.apply_request.topology->empty());
  ASSERT_FALSE(harness.dispatcher.apply_request.topology->front().empty());
  EXPECT_EQ(harness.dispatcher.apply_request.topology->front().front(), "virtual_new");
  ASSERT_FALSE(harness.dispatcher.apply_request.monitor_positions.empty());
  EXPECT_EQ(harness.dispatcher.apply_request.monitor_positions.front().first, "virtual_new");
}

// Test: Successful apply with virtual display enters VirtualDisplayMonitoring state.
// This state monitors for device crashes and triggers re-apply when needed.
TEST(DisplayHelperV2StateMachine, VirtualDisplayEntersMonitoringStateAfterSuccessfulApply) {
  StateMachineHarness harness;
  display_helper::v2::ApplyRequest request;
  request.configuration = display_device::SingleDisplayConfiguration {};
  request.virtual_layout = "extended";  // Virtual display requested

  // Complete a successful apply cycle
  harness.state_machine.handle_message(display_helper::v2::ApplyCommand {request, harness.cancellation.current_generation()});

  display_helper::v2::ApplyOutcome apply_ok;
  apply_ok.status = display_helper::v2::ApplyStatus::Ok;
  harness.dispatcher.apply_completion(apply_ok);
  harness.drain_messages();

  harness.dispatcher.verification_completion(true);
  harness.drain_messages();

  // After successful verification with virtual display, we're in VirtualDisplayMonitoring
  EXPECT_EQ(harness.state_machine.state(), display_helper::v2::State::VirtualDisplayMonitoring);
  EXPECT_TRUE(harness.state_machine.recovery_armed());
}

// Test: Display event in VirtualDisplayMonitoring state triggers re-apply.
// When a virtual display device crashes/reappears during an active streaming session,
// the state machine should re-apply the streaming configuration.
TEST(DisplayHelperV2StateMachine, DisplayEventInMonitoringStateTriggersReapply) {
  StateMachineHarness harness;
  display_helper::v2::ApplyRequest request;
  request.configuration = display_device::SingleDisplayConfiguration {};
  request.configuration->m_device_id = "virtual_old";
  request.topology = display_device::ActiveTopology {{request.configuration->m_device_id}};
  request.monitor_positions.emplace_back(request.configuration->m_device_id, display_device::Point {0, 0});
  request.virtual_layout = "extended";  // Virtual display requested

  // Complete a successful apply cycle to enter VirtualDisplayMonitoring
  harness.state_machine.handle_message(display_helper::v2::ApplyCommand {request, harness.cancellation.current_generation()});

  display_helper::v2::ApplyOutcome apply_ok;
  apply_ok.status = display_helper::v2::ApplyStatus::Ok;
  harness.dispatcher.apply_completion(apply_ok);
  harness.drain_messages();

  harness.dispatcher.verification_completion(true);
  harness.drain_messages();

  EXPECT_EQ(harness.state_machine.state(), display_helper::v2::State::VirtualDisplayMonitoring);

  const int apply_dispatches_before = harness.dispatcher.apply_dispatch_count;

  harness.virtual_display.current_device_id = "virtual_new";
  // Simulate display device removal/arrival (virtual display crash and recovery)
  harness.state_machine.handle_message(display_helper::v2::DisplayEventMessage {display_helper::v2::DisplayEvent::DeviceArrival, harness.cancellation.current_generation()});

  // State machine should re-apply the virtual display configuration
  EXPECT_EQ(harness.state_machine.state(), display_helper::v2::State::InProgress);
  EXPECT_EQ(harness.dispatcher.apply_dispatch_count, apply_dispatches_before + 1);
  ASSERT_TRUE(harness.dispatcher.apply_request.configuration.has_value());
  EXPECT_EQ(harness.dispatcher.apply_request.configuration->m_device_id, "virtual_new");
  ASSERT_TRUE(harness.dispatcher.apply_request.topology.has_value());
  ASSERT_FALSE(harness.dispatcher.apply_request.topology->empty());
  ASSERT_FALSE(harness.dispatcher.apply_request.topology->front().empty());
  EXPECT_EQ(harness.dispatcher.apply_request.topology->front().front(), "virtual_new");
  ASSERT_FALSE(harness.dispatcher.apply_request.monitor_positions.empty());
  EXPECT_EQ(harness.dispatcher.apply_request.monitor_positions.front().first, "virtual_new");
}

TEST(DisplayHelperV2StateMachine, ClientApplyReportsResultsAfterVirtualMonitorReapply) {
  StateMachineHarness harness;
  display_helper::v2::ApplyRequest virtual_request;
  virtual_request.configuration = display_device::SingleDisplayConfiguration {};
  virtual_request.configuration->m_device_id = "virtual_old";
  virtual_request.virtual_layout = "extended";

  display_helper::v2::ApplyCommand initial {virtual_request, harness.cancellation.current_generation()};
  initial.request_id = 41;
  initial.connection_epoch = 3;
  harness.state_machine.handle_message(initial);

  display_helper::v2::ApplyOutcome apply_ok;
  apply_ok.status = display_helper::v2::ApplyStatus::Ok;
  harness.dispatcher.apply_completion(apply_ok);
  harness.drain_messages();
  harness.dispatcher.verification_completion(true);
  harness.drain_messages();
  ASSERT_EQ(harness.state_machine.state(), display_helper::v2::State::VirtualDisplayMonitoring);

  // Monitoring-triggered repair is intentionally uncorrelated and suppresses
  // client callbacks for that internal reapply.
  harness.virtual_display.current_device_id = "virtual_recreated";
  harness.state_machine.handle_message(
    display_helper::v2::DisplayEventMessage {
      display_helper::v2::DisplayEvent::DeviceArrival,
      harness.cancellation.current_generation()
    }
  );
  harness.dispatcher.apply_completion(apply_ok);
  harness.drain_messages();
  harness.dispatcher.verification_completion(true);
  harness.drain_messages();
  ASSERT_EQ(harness.state_machine.state(), display_helper::v2::State::VirtualDisplayMonitoring);

  harness.apply_result.reset();
  harness.verification_result.reset();

  display_helper::v2::ApplyRequest client_request;
  client_request.configuration = display_device::SingleDisplayConfiguration {};
  display_helper::v2::ApplyCommand client {client_request, harness.cancellation.current_generation()};
  client.request_id = 77;
  client.connection_epoch = 9;
  harness.state_machine.handle_message(client);
  harness.dispatcher.apply_completion(apply_ok);
  harness.drain_messages();
  harness.dispatcher.verification_completion(true);
  harness.drain_messages();

  ASSERT_TRUE(harness.apply_result.has_value());
  EXPECT_EQ(*harness.apply_result, display_helper::v2::ApplyStatus::Ok);
  EXPECT_EQ(harness.apply_result_request_id, 77u);
  EXPECT_EQ(harness.apply_result_epoch, 9u);
  ASSERT_TRUE(harness.verification_result.has_value());
  EXPECT_TRUE(*harness.verification_result);
  EXPECT_EQ(harness.verification_result_request_id, 77u);
  EXPECT_EQ(harness.verification_result_epoch, 9u);
}

// Test: Non-virtual display still goes to Waiting state after successful apply.
TEST(DisplayHelperV2StateMachine, NonVirtualDisplayGoesToWaitingState) {
  StateMachineHarness harness;
  display_helper::v2::ApplyRequest request;
  request.configuration = display_device::SingleDisplayConfiguration {};
  // No virtual_layout - regular display

  // Complete a successful apply cycle
  harness.state_machine.handle_message(display_helper::v2::ApplyCommand {request, harness.cancellation.current_generation()});

  display_helper::v2::ApplyOutcome apply_ok;
  apply_ok.status = display_helper::v2::ApplyStatus::Ok;
  harness.dispatcher.apply_completion(apply_ok);
  harness.drain_messages();

  harness.dispatcher.verification_completion(true);
  harness.drain_messages();

  // Non-virtual displays go to Waiting state (not VirtualDisplayMonitoring)
  EXPECT_EQ(harness.state_machine.state(), display_helper::v2::State::Waiting);
  EXPECT_TRUE(harness.state_machine.recovery_armed());
}

TEST(DisplayHelperV2StateMachine, RevertRunsRecoveryAndValidation) {
  StateMachineHarness harness;
  harness.seed_current_snapshot();

  harness.state_machine.handle_message(display_helper::v2::RevertCommand {harness.cancellation.current_generation()});
  EXPECT_EQ(harness.state_machine.state(), display_helper::v2::State::Recovery);
  // The boot-restore scheduled task must survive until the restore is confirmed:
  // a reboot mid-restore still needs it.
  EXPECT_EQ(harness.task_manager.deleted, 0);
  // Explicit REVERTs get a 5s grace window so a fast app switch can DISARM first.
  EXPECT_EQ(harness.dispatcher.recovery_delay, std::chrono::milliseconds(5000));

  display_helper::v2::RecoveryOutcome outcome;
  outcome.success = true;
  outcome.snapshot = make_snapshot("A");

  harness.dispatcher.recovery_completion(outcome);
  harness.drain_messages();

  EXPECT_EQ(harness.state_machine.state(), display_helper::v2::State::RecoveryValidation);
  ASSERT_TRUE(harness.dispatcher.recovery_validation_completion);

  harness.dispatcher.recovery_validation_completion(true);
  harness.drain_messages();

  ASSERT_TRUE(harness.exit_code.has_value());
  EXPECT_EQ(harness.exit_code.value(), 0);
  EXPECT_EQ(harness.task_manager.deleted, 1);
  EXPECT_EQ(harness.workarounds.refresh_calls, 1);
}

TEST(DisplayHelperV2StateMachine, RevertWithNoSnapshotsExitsImmediately) {
  StateMachineHarness harness;

  harness.state_machine.handle_message(display_helper::v2::RevertCommand {harness.cancellation.current_generation()});

  EXPECT_EQ(harness.dispatcher.recovery_dispatch_count, 0);
  ASSERT_TRUE(harness.exit_code.has_value());
  EXPECT_EQ(harness.exit_code.value(), 0);
}

TEST(DisplayHelperV2StateMachine, RecoveryFailureKeepsEventLoopArmed) {
  StateMachineHarness harness;
  harness.seed_current_snapshot();

  harness.state_machine.handle_message(display_helper::v2::RevertCommand {harness.cancellation.current_generation()});

  display_helper::v2::RecoveryOutcome recovery_fail;
  recovery_fail.success = false;
  harness.dispatcher.recovery_completion(recovery_fail);
  harness.drain_messages();

  EXPECT_EQ(harness.state_machine.state(), display_helper::v2::State::EventLoop);
  EXPECT_TRUE(harness.state_machine.recovery_armed());
  EXPECT_FALSE(harness.exit_code.has_value());
}

TEST(DisplayHelperV2StateMachine, RecoveryValidationFailureAllowsReconnectCycle) {
  StateMachineHarness harness;
  harness.seed_current_snapshot();
  display_helper::v2::ApplyRequest request;
  request.configuration = display_device::SingleDisplayConfiguration {};

  harness.state_machine.handle_message(display_helper::v2::RevertCommand {harness.cancellation.current_generation()});

  display_helper::v2::RecoveryOutcome recovery_ok;
  recovery_ok.success = true;
  recovery_ok.snapshot = make_snapshot("A");
  harness.dispatcher.recovery_completion(recovery_ok);
  harness.drain_messages();

  EXPECT_EQ(harness.state_machine.state(), display_helper::v2::State::RecoveryValidation);
  ASSERT_TRUE(harness.dispatcher.recovery_validation_completion);

  harness.dispatcher.recovery_validation_completion(false);
  harness.drain_messages();

  EXPECT_EQ(harness.state_machine.state(), display_helper::v2::State::EventLoop);
  EXPECT_TRUE(harness.state_machine.recovery_armed());
  EXPECT_FALSE(harness.exit_code.has_value());

  harness.state_machine.handle_message(display_helper::v2::DisarmCommand {harness.cancellation.current_generation()});
  EXPECT_EQ(harness.state_machine.state(), display_helper::v2::State::Waiting);
  EXPECT_FALSE(harness.state_machine.recovery_armed());

  harness.state_machine.handle_message(display_helper::v2::ApplyCommand {request, harness.cancellation.current_generation()});
  EXPECT_EQ(harness.state_machine.state(), display_helper::v2::State::InProgress);
  ASSERT_TRUE(harness.dispatcher.apply_completion);

  display_helper::v2::ApplyOutcome apply_ok;
  apply_ok.status = display_helper::v2::ApplyStatus::Ok;
  harness.dispatcher.apply_completion(apply_ok);
  harness.drain_messages();
  ASSERT_TRUE(harness.dispatcher.verification_completion);

  harness.dispatcher.verification_completion(true);
  harness.drain_messages();

  EXPECT_EQ(harness.state_machine.state(), display_helper::v2::State::Waiting);
  EXPECT_TRUE(harness.state_machine.recovery_armed());
  EXPECT_FALSE(harness.exit_code.has_value());
}

TEST(DisplayHelperV2StateMachine, DisarmBeforeApplyWhileRecovering) {
  StateMachineHarness harness;
  harness.seed_current_snapshot();
  std::vector<display_helper::v2::StateTransition> transitions;
  display_helper::v2::ApplyRequest request;
  request.configuration = display_device::SingleDisplayConfiguration {};

  harness.state_machine.set_state_observer([&transitions](display_helper::v2::StateTransition transition) {
    transitions.push_back(std::move(transition));
  });

  harness.state_machine.handle_message(display_helper::v2::RevertCommand {harness.cancellation.current_generation()});
  EXPECT_EQ(harness.state_machine.state(), display_helper::v2::State::Recovery);

  display_helper::v2::RecoveryOutcome recovery_fail;
  recovery_fail.success = false;
  harness.dispatcher.recovery_completion(recovery_fail);
  harness.drain_messages();

  EXPECT_EQ(harness.state_machine.state(), display_helper::v2::State::EventLoop);
  EXPECT_TRUE(harness.state_machine.recovery_armed());

  const int recovery_dispatches_before = harness.dispatcher.recovery_dispatch_count;
  harness.state_machine.handle_message(display_helper::v2::DisarmCommand {harness.cancellation.current_generation()});
  harness.state_machine.handle_message(display_helper::v2::ApplyCommand {request, harness.cancellation.current_generation()});

  EXPECT_EQ(harness.state_machine.state(), display_helper::v2::State::InProgress);
  EXPECT_FALSE(harness.state_machine.recovery_armed());
  EXPECT_EQ(harness.dispatcher.recovery_dispatch_count, recovery_dispatches_before);
  ASSERT_GE(transitions.size(), 2u);
  EXPECT_EQ(transitions[transitions.size() - 2].to, display_helper::v2::State::Waiting);
  EXPECT_EQ(transitions.back().to, display_helper::v2::State::InProgress);
}

TEST(DisplayHelperV2StateMachine, EventLoopTriggersRecovery) {
  StateMachineHarness harness;
  harness.seed_current_snapshot();
  display_helper::v2::ApplyRequest request;
  request.configuration = display_device::SingleDisplayConfiguration {};

  harness.state_machine.handle_message(display_helper::v2::ApplyCommand {request, harness.cancellation.current_generation()});

  display_helper::v2::ApplyOutcome apply_ok;
  apply_ok.status = display_helper::v2::ApplyStatus::Ok;
  harness.dispatcher.apply_completion(apply_ok);
  harness.drain_messages();
  harness.dispatcher.verification_completion(true);
  harness.drain_messages();

  harness.state_machine.handle_message(display_helper::v2::RevertCommand {harness.cancellation.current_generation()});

  display_helper::v2::RecoveryOutcome recovery_fail;
  recovery_fail.success = false;
  harness.dispatcher.recovery_completion(recovery_fail);
  harness.drain_messages();

  EXPECT_EQ(harness.state_machine.state(), display_helper::v2::State::EventLoop);
  EXPECT_TRUE(harness.state_machine.recovery_armed());

  harness.state_machine.handle_message(display_helper::v2::DisplayEventMessage {display_helper::v2::DisplayEvent::DisplayChange, harness.cancellation.current_generation()});

  EXPECT_EQ(harness.state_machine.state(), display_helper::v2::State::Recovery);
  EXPECT_EQ(harness.dispatcher.recovery_dispatch_count, 2);
}

TEST(DisplayHelperV2StateMachine, HeartbeatTimeoutTriggersRecovery) {
  StateMachineHarness harness;
  harness.seed_current_snapshot();
  harness.state_machine.handle_message(display_helper::v2::RevertCommand {harness.cancellation.current_generation()});

  display_helper::v2::RecoveryOutcome recovery_fail;
  recovery_fail.success = false;
  harness.dispatcher.recovery_completion(recovery_fail);
  harness.drain_messages();

  EXPECT_EQ(harness.state_machine.state(), display_helper::v2::State::EventLoop);

  harness.state_machine.handle_message(display_helper::v2::HelperEventMessage {display_helper::v2::HelperEvent::HeartbeatTimeout, harness.cancellation.current_generation()});

  EXPECT_EQ(harness.state_machine.state(), display_helper::v2::State::Recovery);
}

TEST(DisplayHelperV2StateMachine, DisarmFromEventLoopStopsRecoveryAttempts) {
  StateMachineHarness harness;
  harness.seed_current_snapshot();

  harness.state_machine.handle_message(display_helper::v2::RevertCommand {harness.cancellation.current_generation()});
  EXPECT_EQ(harness.state_machine.state(), display_helper::v2::State::Recovery);

  display_helper::v2::RecoveryOutcome recovery_fail;
  recovery_fail.success = false;
  harness.dispatcher.recovery_completion(recovery_fail);
  harness.drain_messages();

  EXPECT_EQ(harness.state_machine.state(), display_helper::v2::State::EventLoop);
  EXPECT_TRUE(harness.state_machine.recovery_armed());
  const auto recovery_dispatches = harness.dispatcher.recovery_dispatch_count;

  harness.state_machine.handle_message(display_helper::v2::DisarmCommand {harness.cancellation.current_generation()});
  EXPECT_EQ(harness.state_machine.state(), display_helper::v2::State::Waiting);
  EXPECT_FALSE(harness.state_machine.recovery_armed());
  EXPECT_EQ(harness.task_manager.deleted, 1);

  harness.state_machine.handle_message(display_helper::v2::DisplayEventMessage {display_helper::v2::DisplayEvent::DisplayChange, harness.cancellation.current_generation()});
  harness.state_machine.handle_message(display_helper::v2::HelperEventMessage {display_helper::v2::HelperEvent::HeartbeatTimeout, harness.cancellation.current_generation()});

  EXPECT_EQ(harness.state_machine.state(), display_helper::v2::State::Waiting);
  EXPECT_EQ(harness.dispatcher.recovery_dispatch_count, recovery_dispatches);
}

TEST(DisplayHelperV2StateMachine, DisarmCancelsStaleOperations) {
  StateMachineHarness harness;
  display_helper::v2::ApplyRequest request;
  request.configuration = display_device::SingleDisplayConfiguration {};

  harness.state_machine.handle_message(display_helper::v2::ApplyCommand {request, harness.cancellation.current_generation()});
  ASSERT_TRUE(harness.dispatcher.apply_completion);

  harness.state_machine.handle_message(display_helper::v2::DisarmCommand {harness.cancellation.current_generation()});
  EXPECT_EQ(harness.state_machine.state(), display_helper::v2::State::Waiting);
  EXPECT_EQ(harness.task_manager.deleted, 1);

  display_helper::v2::ApplyOutcome outcome;
  outcome.status = display_helper::v2::ApplyStatus::Ok;
  harness.dispatcher.apply_completion(outcome);
  harness.drain_messages();

  EXPECT_EQ(harness.state_machine.state(), display_helper::v2::State::Waiting);
}

TEST(DisplayHelperV2StateMachine, ExportGoldenCapturesSnapshotWithBlacklist) {
  StateMachineHarness harness;
  harness.add_active_device("A");
  harness.add_active_device("B");

  display_device::DisplaySettingsSnapshot snapshot;
  snapshot.m_topology = {{"A", "B"}};
  snapshot.m_modes["A"] = display_device::DisplayMode {};
  snapshot.m_modes["B"] = display_device::DisplayMode {};
  snapshot.m_hdr_states["A"] = std::nullopt;
  snapshot.m_hdr_states["B"] = std::nullopt;
  harness.display_settings.snapshot = snapshot;

  display_helper::v2::SnapshotCommandPayload payload;
  payload.exclude_devices = {"B"};
  payload.update_exclusions = true;
  harness.state_machine.handle_message(display_helper::v2::ExportGoldenCommand {payload, harness.cancellation.current_generation()});

  auto stored = harness.storage.load(display_helper::v2::SnapshotTier::Golden);
  ASSERT_TRUE(stored.has_value());
  EXPECT_EQ(stored->m_topology.size(), 1u);
  ASSERT_EQ(stored->m_topology.front().size(), 1u);
  EXPECT_EQ(stored->m_topology.front().front(), "A");
  EXPECT_EQ(stored->m_modes.count("B"), 0u);
}

TEST(DisplayHelperV2StateMachine, SnapshotCurrentRefreshesPreservingPrevious) {
  StateMachineHarness harness;
  harness.add_active_device("new");

  auto previous = make_snapshot("old");
  ASSERT_TRUE(harness.storage.save(display_helper::v2::SnapshotTier::Current, previous));

  auto current = make_snapshot("new");
  harness.display_settings.snapshot = current;

  display_helper::v2::SnapshotCommandPayload payload;
  harness.state_machine.handle_message(display_helper::v2::SnapshotCurrentCommand {payload, harness.cancellation.current_generation(), 1, 1});

  auto stored_prev = harness.storage.load(display_helper::v2::SnapshotTier::Previous);
  ASSERT_TRUE(stored_prev.has_value());
  EXPECT_EQ(*stored_prev, previous);

  auto stored_current = harness.storage.load(display_helper::v2::SnapshotTier::Current);
  ASSERT_TRUE(stored_current.has_value());
  EXPECT_EQ(*stored_current, current);
  ASSERT_TRUE(harness.snapshot_result.has_value());
  EXPECT_TRUE(*harness.snapshot_result);
}

TEST(DisplayHelperV2StateMachine, SnapshotCurrentExpiryDuringCapturePreservesBaselineChain) {
  StateMachineHarness harness;
  harness.add_active_device("new");
  harness.seed_current_snapshot("old-current");
  ASSERT_TRUE(harness.storage.save(
    display_helper::v2::SnapshotTier::Previous,
    make_snapshot("old-previous")
  ));
  harness.display_settings.snapshot = make_snapshot("new");

  const auto expires_at = harness.clock.now() + std::chrono::milliseconds(500);
  harness.display_settings.capture_callback = [&]() {
    harness.clock.advance(std::chrono::milliseconds(600));
  };

  display_helper::v2::SnapshotCommandPayload payload;
  display_helper::v2::SnapshotCurrentCommand command {
    payload,
    harness.cancellation.current_generation(),
    77,
    3,
    expires_at
  };
  harness.state_machine.handle_message(command);

  ASSERT_TRUE(harness.snapshot_result.has_value());
  EXPECT_FALSE(*harness.snapshot_result);
  auto current = harness.storage.load(display_helper::v2::SnapshotTier::Current);
  auto previous = harness.storage.load(display_helper::v2::SnapshotTier::Previous);
  ASSERT_TRUE(current.has_value());
  ASSERT_TRUE(previous.has_value());
  EXPECT_EQ(current->m_topology.front().front(), "old-current");
  EXPECT_EQ(previous->m_topology.front().front(), "old-previous");
}

TEST(DisplayHelperV2StateMachine, ApplyDuringReadOnlyRestoreStillCapturesFallbackBaseline) {
  StateMachineHarness harness;
  harness.add_active_device("SAFE");
  harness.display_settings.snapshot = make_snapshot("SAFE");

  // The restore worker exists but has not crossed the mutation gate. APPLY
  // cancellation must fence it without latching the unsafe-topology policy.
  auto restore_worker = harness.restore_state.mutation_guard.begin_worker();
  display_helper::v2::ApplyRequest request;
  request.configuration = display_device::SingleDisplayConfiguration {};
  harness.state_machine.handle_message(display_helper::v2::ApplyCommand {request, harness.cancellation.current_generation()});

  EXPECT_FALSE(restore_worker.try_begin_mutation([]() {
    return false;
  }));
  auto baseline = harness.storage.load(display_helper::v2::SnapshotTier::Current);
  ASSERT_TRUE(baseline.has_value());
  EXPECT_EQ(baseline->m_topology.front().front(), "SAFE");
}

// f3841ad8: APPLY with no session baseline on disk must self-capture the
// pre-apply state so REVERT always has something to restore.
TEST(DisplayHelperV2StateMachine, ApplySelfCapturesBaselineWhenMissing) {
  StateMachineHarness harness;
  harness.add_active_device("PHYS");
  harness.display_settings.snapshot = make_snapshot("PHYS");

  display_helper::v2::ApplyRequest request;
  request.configuration = display_device::SingleDisplayConfiguration {};

  EXPECT_FALSE(harness.storage.exists(display_helper::v2::SnapshotTier::Current));
  harness.state_machine.handle_message(display_helper::v2::ApplyCommand {request, harness.cancellation.current_generation()});

  auto baseline = harness.storage.load(display_helper::v2::SnapshotTier::Current);
  ASSERT_TRUE(baseline.has_value());
  EXPECT_EQ(baseline->m_topology.front().front(), "PHYS");
}

TEST(DisplayHelperV2StateMachine, ApplyKeepsExistingBaseline) {
  StateMachineHarness harness;
  harness.add_active_device("PHYS");
  harness.display_settings.snapshot = make_snapshot("PHYS");

  auto existing = make_snapshot("EXISTING");
  ASSERT_TRUE(harness.storage.save(display_helper::v2::SnapshotTier::Current, existing));

  display_helper::v2::ApplyRequest request;
  request.configuration = display_device::SingleDisplayConfiguration {};
  harness.state_machine.handle_message(display_helper::v2::ApplyCommand {request, harness.cancellation.current_generation()});

  auto baseline = harness.storage.load(display_helper::v2::SnapshotTier::Current);
  ASSERT_TRUE(baseline.has_value());
  EXPECT_EQ(*baseline, existing);
}

// 72b0d996: stream-start probes must not cancel an in-flight unconfirmed restore.
TEST(DisplayHelperV2StateMachine, DisarmIgnoredWhileRestoreUnconfirmed) {
  StateMachineHarness harness;
  harness.seed_current_snapshot();

  harness.state_machine.handle_message(display_helper::v2::RevertCommand {harness.cancellation.current_generation()});
  display_helper::v2::RecoveryOutcome recovery_fail;
  recovery_fail.success = false;
  harness.dispatcher.recovery_completion(recovery_fail);
  harness.drain_messages();
  EXPECT_EQ(harness.state_machine.state(), display_helper::v2::State::EventLoop);

  // A real RecoveryOperation marks the topology unconfirmed immediately before
  // entering SetDisplayConfig; simulate that side of the mutation gate here.
  auto restore_worker = harness.restore_state.mutation_guard.begin_worker();
  ASSERT_TRUE(restore_worker.try_begin_mutation([]() {
    return false;
  }));

  harness.state_machine.handle_message(display_helper::v2::DisarmCommand {harness.cancellation.current_generation(), 1, 1});
  EXPECT_EQ(harness.state_machine.state(), display_helper::v2::State::EventLoop);
  EXPECT_TRUE(harness.state_machine.recovery_armed());
  EXPECT_EQ(harness.task_manager.deleted, 0);
  ASSERT_TRUE(harness.disarm_result.has_value());
  EXPECT_FALSE(*harness.disarm_result);
}

TEST(DisplayHelperV2StateMachine, DisarmDuringReadOnlyRestoreIsAcknowledgedAndFencesMutation) {
  StateMachineHarness harness;
  harness.seed_current_snapshot();
  harness.state_machine.handle_message(display_helper::v2::RevertCommand {harness.cancellation.current_generation()});
  ASSERT_EQ(harness.state_machine.state(), display_helper::v2::State::Recovery);

  auto restore_worker = harness.restore_state.mutation_guard.begin_worker();
  display_helper::v2::DisarmCommand disarm {harness.cancellation.current_generation()};
  disarm.request_id = 20;
  disarm.connection_epoch = 4;
  disarm.expires_at = harness.clock.now() + std::chrono::seconds(1);
  harness.state_machine.handle_message(disarm);

  ASSERT_TRUE(harness.disarm_result.has_value());
  EXPECT_TRUE(*harness.disarm_result);
  EXPECT_EQ(harness.state_machine.state(), display_helper::v2::State::Waiting);
  EXPECT_FALSE(restore_worker.try_begin_mutation([]() {
    return false;
  }));
  EXPECT_FALSE(harness.restore_state.mutation_guard.topology_unconfirmed());
}

TEST(DisplayHelperV2StateMachine, ExpiredDisarmDoesNotCancelRecovery) {
  StateMachineHarness harness;
  harness.seed_current_snapshot();
  harness.state_machine.handle_message(display_helper::v2::RevertCommand {harness.cancellation.current_generation()});
  ASSERT_EQ(harness.state_machine.state(), display_helper::v2::State::Recovery);
  const auto generation_before = harness.cancellation.current_generation();

  display_helper::v2::DisarmCommand disarm {generation_before};
  disarm.request_id = 21;
  disarm.connection_epoch = 4;
  disarm.expires_at = harness.clock.now();
  harness.state_machine.handle_message(disarm);

  ASSERT_TRUE(harness.disarm_result.has_value());
  EXPECT_FALSE(*harness.disarm_result);
  EXPECT_EQ(harness.cancellation.current_generation(), generation_before);
  EXPECT_EQ(harness.state_machine.state(), display_helper::v2::State::Recovery);
}

TEST(DisplayHelperV2StateMachine, DisarmDoesNotRaceBlockingApplyWorker) {
  StateMachineHarness harness;
  harness.add_active_device("BASE");
  harness.display_settings.snapshot = make_snapshot("BASE");
  display_helper::v2::ApplyRequest request;
  request.configuration = display_device::SingleDisplayConfiguration {};
  harness.state_machine.handle_message(display_helper::v2::ApplyCommand {request, harness.cancellation.current_generation()});
  ASSERT_EQ(harness.restore_state.apply_workers_active.load(std::memory_order_acquire), 1u);
  const auto generation_before = harness.cancellation.current_generation();

  display_helper::v2::DisarmCommand disarm {generation_before};
  disarm.request_id = 22;
  disarm.connection_epoch = 4;
  disarm.expires_at = harness.clock.now() + std::chrono::seconds(1);
  harness.state_machine.handle_message(disarm);

  ASSERT_TRUE(harness.disarm_result.has_value());
  EXPECT_FALSE(*harness.disarm_result);
  EXPECT_EQ(harness.cancellation.current_generation(), generation_before);
  EXPECT_EQ(harness.state_machine.state(), display_helper::v2::State::InProgress);
}

// 72b0d996: SNAPSHOT_CURRENT must not overwrite the baseline mid-restore.
TEST(DisplayHelperV2StateMachine, SnapshotCurrentSkippedWhileRestorePending) {
  StateMachineHarness harness;
  harness.add_active_device("new");
  harness.seed_current_snapshot("old");

  harness.state_machine.handle_message(display_helper::v2::RevertCommand {harness.cancellation.current_generation()});
  display_helper::v2::RecoveryOutcome recovery_fail;
  recovery_fail.success = false;
  harness.dispatcher.recovery_completion(recovery_fail);
  harness.drain_messages();
  EXPECT_EQ(harness.state_machine.state(), display_helper::v2::State::EventLoop);

  harness.display_settings.snapshot = make_snapshot("new");
  display_helper::v2::SnapshotCommandPayload payload;
  harness.state_machine.handle_message(display_helper::v2::SnapshotCurrentCommand {payload, harness.cancellation.current_generation(), 2, 1});

  auto stored_current = harness.storage.load(display_helper::v2::SnapshotTier::Current);
  ASSERT_TRUE(stored_current.has_value());
  EXPECT_EQ(stored_current->m_topology.front().front(), "old");
  ASSERT_TRUE(harness.snapshot_result.has_value());
  EXPECT_FALSE(*harness.snapshot_result);
}

// vibeshine#242: APPLY can supersede a restore while its blocking
// SetDisplayConfig call is still running. Never capture that transitional
// topology as the new session baseline, including on repeated APPLY attempts.
TEST(DisplayHelperV2StateMachine, ApplyDuringUnconfirmedRestoreDoesNotCaptureBaseline) {
  StateMachineHarness harness;
  harness.add_active_device("PARTIAL");
  harness.display_settings.snapshot = make_snapshot("PARTIAL");
  ASSERT_TRUE(harness.storage.save(
    display_helper::v2::SnapshotTier::Golden,
    make_snapshot("GOOD")
  ));

  harness.state_machine.handle_message(display_helper::v2::RevertCommand {harness.cancellation.current_generation()});
  ASSERT_EQ(harness.state_machine.state(), display_helper::v2::State::Recovery);
  auto restore_worker = harness.restore_state.mutation_guard.begin_worker();
  ASSERT_TRUE(restore_worker.try_begin_mutation([]() {
    return false;
  }));

  display_helper::v2::ApplyRequest request;
  request.configuration = display_device::SingleDisplayConfiguration {};
  harness.state_machine.handle_message(display_helper::v2::ApplyCommand {request, harness.cancellation.current_generation()});

  EXPECT_FALSE(harness.storage.exists(display_helper::v2::SnapshotTier::Current));
  ASSERT_TRUE(harness.storage.exists(display_helper::v2::SnapshotTier::Golden));

  display_helper::v2::SnapshotCommandPayload snapshot_payload;
  harness.state_machine.handle_message(display_helper::v2::ExportGoldenCommand {snapshot_payload, harness.cancellation.current_generation()});
  auto golden = harness.storage.load(display_helper::v2::SnapshotTier::Golden);
  ASSERT_TRUE(golden.has_value());
  EXPECT_EQ(golden->m_topology.front().front(), "GOOD");

  harness.state_machine.handle_message(display_helper::v2::SnapshotCurrentCommand {snapshot_payload, harness.cancellation.current_generation(), 3, 1});
  EXPECT_FALSE(harness.storage.exists(display_helper::v2::SnapshotTier::Current));
  ASSERT_TRUE(harness.snapshot_result.has_value());
  EXPECT_FALSE(*harness.snapshot_result);

  // The first APPLY changes the FSM state, but the canceled recovery may still
  // be draining on the worker. A later DISARM probe must report busy, and a
  // second rapid APPLY must preserve the same baseline policy.
  harness.state_machine.handle_message(display_helper::v2::DisarmCommand {harness.cancellation.current_generation()});
  ASSERT_EQ(harness.state_machine.state(), display_helper::v2::State::InProgress);
  harness.state_machine.handle_message(display_helper::v2::ApplyCommand {request, harness.cancellation.current_generation()});
  EXPECT_FALSE(harness.storage.exists(display_helper::v2::SnapshotTier::Current));

  // Drain both serialized APPLY workers before the later recovery; the first
  // completion is stale after the retry canceled its generation.
  display_helper::v2::ApplyOutcome canceled_apply;
  canceled_apply.status = display_helper::v2::ApplyStatus::Fatal;
  harness.dispatcher.apply_completion(canceled_apply);
  harness.drain_messages();
  harness.dispatcher.apply_completion(canceled_apply);
  harness.drain_messages();

  restore_worker = decltype(restore_worker) {};

  // Once a later restore is actually validated, fresh snapshots are safe again.
  harness.state_machine.handle_message(display_helper::v2::RevertCommand {harness.cancellation.current_generation()});
  display_helper::v2::RecoveryOutcome recovery_ok;
  recovery_ok.success = true;
  recovery_ok.snapshot = make_snapshot("GOOD");
  harness.dispatcher.recovery_completion(recovery_ok);
  harness.drain_messages();
  ASSERT_TRUE(harness.dispatcher.recovery_validation_completion);
  harness.dispatcher.recovery_validation_completion(true);
  harness.drain_messages();

  harness.snapshot_result.reset();
  harness.state_machine.handle_message(display_helper::v2::SnapshotCurrentCommand {snapshot_payload, harness.cancellation.current_generation(), 4, 1});
  ASSERT_TRUE(harness.snapshot_result.has_value());
  EXPECT_TRUE(*harness.snapshot_result);
  EXPECT_TRUE(harness.storage.exists(display_helper::v2::SnapshotTier::Current));
}

TEST(DisplayHelperV2StateMachine, VerifiedSupersedingApplyAllowsHandoffWithoutBaselinePoisoning) {
  StateMachineHarness harness;
  harness.add_active_device("SESSION");
  harness.display_settings.snapshot = make_snapshot("SESSION");
  harness.seed_current_snapshot("ORIGINAL");

  auto restore_worker = harness.restore_state.mutation_guard.begin_worker();
  ASSERT_TRUE(restore_worker.try_begin_mutation([]() {
    return false;
  }));

  display_helper::v2::ApplyRequest request;
  request.configuration = display_device::SingleDisplayConfiguration {};
  harness.state_machine.handle_message(display_helper::v2::ApplyCommand {request, harness.cancellation.current_generation()});

  display_helper::v2::ApplyOutcome apply_ok;
  apply_ok.status = display_helper::v2::ApplyStatus::Ok;
  harness.dispatcher.apply_completion(apply_ok);
  harness.drain_messages();
  harness.dispatcher.verification_completion(true);
  harness.drain_messages();

  EXPECT_FALSE(harness.restore_state.mutation_guard.topology_unconfirmed());

  harness.disarm_result.reset();
  display_helper::v2::DisarmCommand disarm {harness.cancellation.current_generation()};
  disarm.request_id = 55;
  disarm.connection_epoch = 8;
  disarm.expires_at = harness.clock.now() + std::chrono::seconds(1);
  harness.state_machine.handle_message(disarm);
  ASSERT_TRUE(harness.disarm_result.has_value());
  EXPECT_TRUE(*harness.disarm_result);

  harness.snapshot_result.reset();
  display_helper::v2::SnapshotCommandPayload payload;
  harness.state_machine.handle_message(display_helper::v2::SnapshotCurrentCommand {payload, harness.cancellation.current_generation(), 56, 8, harness.clock.now() + std::chrono::seconds(1)});
  ASSERT_TRUE(harness.snapshot_result.has_value());
  EXPECT_FALSE(*harness.snapshot_result);

  auto current = harness.storage.load(display_helper::v2::SnapshotTier::Current);
  ASSERT_TRUE(current.has_value());
  EXPECT_EQ(current->m_topology.front().front(), "ORIGINAL");
}

// 3b7a52c4 / 0add1f80: paused sessions with revert_on_disconnect=false must keep
// their display state when the connection drops.
TEST(DisplayHelperV2StateMachine, NoRevertOnDisconnectWhenPolicyFalse) {
  StateMachineHarness harness;
  harness.seed_current_snapshot();

  display_helper::v2::ApplyRequest request;
  request.configuration = display_device::SingleDisplayConfiguration {};
  request.restore_on_disconnect = false;
  harness.state_machine.handle_message(display_helper::v2::ApplyCommand {request, harness.cancellation.current_generation()});

  display_helper::v2::RevertCommand disconnect_revert;
  disconnect_revert.generation = harness.cancellation.current_generation();
  disconnect_revert.from_disconnect = true;
  harness.state_machine.handle_message(disconnect_revert);

  EXPECT_EQ(harness.dispatcher.recovery_dispatch_count, 0);
  EXPECT_EQ(harness.state_machine.state(), display_helper::v2::State::InProgress);

  // An explicit client REVERT still restores.
  harness.state_machine.handle_message(display_helper::v2::RevertCommand {harness.cancellation.current_generation()});
  EXPECT_EQ(harness.dispatcher.recovery_dispatch_count, 1);
  EXPECT_EQ(harness.state_machine.state(), display_helper::v2::State::Recovery);
}

// A pipe can break after APPLY is accepted but before its worker crosses the
// display-mutation boundary. The helper must retain the disconnect restore
// request until that worker drains, then resolve the normal restore policy.
TEST(DisplayHelperV2StateMachine, DisconnectDuringApplyDefersRestoreUntilWorkerDrains) {
  StateMachineHarness harness;
  harness.seed_current_snapshot();

  display_helper::v2::ApplyRequest request;
  request.configuration = display_device::SingleDisplayConfiguration {};
  harness.state_machine.handle_message(
    display_helper::v2::ApplyCommand {request, harness.cancellation.current_generation()}
  );
  ASSERT_EQ(harness.restore_state.apply_workers_active.load(std::memory_order_acquire), 1u);

  const auto apply_generation = harness.cancellation.current_generation();
  display_helper::v2::RevertCommand disconnect;
  disconnect.generation = apply_generation;
  disconnect.from_disconnect = true;
  harness.state_machine.handle_message(disconnect);

  EXPECT_EQ(harness.cancellation.current_generation(), apply_generation + 1);
  EXPECT_EQ(harness.dispatcher.recovery_dispatch_count, 0);
  EXPECT_EQ(harness.state_machine.state(), display_helper::v2::State::InProgress);

  // The canceled completion is stale, but it still retires its worker and
  // therefore unlocks the deferred disconnect decision.
  display_helper::v2::ApplyOutcome canceled;
  canceled.status = display_helper::v2::ApplyStatus::Fatal;
  harness.dispatcher.apply_completion(canceled);
  harness.drain_messages();

  EXPECT_EQ(harness.restore_state.apply_workers_active.load(std::memory_order_acquire), 0u);
  EXPECT_EQ(harness.dispatcher.recovery_dispatch_count, 1);
  EXPECT_EQ(harness.state_machine.state(), display_helper::v2::State::Recovery);
}

// Restore retry pacing: failed attempts back off, display events reset the
// backoff, and an exhausted window pauses attempts until the next event.
TEST(DisplayHelperV2StateMachine, TickDrivesScheduledRestoreRetries) {
  StateMachineHarness harness;
  harness.seed_current_snapshot();

  harness.state_machine.handle_message(display_helper::v2::RevertCommand {harness.cancellation.current_generation()});
  display_helper::v2::RecoveryOutcome recovery_fail;
  recovery_fail.success = false;
  harness.dispatcher.recovery_completion(recovery_fail);
  harness.drain_messages();
  EXPECT_EQ(harness.state_machine.state(), display_helper::v2::State::EventLoop);
  const int dispatches_after_first = harness.dispatcher.recovery_dispatch_count;

  // Backoff (1s after first failure): an immediate tick must not retry.
  harness.state_machine.handle_tick();
  EXPECT_EQ(harness.dispatcher.recovery_dispatch_count, dispatches_after_first);

  // After the backoff elapses, the tick retries.
  harness.clock.advance(std::chrono::milliseconds(1100));
  harness.state_machine.handle_tick();
  EXPECT_EQ(harness.dispatcher.recovery_dispatch_count, dispatches_after_first + 1);
  EXPECT_EQ(harness.state_machine.state(), display_helper::v2::State::Recovery);

  // Fail again, then exhaust the primary window: ticks stop retrying.
  harness.dispatcher.recovery_completion(recovery_fail);
  harness.drain_messages();
  EXPECT_EQ(harness.state_machine.state(), display_helper::v2::State::EventLoop);
  harness.clock.advance(std::chrono::minutes(3));
  harness.state_machine.handle_tick();
  harness.state_machine.handle_tick();
  EXPECT_EQ(harness.dispatcher.recovery_dispatch_count, dispatches_after_first + 1);

  // A display event re-opens an event window and retries immediately.
  harness.state_machine.handle_message(display_helper::v2::DisplayEventMessage {display_helper::v2::DisplayEvent::DisplayChange, harness.cancellation.current_generation()});
  EXPECT_EQ(harness.dispatcher.recovery_dispatch_count, dispatches_after_first + 2);
  EXPECT_EQ(harness.state_machine.state(), display_helper::v2::State::Recovery);
}

TEST(DisplayHelperV2StateMachine, SnapshotCurrentRefreshFailureKeepsBaseline) {
  StateMachineHarness harness;
  // No enumerated devices: the capture filter rejects the new snapshot, so the
  // existing baseline chain must remain untouched (53cd8b4c / 62839421).

  auto previous = make_snapshot("old");
  ASSERT_TRUE(harness.storage.save(display_helper::v2::SnapshotTier::Current, previous));
  harness.display_settings.snapshot = make_snapshot("new");

  display_helper::v2::SnapshotCommandPayload payload;
  harness.state_machine.handle_message(display_helper::v2::SnapshotCurrentCommand {payload, harness.cancellation.current_generation()});

  auto stored_current = harness.storage.load(display_helper::v2::SnapshotTier::Current);
  ASSERT_TRUE(stored_current.has_value());
  EXPECT_EQ(*stored_current, previous);
  EXPECT_FALSE(harness.storage.load(display_helper::v2::SnapshotTier::Previous).has_value());
}

TEST(DisplayHelperV2SnapshotLedger, CurrentSaveFailureRollsPreviousBack) {
  FakeClock clock;
  FakeDisplaySettings display;
  display_device::EnumeratedDevice device;
  device.m_device_id = "new";
  device.m_info = display_device::EnumeratedDevice::Info {};
  display.devices.push_back(std::move(device));
  display.snapshot = make_snapshot("new");

  ControlledSnapshotStorage storage;
  ASSERT_TRUE(storage.save(display_helper::v2::SnapshotTier::Current, make_snapshot("old-current")));
  ASSERT_TRUE(storage.save(display_helper::v2::SnapshotTier::Previous, make_snapshot("old-previous")));
  storage.fail_current_save = true;

  display_helper::v2::SnapshotService service {display};
  display_helper::v2::SnapshotPersistence persistence {storage};
  display_helper::v2::SnapshotLedger ledger {service, persistence, clock};
  EXPECT_FALSE(ledger.refresh_current_preserving_previous({}));

  const auto current = storage.load(display_helper::v2::SnapshotTier::Current);
  const auto previous = storage.load(display_helper::v2::SnapshotTier::Previous);
  ASSERT_TRUE(current.has_value());
  ASSERT_TRUE(previous.has_value());
  EXPECT_EQ(current->m_topology.front().front(), "old-current");
  EXPECT_EQ(previous->m_topology.front().front(), "old-previous");
}

TEST(DisplayHelperV2SnapshotLedger, LeaseExpiryAfterRotationRollsPreviousBack) {
  FakeClock clock;
  FakeDisplaySettings display;
  display_device::EnumeratedDevice device;
  device.m_device_id = "new";
  device.m_info = display_device::EnumeratedDevice::Info {};
  display.devices.push_back(std::move(device));
  display.snapshot = make_snapshot("new");

  ControlledSnapshotStorage storage;
  storage.clock = &clock;
  ASSERT_TRUE(storage.save(display_helper::v2::SnapshotTier::Current, make_snapshot("old-current")));
  ASSERT_TRUE(storage.save(display_helper::v2::SnapshotTier::Previous, make_snapshot("old-previous")));
  storage.expire_after_next_previous_save = true;

  display_helper::v2::SnapshotService service {display};
  display_helper::v2::SnapshotPersistence persistence {storage};
  display_helper::v2::SnapshotLedger ledger {service, persistence, clock};
  EXPECT_FALSE(ledger.refresh_current_preserving_previous(
    {},
    clock.now() + std::chrono::milliseconds(100)
  ));

  const auto current = storage.load(display_helper::v2::SnapshotTier::Current);
  const auto previous = storage.load(display_helper::v2::SnapshotTier::Previous);
  ASSERT_TRUE(current.has_value());
  ASSERT_TRUE(previous.has_value());
  EXPECT_EQ(current->m_topology.front().front(), "old-current");
  EXPECT_EQ(previous->m_topology.front().front(), "old-previous");
}

#endif  // _WIN32

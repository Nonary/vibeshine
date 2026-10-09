"""Run legacy helper safety boundaries with deterministic display/filesystem fakes."""
from pathlib import Path
import subprocess
import sys
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root / 'tools/display_settings_helper.cpp').read_text()
compiler = sys.argv[1] if len(sys.argv) > 1 else 'g++'


def extract(signature):
    start = source.index(signature)
    brace = source.index('{', start)
    depth = 1
    end = brace + 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]


prepare = extract('bool prepare_recovery_baseline(const char *reason)')
ready = extract('bool recovery_baseline_ready() const')
canonical = extract('static display_device::ActiveTopology canonical_topology(')
compare_snapshots = extract('static bool equal_snapshots_strict(')
authoritative = extract('std::array<std::filesystem::path, 3> authoritative_snapshot_paths() const')
restore_once = extract('bool try_restore_once_if_valid(std::stop_token st, uint64_t guard_generation)')
headless = extract('bool proven_headless_without_baseline() const')
visible_fallback = extract('void try_visible_physical_fallback(std::stop_token st, uint64_t generation)')
missing_snapshot = extract('if (!has_session && !has_previous && !has_golden)')
deadline_tail = extract('if (current_deadline_ms != 0 && post_ms > current_deadline_ms)')
disarm = extract('void disarm_restore_requests(const char *reason = nullptr)')
retire = extract('static bool retire_snapshot_file(')
promote = extract('bool promote_current_snapshot_to_previous(const char *reason = nullptr)')
golden_cleanup = extract('bool clear_session_restore_snapshots_after_golden()')
golden_confirmation = extract('if (confirm_current_matches_golden())')
for old, new in [('std::filesystem::remove', 'housekeeping_fake::remove'),
                 ('std::filesystem::exists', 'housekeeping_fake::exists')]:
    retire = retire.replace(old, new)
    promote = promote.replace(old, new)
assert extract('bool apply_golden_and_confirm(').count('if (!clear_session_restore_snapshots_after_golden()) return false;') == 4


# The transaction must check its guard before canceling old recovery or calling
# any mutator, including the formerly automatic display-stack reset.
apply = extract('bool handle_apply(ServiceState &state, std::span<const uint8_t> payload, std::string &error_msg)')
assert apply.index('if (!recovery_ready)') < apply.index('state.stop_restore_polling()')
assert apply.index('if (!recovery_ready)') < apply.index('state.controller.apply(')
assert 'recover_display_stack()' not in apply
assert 'if (!state.prepare_recovery_baseline("apply mutation boundary"))' in apply
assert 'CDS_UPDATEREGISTRY' not in source
assert '->setDisplayModes(' not in source and '->setDisplayOrigin(' not in source
assert 'display_helper_paths::ensure_single_instance(singleton)' in source
assert 'bool ensure_single_instance(' not in source, 'legacy must use the shared global-only mutation fence'
assert 'Local\\\\SunshineDisplayHelper' not in source

program = r'''
#include <algorithm>
#include <array>
#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <iostream>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <stop_token>
#include <string>
#include <vector>
// This harness supplies its own display-device types on every test host.
// Load only the portable policy branches, including on native Windows.
#ifdef _WIN32
#define LEGACY_SAFETY_RESTORE_WIN32
#undef _WIN32
#endif
#include "src/platform/windows/display_recovery_safety.h"
#include "src/platform/windows/physical_display_recovery.h"
#ifdef LEGACY_SAFETY_RESTORE_WIN32
#define _WIN32 1
#undef LEGACY_SAFETY_RESTORE_WIN32
#endif
struct NullLog { template<class T> NullLog &operator<<(const T &) { return *this; } };
#define BOOST_LOG(...) NullLog {}
namespace boost { bool iequals(std::string a, std::string b) {
  auto lower = [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); };
  std::transform(a.begin(), a.end(), a.begin(), lower);
  std::transform(b.begin(), b.end(), b.begin(), lower);
  return a == b;
} }
namespace display_device {
  using ActiveTopology = std::vector<std::vector<std::string>>;
  enum class DeviceEnumerationDetail { Minimal };
  struct DisplaySettingsSnapshot {
    ActiveTopology m_topology;
    std::map<std::string, int> m_modes, m_hdr_states;
    std::string m_primary_device;
    std::map<std::string, std::pair<int, int>> m_origins;
  };
  bool equalDisplayModes(const auto &a, const auto &b) { return a == b; }
  struct SingleDisplayConfiguration {
    enum class DevicePreparation { VerifyOnly, EnsureOnlyDisplay };
    DevicePreparation m_device_prep = DevicePreparation::VerifyOnly;
  };
}
struct Device { std::string m_device_id; bool physical; bool active; };
struct Controller {
  std::optional<std::vector<Device>> devices = std::vector<Device> {};
  std::optional<display_device::ActiveTopology> expected = display_device::ActiveTopology {};
  auto enumerate_nonempty_devices_known(display_device::DeviceEnumerationDetail) const { return devices; }
  auto compute_expected_topology(const display_device::SingleDisplayConfiguration &,
                                const std::optional<display_device::ActiveTopology> &) const { return expected; }
  static auto flatten_topology_device_ids(const display_device::ActiveTopology &topology) {
    std::vector<std::string> ids;
    for (const auto &group : topology) ids.insert(ids.end(), group.begin(), group.end());
    return ids;
  }
  static bool is_virtual_display_device(const Device &device) { return !device.physical; }
  static bool is_active_display_device(const Device &device) { return device.active; }

  bool payload = false, loadable = false;
  display_recovery_safety::PhysicalDisplayState physical_state = display_recovery_safety::PhysicalDisplayState::active;
  std::map<std::string, std::pair<bool, bool>> snapshot_results;
  bool snapshot_file_has_restore_payload(const std::filesystem::path &path) const {
    const auto found = snapshot_results.find(path.string());
    return found == snapshot_results.end() ? payload : found->second.first;
  }
  std::optional<int> load_display_settings_snapshot(const std::filesystem::path &path) const {
    const auto found = snapshot_results.find(path.string());
    return (found == snapshot_results.end() ? loadable : found->second.second) ? std::optional<int> {1} : std::nullopt;
  }
  auto physical_display_state() const { return physical_state; }
  int visibility_calls = 0;
  std::array<std::filesystem::path, 3> fallback_paths;
  display_helper::physical_recovery::Outcome enable_visible_physical_output(const std::array<std::filesystem::path, 3> &paths,
                                     const std::function<bool()> &cancelled) {
    assert(!cancelled()); ++visibility_calls; fallback_paths = paths; return {true, true};
  }
};
namespace display_helper::recovery_status { enum class status { unknown, failed, restored }; }
int task_calls = 0, task_deletes = 0;
bool task_result = false;
bool create_restore_scheduled_task() { ++task_calls; return task_result; }
[[maybe_unused]] bool delete_restore_scheduled_task() { ++task_deletes; return true; }
struct ServiceState {
  Controller controller;
  std::filesystem::path session_current_path = "current", session_previous_path = "previous", golden_path = "golden";
  bool evidence = false, existing_current = false, capture_result = false;
  int captures = 0;
  bool has_recovery_evidence() const { return evidence; }
  bool path_may_exist(const std::filesystem::path &path) const {
    return controller.snapshot_results.contains(path.string()) || (existing_current && path == session_current_path);
  }
  std::atomic<bool> session_saved {false};
  bool capture_current_snapshot(const char *) {
    ++captures;
    if (capture_result) { controller.payload = controller.loadable = true; evidence = existing_current = true; }
    return capture_result;
  }
''' + canonical + compare_snapshots + ready + headless + prepare + r'''
  std::mutex disconnect_settlement_mutex;
  std::atomic<std::uint64_t> restore_cancel_generation {4}, host_loss_connection_epoch {7};
  std::atomic<bool> restore_requested {true}, host_loss_recovery {true}, heartbeat_monitor_active {false};
  std::atomic<bool> disconnect_settlement_pending {false}, visible_fallback_attempted {false};
  std::atomic<bool> always_restore_from_golden {false}, restore_stage_running {true}, restore_poll_active {true};
  std::atomic<bool> prefer_golden_if_current_missing {true};
  std::atomic<bool> restore_attempted_unconfirmed {false}, event_pump_running {true};
  std::atomic<bool> direct_revert_bypass_grace {false}, exit_after_revert {true};
  std::atomic<bool> retry_apply_on_topology {false}, retry_revert_on_topology {false};
  std::atomic<bool> running {true};
  std::atomic<bool> *running_flag = &running;
  bool epoch_current = true;
  std::uint64_t current_connection_epoch() const { return epoch_current ? 7 : 8; }
  int published = 0;
  display_helper::recovery_status::status status = display_helper::recovery_status::status::unknown;
  std::uint64_t current_recovery_ticket() const { return 9; }
  void publish_recovery_status(display_helper::recovery_status::status value, std::uint64_t,
                               std::uint64_t, std::uint64_t) { ++published; status = value; }
  void stop_restore_polling() { restore_requested.store(false); }
  void cancel_delayed_reapply() {}
  void cancel_post_apply_tasks() {}
  int session_restore_calls = 0, golden_restore_calls = 0, promotions = 0;
  bool session_restore_result = false, golden_restore_result = false, promotion_result = true;
  std::filesystem::path last_session_restore;
  void reset_pending_golden_session_fallbacks() {}
  void note_golden_restore_issue(const char *) {}
  bool apply_golden_and_confirm(std::stop_token, uint64_t) { ++golden_restore_calls; return golden_restore_result; }
  bool apply_session_snapshot_from_path(const std::filesystem::path &path, const char *, std::stop_token, uint64_t, bool &attempted) {
    attempted = true; ++session_restore_calls; last_session_restore = path; return session_restore_result;
  }
  bool promote_current_snapshot_to_previous(const char *) { ++promotions; return promotion_result; }
''' + authoritative + visible_fallback + disarm + restore_once + r'''
};
namespace housekeeping_fake {
  struct File {
    bool present = false, remove_fails = false, remains_after_remove = false;
    bool existence_fails = false, existence_fails_after_remove = false;
    int removes = 0;
  };
  std::map<std::string, File> files;
  bool copy_succeeds = true;
  int copies = 0;
  void reset() { files.clear(); copy_succeeds = true; copies = 0; }
  bool exists(const std::filesystem::path &path, std::error_code &error) {
    const auto &file = files[path.string()];
    error = file.existence_fails ? std::make_error_code(std::errc::permission_denied) : std::error_code {};
    return file.present;
  }
  bool remove(const std::filesystem::path &path, std::error_code &error) {
    auto &file = files[path.string()];
    ++file.removes;
    error = file.remove_fails ? std::make_error_code(std::errc::permission_denied) : std::error_code {};
    if (error) return false;
    const bool was_present = file.present;
    if (!file.remains_after_remove) file.present = false;
    if (file.existence_fails_after_remove) file.existence_fails = true;
    return was_present;
  }
}
struct HousekeepingState {
  std::filesystem::path session_current_path = "current", session_previous_path = "previous";
  std::atomic<bool> session_saved {true};
  int health_clears = 0;
  void clear_golden_restore_status(const char *) { ++health_clears; }
  static bool copy_file_overwrite(const std::filesystem::path &, const std::filesystem::path &to) {
    ++housekeeping_fake::copies;
    if (!housekeeping_fake::copy_succeeds) return false;
    housekeeping_fake::files[to.string()].present = true;
    return true;
  }
''' + retire + promote + golden_cleanup + r'''
  bool finish_verified_golden() {
    const auto confirm_current_matches_golden = [] { return true; };
''' + golden_confirmation + r'''
    return false;
  }
};

void handle_missing_snapshot(ServiceState *self) {
  const bool has_session = false, has_previous = false, has_golden = false;
  const std::uint64_t guard_generation = 4, status_epoch = 7;
''' + missing_snapshot + r'''
}

void test_baseline_capture_and_task_failures() {
  ServiceState state;
  task_calls = 0; task_result = false;
  assert(!state.prepare_recovery_baseline("test"));
  assert(state.captures == 1 && task_calls == 0);
  state.capture_result = true;
  assert(!state.prepare_recovery_baseline("test"));
  assert(state.captures == 2 && task_calls == 1);
  // A later task retry uses the same full baseline rather than recapturing an
  // already changed desktop. A user-disabled task is an explicit true result.
  task_result = true;
  assert(state.prepare_recovery_baseline("test"));
  assert(state.captures == 2 && task_calls == 2);
  state.controller.loadable = false;
  assert(!state.prepare_recovery_baseline("test"));
  assert(state.captures == 2 && task_calls == 2);
  // A corrupted baseline remains evidence; it cannot be overwritten in place.
  state.controller.payload = false;
  assert(!state.prepare_recovery_baseline("test"));
  assert(state.captures == 2 && task_calls == 2);
}

void test_headless_is_positive_and_does_not_erase_baseline() {
  using State = display_recovery_safety::PhysicalDisplayState;
  ServiceState state;
  state.controller.physical_state = State::none_connected;
  task_calls = 0;
  assert(state.prepare_recovery_baseline("headless"));
  assert(state.captures == 0 && task_calls == 0);
  for (const auto observed : {State::unknown, State::connected_inactive, State::active}) {
    state.controller.physical_state = observed;
    assert(!state.prepare_recovery_baseline("not-proven-headless"));
  }
  state.controller.physical_state = State::none_connected;
  state.evidence = true;
  assert(!state.prepare_recovery_baseline("unplugged-dock"));
  assert(task_calls == 0);
}

void test_missing_baseline_retains_failed_recovery() {
  for (const bool host_lost : {false, true}) {
    ServiceState state;
    state.host_loss_recovery.store(host_lost);
    task_deletes = 0;
    handle_missing_snapshot(&state);
    assert(state.running.load() && task_deletes == 0 && state.event_pump_running.load());
    assert(!state.restore_poll_active.load() && !state.restore_stage_running.load());
    assert(state.restore_attempted_unconfirmed.load());
    assert(state.restore_requested.load() == host_lost);
    assert(state.published == 1 && state.status == display_helper::recovery_status::status::failed);
  }
}

void test_visible_fallback_is_bounded_and_requires_idle_lost_host() {
  ServiceState state;
  state.heartbeat_monitor_active.store(true);
  state.try_visible_physical_fallback({}, 4);
  assert(state.controller.visibility_calls == 0);
  state.heartbeat_monitor_active.store(false);
  state.host_loss_recovery.store(false);
  state.try_visible_physical_fallback({}, 4);
  assert(state.controller.visibility_calls == 0);
  state.host_loss_recovery.store(true);
  state.epoch_current = false;
  state.try_visible_physical_fallback({}, 4);
  assert(state.controller.visibility_calls == 0);
  state.epoch_current = true;
  state.try_visible_physical_fallback({}, 3);
  assert(state.controller.visibility_calls == 0);
  state.disconnect_settlement_pending.store(true);
  state.try_visible_physical_fallback({}, 4);
  assert(state.controller.visibility_calls == 0);
  state.disconnect_settlement_pending.store(false);
  state.try_visible_physical_fallback({}, 4);
  state.try_visible_physical_fallback({}, 4);
  assert(state.controller.visibility_calls == 1 && state.controller.fallback_paths.front() == "current");
  assert(state.restore_requested.load() && state.published == 0 && task_deletes == 0);
  ServiceState golden;
  golden.always_restore_from_golden.store(true);
  golden.try_visible_physical_fallback({}, 4);
  assert(golden.controller.fallback_paths.front() == "golden");
}

void test_failed_authority_never_applies_an_alternate_exact_tier() {
  for (const bool corrupt : {false, true}) {
    ServiceState state;
    state.controller.snapshot_results = {{"current", {!corrupt, false}}, {"golden", {true, true}}, {"previous", {true, true}}};
    state.golden_restore_result = true; // The alternate would succeed if called.
    assert(!state.try_restore_once_if_valid({}, 4));
    assert(state.golden_restore_calls == 0);
    assert(state.session_restore_calls == (corrupt ? 0 : 1));
    assert(state.promotions == 0 && state.restore_attempted_unconfirmed.load());
    assert(state.controller.visibility_calls == 1 && task_deletes == 0);
  }
  ServiceState golden;
  golden.always_restore_from_golden.store(true);
  golden.controller.snapshot_results = {{"current", {true, true}}, {"golden", {true, false}}};
  golden.session_restore_result = true;
  assert(!golden.try_restore_once_if_valid({}, 4));
  assert(golden.golden_restore_calls == 1 && golden.session_restore_calls == 0);
  assert(golden.promotions == 0 && golden.controller.visibility_calls == 1);

  ServiceState restored;
  restored.controller.snapshot_results = {{"current", {true, true}}, {"golden", {true, true}}};
  restored.session_restore_result = true;
  assert(restored.try_restore_once_if_valid({}, 4));
  assert(restored.promotions == 1 && restored.controller.visibility_calls == 0 && restored.golden_restore_calls == 0);

  ServiceState previous;
  previous.prefer_golden_if_current_missing.store(false);
  previous.controller.snapshot_results = {{"previous", {true, true}}, {"golden", {true, true}}};
  previous.session_restore_result = true;
  assert(previous.try_restore_once_if_valid({}, 4));
  assert(previous.last_session_restore == "previous" && previous.golden_restore_calls == 0);
}

void test_slow_failed_attempt_reaches_terminal_timeout() {
  struct DeadlineState { std::atomic<long long> restore_active_until_ms {100}; } state;
  auto *self = &state;
  // A display call started before 100 and completed at 101. A fresh event may
  // extend its deadline to 200 while it runs; that new window must survive.
  for (const auto live_deadline : {100LL, 200LL}) {
    self->restore_active_until_ms.store(live_deadline);
    bool exit_due_to_timeout = false;
    const long long post_ms = 101;
    const auto current_deadline_ms = self->restore_active_until_ms.load(std::memory_order_acquire);
    do {
''' + deadline_tail + r'''
    } while (false);
    assert(exit_due_to_timeout == (live_deadline == 100));
    assert(self->restore_active_until_ms.load() == live_deadline);
  }
}

void test_known_baseline_origins_require_positive_readback() {
  display_device::DisplaySettingsSnapshot baseline;
  baseline.m_topology = {{"A"}, {"B"}};
  baseline.m_modes = {{"A", 144}, {"B", 60}};
  baseline.m_origins = {{"A", {0, 0}}, {"B", {1920, 0}}};
  auto actual = baseline;
  assert(ServiceState::equal_snapshots_strict(actual, baseline));
  actual.m_origins.clear();
  assert(!ServiceState::equal_snapshots_strict(actual, baseline));
  actual.m_origins = {{"A", {0, 0}}};
  assert(!ServiceState::equal_snapshots_strict(actual, baseline));
  actual = baseline;
  actual.m_origins["B"] = {0, 1920};
  assert(!ServiceState::equal_snapshots_strict(actual, baseline));
  baseline.m_origins.clear();
  assert(ServiceState::equal_snapshots_strict(actual, baseline));
}

void test_failed_current_retirement_keeps_restore_pending() {
  ServiceState state;
  state.controller.snapshot_results = {{"current", {true, true}}, {"golden", {true, true}}};
  state.session_restore_result = true;
  state.promotion_result = false;
  task_deletes = 0;
  assert(!state.try_restore_once_if_valid({}, 4));
  assert(state.promotions == 1 && state.restore_attempted_unconfirmed.load());
  assert(state.restore_requested.load() && task_deletes == 0 && state.golden_restore_calls == 0);
  state.promotion_result = true;
  assert(state.try_restore_once_if_valid({}, 4));
  assert(state.promotions == 2);
}

void test_current_promotion_requires_actual_retirement() {
  using namespace housekeeping_fake;
  reset();
  HousekeepingState absent;
  assert(absent.promote_current_snapshot_to_previous());
  assert(!absent.session_saved.load() && copies == 0);

  reset(); files["current"].present = true; files["current"].existence_fails = true;
  HousekeepingState unreadable;
  assert(!unreadable.promote_current_snapshot_to_previous());
  assert(unreadable.session_saved.load() && copies == 0);

  reset(); files["current"].present = true; files["previous"].present = true; copy_succeeds = false;
  HousekeepingState copy_failed;
  assert(!copy_failed.promote_current_snapshot_to_previous());
  assert(files["current"].present && files["previous"].present);
  assert(files["current"].removes == 0 && files["previous"].removes == 0);

  reset(); files["current"].present = true; files["current"].remove_fails = true;
  HousekeepingState remove_failed;
  assert(!remove_failed.promote_current_snapshot_to_previous());
  assert(files["current"].present && files["previous"].present && remove_failed.session_saved.load());
  files["current"].remove_fails = false;
  assert(remove_failed.promote_current_snapshot_to_previous());
  assert(!files["current"].present && files["previous"].present && !remove_failed.session_saved.load());

  reset(); files["current"].present = true; files["current"].remains_after_remove = true;
  HousekeepingState survived;
  assert(!survived.promote_current_snapshot_to_previous());
  assert(survived.session_saved.load() && files["current"].present);

  reset(); files["current"].present = true; files["current"].existence_fails_after_remove = true;
  HousekeepingState unconfirmed;
  assert(!unconfirmed.promote_current_snapshot_to_previous());
  assert(unconfirmed.session_saved.load());
}

void test_golden_confirmation_waits_for_session_file_cleanup() {
  using namespace housekeeping_fake;
  reset();
  HousekeepingState absent;
  assert(absent.finish_verified_golden());
  assert(!absent.session_saved.load() && absent.health_clears == 1);

  reset(); files["current"].present = true; files["previous"].present = true;
  files["previous"].remove_fails = true;
  HousekeepingState previous_failed;
  assert(!previous_failed.finish_verified_golden());
  assert(files["current"].present && files["current"].removes == 0);
  assert(previous_failed.session_saved.load() && previous_failed.health_clears == 0);
  files["previous"].remove_fails = false;
  files["current"].remove_fails = true;
  assert(!previous_failed.finish_verified_golden());
  assert(!files["previous"].present && files["current"].present);
  assert(previous_failed.session_saved.load() && previous_failed.health_clears == 0);
  files["current"].remove_fails = false;
  assert(previous_failed.finish_verified_golden());
  assert(!files["current"].present && !previous_failed.session_saved.load() && previous_failed.health_clears == 1);
}

void test_disarm_keeps_durable_recovery_task() {
  ServiceState state;
  task_deletes = 0;
  state.disarm_restore_requests("host resumed");
  assert(!state.restore_requested.load() && !state.host_loss_recovery.load());
  assert(task_deletes == 0);
}
int main() {
  test_baseline_capture_and_task_failures();
  test_headless_is_positive_and_does_not_erase_baseline();
  test_missing_baseline_retains_failed_recovery();
  test_visible_fallback_is_bounded_and_requires_idle_lost_host();
  test_disarm_keeps_durable_recovery_task();
  test_failed_current_retirement_keeps_restore_pending();
  test_current_promotion_requires_actual_retirement();
  test_golden_confirmation_waits_for_session_file_cleanup();
  test_known_baseline_origins_require_positive_readback();
  test_slow_failed_attempt_reaches_terminal_timeout();
  test_failed_authority_never_applies_an_alternate_exact_tier();
  std::cout << "legacy display recovery safety cases passed\n";
}
'''
with tempfile.TemporaryDirectory(prefix='legacy-display-safety-') as temporary:
    directory = Path(temporary)
    path = directory / 'test.cpp'
    path.write_text(program)
    binary = directory / 'test'
    subprocess.run([compiler, '-std=c++20', '-Wall', '-Wextra', '-Werror', '-I', str(root), str(path), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)

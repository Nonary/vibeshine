#!/usr/bin/env python3
"""Run request reads of app_resume_terminate against concurrent config applies.

Compiles the production reset helper, apply_config entry, app_resume_terminate
parse and publication, the hot-apply lock entry, the real remote_session
decisions, and the three nvhttp statements that read the setting: serverinfo's
busy state, applist's projection and the launch gate for cached tiles.
HTTP transport, the rest of configuration parsing, file I/O and display side
effects are not compiled. Unrelated config objects are stand-ins.

Deterministic pauses check every stage of an apply that overlaps a request:
requests never wait for the apply write gate and never see the reset default
or a half-applied value. Free-running applies then race request reads, under
ThreadSanitizer when the compiler and host can run it.
"""
import os
import pathlib
import subprocess
import sys
import tempfile

root = pathlib.Path(sys.argv[1]) if len(sys.argv) > 1 else pathlib.Path(__file__).resolve().parents[2]
compiler = sys.argv[2] if len(sys.argv) > 2 else "c++"
config = (root / "src/config.cpp").read_text()
nvhttp = (root / "src/nvhttp.cpp").read_text()


def between(source, start, end):
    first = source.index(start)
    return source[first:source.index(end, first)]


def function(source, first):
    begin = source.index(first)
    depth = 0
    for end in range(source.index("{", begin), len(source)):
        depth += (source[end] == "{") - (source[end] == "}")
        if depth == 0:
            return source[begin:end + 1] + "\n"
    raise ValueError("unterminated function")


publication_state = between(config, "    std::atomic<bool> g_app_resume_terminate", "\n") + "\n"
getter = function(config, "  bool get_app_resume_terminate() {")
reset = function(config, "    void reset_runtime_config_to_defaults() {")
apply_entry = between(config, "  void apply_config(std::unordered_map<std::string, std::string> &&vars) {", "#ifndef __ANDROID__")
helpers = between(config, "  void string_f(", "  void string_restricted_f(")
to_bool = function(config, "  bool to_bool(std::string &boolean) {")
bool_f = function(config, "  void bool_f(std::unordered_map<std::string, std::string> &vars, const std::string &name, bool &input) {")
parse = between(config, "    bool app_resume_terminate = true;", '    generic_f(vars, "dd_configuration_option"')
publication = between(config, "    g_app_resume_terminate.store(", "\n") + "\n"
hot_entry = between(config, "  void apply_config_now(const display_transition::before_mutation_f &before_display_mutation) {", "    try {")
hot_install = between(config, "      auto install_effective_configuration = [&] {", "        if (sunshine.session_history_enabled")

serverinfo = between(nvhttp, "    const bool expose_active_game = remote_session::exposes_active_game(", '    tree.put("root.currentgame"')
applist = between(nvhttp, "    auto projection = remote_session::project(", "\n") + "\n"
launch_gate = between(nvhttp, "      // Cached Resume and Terminate tiles follow the setting that hides them.", "      const auto decision = remote_session::dispatch(")
for name, statement in {"serverinfo": serverinfo, "applist": applist, "launch": launch_gate}.items():
    # Each request path must read the published value, never the reset-in-progress config objects.
    assert "config::get_app_resume_terminate()" in statement, name
    assert "config::video" not in statement, name
assert launch_gate.count("        return;\n") == 1
launch_gate = launch_gate.replace("        return;\n", "        return false;\n")

program = r'''
#include "src/remote_session.h"
#include <algorithm>
#include <atomic>
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <vector>
using namespace std::literals;
// Scheduling hooks are fixture-only. Production has no hooks or extra locks.
std::function<void(int)> apply_test_hook = [](int) {};
namespace display_transition { using before_mutation_f = std::function<void()>; }
namespace proc::catalog { struct config_apply_scope { ~config_apply_scope() {} }; }
namespace config {
std::shared_mutex g_apply_gate;
std::mutex g_apply_mutex;
std::unordered_map<std::string, std::string> fixture_hot_vars;
''' + publication_state + getter + r'''
// Unrelated config objects only satisfy the production reset helper's interface.
int video, audio, stream, input, frame_limiter, rtss, lossless_scaling;
constexpr int default_video = 0, default_audio = 0, default_stream = 0, default_input = 0,
  default_frame_limiter = 0, default_rtss = 0, default_lossless_scaling = 0;
struct sunshine_t { std::string username, password, salt, config_file, cmd; } sunshine;
const sunshine_t default_sunshine;
''' + reset + helpers + to_bool + bool_f + r'''
// The production entry calls the full production reset before this option block.
''' + apply_entry + r'''
  (void) control_peer_timeout;
  apply_test_hook(1); // actual reset completed; the setting is still local/default
''' + parse + r'''
  apply_test_hook(2); // actual parse completed; not yet published
  apply_test_hook(3); // stand-in for the omitted unrelated parse/side effects
''' + publication + r'''
  apply_test_hook(4); // store completed; the writer still holds the apply gate
}
// Compile the real hot-apply lock entry and effective-install call. File I/O,
// override merges, display side effects and the outer error handler are omitted.
''' + hot_entry + r'''
    (void) before_display_mutation;
    auto vars = std::move(fixture_hot_vars);
''' + hot_install + r'''
      };
    install_effective_configuration();
  }
}

struct tree_t {
  void put(const char *, int) {}
  void put(const char *, const char *) {}
};
struct remote_role_gate_snapshot_t {
  remote_session::owner_t owner;
  bool active {};
};

const remote_session::app_t game_app {42, "game-uuid", "Game", false};
const std::vector<remote_session::app_t> configured {game_app, {77, "other-uuid", "Other", false}};
// Another client owns the running game; the caller is a second paired device.
const remote_session::game_t running_game {.running = true, .owner_uuid = "owner", .generation = 1, .app = game_app};
const remote_session::caller_t viewer {.uuid = "viewer", .paired = true, .may_view = true, .may_launch = true, .may_terminate = true};
const std::vector<remote_session::control_e> controls {
  remote_session::control_e::resume, remote_session::control_e::terminate,
  remote_session::control_e::running_game, remote_session::control_e::input, remote_session::control_e::monitor,
};

struct scenario_t {
  const char *name;
  remote_role_gate_snapshot_t remote_gate;
};
const std::vector<scenario_t> scenarios {
  {"no remote session", {}},
  {"another client's remote session", {.owner = {}, .active = true}},
  {"caller's Remote Input", {.owner = {.role = remote_session::role_e::input}, .active = true}},
  {"caller's Remote Monitor", {.owner = {.role = remote_session::role_e::monitor, .retained = true, .ready = true}, .active = true}},
};

struct view_t {
  bool free {};
  std::int32_t current_game {};
  std::vector<std::int32_t> ids;
  bool operator==(const view_t &) const = default;
  bool shows(std::initializer_list<std::int32_t> any) const {
    return std::any_of(ids.begin(), ids.end(), [&](auto id) { return std::find(any.begin(), any.end(), id) != any.end(); });
  }
};
view_t summarize(const remote_session::projection_t &projection) {
  view_t view {projection.free, projection.current_game, {}};
  for (const auto &entry : projection.catalogue) view.ids.push_back(entry.id);
  return view;
}

// Each function below is one request, built around the production statement.
bool serverinfo_busy(const remote_role_gate_snapshot_t &remote_gate) {
  const auto &caller = viewer;
  const auto &game = running_game;
  const bool replacement_confirmation_active = false;
''' + serverinfo + r'''
  return expose_active_game;
}
view_t applist_view(const remote_role_gate_snapshot_t &remote_gate) {
  const auto &caller = viewer;
  const auto &game = running_game;
  const auto &remote_configured_apps = configured;
''' + applist + r'''
  return summarize(projection);
}
bool launch_allowed(remote_session::control_e synthetic_control, const remote_role_gate_snapshot_t &remote_gate) {
  const auto &owner = remote_gate.owner;
  tree_t tree;
''' + launch_gate + r'''
  return true;
}

struct decisions_t {
  bool busy {};
  view_t view;
  std::vector<bool> launch;
  bool operator==(const decisions_t &) const = default;
};
decisions_t expected(const scenario_t &scenario, bool value) {
  const auto &gate = scenario.remote_gate;
  decisions_t result {
    remote_session::exposes_active_game(viewer, running_game, gate.owner, gate.active, false, value),
    summarize(remote_session::project(viewer, running_game, gate.owner, configured, gate.active, value)),
    {},
  };
  for (const auto control : controls) result.launch.push_back(remote_session::control_enabled(control, gate.owner.role, value, gate.active));
  return result;
}

// Every request sees exactly this completed value.
void check(const scenario_t &scenario, bool value) {
  const auto want = expected(scenario, value);
  assert(serverinfo_busy(scenario.remote_gate) == want.busy);
  assert(applist_view(scenario.remote_gate) == want.view);
  for (std::size_t i = 0; i != controls.size(); ++i) assert(launch_allowed(controls[i], scenario.remote_gate) == want.launch[i]);
}
// While values alternate, each request sees one completed value or the other.
void check_either(const scenario_t &scenario) {
  const auto on = expected(scenario, true), off = expected(scenario, false);
  const auto busy = serverinfo_busy(scenario.remote_gate);
  assert(busy == on.busy || busy == off.busy);
  const auto view = applist_view(scenario.remote_gate);
  assert(view == on.view || view == off.view);
  for (std::size_t i = 0; i != controls.size(); ++i) {
    const auto allowed = launch_allowed(controls[i], scenario.remote_gate);
    assert(allowed == on.launch[i] || allowed == off.launch[i]);
  }
}

void apply(std::optional<std::string> value) {
  config::fixture_hot_vars = {{"realtime_stats_enabled", "true"}};
  if (value) config::fixture_hot_vars.emplace("app_resume_terminate", *value);
  config::apply_config_now({});
}

// Pause a real apply at `stage` while requests run against it.
void overlap(int stage, bool before, const std::optional<std::string> &next, bool expected_before,
             bool expected_after, bool fail = false) {
  apply(before ? "true" : "false");
  check(scenarios.front(), before);
  std::mutex mutex;
  std::condition_variable cv;
  bool reached = false, proceed = false, caught = false;
  apply_test_hook = [&](int current) {
    if (current != stage) return;
    std::unique_lock lock(mutex);
    reached = true;
    cv.notify_one();
    cv.wait(lock, [&] { return proceed; });
    if (fail) throw std::runtime_error("fixture late apply failure");
  };
  // An unrelated key comes along so a save that leaves the setting alone is covered too.
  config::fixture_hot_vars = {{"realtime_stats_enabled", "true"}};
  if (next) config::fixture_hot_vars.emplace("app_resume_terminate", *next);
  std::thread writer([&] {
    try { config::apply_config_now({}); }
    catch (const std::runtime_error &) { caught = true; }
  });
  {
    std::unique_lock lock(mutex);
    cv.wait(lock, [&] { return reached; });
  }
  // The writer owns the apply gate; requests neither wait for it nor see a partial value.
  assert(!config::g_apply_gate.try_lock_shared());
  for (const auto &scenario : scenarios) check(scenario, expected_before);
  {
    std::lock_guard lock(mutex);
    proceed = true;
  }
  cv.notify_one();
  writer.join();
  apply_test_hook = [](int) {};
  assert(caught == fail);
  for (const auto &scenario : scenarios) check(scenario, expected_after);
}

// Free-running applies race request threads across every scenario.
std::uint64_t hammer(const std::vector<std::string> &values, bool either, int applies) {
  constexpr int reader_count = 3;
  std::atomic_bool run {true};
  std::atomic<int> ready {0};
  std::atomic<std::uint64_t> reads {0};
  std::vector<std::thread> readers;
  for (int r = 0; r != reader_count; ++r) {
    readers.emplace_back([&, r] {
      bool counted = false;
      for (std::size_t i = r; run.load(std::memory_order_acquire); ++i) {
        const auto &scenario = scenarios[i % scenarios.size()];
        if (either) check_either(scenario);
        else check(scenario, false);
        reads.fetch_add(1, std::memory_order_relaxed);
        if (!counted) {
          counted = true;
          ready.fetch_add(1, std::memory_order_release);
        }
      }
    });
  }
  while (ready.load(std::memory_order_acquire) != reader_count) std::this_thread::yield();
  for (int i = 0; i != applies; ++i) apply(values[i % values.size()]);
  run.store(false, std::memory_order_release);
  for (auto &reader : readers) reader.join();
  assert(reads.load() > 0);
  return reads.load();
}

int main() {
  // Before the first apply the setting reads as its default.
  assert(config::get_app_resume_terminate());
  check(scenarios.front(), true);

  // The setting must change what a second device sees when no remote session
  // is active, or the race checks below could not tell the values apart.
  const auto on = expected(scenarios.front(), true), off = expected(scenarios.front(), false);
  assert(!on.busy && off.busy);
  assert(on.view.shows({remote_session::resume_id, remote_session::secondary_resume_id}));
  assert(on.view.shows({remote_session::terminate_id, remote_session::secondary_terminate_id}));
  assert(!off.view.shows({remote_session::resume_id, remote_session::secondary_resume_id,
                          remote_session::terminate_id, remote_session::secondary_terminate_id}));
  assert(on.launch[0] && on.launch[1] && !off.launch[0] && !off.launch[1]);
  // While any Remote Input or Remote Monitor is active, the pair is back regardless.
  for (std::size_t i = 1; i != scenarios.size(); ++i) {
    assert(expected(scenarios[i], true) == expected(scenarios[i], false));
  }

  for (int stage : {1, 2, 3}) {
    // A save that keeps the setting off never shows the reset default (on).
    overlap(stage, false, "false", false, false);
    overlap(stage, false, std::nullopt, false, true);  // removing the key restores the default
    overlap(stage, false, "true", false, true);
    overlap(stage, true, "false", true, false);
    overlap(stage, true, "off", true, false);
    // A failed apply leaves the last completed value in place.
    overlap(stage, false, "true", false, false, true);
    overlap(stage, true, "false", true, true, true);
  }
  // The release store is the cutover, even before the writer releases the gate.
  overlap(4, false, "true", true, true);
  overlap(4, true, "false", false, false);
  overlap(4, false, "false", false, false);

  apply("false");
  const auto steady = hammer({"false", "no"}, false, 5000);
  const auto toggling = hammer({"true", "false"}, true, 5000);
  std::printf("steady reads=%llu toggling reads=%llu\n", (unsigned long long) steady, (unsigned long long) toggling);
}
'''

TSAN_PROBE = r'''
#include <atomic>
#include <thread>
int main() {
  std::atomic<int> value {0};
  std::thread worker([&] { value.store(1, std::memory_order_release); });
  worker.join();
  return value.load(std::memory_order_acquire) == 1 ? 0 : 1;
}
'''


def thread_sanitizer_flags(directory):
    """Use ThreadSanitizer only where this compiler can build and run it."""
    probe = pathlib.Path(directory) / "tsan_probe.cpp"
    binary = pathlib.Path(directory) / "tsan_probe"
    probe.write_text(TSAN_PROBE)
    flags = ["-fsanitize=thread", "-g"]
    try:
        subprocess.run([compiler, "-std=c++23", "-pthread", *flags, str(probe), "-o", str(binary)],
                       check=True, capture_output=True, timeout=120)
        subprocess.run([str(binary)], check=True, capture_output=True, timeout=30)
    except (subprocess.CalledProcessError, subprocess.TimeoutExpired, OSError):
        return []
    return flags


with tempfile.TemporaryDirectory(prefix="resume-terminate-publication-") as directory:
    sanitizer = thread_sanitizer_flags(directory)
    src = pathlib.Path(directory) / "test.cpp"
    remote_session = pathlib.Path(directory) / "remote_session.o"
    binary = pathlib.Path(directory) / "test"
    src.write_text(program)
    common = [compiler, "-std=c++23", "-O1", "-pthread", *sanitizer, "-I", str(root)]
    # Production remote_session.cpp builds with the project's own warning set.
    subprocess.run([*common, "-c", str(root / "src/remote_session.cpp"), "-o", str(remote_session)], check=True)
    subprocess.run([*common, "-Wall", "-Wextra", "-Werror", "-Wno-missing-field-initializers",
                    str(src), str(remote_session), "-o", str(binary)], check=True)
    # halt_on_error turns any reported race into a failing exit status.
    env = dict(os.environ, TSAN_OPTIONS="halt_on_error=1 second_deadlock_stack=1") if sanitizer else None
    subprocess.run([str(binary)], check=True, timeout=120, env=env)
print("app_resume_terminate publication: staged overlaps, failed applies and concurrent request reads passed"
      + (" under ThreadSanitizer" if sanitizer else " (ThreadSanitizer unavailable for this compiler)"))

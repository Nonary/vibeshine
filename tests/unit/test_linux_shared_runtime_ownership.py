"""Run the production Linux idle finalizer against the real display coordinator.

Only transport observations and asynchronous platform effects are faked. This
exercises paused settings, failed normal retirement, capture drain and retained
Monitor admission together, including the verified restore completion boundary.
Usage: python3 test_linux_shared_runtime_ownership.py REPO CXX [JSON_INCLUDE ...]
"""
import pathlib
import re
import subprocess
import sys
import tempfile

root = pathlib.Path(sys.argv[1]).resolve()
compiler = sys.argv[2]


def definition(source, signature):
    tokens = re.findall(r"[A-Za-z_]\w*|::|[^\w\s]", signature)
    match = re.search(r"\s*".join(re.escape(token) for token in tokens), source)
    if match is None:
        raise ValueError(f"Production definition not found: {signature}")
    brace = source.index("{", match.start())
    depth, end = 1, brace + 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[match.start():end]


stream = (root / "src/stream.cpp").read_text()
header = (root / "src/stream.h").read_text()
rtsp = (root / "src/rtsp.cpp").read_text()
webrtc = (root / "src/webrtc_stream.cpp").read_text()
nvhttp = (root / "src/nvhttp.cpp").read_text()
context = definition(header, "struct shared_runtime_finalize_context_t") + ";"
functions = "\n".join(definition(stream, name) for name in (
    "bool has_capture_runtime_owner(const shared_runtime_finalize_context_t &context)",
    "bool has_shared_runtime_owner(const shared_runtime_finalize_context_t &context)",
    "void arm_shared_runtime_cleanup(",
    "void start_shared_platform_if_needed()",
    "bool finalize_shared_runtime_if_idle(",
))
observations = "\n".join(definition(webrtc, name) for name in (
    "bool has_active_or_pending_sessions()", "bool has_capture_active()", "unsigned int teardown_session_count()",
))
program = r'''
#include <algorithm>
#include <array>
#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>
#include "src/remote_display_topology.h"
#include "src/platform/linux/private_display_cleanup_policy.h"
#undef _WIN32
#ifndef __linux__
#define __linux__
#endif
struct Log { template<class T> Log &operator<<(const T &) { return *this; } };
#define BOOST_LOG(level) Log {}
using guid_bytes_t = std::array<std::uint8_t,16>;
namespace fake {
  int app = 0;
  unsigned pending = 0, starting = 0, rtsp = 0, stops = 0, clears = 0;
  bool apply_ok = true;
  std::vector<std::chrono::milliseconds> restores;
  std::vector<std::string> composed;
  std::mutex lifecycle;
}
namespace rtsp_stream {
  bool has_pending_launches() { return fake::pending != 0; }
  unsigned startup_count() { return fake::starting; }
  unsigned session_count_no_cleanup() { return fake::rtsp; }
''' + definition(rtsp, "bool has_pending_launch_or_startup()") + r'''
}
namespace webrtc_stream {
  std::atomic_uint active_sessions {0}, teardown_sessions {0};
  struct { std::atomic_uint pending_session_creations {0}; std::atomic_bool active {false}; } webrtc_capture;
''' + observations + r'''
}
namespace proc { struct { int current_app_id() { return fake::app; } } proc; }
namespace nvhttp { void reconcile_remote_monitor_owners() {} }
namespace config {
  struct { struct {
    bool config_revert_on_disconnect = false;
    int paused_virtual_display_timeout_secs = 0;
    std::chrono::milliseconds config_revert_delay {0};
  } dd; } video;
  void set_runtime_output_name_override(std::nullopt_t) {}
  void clear_runtime_config_overrides() { ++fake::clears; }
  void apply_config_now() {}
  void maybe_apply_deferred() {}
}
namespace platf {
  void streaming_will_start() {}
  void streaming_will_stop() { ++fake::stops; }
}
namespace platf::linux_display {
  struct Backend {
    void schedule_revert(std::chrono::milliseconds delay, std::string_view) { fake::restores.push_back(delay); }
    bool revert() { schedule_revert(std::chrono::milliseconds {0}, "immediate"); return true; }
  };
  Backend &backend() { static Backend value; return value; }
}
namespace stream::session {
''' + context + r'''
  std::atomic_uint running_sessions {0}, teardown_sessions {0};
  bool shared_platform_started = false, shared_runtime_cleanup_armed = false;
  bool shared_runtime_force_display_revert_when_idle = false;
  std::optional<guid_bytes_t> shared_runtime_virtual_display_guid_bytes;
  void arm_shared_runtime_cleanup(std::optional<guid_bytes_t> = std::nullopt);
''' + functions + r'''
}
namespace nvhttp {
  bool has_stream_session_activity() { return stream::session::has_capture_runtime_owner({}); }
''' + definition(nvhttp, "void restore_linux_display_after_failed_start()") + r'''
}
using namespace stream::session;
auto &topology = remote_display_topology::instance();
void reset() {
  topology.shutdown(true);
  topology.set_physical_baseline({}); topology.set_layout({});
  fake::app = 0; fake::pending = fake::starting = fake::rtsp = fake::stops = fake::clears = 0;
  fake::apply_ok = true; fake::restores.clear(); fake::composed.clear();
  config::video.dd = {};
  running_sessions = 0; teardown_sessions = 0;
  webrtc_stream::active_sessions = 0; webrtc_stream::teardown_sessions = 0;
  webrtc_stream::webrtc_capture.active = false; webrtc_stream::webrtc_capture.pending_session_creations = 0;
  shared_platform_started = shared_runtime_cleanup_armed = shared_runtime_force_display_revert_when_idle = false;
  shared_runtime_virtual_display_guid_bytes.reset();
  topology.set_runtime_callbacks({
    .create_or_reclaim = [](const auto &, const auto &, const auto &) { return true; },
    .apply_composed_topology = [](const auto &nodes) {
      if (!fake::apply_ok) return false;
      fake::composed.clear(); for (const auto &node : nodes) fake::composed.push_back(node.id);
      return true;
    },
    .exact_target_has_current_mode_and_dxgi = [](const auto &id, const auto &) { return std::optional<std::string> {id}; },
    .remove_owned_display = [](const auto &) { return true; },
  });
  start_shared_platform_if_needed();
}
std::uint64_t reserve() {
  const auto r = topology.reserve_normal_game_identity("desktop", "Desktop", {});
  assert(r.accepted && r.token); return r.token;
}
bool finalize(const shared_runtime_finalize_context_t &ctx = {}) {
  std::lock_guard lock(fake::lifecycle);
  return finalize_shared_runtime_if_idle("regression", ctx);
}
void paused_settings_and_resume() {
  for (int policy = 0; policy < 3; ++policy) {
    reset(); fake::app = 42; const auto token = reserve();
    config::video.dd.config_revert_on_disconnect = policy == 0;
    config::video.dd.paused_virtual_display_timeout_secs = policy == 1 ? 45 : 0;
    config::video.dd.config_revert_delay = std::chrono::milliseconds {125};
    assert(finalize()); assert(fake::stops == 1 && fake::clears == 0);
    assert(topology.managed_client_identity_count() == 1);
    if (policy == 2) { assert(fake::restores.empty()); continue; }
    assert(fake::restores.size() == 1);
    assert(fake::restores.front() == (policy == 0 ? std::chrono::milliseconds {125} : std::chrono::seconds {45}));
    // Async success commits bookkeeping only after actual platform restoration.
    topology.complete_restored_normal_game_cleanup(true);
    assert(!topology.retain_normal_game_capture("desktop", token));
    assert(topology.activate_or_resume("monitor", "Monitor", {}, 1).ready);
    assert(fake::composed == std::vector<std::string> {"monitor"});
    assert(topology.explicit_release("monitor", 1, "disconnect"));
    const auto resumed = topology.reserve_normal_game_identity("desktop", "Desktop", {});
    assert(resumed.accepted && !resumed.newly_reserved && resumed.token == token);
    assert(topology.retain_normal_game_capture("desktop", token));
  }
}
void failed_resume_rearms_paused_policy() {
  for (int policy = 0; policy < 3; ++policy) {
    reset(); fake::app = 42; const auto token = reserve();
    topology.complete_restored_normal_game_cleanup(true);
    const auto resumed = topology.reserve_normal_game_identity("desktop", "Desktop", {});
    assert(resumed.token == token && !resumed.newly_reserved);
    config::video.dd.config_revert_on_disconnect = policy == 0;
    config::video.dd.paused_virtual_display_timeout_secs = policy == 1 ? 17 : 0;
    config::video.dd.config_revert_delay = std::chrono::milliseconds {200};
    nvhttp::restore_linux_display_after_failed_start();
    if (policy == 2) assert(fake::restores.empty());
    else {
      assert(fake::restores.size() == 1);
      assert(fake::restores.front() == (policy == 0 ? std::chrono::milliseconds {200} : std::chrono::seconds {17}));
    }
  }
  reset(); const auto token = reserve();
  auto capture = topology.retain_normal_game_capture("desktop", token);
  nvhttp::restore_linux_display_after_failed_start(); assert(fake::restores.empty());
  capture.reset(); assert(topology.activate_or_resume("monitor", "Monitor", {}, 1).ready);
  nvhttp::restore_linux_display_after_failed_start(); assert(fake::restores.empty());
}
void drained_failure_reaches_restore() {
  reset(); const auto token = reserve(); (void) token;
  fake::apply_ok = false;
  assert(finalize());
  assert(fake::restores.size() == 1 && topology.normal_game_release_pending());
  assert(topology.managed_client_identity_count() == 1);
  // A failed platform restore must leave the pending record intact.
  assert(!topology.has_live_managed_client_identity());
  topology.complete_restored_normal_game_cleanup(true);
  assert(topology.managed_client_identity_count() == 0);
  fake::apply_ok = true;
  assert(topology.activate_or_resume("monitor", "Monitor", {}, 1).ready);
  assert(fake::composed == std::vector<std::string> {"monitor"});
}
void captures_and_monitors_fence_restore() {
  for (int owner = 0; owner < 10; ++owner) {
    reset(); fake::app = 42; const auto token = reserve();
    config::video.dd.config_revert_on_disconnect = true;
    std::shared_ptr<void> lease;
    switch (owner) {
      case 0: fake::pending = 1; break;
      case 1: fake::starting = 1; break;
      case 2: fake::rtsp = 1; break;
      case 3: running_sessions = 1; break;
      case 4: teardown_sessions = 1; break;
      case 5: webrtc_stream::active_sessions = 1; break;
      case 6: webrtc_stream::webrtc_capture.pending_session_creations = 1; break;
      case 7: webrtc_stream::webrtc_capture.active = true; break;
      case 8: webrtc_stream::teardown_sessions = 1; break;
      case 9: lease = topology.retain_normal_game_capture("desktop", token); break;
    }
    assert(!finalize()); assert(fake::restores.empty() && fake::stops == 0);
  }
  reset(); fake::app = 42; reserve();
  assert(topology.activate_or_resume("monitor", "Monitor", {}, 1).ready);
  topology.transport_lost("monitor", 1);
  assert(!finalize()); assert(fake::restores.empty());
  assert(topology.explicit_release("monitor", 1, "disconnect"));
  config::video.dd.paused_virtual_display_timeout_secs = 4;
  assert(finalize()); assert(fake::restores.size() == 1);
}
int main() {
  paused_settings_and_resume(); failed_resume_rearms_paused_policy(); drained_failure_reaches_restore(); captures_and_monitors_fence_restore();
  std::cout << "PASS Linux shared runtime ownership regression\n";
}
'''
with tempfile.TemporaryDirectory(prefix="linux-shared-runtime-") as directory:
    source = pathlib.Path(directory) / "test.cpp"
    binary = pathlib.Path(directory) / "test"
    source.write_text(program)
    command = [compiler, "-std=c++20", "-Wall", "-Wextra", "-Werror", "-Wno-unused-parameter", "-Wno-missing-field-initializers", "-pthread", "-I", str(root)]
    for include in sys.argv[3:]:
        command.extend(["-I", include])
    command.extend([str(source), str(root / "src/remote_display_topology.cpp"), "-o", str(binary)])
    subprocess.run(command, check=True)
    subprocess.run([str(binary)], check=True, timeout=40)

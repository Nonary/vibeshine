"""Execute the production Windows idle finalizer with the real topology owner.

Only transport observations and platform effects are simulated. The finalizer,
capture ownership predicates, and coordinator execute their production code.
Run with: python3 test_shared_runtime_idle_ownership.py REPO CXX [JSON_INCLUDE ...]
Extra include directories support a FetchContent or otherwise non-system JSON.
"""

import pathlib
import re
import subprocess
import sys
import tempfile


root = pathlib.Path(sys.argv[1]).resolve() if len(sys.argv) > 1 else pathlib.Path(__file__).resolve().parents[2]
compiler = sys.argv[2] if len(sys.argv) > 2 else "clang++"
include_dirs = sys.argv[3:]


def definition(source, signature):
    """Extract the unchanged definition, tolerating formatting differences."""
    tokens = re.findall(r"[A-Za-z_]\w*|::|[^\w\s]", signature)
    match = re.search(r"\s*".join(re.escape(token) for token in tokens), source)
    if match is None:
        raise ValueError(f"Production definition not found: {signature}")
    start = match.start()
    brace = source.index("{", start)
    depth, end = 1, brace + 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end]


stream = (root / "src/stream.cpp").read_text()
stream_header = (root / "src/stream.h").read_text()
rtsp = (root / "src/rtsp.cpp").read_text()
webrtc = (root / "src/webrtc_stream.cpp").read_text()
context = definition(stream_header, "struct shared_runtime_finalize_context_t") + ";"
stream_functions = "\n".join(
    definition(stream, signature)
    for signature in (
        "bool has_capture_runtime_owner(const shared_runtime_finalize_context_t &context)",
        "bool has_shared_runtime_owner(const shared_runtime_finalize_context_t &context)",
        "void arm_shared_runtime_cleanup(",
        "void start_shared_platform_if_needed()",
        "bool finalize_shared_runtime_if_idle(",
    )
)
rtsp_pending = definition(rtsp, "bool has_pending_launch_or_startup()")
webrtc_observations = "\n".join(
    definition(webrtc, signature)
    for signature in (
        "bool has_active_or_pending_sessions()",
        "bool has_capture_active()",
        "unsigned int teardown_session_count()",
    )
)

program = r'''
#include <algorithm>
#include <array>
#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <functional>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
#include "src/remote_display_topology.h"

// Select only the production Windows branches, after loading host C++ headers.
#undef __linux__
#ifndef _WIN32
#define _WIN32
#endif
#include "src/platform/windows/virtual_display_cleanup.h"

struct NullLog {
  template<class T> NullLog &operator<<(const T &) { return *this; }
};
#define BOOST_LOG(level) NullLog {}
struct GUID { std::uint8_t bytes[16] {}; };
using guid_bytes_t = std::array<std::uint8_t, 16>;

namespace fake {
  int app_id = 0;
  unsigned rtsp_pending = 0, rtsp_starting = 0, rtsp_sessions = 0;
  unsigned applies = 0, empty_applies = 0, cleanups = 0, restores = 0;
  unsigned starts = 0, stops = 0, cancelled_all = 0, cleared_config = 0;
  unsigned applied_config = 0, deferred_config = 0, reconciliations = 0;
  bool reject_empty_topology = true, restore_succeeds = true, deferred_revert = false;
  std::vector<std::string> removed, events;
  std::set<std::string> displays;
  std::vector<GUID> cancelled_clients;
  std::optional<guid_bytes_t> cleanup_guid;
  std::mutex lifecycle;
}
namespace rtsp_stream {
  bool has_pending_launches() { return fake::rtsp_pending != 0; }
  unsigned startup_count() { return fake::rtsp_starting; }
  unsigned session_count_no_cleanup() { return fake::rtsp_sessions; }
''' + rtsp_pending + r'''
}
namespace webrtc_stream {
  std::atomic_uint active_sessions {0}, teardown_sessions {0};
  struct {
    std::atomic_uint pending_session_creations {0};
    std::atomic_bool active {false};
  } webrtc_capture;
''' + webrtc_observations + r'''
}
namespace proc {
  struct Process {
    int current_app_id() const { return fake::app_id; }
    // A commandless app has no child process but still owns a positive app ID.
    bool running() const { return false; }
  } proc;
  bool consume_deferred_display_revert() {
    return std::exchange(fake::deferred_revert, false);
  }
}
namespace nvhttp {
  void reconcile_remote_monitor_owners() { ++fake::reconciliations; }
}
namespace config {
  struct {
    struct {
      bool config_revert_on_disconnect = false;
      int paused_virtual_display_timeout_secs = 0;
    } dd;
  } video;
  void set_runtime_output_name_override(std::nullopt_t) {
    fake::events.push_back("clear_output");
  }
  void clear_runtime_config_overrides() { ++fake::cleared_config; }
  void apply_config_now() { ++fake::applied_config; }
  void maybe_apply_deferred() { ++fake::deferred_config; }
}
namespace display_helper_integration {
  void clear_pending_apply() { fake::events.push_back("clear_pending"); }
  bool revert(bool keep_display) {
    assert(keep_display);
    assert(remote_display_topology::instance().managed_client_identity_count() == 0);
    assert(fake::cancelled_all == 1);
    ++fake::restores;
    fake::events.push_back("restore");
    return fake::restore_succeeds;
  }
}
namespace VDISPLAY {
  struct uuid_t { std::uint8_t b8[16] {}; };
  uuid_t virtualDisplayUuidFromStableId(const std::string &id) {
    uuid_t uuid;
    std::copy_n(id.begin(), std::min(id.size(), sizeof(uuid.b8)), uuid.b8);
    return uuid;
  }
  void cancel_virtual_display_recovery_monitor(const GUID &guid) {
    fake::cancelled_clients.push_back(guid);
  }
  void cancel_all_virtual_display_recovery_monitors() {
    ++fake::cancelled_all;
    fake::events.push_back("cancel_all");
  }
  void restorePhysicalHdrProfiles() { fake::events.push_back("hdr"); }
}
namespace platf {
  void streaming_will_start() { ++fake::starts; }
  void streaming_will_stop() { ++fake::stops; }
  void rtss_set_sync_limiter_override(std::nullopt_t) {}
}
namespace platf::virtual_display_cleanup {
  cleanup_result_t run(
    std::string_view reason, bool enforce_db_restore, revert_order_t order,
    bool prefer_golden, std::optional<guid_bytes_t> guid,
    recovery_monitor_policy_t recovery_policy, cleanup_admission_policy_t admission,
    bool allow_disabled_recovery
  ) {
    assert(reason == "test_idle");
    assert(!enforce_db_restore && prefer_golden);
    assert(order == revert_order_t::remove_before_restore);
    assert(recovery_policy == recovery_monitor_policy_t::disengage_before_admission);
    assert(admission == cleanup_admission_policy_t::respect_managed_owners);
    assert(!allow_disabled_recovery);
    assert(remote_display_topology::instance().managed_client_identity_count() == 0);
    assert(remote_display_topology::instance().generic_virtual_display_cleanup_allowed());
    assert(fake::cancelled_all == 1);
    ++fake::cleanups;
    fake::cleanup_guid = guid;
    fake::displays.clear();
    fake::events.push_back("cleanup");
    return {.virtual_displays_removed = true};
  }
}
namespace stream::session {
''' + context + r'''
  std::atomic_uint running_sessions {0}, teardown_sessions {0};
  bool shared_platform_started = false, shared_runtime_cleanup_armed = false;
  bool shared_runtime_force_display_revert_when_idle = false;
  std::optional<guid_bytes_t> shared_runtime_virtual_display_guid_bytes;
  std::atomic_uint g_paused_display_cleanup_generation {0};
  void clear_deferred_stream_start_actions() { fake::events.push_back("clear_start"); }
  void schedule_paused_display_cleanup(
    std::chrono::seconds, std::string_view, bool, std::optional<guid_bytes_t>
  ) { fake::events.push_back("schedule"); }
  void arm_shared_runtime_cleanup(std::optional<guid_bytes_t> = std::nullopt);
''' + stream_functions + r'''
}

namespace {
  using namespace stream::session;
  auto &topology = remote_display_topology::instance();

  void reset(bool arm = true) {
    topology.shutdown(true);
    topology.set_physical_baseline({});
    topology.set_layout({});
    fake::app_id = 0;
    fake::rtsp_pending = fake::rtsp_starting = fake::rtsp_sessions = 0;
    fake::applies = fake::empty_applies = fake::cleanups = fake::restores = 0;
    fake::starts = fake::stops = fake::cancelled_all = fake::cleared_config = 0;
    fake::applied_config = fake::deferred_config = fake::reconciliations = 0;
    fake::reject_empty_topology = fake::restore_succeeds = true;
    fake::deferred_revert = false;
    fake::removed.clear(); fake::events.clear(); fake::displays.clear();
    fake::cancelled_clients.clear(); fake::cleanup_guid.reset();
    webrtc_stream::active_sessions = 0;
    webrtc_stream::teardown_sessions = 0;
    webrtc_stream::webrtc_capture.pending_session_creations = 0;
    webrtc_stream::webrtc_capture.active = false;
    running_sessions = 0; teardown_sessions = 0;
    shared_platform_started = shared_runtime_cleanup_armed = false;
    shared_runtime_force_display_revert_when_idle = false;
    shared_runtime_virtual_display_guid_bytes.reset();
    g_paused_display_cleanup_generation = 0;
    config::video.dd.config_revert_on_disconnect = false;
    config::video.dd.paused_virtual_display_timeout_secs = 0;
    topology.set_runtime_callbacks({
      .create_or_reclaim = [](const auto &id, const auto &, const auto &) {
        fake::displays.insert(id);
        return true;
      },
      .apply_composed_topology = [](const auto &nodes) {
        ++fake::applies;
        if (nodes.empty()) ++fake::empty_applies;
        return !nodes.empty() || !fake::reject_empty_topology;
      },
      .exact_target_has_current_mode_and_dxgi = [](const auto &id, const auto &) {
        return std::optional<std::string> {id};
      },
      .remove_owned_display = [](const auto &id) {
        fake::removed.push_back(id);
        fake::displays.erase(id);
        return true;
      },
    });
    if (arm) start_shared_platform_if_needed();
  }

  std::uint64_t desktop(const std::string &id = "desktop") {
    const auto reservation = topology.reserve_normal_game_identity(id, "Desktop", {});
    assert(reservation.accepted && reservation.newly_reserved && reservation.token != 0);
    fake::displays.insert(id);
    assert(topology.reapply_composed_topology());
    return reservation.token;
  }

  bool finalize(const shared_runtime_finalize_context_t &context = {}) {
    // Match the production caller's serialization around ownership and cleanup.
    std::lock_guard lock(fake::lifecycle);
    return finalize_shared_runtime_if_idle("test_idle", context);
  }

  void assert_deferred(std::size_t identities = 1) {
    assert(!finalize());
    assert(topology.managed_client_identity_count() == identities);
    assert(shared_runtime_cleanup_armed);
    assert(fake::cleanups == 0 && fake::restores == 0 && fake::stops == 0);
    assert(fake::cancelled_all == 0 && fake::cleared_config == 0);
  }

  void assert_cleaned_once() {
    assert(finalize());
    assert(topology.managed_client_identity_count() == 0);
    assert(fake::cleanups == 1 && fake::restores == 0 && fake::stops == 1);
    assert(fake::cancelled_all == 1 && fake::cleared_config == 1);
    assert(fake::applied_config == 1 && fake::deferred_config == 1);
    assert(!shared_runtime_cleanup_armed && !shared_platform_started);
    assert(!has_shared_runtime_owner({}));
    assert(fake::displays.empty());
    assert(!finalize());
    assert(fake::cleanups == 1 && fake::stops == 1 && fake::cancelled_all == 1);
  }

  void headless_desktop_hands_off_cleanup() {
    reset();
    const auto token = desktop();
    auto rtsp_capture = topology.retain_normal_game_capture("desktop", token);
    auto webrtc_capture = topology.retain_normal_game_capture("desktop", token);
    assert(rtsp_capture && webrtc_capture);
    fake::rtsp_sessions = 1;
    webrtc_stream::webrtc_capture.active = true;
    assert_deferred();
    rtsp_capture.reset(); fake::rtsp_sessions = 0;
    assert_deferred();
    webrtc_capture.reset(); webrtc_stream::webrtc_capture.active = false;
    guid_bytes_t guid {}; guid[0] = 17;
    arm_shared_runtime_cleanup(guid);
    start_shared_platform_if_needed();
    assert(fake::starts == 1);
    assert_cleaned_once();
    // An exclusive/headless desktop has no physical composition to apply.
    // Only the existing Windows cleanup path is authorized to remove it.
    assert(fake::applies == 1 && fake::empty_applies == 0 && fake::removed.empty());
    assert(fake::cleanup_guid == guid);
  }

  void every_transport_state_fences_retirement() {
    const std::vector<std::pair<const char *, std::function<void(bool)>>> owners {
      {"RTSP pending launch", [](bool set) { fake::rtsp_pending = set; }},
      {"RTSP startup reservation", [](bool set) { fake::rtsp_starting = set; }},
      {"RTSP session", [](bool set) { fake::rtsp_sessions = set; }},
      {"RTSP capture", [](bool set) { running_sessions = set; }},
      {"RTSP draining", [](bool set) { teardown_sessions = set; }},
      {"WebRTC pending/startup", [](bool set) { webrtc_stream::webrtc_capture.pending_session_creations = set; }},
      {"WebRTC session", [](bool set) { webrtc_stream::active_sessions = set; }},
      {"WebRTC capture", [](bool set) { webrtc_stream::webrtc_capture.active = set; }},
      {"WebRTC draining", [](bool set) { webrtc_stream::teardown_sessions = set; }},
    };
    for (const auto &[name, set_owner] : owners) {
      reset(); desktop(); set_owner(true);
      assert(has_capture_runtime_owner({}) && has_shared_runtime_owner({}));
      assert_deferred();
      assert(fake::applies == 1 && fake::removed.empty());
      assert(!topology.normal_game_release_pending());
      set_owner(false);
      assert_cleaned_once();
      std::cout << "PASS " << name << '\n';
    }
  }

  void ignore_only_the_current_teardown() {
    for (const bool rtsp : {true, false}) {
      reset(); desktop();
      shared_runtime_finalize_context_t context;
      context.ignore_current_rtsp_teardown = rtsp;
      context.ignore_current_webrtc_teardown = !rtsp;
      auto &current = rtsp ? teardown_sessions : webrtc_stream::teardown_sessions;
      auto &other = rtsp ? webrtc_stream::teardown_sessions : teardown_sessions;
      current = 2;
      assert(has_capture_runtime_owner(context) && !finalize(context));
      current = 1; other = 1;
      assert(has_capture_runtime_owner(context) && !finalize(context));
      other = 0;
      fake::rtsp_starting = 1;
      assert(has_capture_runtime_owner(context) && !finalize(context));
      fake::rtsp_starting = 0;
      assert(!has_capture_runtime_owner(context));
      assert(finalize(context));
      assert(fake::cleanups == 1 && topology.managed_client_identity_count() == 0);
    }
  }

  void paused_commandless_app_keeps_identity() {
    reset();
    fake::app_id = 42;
    assert(!proc::proc.running());
    const auto token = desktop();
    assert(!has_capture_runtime_owner({}) && has_shared_runtime_owner({}));
    assert_deferred();
    assert(fake::applies == 1 && fake::removed.empty());
    assert(!topology.normal_game_release_pending());
    assert(fake::cancelled_clients.size() == 1);
    const auto resumed = topology.reserve_normal_game_identity("desktop", "Desktop", {});
    assert(resumed.accepted && !resumed.newly_reserved && resumed.token == token);
    auto capture = topology.retain_normal_game_capture("desktop", token);
    assert(capture);
    capture.reset();
    fake::app_id = 0;
    assert_cleaned_once();
  }

  void retained_monitors_prevent_generic_cleanup() {
    for (const bool shared : {false, true}) {
      reset(); desktop();
      const std::string monitor = shared ? "desktop" : "monitor";
      assert(topology.activate_or_resume(monitor, "Remote Monitor", {}, 7).ready);
      // A transport-less/retryable monitor still owns its stable display.
      topology.note_lease_lost(monitor);
      assert_deferred(1);
      assert(topology.protected_remote_monitor_client_ids() == std::vector<std::string> {monitor});
      assert(!topology.generic_virtual_display_cleanup_allowed());
      assert(fake::displays.contains(monitor));
      assert(fake::cancelled_clients.empty());
      assert(has_shared_runtime_owner({}) && !has_capture_runtime_owner({}));
      if (shared) assert(fake::removed.empty());
      else assert(fake::removed == std::vector<std::string> {"desktop"});
      fake::reject_empty_topology = false;
      assert(topology.explicit_release(monitor, 7, "User closed monitor"));
      assert_cleaned_once();
    }
  }

  void live_capture_reference_outlasts_transport_counters() {
    reset();
    const auto token = desktop();
    auto capture = topology.retain_normal_game_capture("desktop", token);
    assert(capture && !has_capture_runtime_owner({}));
    assert_deferred();
    assert(fake::applies == 1 && fake::removed.empty());
    capture.reset();
    // The capture destructor releases only its reference; finalization owns I/O.
    assert(fake::applies == 1 && fake::cleanups == 0 && fake::removed.empty());
    assert_cleaned_once();
  }

  void unarmed_finalizer_cannot_retire_desktop() {
    reset(false); desktop();
    assert(!finalize());
    assert(topology.managed_client_identity_count() == 1);
    assert(!topology.normal_game_release_pending());
    assert(fake::applies == 1 && fake::removed.empty() && fake::cleanups == 0);
    start_shared_platform_if_needed();
    assert_cleaned_once();
  }

  void idle_desktop_reaches_existing_restore_branches() {
    for (unsigned reason = 0; reason != 3; ++reason) {
      reset(); desktop();
      shared_runtime_finalize_context_t context;
      config::video.dd.config_revert_on_disconnect = reason == 0;
      fake::deferred_revert = reason == 1;
      context.force_display_revert_when_idle = reason == 2;
      assert(finalize(context));
      assert(fake::restores == 1 && fake::cleanups == 0 && fake::stops == 1);
      assert(fake::applies == 1 && fake::empty_applies == 0 && fake::removed.empty());
      assert(fake::displays.contains("desktop"));
      assert(topology.managed_client_identity_count() == 0);
      assert(!finalize(context) && fake::restores == 1);
    }
  }
}

int main() {
  headless_desktop_hands_off_cleanup();
  every_transport_state_fences_retirement();
  ignore_only_the_current_teardown();
  paused_commandless_app_keeps_identity();
  retained_monitors_prevent_generic_cleanup();
  live_capture_reference_outlasts_transport_counters();
  unarmed_finalizer_cannot_retire_desktop();
  idle_desktop_reaches_existing_restore_branches();
  std::cout << "PASS shared runtime idle ownership regression\n";
}
'''

with tempfile.TemporaryDirectory(prefix="shared-runtime-idle-ownership-") as directory:
    generated = pathlib.Path(directory) / "test.cpp"
    binary = pathlib.Path(directory) / "test"
    generated.write_text(program)
    command = [compiler, "-std=c++20", "-Wall", "-Wextra", "-Werror", "-pthread", "-I", str(root)]
    for include_dir in include_dirs:
        command.extend(["-I", include_dir])
    command.extend([str(generated), str(root / "src/remote_display_topology.cpp"), "-o", str(binary)])
    subprocess.run(command, check=True)
    subprocess.run([str(binary)], check=True, timeout=40)

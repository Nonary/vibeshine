"""Run both production virtual-display monitors with a deterministic clock.

Usage: python3 tests/unit/test_virtual_display_live_mode.py [repo] [compiler]
Only steady-clock reads and OS/driver boundaries are faked. The production
presence classifier, polling control flow, and grace constants are compiled.
"""
import pathlib
import re
import subprocess
import sys
import tempfile

root = pathlib.Path(sys.argv[1]).resolve() if len(sys.argv) > 1 else pathlib.Path(__file__).resolve().parents[2]
compiler = sys.argv[2] if len(sys.argv) > 2 else "c++"


def function(source, signature):
    start = source.index(signature)
    brace = source.index("{", start)
    depth, end = 1, brace + 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end]


prefix = r'''
#include <algorithm>
#include <cassert>
#include <chrono>
#include <functional>
#include <iostream>
#include <map>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <tuple>
#include <vector>
using namespace std::chrono_literals;
struct NullLog { template<class T> NullLog &operator<<(const T &) { return *this; } };
#define BOOST_LOG(level) NullLog {}
enum class Observation {
  active, inactive, missing, unknown, empty_error, empty_stale, empty_missing,
  partial_active, partial_identity_error, partial_stale, partial_query_error,
  partial_inactive, partial_path_match, display_name_error, partial_replacement,
  active_without_path,
};
namespace fake {
  std::chrono::steady_clock::time_point now {};
  std::chrono::milliseconds end {30000};
  std::optional<std::chrono::steady_clock::time_point> recovered_at;
  std::function<Observation(std::chrono::milliseconds)> observe;
  Observation current() { return observe(std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch())); }
}
constexpr int FALSE = 0;
constexpr unsigned DISPLAYCONFIG_PATH_ACTIVE = 1;
using LONG = int;
using DWORD = unsigned;
using UINT32 = unsigned;
namespace display_device {
  enum class DeviceEnumerationDetail { Minimal };
  struct EnumeratedDevice {
    std::string m_device_id, m_display_name, m_monitor_device_path, m_friendly_name;
    // A healthy resolution/HDR switch must not become an identity mismatch.
    unsigned width = 1920;
    bool hdr = false;
  };
  struct Path {
    struct { bool targetAvailable; struct { LONG HighPart = 0; DWORD LowPart = 1; } adapterId; UINT32 id; } targetInfo;
    unsigned flags;
    std::string device_id, monitor_path;
  };
  Path target(std::string id, std::string path, bool available = true, bool active = true, UINT32 target_id = 1) {
    return {{available, {}, target_id}, active ? DISPLAYCONFIG_PATH_ACTIVE : 0, id, path};
  }
  struct PathAndModeData { std::vector<Path> m_paths; };
  enum class QueryType { All };
  enum class DisplayRecoveryBehavior { Skip };
  struct DisplayRecoveryBehaviorGuard { explicit DisplayRecoveryBehaviorGuard(DisplayRecoveryBehavior) {} };
  struct WinApiLayerInterface {
    std::optional<PathAndModeData> queryDisplayConfig(QueryType) {
      const auto state = fake::current();
      if (state == Observation::empty_error || state == Observation::partial_query_error) return std::nullopt;
      if (state == Observation::empty_missing) return PathAndModeData {};
      if (state == Observation::empty_stale) return PathAndModeData {{target("VIRTUAL", "VIRTUAL_PATH", false, true)}};
      PathAndModeData data {{target("PHYSICAL", "PHYSICAL_PATH", true, true, 2)}};
      if (state == Observation::missing) return data;
      if (state == Observation::partial_identity_error) data.m_paths.push_back(target("", ""));
      else if (state == Observation::partial_stale) data.m_paths.push_back(target("VIRTUAL", "VIRTUAL_PATH", false, true));
      else if (state == Observation::partial_path_match) data.m_paths.push_back(target("fallback-id", "VIRTUAL_PATH"));
      else if (state == Observation::partial_replacement) data.m_paths.push_back(target("REPLACEMENT", "REPLACEMENT_PATH"));
      else data.m_paths.push_back(target("VIRTUAL", "VIRTUAL_PATH", true,
                                        state != Observation::inactive && state != Observation::partial_inactive));
      return data;
    }
    std::string getDeviceId(const Path &path) { return path.device_id; }
    std::string getMonitorDevicePath(const Path &path) { return path.monitor_path; }
  };
  using WinApiLayer = WinApiLayerInterface;
}
namespace platf {
  std::wstring from_utf8(const std::string &value) { return {value.begin(), value.end()}; }
  namespace display_helper {
    struct Coordinator {
      static Coordinator &instance() { static Coordinator value; return value; }
      std::optional<std::vector<display_device::EnumeratedDevice>> enumerate_devices(display_device::DeviceEnumerationDetail) {
        using Device = display_device::EnumeratedDevice;
        const auto state = fake::current();
        if (state == Observation::unknown) return std::nullopt;
        if (state == Observation::empty_error || state == Observation::empty_stale || state == Observation::empty_missing)
          return std::vector<Device> {};
        if (state == Observation::missing || state == Observation::partial_active ||
            state == Observation::partial_identity_error || state == Observation::partial_stale ||
            state == Observation::partial_query_error || state == Observation::partial_inactive ||
            state == Observation::partial_path_match || state == Observation::partial_replacement)
          return std::vector<Device> {{"PHYSICAL", "DISPLAY1", "PHYSICAL_PATH", "physical"}};
        Device target {"VIRTUAL", state == Observation::active || state == Observation::active_without_path ? "DISPLAY2" : "",
                       state == Observation::active_without_path ? "" : "VIRTUAL_PATH", "client"};
        target.width = fake::now.time_since_epoch() < 7s ? 3840 : 1920;
        target.hdr = fake::now.time_since_epoch() < 7s;
        return std::vector<Device> {target};
      }
    };
  }
}
struct RecoveryMonitorState {
  struct { std::string client_name = "client"; unsigned max_attempts = 3; } params;
  bool confirmed_active_at_schedule = true;
  std::optional<std::string> current_device_id = "VIRTUAL", normalized_display_name, normalized_monitor_device_path = "VIRTUAL_PATH";
  std::optional<std::wstring> current_display_name, current_monitor_device_path;
  std::string describe_target() const { return "VIRTUAL"; }
  void update_identifiers(std::optional<std::wstring> display, std::optional<std::string> id, std::optional<std::wstring> path) {
    current_display_name = display; current_device_id = id; current_monitor_device_path = path;
  }
};
bool equals_ci(const std::string &a, const std::string &b) { return a == b; }
std::string normalize_display_name(const std::string &name) { return name; }
bool is_virtual_display_device(const display_device::EnumeratedDevice &device) { return device.m_device_id == "VIRTUAL"; }
bool monitor_should_abort(const RecoveryMonitorState &, std::stop_token token) {
  return token.stop_requested() || fake::now.time_since_epoch() >= fake::end || fake::recovered_at.has_value();
}
bool wait_for_monitor_stop(std::stop_token token, std::chrono::steady_clock::duration duration) {
  fake::now += duration;
  return monitor_should_abort({}, token);
}
bool attempt_virtual_display_recovery(RecoveryMonitorState &, std::stop_token) {
  fake::recovered_at = fake::now;
  return true;
}
'''
safety_source = (root / "src/platform/windows/display_recovery_safety.h").read_text()
probe = function(safety_source, "inline DisplayTargetPresence probe_display_target_presence(")
target_enum = function(safety_source, "enum class DisplayTargetPresence") + ";"
case_helpers = function(safety_source, "constexpr char ascii_lower(")
case_helpers += "\n" + function(safety_source, "inline bool starts_with_ci(")
suffix = r'''
void run(std::function<Observation(std::chrono::milliseconds)> observe, bool should_recover,
         std::chrono::milliseconds earliest = 0ms, bool retained_path = true) {
  fake::now = {}; fake::recovered_at.reset(); fake::observe = std::move(observe);
  RecoveryMonitorState state;
  if (!retained_path) state.normalized_monitor_device_path.reset();
  run_virtual_display_recovery_monitor(state, {});
  assert(fake::recovered_at.has_value() == should_recover);
  if (should_recover) assert(fake::recovered_at->time_since_epoch() >= earliest);
}
int main() {
  // RTSP rearm can supply only an ID. A fallback hash before the first
  // healthy observation must not authorize recreation of the active target.
  fake::observe = [](auto) { return Observation::partial_path_match; };
  RecoveryMonitorState id_only;
  id_only.normalized_monitor_device_path.reset();
  assert(monitor_target_presence(id_only) == MonitorTargetPresence::unknown);
  run([](auto) { return Observation::partial_path_match; }, false, 0ms, false);
  // Learn a stable path from a healthy ID match and retain it through missing
  // path metadata. Subsequent real loss beside physical peers still repairs.
  fake::observe = [](auto) { return Observation::active; };
  assert(monitor_target_presence(id_only) == MonitorTargetPresence::present_active);
  assert(id_only.normalized_monitor_device_path == "VIRTUAL_PATH");
  fake::observe = [](auto) { return Observation::active_without_path; };
  assert(monitor_target_presence(id_only) == MonitorTargetPresence::present_active);
  assert(id_only.normalized_monitor_device_path == "VIRTUAL_PATH");
  run([](auto t) { return t >= 7s ? Observation::partial_path_match : Observation::active; }, false, 0ms, false);
  run([](auto t) { return t >= 7s ? Observation::missing : Observation::active; }, true, 7500ms, false);
  run([](auto t) { return t >= 7s ? Observation::partial_replacement : Observation::active; }, true, 7500ms, false);
  // A surviving physical device does not prove the omitted virtual target was
  // removed. Raw identity/presence distinguishes partial reads from real loss.
  for (auto partial : {Observation::partial_active, Observation::partial_path_match, Observation::display_name_error}) {
    fake::observe = [partial](auto) { return partial; };
    RecoveryMonitorState state;
    assert(monitor_target_presence(state) == MonitorTargetPresence::present_active);
    run([partial](auto t) { return t >= 7s ? partial : Observation::active; }, false);
  }
  for (auto uncertain : {Observation::partial_identity_error, Observation::partial_stale, Observation::partial_query_error}) {
    fake::observe = [uncertain](auto) { return uncertain; };
    RecoveryMonitorState state;
    assert(monitor_target_presence(state) == MonitorTargetPresence::unknown);
    run([uncertain](auto t) { return t >= 7s ? uncertain : Observation::active; }, false);
  }
  run([](auto t) { return t >= 7s ? Observation::partial_inactive : Observation::active; }, true, 19s);
  // Live mode/HDR differs after 7s, but the same target remains active.
  run([](auto) { return Observation::active; }, false);
  // An 8s inactive fullscreen/HDR transition settles without driver recreation.
  run([](auto t) { return t >= 7s && t < 15s ? Observation::inactive : Observation::active; }, false);
  // Persistent inactivity is still repaired after the bounded 12s grace.
  run([](auto t) { return t >= 7s ? Observation::inactive : Observation::active; }, true, 19s);
  run([](auto t) { return t >= 7s ? Observation::missing : Observation::active; }, true, 7500ms);
  run([](auto t) { return t >= 7s ? Observation::empty_missing : Observation::active; }, true, 7500ms);
  for (auto unknown : {Observation::unknown, Observation::empty_error, Observation::empty_stale}) {
    run([unknown](auto t) { return t >= 7s ? unknown : Observation::active; }, false);
  }
  // An unreadable interval breaks continuous loss evidence; brief loss samples
  // separated by failures must not aggregate into the shorter missing grace.
  run([](auto t) {
    if (t < 7s || t >= 11s) return Observation::active;
    return (t.count() % 600) < 150 ? Observation::missing : Observation::unknown;
  }, false);
  std::cout << "20 production live-mode monitor scenarios passed.\n";
}
'''

with tempfile.TemporaryDirectory(prefix="vibeshine-virtual-monitor-") as temporary:
    directory = pathlib.Path(temporary)
    for backend in ("sunshine", "sudovda"):
        source = (root / f"src/platform/windows/virtual_display_{backend}.cpp").read_text()
        constants = "\n".join(re.findall(r"constexpr auto RECOVERY_\w+ = .*?;", source))
        presence = function(source, "MonitorTargetPresence monitor_target_presence(")
        monitor = function(source, "void run_virtual_display_recovery_monitor(")
        monitor = monitor.replace("std::chrono::steady_clock::now()", "fake::now")
        program = prefix + "namespace display_recovery_safety {\n" + target_enum
        program += "\nnamespace detail {\n" + case_helpers + "\n}\n" + probe + "\n}\n"
        program += "using MonitorTargetPresence = display_recovery_safety::DisplayTargetPresence;\n"
        program += constants + "\n" + presence + "\n" + monitor + "\n" + suffix
        fixture, binary = directory / f"{backend}.cpp", directory / backend
        fixture.write_text(program)
        subprocess.run([compiler, "-std=c++23", "-Wall", "-Wextra", "-Werror", str(fixture), "-o", str(binary)], check=True)
        subprocess.run([str(binary)], check=True)
        print(f"{backend} recovery monitor passed")

#!/usr/bin/env python3
"""Execute production terminal cleanup and its helper mutation fence.

Usage: python3 test_terminal_display_recovery.py [repository] [compiler]
Optional --cleanup-source, --integration-source, --paths-source and --main-source overrides permit
negative controls against earlier revisions. Only OS/process/driver/device edges
are faked; cleanup ordering, physical selection/classification, singleton admission,
fence and startup decision execute the production C++ bodies. The CCD path builder
and device setters are boundary fakes; this is not native Windows validation.
"""
import argparse
import os
from pathlib import Path
import re
import shlex
import subprocess
import tempfile


def block(source, signature):
    start = source.index(signature)
    opening = source.index("{", start)
    # Ignore braces inside comments and quoted literals when extracting a body.
    token = re.compile(r'//[^\n]*|/\*[\s\S]*?\*/|"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'|[{}]')
    depth = 0
    for match in token.finditer(source, opening):
        if match.group() == "{":
            depth += 1
        elif match.group() == "}":
            depth -= 1
            if depth == 0:
                return source[start:match.end()]
    raise ValueError(f"Unclosed production block: {signature}")


parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("repository", nargs="?", type=Path, default=Path(__file__).resolve().parents[2])
parser.add_argument("compiler", nargs="?", default=os.environ.get("CXX", "c++"))
parser.add_argument("--cleanup-source", type=Path)
parser.add_argument("--integration-source", type=Path)
parser.add_argument("--main-source", type=Path)
parser.add_argument("--paths-source", type=Path)
args = parser.parse_args()
root = args.repository.resolve()
cleanup = (args.cleanup_source or root / "src/platform/windows/virtual_display_cleanup.cpp").read_text()
integration = (args.integration_source or root / "src/platform/windows/display_helper_integration.cpp").read_text()
main = (args.main_source or root / "src/main.cpp").read_text()
paths = (args.paths_source or root / "tools/display_helper_paths.h").read_text()
header = (root / "src/platform/windows/virtual_display_cleanup.h").read_text().replace("#ifdef _WIN32", "#if 1")
cleanup_namespace = block(cleanup, "namespace platf::virtual_display_cleanup")
fence = block(integration, "bool run_terminal_physical_recovery(")
clear_pending = block(integration, "void clear_pending_apply_queue_locked()")
invalidate = block(integration, "static void invalidate_apply_verification()")
pending_snapshot = block(integration, "bool has_pending_recovery_snapshot()")
pending_snapshot = pending_snapshot.replace("std::filesystem::exists", "boundary::snapshot_exists")
startup = block(main, "auto startup_display_recovery =") + ";"
startup_activity = block(main, "const auto has_startup_stream_activity =") + ";"
singleton = block(paths, "inline HANDLE make_named_mutex(") + "\n" + block(paths, "inline bool ensure_single_instance(")
physical = block((root / "src/platform/windows/physical_display_recovery.h").read_text(),
                 "namespace display_helper::physical_recovery").replace("#ifdef _WIN32", "#if 1")
safety = (root / "src/platform/windows/display_recovery_safety.h").read_text().replace("#ifdef _WIN32", "#if 0")
utility = (root / "src/utility.h").read_text()
fail_guard = "template<class T>\n" + block(utility, "class FailGuard") + ";\n"
fail_guard += "template<class T>\n" + block(utility, "[[nodiscard]] auto fail_guard(T &&f)")

program = r'''
#include <algorithm>
#include <array>
#include <atomic>
#include <cassert>
#include <chrono>
#include <cstring>
#include <cwctype>
#include <exception>
#include <filesystem>
#include <functional>
#include <future>
#include <iostream>
#include <memory>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>
#include <display_device/types.h>
#include "cleanup.h"
#include "safety.h"
using namespace std::chrono_literals;
using DWORD = std::uint32_t;
using UINT32 = std::uint32_t;
using HANDLE = void *;
constexpr DWORD WAIT_OBJECT_0 = 0, WAIT_TIMEOUT = 258, WAIT_FAILED = 0xffffffff;
constexpr DWORD ERROR_SUCCESS = 0, ERROR_ACCESS_DENIED = 5, ERROR_ALREADY_EXISTS = 183;
constexpr DWORD ERROR_NO_MORE_FILES = 18, TH32CS_SNAPPROCESS = 2;
const HANDLE INVALID_HANDLE_VALUE = reinterpret_cast<HANDLE>(static_cast<std::intptr_t>(-1));
constexpr int FALSE = 0;
struct SECURITY_ATTRIBUTES { DWORD nLength; void *lpSecurityDescriptor; int bInheritHandle; };
struct PROCESSENTRY32W { DWORD dwSize; wchar_t szExeFile[260]; };
constexpr DWORD DISPLAYCONFIG_PATH_ACTIVE = 1;
constexpr int DISPLAYCONFIG_OUTPUT_TECHNOLOGY_INDIRECT_VIRTUAL = 17;
constexpr UINT32 SDC_APPLY = 1, SDC_USE_SUPPLIED_DISPLAY_CONFIG = 2, SDC_ALLOW_CHANGES = 4, SDC_VIRTUAL_MODE_AWARE = 8;
struct GUID { std::array<std::uint8_t, 16> bytes; };
std::ostringstream logs;
#define BOOST_LOG(level) logs
namespace util {
  // FAIL_GUARD
}

namespace boundary {
  struct State {
    bool aggregate_remove = false, revert_sent = true, managed_allowed = false;
    bool tracked_present = true, retained_present = false, helper_owned = true, helper_live = true;
    bool wait_succeeds = true, database_succeeds = false, topology_succeeds = true;
    bool shutdown = false, stream_active = false, stream_starts_during_enumeration = false;
    bool pending_launch = false, capture_runtime = false, webrtc_sessions = false;
    bool webrtc_capture = false, webrtc_teardown = false, current_app = false, remote_role = false;
    bool startup_context = false, startup_without_lifecycle_gate = false;
    int destructive_session_queries = 0, nonmutating_session_queries = 0;
    bool disabled_recovery_allowed = false;
    bool global_exists = false, local_exists = false, global_denied = false, local_denied = false;
    DWORD initial_wait = WAIT_TIMEOUT;
    int termination_calls = 0, owned_wait_calls = 0, ipc_resets = 0, revert_calls = 0;
    int remove_all_calls = 0, explicit_remove_calls = 0, database_calls = 0, topology_calls = 0;
    int close_calls = 0, forced_watchdog_stops = 0, cancelled_monitors = 0;
    bool deferred_revert = true, native_saw_live_helper = false;
    bool native_saw_unfenced_execution = false, native_saw_locked_helper = false;
    bool native_saw_unclaimed_singleton = false;
    int singleton_open_handles = 0;
    bool process_snapshot_failure = false, process_first_failure = false, process_next_failure = false;
    int process_open_handles = 0, process_snapshots = 0;
    std::vector<std::wstring> process_names {L"explorer.exe", L"sunshine.exe"};
    std::vector<std::wstring> mutex_requests;
    std::set<std::wstring> singleton_claims;
    std::set<std::string> unknown_displays {"permanent_virtual", "unowned_virtual"};
    std::set<std::string> existing_snapshots, snapshot_errors;
    std::vector<std::string> snapshot_queries;
    std::vector<std::string> events, topology_targets;
    display_device::EnumeratedDeviceList outputs;
    std::vector<std::vector<std::string>> active_topology {{"permanent_virtual"}};
    std::set<std::string> indirect_virtual_ids {"permanent_virtual"}, disconnected_ids;
    bool active_query_failure = false, all_query_failure = false, topology_query_failure = false;
    bool query_throws = false;
    bool setter_changes_state = true, drop_previous_on_apply = false;
    bool as_system = false, token_available = true, impersonation_succeeds = true, impersonating = false;
    bool raw_query_without_impersonation = false;
    int raw_set_calls = 0, token_open_handles = 0;
    UINT32 raw_set_flags = 0;
  } state;
  std::mutex execution_mutex, process_mutex, queue_mutex, lifecycle_mutex;
  std::atomic<int> running_sessions {0}, teardown_sessions {0};
  std::optional<int> pending_apply {42};
  std::atomic<bool> restore_expected {true};
  std::atomic<std::uint64_t> apply_generation {7};
  bool snapshot_exists(const std::filesystem::path &path, std::error_code &error) {
    const auto name = path.generic_string();
    state.snapshot_queries.push_back(name);
    error = state.snapshot_errors.contains(name) ? std::make_error_code(std::errc::permission_denied) : std::error_code {};
    return state.existing_snapshots.contains(name);
  }
  bool available_to_other_thread(std::mutex &mutex) {
    bool available = false;
    std::thread observer([&] {
      available = mutex.try_lock();
      if (available) mutex.unlock();
    });
    observer.join();
    return available;
  }
  void observe_native_mutation() {
    state.native_saw_live_helper |= state.helper_live;
    state.native_saw_unfenced_execution |= available_to_other_thread(execution_mutex);
    state.native_saw_locked_helper |= !available_to_other_thread(process_mutex);
    state.native_saw_unclaimed_singleton |=
      !state.singleton_claims.contains(L"Global\\SunshineDisplayHelper") ||
      !state.singleton_claims.contains(L"Local\\SunshineDisplayHelper");
  }
  void reset() {
    assert(state.singleton_open_handles == 0 && state.singleton_claims.empty() && state.process_open_handles == 0 && state.token_open_handles == 0);
    state = State {};
    pending_apply = 42;
    restore_expected = true;
    apply_generation = 7;
    running_sessions = 0;
    teardown_sessions = 0;
    display_device::EnumeratedDevice physical;
    physical.m_device_id = "physical";
    physical.m_monitor_device_path = "physical_path";
    display_device::EnumeratedDevice permanent;
    permanent.m_device_id = "permanent_virtual";
    permanent.m_monitor_device_path = "virtual_path";
    permanent.m_info = display_device::EnumeratedDevice::Info {};
    state.outputs = {permanent, physical};
    logs.str("");
  }
}
struct FakeHandle { bool process_snapshot; std::wstring name; bool fresh; std::size_t index = 0; bool token = false; };
thread_local DWORD last_error = ERROR_SUCCESS;
void SetLastError(DWORD value) { last_error = value; }
DWORD GetLastError() { return last_error; }
bool CloseHandle(HANDLE raw_handle) {
  assert(raw_handle);
  auto *handle = static_cast<FakeHandle *>(raw_handle);
  if (handle->token) {
    --boundary::state.token_open_handles;
  } else if (handle->process_snapshot) {
    --boundary::state.process_open_handles;
  } else {
    if (handle->fresh) boundary::state.singleton_claims.erase(handle->name);
    --boundary::state.singleton_open_handles;
  }
  delete handle;
  return true;
}
HANDLE CreateMutexW(SECURITY_ATTRIBUTES *attributes, int initial_owner, const wchar_t *name) {
  assert(attributes && attributes->nLength == sizeof(*attributes) && !attributes->bInheritHandle && !initial_owner);
  boundary::state.mutex_requests.emplace_back(name);
  const bool global = std::wstring_view(name) == L"Global\\SunshineDisplayHelper";
  assert(global || std::wstring_view(name) == L"Local\\SunshineDisplayHelper");
  if (global ? boundary::state.global_denied : boundary::state.local_denied) {
    SetLastError(ERROR_ACCESS_DENIED);
    return nullptr;
  }
  const bool existing = (global ? boundary::state.global_exists : boundary::state.local_exists) ||
                        boundary::state.singleton_claims.contains(name);
  SetLastError(existing ? ERROR_ALREADY_EXISTS : ERROR_SUCCESS);
  if (!existing) boundary::state.singleton_claims.insert(name);
  ++boundary::state.singleton_open_handles;
  return new FakeHandle {false, name, !existing};
}
HANDLE CreateToolhelp32Snapshot(DWORD flags, DWORD process_id) {
  assert(flags == TH32CS_SNAPPROCESS && process_id == 0);
  ++boundary::state.process_snapshots;
  if (boundary::state.process_snapshot_failure) {
    SetLastError(ERROR_ACCESS_DENIED);
    return INVALID_HANDLE_VALUE;
  }
  ++boundary::state.process_open_handles;
  return new FakeHandle {true, {}, false};
}
bool read_process(FakeHandle *handle, PROCESSENTRY32W *entry, bool first) {
  assert(handle->process_snapshot && entry && entry->dwSize == sizeof(*entry));
  if (first ? boundary::state.process_first_failure : boundary::state.process_next_failure) {
    SetLastError(ERROR_ACCESS_DENIED);
    return false;
  }
  if (handle->index == boundary::state.process_names.size()) {
    SetLastError(ERROR_NO_MORE_FILES);
    return false;
  }
  const auto &name = boundary::state.process_names.at(handle->index++);
  assert(name.size() < std::size(entry->szExeFile));
  std::copy(name.begin(), name.end(), entry->szExeFile);
  entry->szExeFile[name.size()] = L'\0';
  SetLastError(ERROR_SUCCESS);
  return true;
}
bool Process32FirstW(HANDLE handle, PROCESSENTRY32W *entry) { return read_process(static_cast<FakeHandle *>(handle), entry, true); }
bool Process32NextW(HANDLE handle, PROCESSENTRY32W *entry) { return read_process(static_cast<FakeHandle *>(handle), entry, false); }
int _wcsicmp(const wchar_t *left, const wchar_t *right) {
  while (*left && std::towlower(*left) == std::towlower(*right)) { ++left; ++right; }
  return static_cast<int>(std::towlower(*left)) - static_cast<int>(std::towlower(*right));
}
DWORD WaitForSingleObject(HANDLE handle, DWORD timeout) {
  assert(handle && timeout == 3000);
  assert(!boundary::available_to_other_thread(boundary::execution_mutex));
  assert(!boundary::available_to_other_thread(boundary::process_mutex));
  boundary::state.events.push_back("helper_wait");
  if (boundary::state.initial_wait == WAIT_OBJECT_0) boundary::state.helper_live = false;
  return boundary::state.initial_wait;
}
struct FakeOwnedHelper {
  HANDLE get_process_handle() { return boundary::state.helper_owned ? this : nullptr; }
  void terminate() {
    ++boundary::state.termination_calls;
    boundary::state.events.push_back("helper_terminate");
  }
  bool wait_for(DWORD &exit_code, int timeout) {
    assert(timeout == 5000);
    ++boundary::state.owned_wait_calls;
    boundary::state.events.push_back("helper_exit_wait");
    exit_code = 0;
    if (boundary::state.wait_succeeds) boundary::state.helper_live = false;
    return boundary::state.wait_succeeds;
  }
};
namespace platf {
  std::string from_utf8(const std::string &value) { return value; }
  bool is_running_as_system() { return boundary::state.as_system; }
  HANDLE retrieve_users_token(bool elevated) {
    assert(elevated);
    if (!boundary::state.token_available) return nullptr;
    ++boundary::state.token_open_handles;
    return new FakeHandle {false, {}, false, 0, true};
  }
  int impersonate_current_user(HANDLE token, const std::function<void()> &callback) {
    assert(token);
    if (!boundary::state.impersonation_succeeds) return ERROR_ACCESS_DENIED;
    boundary::state.impersonating = true;
    callback();
    boundary::state.impersonating = false;
    return ERROR_SUCCESS;
  }
  namespace display_helper_client {
    void reset_connection() {
      ++boundary::state.ipc_resets;
      boundary::state.events.push_back("ipc_reset");
    }
  }
}
namespace display_helper_paths {
  // SINGLETON
  std::vector<std::filesystem::path> snapshot_search_roots() { return {"user", "system"}; }
  struct Paths { std::filesystem::path session_current, session_previous, golden; };
  Paths make_snapshot_paths(const std::filesystem::path &root) {
    return {root / "current.json", root / "previous.json", root / "golden.json"};
  }
}
namespace display_helper_integration {
  std::mutex &pending_apply_execution_mutex() { return boundary::execution_mutex; }
  std::mutex &pending_apply_mutex() { return boundary::queue_mutex; }
  std::optional<int> &pending_apply_state() { return boundary::pending_apply; }
  std::mutex &helper_mutex() { return boundary::process_mutex; }
  FakeOwnedHelper &helper_proc() { static FakeOwnedHelper helper; return helper; }
  auto &g_restore_expected = boundary::restore_expected;
  auto &g_last_apply_generation = boundary::apply_generation;
  // CLEAR_PENDING
  // INVALIDATE
  // FENCE
  bool revert(bool prefer_golden, bool override_owner, bool allow_disabled_recovery = false) {
    ++boundary::state.revert_calls;
    boundary::state.events.push_back("revert");
    (void) prefer_golden;
    (void) override_owner;
    boundary::state.disabled_recovery_allowed = allow_disabled_recovery;
    return boundary::state.revert_sent;
  }
  // PENDING_SNAPSHOT
  void stop_watchdog(bool force) {
    assert(force);
    ++boundary::state.forced_watchdog_stops;
    boundary::state.events.push_back("watchdog_stop");
  }
}
namespace proc {
  struct Process { int current_app_id() { return boundary::state.current_app ? 1 : 0; } } proc;
  void clear_deferred_display_revert() {
    boundary::state.deferred_revert = false;
    boundary::state.events.push_back("clear_deferred");
  }
  void defer_display_revert() { boundary::state.deferred_revert = true; }
}
namespace remote_display_topology {
  struct Topology {
    bool generic_virtual_display_cleanup_allowed() {
      boundary::state.events.push_back("ownership_check");
      return boundary::state.managed_allowed;
    }
  };
  Topology &instance() { static Topology topology; return topology; }
}
namespace VDISPLAY {
  struct VirtualDisplayInfo { bool is_active; };
  std::vector<VirtualDisplayInfo> enumerateVirtualDisplays() {
    if (boundary::state.stream_starts_during_enumeration) boundary::state.stream_active = true;
    return boundary::state.tracked_present ? std::vector<VirtualDisplayInfo>{{true}} : std::vector<VirtualDisplayInfo>{};
  }
  bool has_retained_ensure_display() { return boundary::state.retained_present; }
  bool removeVirtualDisplay(const GUID &) {
    ++boundary::state.explicit_remove_calls;
    return true;
  }
  bool removeAllVirtualDisplays() {
    ++boundary::state.remove_all_calls;
    boundary::state.events.push_back("tracked_remove");
    if (boundary::state.startup_context) {
      boundary::state.startup_without_lifecycle_gate |= boundary::available_to_other_thread(boundary::lifecycle_mutex);
    }
    boundary::state.tracked_present = false;
    // Models tracked removals succeeding while journal inspection still fails.
    return boundary::state.aggregate_remove;
  }
  void cleanup_retained_ensure_display() {
    boundary::state.retained_present = false;
    boundary::state.events.push_back("retained_remove");
  }
  void cancel_all_virtual_display_recovery_monitors() {
    ++boundary::state.cancelled_monitors;
    boundary::state.events.push_back("cancel_monitors");
  }
  void cancel_virtual_display_recovery_monitor(const GUID &) { assert(false); }
  void setWatchdogFeedingEnabled(bool enabled) {
    assert(!enabled);
    boundary::state.events.push_back("feeding_disabled");
  }
  void closeVDisplayDevice() {
    ++boundary::state.close_calls;
    boundary::state.events.push_back("driver_close");
  }
  bool is_virtual_display_monitor_path(const std::string &path) { return path == "virtual_path"; }
}
namespace display_device {
  enum class QueryType { All, Active };
  enum class DisplayRecoveryBehavior { Automatic, Skip };
  class DisplayRecoveryBehaviorGuard {
  public:
    explicit DisplayRecoveryBehaviorGuard(DisplayRecoveryBehavior behavior) { assert(behavior == DisplayRecoveryBehavior::Skip); }
    ~DisplayRecoveryBehaviorGuard() {}
  };
  struct Path {
    std::string id;
    struct Target { bool targetAvailable = true; int outputTechnology = 0; } targetInfo;
    DWORD flags = 0;
  };
  struct Query { std::vector<Path> m_paths; };
  bool apply_topology(const std::vector<std::vector<std::string>> &topology) {
    ++boundary::state.topology_calls;
    boundary::state.events.push_back("physical_activate");
    boundary::observe_native_mutation();
    assert(!topology.empty());
    for (const auto &group : topology) {
      for (const auto &id : group) boundary::state.topology_targets.push_back(id);
    }
    if (boundary::state.topology_succeeds && boundary::state.setter_changes_state) {
      boundary::state.active_topology = topology;
      if (boundary::state.drop_previous_on_apply) boundary::state.active_topology = {{topology.back().back()}};
      for (auto &output : boundary::state.outputs) {
        output.m_info.reset();
        for (const auto &group : boundary::state.active_topology) {
          if (std::ranges::find(group, output.m_device_id) != group.end()) output.m_info = EnumeratedDevice::Info {};
        }
      }
    }
    return boundary::state.topology_succeeds;
  }
  class WinApiLayer {
  public:
    std::optional<Query> queryDisplayConfig(QueryType type) {
      boundary::state.raw_query_without_impersonation |= boundary::state.as_system && !boundary::state.impersonating;
      if (boundary::state.query_throws) throw std::runtime_error("CCD failure");
      if (type == QueryType::Active ? boundary::state.active_query_failure : boundary::state.all_query_failure) return std::nullopt;
      Query result;
      for (const auto &output : boundary::state.outputs) {
        if (type == QueryType::Active && !output.m_info) continue;
        result.m_paths.push_back(Path {output.m_device_id,
          {!boundary::state.disconnected_ids.contains(output.m_device_id),
           boundary::state.indirect_virtual_ids.contains(output.m_device_id) ? DISPLAYCONFIG_OUTPUT_TECHNOLOGY_INDIRECT_VIRTUAL : 0},
          output.m_info ? DISPLAYCONFIG_PATH_ACTIVE : 0});
      }
      return result;
    }
    std::string getDeviceId(const Path &path) { return path.id; }
    std::string getMonitorDevicePath(const Path &path) {
      for (const auto &output : boundary::state.outputs) if (output.m_device_id == path.id) return output.m_monitor_device_path;
      return {};
    }
    std::vector<std::byte> getEdid(const Path &) { return {}; }
    int setDisplayConfig(const std::vector<Path> &paths, const std::vector<int> &modes, UINT32 flags) {
      assert(modes.empty());
      ++boundary::state.raw_set_calls;
      boundary::state.raw_set_flags = flags;
      std::vector<std::vector<std::string>> topology;
      for (const auto &path : paths) topology.push_back({path.id});
      return apply_topology(topology) ? ERROR_SUCCESS : ERROR_ACCESS_DENIED;
    }
  };
  using WinApiLayerInterface = WinApiLayer;
  namespace win_utils {
    int collectSourceDataForMatchingPaths(WinApiLayer &, const std::vector<Path> &) { return 1; }
    std::vector<Path> makePathsForNewTopology(const std::vector<std::vector<std::string>> &requested, int,
                                            const std::vector<Path> &available) {
      std::vector<Path> result;
      for (const auto &group : requested) {
        for (const auto &id : group) {
          const auto found = std::ranges::find_if(available, [&](const auto &path) { return path.id == id; });
          if (found != available.end()) result.push_back(*found);
        }
      }
      return result;
    }
  }
  class WinDisplayDevice {
  public:
    explicit WinDisplayDevice(const std::shared_ptr<WinApiLayer> &) {}
    std::vector<std::vector<std::string>> getCurrentTopology() {
      return boundary::state.topology_query_failure ? std::vector<std::vector<std::string>>{} : boundary::state.active_topology;
    }
    bool setTopology(const std::vector<std::vector<std::string>> &topology) { return apply_topology(topology); }
  };
  class ImpersonatingDisplayDevice {
  public:
    explicit ImpersonatingDisplayDevice(const std::shared_ptr<WinDisplayDevice> &) {}
    EnumeratedDeviceList enumAvailableDevices() {
      boundary::state.events.push_back("physical_enumerate");
      return boundary::state.outputs;
    }
    bool restoreMonitorSettings() {
      ++boundary::state.database_calls;
      boundary::state.events.push_back("database_restore");
      boundary::observe_native_mutation();
      if (boundary::state.database_succeeds) {
        for (auto &output : boundary::state.outputs) {
          if (output.m_device_id == "physical") output.m_info = EnumeratedDevice::Info {};
        }
      }
      return boundary::state.database_succeeds;
    }
  };
}
// PHYSICAL_POLICY
// CLEANUP_NAMESPACE
namespace config {
  struct Video { struct Dd { bool config_revert_on_disconnect = false; } dd; } video;
}
struct Shutdown { bool peek() const { return boundary::state.shutdown; } };
namespace nvhttp {
  std::mutex &stream_lifecycle_mutex() { return boundary::lifecycle_mutex; }
  bool has_remote_role_owner() { return boundary::state.remote_role; }
}
namespace rtsp_stream {
  bool has_pending_launch_or_startup() { return boundary::state.pending_launch; }
  int session_count_no_cleanup() {
    ++boundary::state.nonmutating_session_queries;
    return boundary::state.stream_active ? 1 : 0;
  }
  int session_count() {
    ++boundary::state.destructive_session_queries;
    return boundary::state.stream_active ? 1 : 0;
  }
}
namespace stream::session {
  auto &running_sessions = boundary::running_sessions;
  auto &teardown_sessions = boundary::teardown_sessions;
  bool has_capture_runtime_owner() { return boundary::state.capture_runtime; }
}
namespace webrtc_stream {
  bool has_active_or_pending_sessions() { return boundary::state.webrtc_sessions; }
  bool has_capture_active() { return boundary::state.webrtc_capture; }
  bool has_teardown_in_progress() { return boundary::state.webrtc_teardown; }
}
void run_startup() {
  const auto shutdown_event = std::make_shared<Shutdown>();
  // STARTUP_ACTIVITY
  // STARTUP
  boundary::state.startup_context = true;
  startup_display_recovery();
  boundary::state.startup_context = false;
}
int failures = 0;
void check(bool condition, const char *message) {
  if (!condition) { ++failures; std::cerr << "FAIL: " << message << '\n'; }
}
void check_no_native(const char *message) {
  check(boundary::state.database_calls == 0 && boundary::state.topology_calls == 0, message);
}
void check_queue_invalidated() {
  check(!boundary::pending_apply && boundary::apply_generation == 8, "terminal fence clears pending APPLY and invalidates old proof");
}
int main() {
  using platf::virtual_display_cleanup::terminate_all;
  using namespace boundary;
  reset();
  auto result = terminate_all("corrupt_driver_journal");
  check(!result.virtual_displays_removed && result.physical_display_recovered,
        "aggregate virtual removal failure must not suppress physical rescue");
  check(state.termination_calls == 1 && state.owned_wait_calls == 1 && !state.helper_live,
        "owned active helper is stopped and verified exited before native recovery");
  check(state.ipc_resets == 1 && !restore_expected && !state.native_saw_live_helper &&
        !state.native_saw_unfenced_execution && !state.native_saw_locked_helper && !state.native_saw_unclaimed_singleton,
        "native recovery holds execution fence and runs after helper lock/connection release");
  check_queue_invalidated();
  check(state.unknown_displays == std::set<std::string>{"permanent_virtual", "unowned_virtual"} &&
        state.explicit_remove_calls == 0 && state.remove_all_calls == 1 &&
        state.topology_targets == std::vector<std::string>{"permanent_virtual", "physical"} && state.database_calls == 0,
        "terminal rescue retains tracked removal scope and adds physical output without replacing permanent virtual paths");
  check(!state.deferred_revert && state.cancelled_monitors == 1 &&
        state.close_calls == 1 && state.forced_watchdog_stops == 1,
        "terminal intent clears deferred restore and ends repair/watchdog workers");
  check(std::ranges::find(state.events, "cancel_monitors") < std::ranges::find(state.events, "ownership_check") &&
        std::ranges::find(state.events, "feeding_disabled") < std::ranges::find(state.events, "tracked_remove"),
        "repair and feeding stop before forced teardown despite live managed owner");
  check(!platf::virtual_display_cleanup::in_progress(), "terminal cleanup reservation is released");
  check(state.singleton_open_handles == 0 && state.singleton_claims.empty(), "singleton handles release after native fallback");
  check(state.process_snapshots == 1 && state.process_open_handles == 0,
        "native fallback requires process inventory and releases its snapshot");

  reset(); state.initial_wait = WAIT_OBJECT_0;
  state.outputs.back().m_info = display_device::EnumeratedDevice::Info {};
  state.active_topology.push_back({"physical"});
  result = terminate_all("helper_completed");
  check(result.physical_display_recovered && !result.database_restore_applied &&
        state.termination_calls == 0 && state.topology_calls == 0,
        "already completed helper and confirmed physical output require no extra display mutation");

  reset(); state.wait_succeeds = false;
  result = terminate_all("helper_wont_stop");
  check(!result.physical_display_recovered && state.termination_calls == 1 && restore_expected && state.ipc_resets == 0,
        "failed owned process exit wait retains restore obligation and declines rescue");
  check_no_native("failed helper exit wait prohibits every native display mutation");
  check_queue_invalidated();

  reset(); state.initial_wait = WAIT_FAILED;
  result = terminate_all("helper_wait_error");
  check(!result.physical_display_recovered && state.termination_calls == 0 && restore_expected,
        "unknown helper wait result cannot authorize native rescue");
  check_no_native("helper wait error prohibits native mutation");

  reset(); state.helper_owned = false;
  result = terminate_all("unowned_helper");
  check(!result.physical_display_recovered && state.termination_calls == 0 && restore_expected,
        "pending restore without an owned handle cannot be safely quiesced");
  check_no_native("unknown helper is never assumed exited");

  reset(); state.helper_owned = false; state.helper_live = false; restore_expected = false;
  result = terminate_all("no_helper");
  check(result.physical_display_recovered && state.termination_calls == 0 && state.ipc_resets == 1,
        "native fallback may run when no helper or restore obligation exists");
  check_queue_invalidated();

  reset(); state.aggregate_remove = true; state.topology_succeeds = false;
  result = terminate_all("physical_activation_failed");
  check(result.virtual_displays_removed && !result.physical_display_recovered,
        "successful removal is not misreported as observed physical recovery");

  reset(); state.outputs.erase(state.outputs.begin() + 1);
  result = terminate_all("only_permanent_virtual_outputs");
  check(!result.physical_display_recovered && state.topology_calls == 0,
        "active virtual output alone cannot satisfy physical rescue or become its target");

  for (int invalid_physical = 0; invalid_physical < 3; ++invalid_physical) {
    reset();
    if (invalid_physical == 0) state.indirect_virtual_ids.insert("physical");
    if (invalid_physical == 1) state.outputs.back().m_monitor_device_path = "DISPLAY#SDD4001#managed";
    if (invalid_physical == 2) state.disconnected_ids.insert("physical");
    result = terminate_all("not_connected_physical");
    check(!result.physical_display_recovered, "other vendors' indirect virtual, managed and disconnected targets cannot count as physical");
    check_no_native("virtual or disconnected targets never authorize emergency activation");
  }
  for (int failed_read = 0; failed_read < 3; ++failed_read) {
    reset();
    state.active_query_failure = failed_read == 0;
    state.all_query_failure = failed_read == 1;
    state.topology_query_failure = failed_read == 2;
    result = terminate_all("untrustworthy_desktop_read");
    check(!result.physical_display_recovered, "failed or inconsistent desktop query cannot authorize physical recovery");
    check_no_native("unknown desktop cannot become a replacement topology");
  }
  reset(); state.setter_changes_state = false;
  result = terminate_all("accepted_without_physical_readback");
  check(!result.physical_display_recovered && state.topology_calls == 1,
        "setter acceptance requires fresh physical activity before recovery succeeds");
  reset(); state.drop_previous_on_apply = true;
  result = terminate_all("driver_dropped_existing_output");
  check(!result.physical_display_recovered && state.topology_calls == 2,
        "dropping an existing permanent path triggers additive repair and cannot report success");

  for (bool changes_state : {true, false}) {
    reset(); state.active_topology.clear(); state.outputs.front().m_info.reset();
    state.setter_changes_state = changes_state;
    result = terminate_all("proven_empty_active_topology");
    check(result.physical_display_recovered == changes_state && state.raw_set_calls == 1 &&
          state.topology_targets == std::vector<std::string>{"physical"},
          "confirmed empty desktop permits one physical activation but still requires fresh readback");
    check(state.raw_set_flags == (SDC_APPLY | SDC_USE_SUPPLIED_DISPLAY_CONFIG | SDC_ALLOW_CHANGES | SDC_VIRTUAL_MODE_AWARE),
          "empty-desktop activation is temporary and never requests database persistence");
  }

  for (int identity_failure = 0; identity_failure < 4; ++identity_failure) {
    reset(); state.as_system = true;
    state.token_available = identity_failure != 1;
    state.impersonation_succeeds = identity_failure != 2;
    state.query_throws = identity_failure == 3;
    result = terminate_all("service_user_context");
    check(result.physical_display_recovered == (identity_failure == 0),
          "service physical recovery requires user token and successful impersonation");
    check(!state.raw_query_without_impersonation && !state.impersonating && state.token_open_handles == 0,
          "service queries and mutations use the same user context and release token/identity on errors");
    if (identity_failure != 0) check_no_native("missing user context or failed query cannot mutate display state");
  }

  reset(); state.revert_sent = false; state.wait_succeeds = false;
  result = terminate_all("dispatch_failed_helper_still_running");
  check(!result.physical_display_recovered && state.termination_calls == 1,
        "failed REVERT dispatch still requires owned helper shutdown");
  check_no_native("failed REVERT plus failed helper shutdown cannot bypass fence through database fallback");

  for (int conflict = 0; conflict < 4; ++conflict) {
    reset(); state.initial_wait = WAIT_OBJECT_0;
    state.global_exists = conflict == 0;
    state.local_exists = conflict == 1;
    state.global_denied = conflict == 2;
    state.local_denied = conflict == 3;
    result = terminate_all("external_helper_singleton");
    check(!result.physical_display_recovered,
          "existing or inaccessible helper singleton prevents native rescue even after owned child exit");
    check_no_native("external helper singleton conflict prohibits every native display mutation");
    check(state.singleton_open_handles == 0 && state.singleton_claims.empty(),
          "partially acquired singleton handles release on every blocked path");
  }

  for (int failure = 0; failure < 6; ++failure) {
    reset(); state.initial_wait = WAIT_OBJECT_0;
    state.process_snapshot_failure = failure == 0;
    state.process_first_failure = failure == 1;
    state.process_next_failure = failure == 2;
    if (failure == 3) state.process_names = {L"sunshine_display_helper.exe", L"explorer.exe"};
    if (failure == 4) state.process_names = {L"explorer.exe", L"SUNSHINE_DISPLAY_HELPER.EXE"};
    if (failure == 5) state.process_names = {L"explorer.exe", L"not_the_helper.exe", L"sunshine_display_helper.exe"};
    result = terminate_all("external_helper_or_unknown_inventory");
    check(!result.physical_display_recovered && state.process_snapshots == 1 && state.termination_calls == 0,
          "failed process inventory or any surviving helper declines rescue without terminating unowned processes");
    check_no_native("process inventory must prove every external helper absent before native mutation");
    check(state.process_open_handles == 0 && state.singleton_open_handles == 0 && state.singleton_claims.empty(),
          "process inventory failure releases its snapshot and both singleton gates");
  }
  reset(); state.process_names.clear();
  result = terminate_all("confirmed_empty_process_inventory");
  check(result.physical_display_recovered, "confirmed empty process inventory permits fenced native recovery");

  for (int conflict = 0; conflict < 3; ++conflict) {
    reset(); state.global_denied = conflict == 0; state.global_exists = conflict == 1;
    SetLastError(ERROR_ALREADY_EXISTS);  // Admission must overwrite stale error state.
    HANDLE admitted = nullptr;
    const bool success = display_helper_paths::ensure_single_instance(admitted);
    check(success == (conflict == 2), "helper admission requires a fresh accessible global singleton");
    check(state.mutex_requests == std::vector<std::wstring>{L"Global\\SunshineDisplayHelper"},
          "helper admission never falls back to a session-local mutex after global failure");
    if (success) {
      check(admitted && state.singleton_claims.contains(L"Global\\SunshineDisplayHelper"),
            "successful helper admission retains global gate");
      CloseHandle(admitted);
    } else {
      check(!admitted && state.singleton_open_handles == 0,
            "rejected helper admission returns no live handle");
      check(GetLastError() == (conflict == 0 ? ERROR_ACCESS_DENIED : ERROR_ALREADY_EXISTS),
            "rejected helper admission preserves failure reason");
    }
  }

  reset();
  bool callback_held_fence = false;
  const auto fence_result = display_helper_integration::run_terminal_physical_recovery([&] {
    callback_held_fence = !available_to_other_thread(execution_mutex) && available_to_other_thread(process_mutex);
    for (const auto name : {L"Global\\SunshineDisplayHelper", L"Local\\SunshineDisplayHelper"}) {
      HANDLE competing_helper = display_helper_paths::make_named_mutex(name);
      check(competing_helper && GetLastError() == ERROR_ALREADY_EXISTS,
            "both singleton names remain claimed while the native callback runs");
      CloseHandle(competing_helper);
    }
    return false;
  });
  check(!fence_result && callback_held_fence, "fence spans callback and preserves failure result");

  for (int failure = 0; failure < 4; ++failure) {
    reset(); state.managed_allowed = true; state.revert_sent = false; state.database_succeeds = true;
    state.wait_succeeds = failure != 1;
    state.global_exists = failure == 2;
    state.process_first_failure = failure == 3;
    result = platf::virtual_display_cleanup::run("ordinary_pipe_unavailable", true);
    check(result.database_restore_applied == (failure == 0),
          "ordinary database fallback requires helper exit, singleton gate and trustworthy process inventory");
    if (failure == 0) {
      check(state.database_calls == 1 && !state.native_saw_live_helper && !state.native_saw_unfenced_execution &&
            !state.native_saw_unclaimed_singleton && !state.native_saw_locked_helper,
            "ordinary failed dispatch fallback has the same mutation fence as terminal rescue");
    } else {
      check_no_native("ordinary failed dispatch cannot race a live or unknown helper through direct database restore");
    }
    check_queue_invalidated();
  }

  reset();
  state.existing_snapshots = {"user/previous.json", "system/golden.json"};
  check(!display_helper_integration::has_pending_recovery_snapshot() &&
        state.snapshot_queries == std::vector<std::string>{"user/current.json", "system/current.json"},
        "history-only snapshots cannot authorize recovery of a healthy startup desktop");
  state.existing_snapshots.insert("system/current.json");
  check(display_helper_integration::has_pending_recovery_snapshot(),
        "unfinished Current is found across the configured user/system roots");
  reset(); state.snapshot_errors.insert("user/current.json");
  check(display_helper_integration::has_pending_recovery_snapshot(),
        "unreadable Current preserves the recovery obligation conservatively");

  for (bool preference : {false, true}) {
    reset(); state.managed_allowed = true; state.helper_live = false;
    config::video.dd.config_revert_on_disconnect = preference;
    run_startup();
    check(state.revert_calls == 1 && state.remove_all_calls == 1,
          "fresh-host orphan recovery runs independently of client disconnect preference");
    check(state.disabled_recovery_allowed, "startup recovery remains allowed after display configuration was disabled");
    check(!state.startup_without_lifecycle_gate && state.nonmutating_session_queries >= 2 &&
          state.destructive_session_queries == 0,
          "startup holds lifecycle gate across cleanup and inspects sessions without cleanup side effects");
  }
  reset(); state.managed_allowed = true; state.tracked_present = false;
  state.existing_snapshots.insert("system/current.json");
  run_startup();
  check(state.revert_calls == 1, "unfinished Current triggers recovery after the virtual display is already gone");
  reset(); state.managed_allowed = true; state.tracked_present = false;
  state.existing_snapshots = {"user/previous.json", "system/golden.json"};
  run_startup();
  check(state.revert_calls == 0 && state.remove_all_calls == 0,
        "history alone leaves a healthy startup desktop unchanged");
  reset(); state.managed_allowed = true; state.tracked_present = false;
  state.snapshot_errors.insert("system/current.json");
  run_startup();
  check(state.revert_calls == 1, "Current inspection failure still triggers conservative startup recovery");
  for (int guard = 0; guard < 13; ++guard) {
    reset(); state.managed_allowed = true;
    state.shutdown = guard == 0;
    state.stream_active = guard == 1;
    state.retained_present = guard == 2;
    state.stream_starts_during_enumeration = guard == 3;
    state.pending_launch = guard == 4;
    running_sessions = guard == 5 ? 1 : 0;
    teardown_sessions = guard == 6 ? 1 : 0;
    state.capture_runtime = guard == 7;
    state.webrtc_sessions = guard == 8;
    state.webrtc_capture = guard == 9;
    state.webrtc_teardown = guard == 10;
    state.current_app = guard == 11;
    state.remote_role = guard == 12;
    run_startup();
    check(state.revert_calls == 0 && state.remove_all_calls == 0,
          "startup respects shutdown, pending/live/capture/teardown, paused app, remote role and retained probe ownership");
    check(state.destructive_session_queries == 0, "startup activity observation never performs session cleanup");
  }
  reset(); state.managed_allowed = true;
  std::unique_lock lifecycle_owner(lifecycle_mutex);
  auto startup_waiter = std::async(std::launch::async, run_startup);
  const bool skipped_busy_gate = startup_waiter.wait_for(1s) == std::future_status::ready;
  lifecycle_owner.unlock();
  startup_waiter.get();
  check(skipped_busy_gate && state.revert_calls == 0 && state.remove_all_calls == 0,
        "startup try-lock skips a lifecycle owner before pending session state becomes visible");
  if (failures) return 1;
  std::cout << "Production terminal cleanup, helper mutation fence and startup recovery regressions passed\n";
}
'''
for marker, code in {
    "// FAIL_GUARD": fail_guard,
    "// SINGLETON": singleton,
    "// CLEAR_PENDING": clear_pending,
    "// INVALIDATE": invalidate,
    "// PENDING_SNAPSHOT": pending_snapshot,
    "// FENCE": fence,
    "// PHYSICAL_POLICY": physical,
    "// CLEANUP_NAMESPACE": cleanup_namespace,
    "// STARTUP_ACTIVITY": startup_activity,
    "// STARTUP": startup,
}.items():
    program = program.replace(marker, code)

# Redirect the extracted OS boundary explicitly. Distinct names also prevent
# collisions when the native compiler's C++ headers expose Windows SDK types.
for symbol in (
    "DWORD", "UINT32", "HANDLE", "GUID", "WaitForSingleObject", "SetLastError", "GetLastError", "CloseHandle",
    "WAIT_OBJECT_0", "WAIT_TIMEOUT", "WAIT_FAILED", "ERROR_SUCCESS", "ERROR_ACCESS_DENIED", "ERROR_ALREADY_EXISTS",
    "ERROR_NO_MORE_FILES", "TH32CS_SNAPPROCESS", "INVALID_HANDLE_VALUE", "FALSE", "SECURITY_ATTRIBUTES", "PROCESSENTRY32W",
    "CreateMutexW", "CreateToolhelp32Snapshot", "Process32FirstW", "Process32NextW", "_wcsicmp",
    "DISPLAYCONFIG_PATH_ACTIVE", "DISPLAYCONFIG_OUTPUT_TECHNOLOGY_INDIRECT_VIRTUAL",
    "SDC_APPLY", "SDC_USE_SUPPLIED_DISPLAY_CONFIG", "SDC_ALLOW_CHANGES", "SDC_VIRTUAL_MODE_AWARE",
):
    program = re.sub(rf"\b{symbol}\b", f"Test_{symbol}", program)

with tempfile.TemporaryDirectory(prefix="terminal-display-recovery-") as temporary:
    directory = Path(temporary)
    (directory / "cleanup.h").write_text(header)
    (directory / "safety.h").write_text(safety)
    source = directory / "test.cpp"
    source.write_text(program)
    binary = directory / "test"
    subprocess.run([
        *shlex.split(args.compiler), "-std=c++23", "-Wall", "-Wextra", "-Werror", "-pthread",
        "-I", str(root / "third-party/libdisplaydevice/src/common/include"),
        str(root / "third-party/libdisplaydevice/src/common/types.cpp"),
        str(root / "third-party/libdisplaydevice/src/common/logging.cpp"),
        str(source), "-o", str(binary),
    ], check=True)
    subprocess.run([str(binary)], check=True, timeout=30)

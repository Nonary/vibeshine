/**
 * @file src/platform/windows/display_helper_integration.cpp
 */
#ifdef _WIN32

  #include <winsock2.h>

  // standard
  #include <algorithm>
  #include <atomic>
  #include <boost/algorithm/string/predicate.hpp>
  #include <chrono>
  #include <cmath>
  #include <condition_variable>
  #include <cstdint>
  #include <exception>
  #include <filesystem>
  #include <limits>
  #include <mutex>
  #include <optional>
  #include <string>
  #include <string_view>
  #include <thread>
  #include <vector>

  // libdisplaydevice
  #include <display_device/json.h>
  #include <display_device/windows/win_api_layer.h>
  #include <display_device/windows/win_api_recovery.h>
  #include <display_device/windows/win_api_utils.h>
  #include <display_device/windows/win_display_device.h>
  #include <nlohmann/json.hpp>

  // sunshine
  #include "display_helper_integration.h"
  #include "src/globals.h"
  #include "src/logging.h"
  #include "src/platform/windows/display_helper_coordinator.h"
  #include "src/platform/windows/display_helper_request_helpers.h"
  #include "src/platform/windows/display_restore_guard.h"
  #include "src/platform/windows/frame_limiter_nvcp.h"
  #include "src/platform/windows/impersonating_display_device.h"
  #include "src/platform/windows/ipc/display_settings_client.h"
  #include "src/platform/windows/ipc/misc_utils.h"
  #include "src/platform/windows/ipc/process_handler.h"
  #include "src/platform/windows/misc.h"
  #include "src/platform/windows/virtual_display.h"
  #include "src/process.h"
  #include "src/state_storage.h"
  #include "src/stream.h"
  #include "src/webrtc_stream.h"

  #include <display_device/noop_audio_context.h>
  #include <display_device/noop_settings_persistence.h>
  #include <display_device/windows/persistent_state.h>
  #include <display_device/windows/settings_manager.h>
  #include <display_device/windows/types.h>
  #include <tlhelp32.h>

namespace {
  // Serialize helper start/inspect to avoid races that could spawn duplicate helpers
  std::mutex &helper_mutex() {
    static std::mutex m;
    return m;
  }

  // Persistent process handler to keep helper alive while Sunshine runs
  ProcessHandler &helper_proc() {
    static ProcessHandler h(/*use_job=*/false);
    return h;
  }

  struct PendingSessionSnapshot {
    int width = 0;
    int height = 0;
    int fps = 0;
    bool enable_hdr = false;
    bool enable_sops = false;
    bool virtual_display = false;
    std::string virtual_display_device_id;
    std::optional<std::chrono::steady_clock::time_point> virtual_display_ready_since;
    std::optional<int> framegen_refresh_rate;
    int framegen_refresh_multiplier = 1;
    bool gen1_framegen_fix = false;
    bool gen2_framegen_fix = false;
  };

  struct PendingApplyState {
    display_helper_integration::DisplayApplyRequest request;
    PendingSessionSnapshot session_snapshot;
    uint32_t session_id {0};
    bool has_session {false};
    int attempts {0};
    std::optional<std::chrono::steady_clock::time_point> ready_since;
    std::chrono::steady_clock::time_point next_attempt {};
    std::uint64_t epoch {0};
  };

  std::mutex &pending_apply_mutex() {
    static std::mutex m;
    return m;
  }

  std::optional<PendingApplyState> &pending_apply_state() {
    static std::optional<PendingApplyState> state;
    return state;
  }

  std::atomic<std::uint64_t> &pending_apply_epoch() {
    static std::atomic<std::uint64_t> epoch {0};
    return epoch;
  }

  std::atomic<bool> &cold_start_resolution_deferral_armed() {
    static std::atomic<bool> armed {true};
    return armed;
  }

  bool user_session_ready();

  bool request_includes_resolution(const display_helper_integration::DisplayApplyRequest &request) {
    if (!request.configuration) {
      return false;
    }
    return request.configuration->m_resolution.has_value();
  }

  PendingApplyState make_pending_apply_state(const display_helper_integration::DisplayApplyRequest &request) {
    PendingApplyState state;
    state.request = request;
    state.has_session = request.session != nullptr;
    state.request.session = nullptr;

    if (request.session) {
      state.session_id = request.session->id;
      state.session_snapshot.width = request.session->width;
      state.session_snapshot.height = request.session->height;
      state.session_snapshot.fps = request.session->fps;
      state.session_snapshot.enable_hdr = rtsp_stream::effective_hdr_requested(*request.session);
      state.session_snapshot.enable_sops = request.session->enable_sops;
      state.session_snapshot.virtual_display = request.session->virtual_display;
      state.session_snapshot.virtual_display_device_id = request.session->virtual_display_device_id;
      state.session_snapshot.virtual_display_ready_since = request.session->virtual_display_ready_since;
      state.session_snapshot.framegen_refresh_rate = request.session->framegen_refresh_rate;
      state.session_snapshot.framegen_refresh_multiplier = request.session->framegen_refresh_multiplier;
      state.session_snapshot.gen1_framegen_fix = request.session->gen1_framegen_fix;
      state.session_snapshot.gen2_framegen_fix = request.session->gen2_framegen_fix;
    }

    return state;
  }

  void queue_deferred_resolution_apply(const display_helper_integration::DisplayApplyRequest &request) {
    PendingApplyState state = make_pending_apply_state(request);
    std::lock_guard<std::mutex> lock(pending_apply_mutex());
    state.epoch = pending_apply_epoch().fetch_add(1, std::memory_order_acq_rel) + 1;
    pending_apply_state() = std::move(state);
    BOOST_LOG(info) << "Display helper: deferring resolution apply for session " << pending_apply_state()->session_id << ".";
  }

  void maybe_queue_deferred_resolution_apply_on_api_unavailable(
    const display_helper_integration::DisplayApplyRequest &request
  ) {
    if (!request.session) {
      return;
    }
    if (!request_includes_resolution(request)) {
      return;
    }
    queue_deferred_resolution_apply(request);
    BOOST_LOG(info) << "Display helper: API unavailable; queued deferred resolution apply.";
  }

  bool should_defer_resolution_apply(const display_helper_integration::DisplayApplyRequest &request) {
    if (!request.session) {
      return false;
    }
    if (!request_includes_resolution(request)) {
      return false;
    }
    if (!platf::is_running_as_system()) {
      return false;
    }
    if (user_session_ready()) {
      return false;
    }
    return true;
  }

  void maybe_queue_deferred_resolution_apply(
    const display_helper_integration::DisplayApplyRequest &request,
    bool allow_resolution_deferral
  ) {
    if (!allow_resolution_deferral) {
      return;
    }
    if (!should_defer_resolution_apply(request)) {
      return;
    }
    bool expected = true;
    if (!cold_start_resolution_deferral_armed().compare_exchange_strong(expected, false)) {
      return;
    }
    queue_deferred_resolution_apply(request);
  }

  bool user_session_ready() {
    HANDLE user_token = platf::dxgi::retrieve_users_token(false);
    if (!user_token) {
      return false;
    }
    CloseHandle(user_token);
    return true;
  }

  constexpr std::chrono::seconds kTopologyWaitTimeout {6};
  constexpr std::chrono::milliseconds kHelperIpcReadyTimeout {5000};
  constexpr std::chrono::milliseconds kHelperIpcReadyPoll {100};

  // Stream-start requirement: stop very recent helper restore activity quickly.
  // Once the helper has had time to begin an actual restore, do not kill/overwrite
  // that in-flight restore from a later stream-start probe; the helper will either
  // finish restoring or an explicit APPLY will supersede it.
  constexpr std::chrono::milliseconds kDisarmRestoreBudget {300};
  constexpr std::chrono::milliseconds kDisarmRetryThrottle {150};
  constexpr int kRestoreHandoffApplyResultTimeoutMs = 10000;
  constexpr std::chrono::milliseconds kDeferredApplyInitialDelay {2000};
  constexpr std::chrono::milliseconds kDeferredApplyRetryBase {500};
  constexpr std::chrono::milliseconds kDeferredApplyRetryMax {10000};
  constexpr std::chrono::milliseconds kHelperStartFailureCooldown {30000};
  constexpr int kMaxDeferredApplyAttempts = 6;

  bool shutdown_requested();
  bool ensure_helper_started(bool force_restart = false, bool force_enable = false);
  const char *virtual_layout_to_string(const display_helper_integration::VirtualDisplayArrangement layout);

  bool external_helper_process_running() {
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) {
      return false;
    }

    PROCESSENTRY32W entry {};
    entry.dwSize = sizeof(entry);
    bool found = false;
    if (Process32FirstW(snapshot, &entry)) {
      do {
        if (_wcsicmp(entry.szExeFile, L"sunshine_display_helper.exe") == 0 &&
            entry.th32ProcessID != GetCurrentProcessId()) {
          found = true;
          break;
        }
      } while (Process32NextW(snapshot, &entry));
    }
    CloseHandle(snapshot);
    return found;
  }

  bool helper_process_running() {
    std::lock_guard<std::mutex> lg(helper_mutex());
    if (HANDLE h = helper_proc().get_process_handle()) {
      if (WaitForSingleObject(h, 0) == WAIT_TIMEOUT) {
        return true;
      }
    }
    // A helper can outlive Sunshine after a crash or be launched by the restore
    // scheduled task. It has no ProcessHandler handle in this process but must
    // still participate in the handoff before any kill/start/display mutation.
    return external_helper_process_running();
  }

  bool helper_process_is_external() {
    std::lock_guard<std::mutex> lg(helper_mutex());
    if (HANDLE h = helper_proc().get_process_handle()) {
      if (WaitForSingleObject(h, 0) == WAIT_TIMEOUT) {
        return false;
      }
    }
    return external_helper_process_running();
  }

  bool restore_expected_with_live_helper();

  std::chrono::milliseconds deferred_apply_retry_delay(int attempts) {
    if (attempts <= 0) {
      return kDeferredApplyRetryBase;
    }
    const int shift = std::min(attempts - 1, 5);
    auto delay = kDeferredApplyRetryBase * (1 << shift);
    if (delay > kDeferredApplyRetryMax) {
      delay = kDeferredApplyRetryMax;
    }
    return delay;
  }

  struct InProcessDisplayContext {
    std::shared_ptr<display_device::SettingsManagerInterface> settings_mgr;
    std::shared_ptr<display_device::WinDisplayDeviceInterface> display;
  };

  std::optional<InProcessDisplayContext> make_settings_manager() {
    try {
      auto api = std::make_shared<display_device::WinApiLayer>();
      auto dd = std::make_shared<display_device::WinDisplayDevice>(api);
      auto impersonated_dd = std::make_shared<display_device::ImpersonatingDisplayDevice>(dd);
      auto audio = std::make_shared<display_device::NoopAudioContext>();
      auto persistence = std::make_unique<display_device::PersistentState>(
        std::make_shared<display_device::NoopSettingsPersistence>()
      );
      auto settings_mgr = std::make_shared<display_device::SettingsManager>(
        impersonated_dd,
        audio,
        std::move(persistence),
        display_device::WinWorkarounds {}
      );
      return InProcessDisplayContext {
        .settings_mgr = std::move(settings_mgr),
        .display = std::move(impersonated_dd),
      };
    } catch (const std::exception &ex) {
      BOOST_LOG(error) << "Display helper (in-process): failed to initialize SettingsManager: " << ex.what();
    } catch (...) {
      BOOST_LOG(error) << "Display helper (in-process): failed to initialize SettingsManager due to unknown error.";
    }
    return std::nullopt;
  }

  bool device_id_equals_ci(const std::string &lhs, const std::string &rhs) {
    if (lhs.empty() || rhs.empty()) {
      return false;
    }
    return boost::iequals(lhs, rhs);
  }

  bool device_is_active(const std::string &device_id) {
    if (device_id.empty()) {
      return false;
    }

    auto devices = platf::display_helper::Coordinator::instance().enumerate_devices(display_device::DeviceEnumerationDetail::Minimal);
    if (!devices) {
      return false;
    }

    for (const auto &device : *devices) {
      if (device.m_device_id.empty() || !device.m_info) {
        continue;
      }
      if (device_id_equals_ci(device.m_device_id, device_id)) {
        return true;
      }
    }
    return false;
  }

  // User-configured exclusions plus every Sunshine-managed virtual display device id we have
  // seen. Virtual displays must never end up in restore baselines: a baseline captured while
  // one was active "restores" the physical monitors away (vibeshine#223).
  std::vector<std::string> effective_snapshot_exclude_devices() {
    std::vector<std::string> ids = config::video.dd.snapshot_exclude_devices;
    for (auto &vd : statefile::load_virtual_display_devices()) {
      const bool present = std::any_of(ids.begin(), ids.end(), [&](const std::string &existing) {
        return boost::algorithm::iequals(existing, vd);
      });
      if (!present) {
        ids.push_back(std::move(vd));
      }
    }
    return ids;
  }

  std::string build_snapshot_exclude_payload() {
    try {
      nlohmann::json j = effective_snapshot_exclude_devices();
      return j.dump();
    } catch (...) {
      return std::string {};
    }
  }

  bool wait_for_device_activation(const std::string &device_id, std::chrono::steady_clock::duration timeout) {
    if (device_id.empty()) {
      return false;
    }

    auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
      if (device_is_active(device_id)) {
        return true;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    return false;
  }

  bool wait_for_virtual_display_activation(std::chrono::steady_clock::duration timeout) {
    auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
      auto virtual_displays = VDISPLAY::enumerateVirtualDisplays();
      bool any_active = std::any_of(
        virtual_displays.begin(),
        virtual_displays.end(),
        [](const VDISPLAY::VirtualDisplayInfo &info) {
          return info.is_active;
        }
      );
      if (any_active) {
        return true;
      }

      std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    return false;
  }

  bool verify_helper_topology(
    const rtsp_stream::launch_session_t &session,
    const std::string &device_id
  ) {
    if (!device_id.empty()) {
      const bool has_activation_hint = session.virtual_display &&
                                       session.virtual_display_ready_since.has_value() &&
                                       !session.virtual_display_device_id.empty() &&
                                       device_id_equals_ci(device_id, session.virtual_display_device_id);
      if (has_activation_hint && device_is_active(device_id)) {
        BOOST_LOG(debug) << "Display helper: device_id " << device_id
                         << " already active; skipping activation wait.";
        return true;
      }

      if (!wait_for_device_activation(device_id, kTopologyWaitTimeout)) {
        BOOST_LOG(error) << "Display helper: device_id " << device_id << " did not become active after APPLY.";
        return false;
      }
      return true;
    }

    if (session.virtual_display) {
      const bool hint_ready = session.virtual_display_ready_since.has_value();
      if (hint_ready) {
        BOOST_LOG(debug) << "Display helper: virtual display ready hint satisfied. Skipping activation wait.";
        return true;
      }
      if (!wait_for_virtual_display_activation(kTopologyWaitTimeout)) {
        BOOST_LOG(error) << "Display helper: virtual display topology did not become active after APPLY.";
        return false;
      }
    }

    return true;
  }

  bool apply_topology_definition(
    const display_helper_integration::DisplayTopologyDefinition &topology,
    const char *label
  ) {
    if (topology.topology.empty() && topology.monitor_positions.empty()) {
      return true;
    }

    auto ctx = make_settings_manager();
    if (!ctx) {
      BOOST_LOG(warning) << "Display helper: unable to initialize display context for topology apply (" << label << ").";
      return false;
    }

    bool topology_ok = true;
    if (!topology.topology.empty()) {
      try {
        auto current_topology = ctx->display->getCurrentTopology();
        const bool already_matches = ctx->display->isTopologyTheSame(current_topology, topology.topology);
        if (!already_matches) {
          BOOST_LOG(info) << "Display helper: applying requested topology (" << label << ").";
          topology_ok = ctx->display->setTopology(topology.topology);
          if (!topology_ok) {
            BOOST_LOG(warning) << "Display helper: requested topology apply failed (" << label << ").";
          }
        } else {
          BOOST_LOG(debug) << "Display helper: requested topology already active (" << label << ").";
        }
      } catch (const std::exception &ex) {
        BOOST_LOG(warning) << "Display helper: topology inspection failed (" << label << "): " << ex.what();
        topology_ok = false;
      } catch (...) {
        BOOST_LOG(warning) << "Display helper: topology inspection failed (" << label << ") with an unknown error.";
        topology_ok = false;
      }
    }

    for (const auto &[device_id, point] : topology.monitor_positions) {
      BOOST_LOG(debug) << "Display helper: setting origin for " << device_id
                       << " to (" << point.m_x << "," << point.m_y << ") after " << label << ".";
      (void) ctx->display->setDisplayOrigin(device_id, point);
    }

    return topology_ok;
  }

  display_device::SettingsManagerInterface::ApplyResult apply_in_process(
    const display_helper_integration::DisplayApplyRequest &request
  ) {
    if (!request.configuration) {
      BOOST_LOG(error) << "Display helper (in-process): no configuration provided for APPLY request.";
      return display_device::SettingsManagerInterface::ApplyResult::DevicePrepFailed;
    }

    auto ctx = make_settings_manager();
    if (!ctx) {
      return display_device::SettingsManagerInterface::ApplyResult::DevicePrepFailed;
    }

    const auto result = ctx->settings_mgr->applySettings(*request.configuration);
    const bool ok = (result == display_device::SettingsManagerInterface::ApplyResult::Ok);
    BOOST_LOG(info) << "Display helper (in-process): APPLY result=" << (ok ? "Ok" : "Failed");
    if (!ok) {
      return result;
    }

    // Apply optional topology/placement tweaks when provided.
    if (!request.topology.topology.empty()) {
      BOOST_LOG(debug) << "Display helper (in-process): applying topology override.";
      (void) ctx->display->setTopology(request.topology.topology);
    }
    for (const auto &[device_id, point] : request.topology.monitor_positions) {
      BOOST_LOG(debug) << "Display helper (in-process): setting origin for " << device_id
                       << " to (" << point.m_x << "," << point.m_y << ").";
      (void) ctx->display->setDisplayOrigin(device_id, point);
    }

    return display_device::SettingsManagerInterface::ApplyResult::Ok;
  }

  constexpr DWORD kHelperForceKillWaitMs = 2000;

  bool wait_for_helper_ipc_ready_locked() {
    const auto deadline = std::chrono::steady_clock::now() + kHelperIpcReadyTimeout;
    int attempts = 0;

    platf::display_helper_client::reset_connection();
    while (std::chrono::steady_clock::now() < deadline) {
      if (shutdown_requested()) {
        return false;
      }
      if (platf::display_helper_client::send_ping()) {
        if (attempts > 0) {
          BOOST_LOG(debug) << "Display helper IPC became reachable after " << attempts << " retries.";
        }
        return true;
      }
      ++attempts;
      std::this_thread::sleep_for(kHelperIpcReadyPoll);
      platf::display_helper_client::reset_connection();
    }

    BOOST_LOG(warning) << "Display helper IPC did not respond within " << kHelperIpcReadyTimeout.count()
                       << " ms of helper start.";
    return false;
  }

  const char *virtual_layout_to_string(const display_helper_integration::VirtualDisplayArrangement layout) {
    using enum display_helper_integration::VirtualDisplayArrangement;
    switch (layout) {
      case Extended:
        return "extended";
      case ExtendedPrimary:
        return "extended_primary";
      case ExtendedIsolated:
        return "extended_isolated";
      case ExtendedPrimaryIsolated:
        return "extended_primary_isolated";
      case Exclusive:
      default:
        return "exclusive";
    }
  }

  void kill_all_helper_processes() {
    helper_proc().terminate();

    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) {
      DWORD err = GetLastError();
      BOOST_LOG(error) << "Display helper: failed to snapshot processes for cleanup (winerr=" << err << ").";
      return;
    }

    PROCESSENTRY32W entry {};
    entry.dwSize = sizeof(entry);
    std::vector<DWORD> targets;

    if (Process32FirstW(snapshot, &entry)) {
      do {
        if (_wcsicmp(entry.szExeFile, L"sunshine_display_helper.exe") == 0 &&
            entry.th32ProcessID != GetCurrentProcessId()) {
          targets.push_back(entry.th32ProcessID);
        }
      } while (Process32NextW(snapshot, &entry));
    } else {
      DWORD err = GetLastError();
      if (err != ERROR_NO_MORE_FILES) {
        BOOST_LOG(warning) << "Display helper: process enumeration failed during cleanup (winerr=" << err << ").";
      }
    }

    CloseHandle(snapshot);

    for (DWORD pid : targets) {
      HANDLE h = OpenProcess(PROCESS_TERMINATE | SYNCHRONIZE | PROCESS_QUERY_INFORMATION, FALSE, pid);
      if (!h) {
        DWORD err = GetLastError();
        BOOST_LOG(warning) << "Display helper: unable to open external instance (pid=" << pid
                           << ", winerr=" << err << ") for termination.";
        continue;
      }

      DWORD wait = WaitForSingleObject(h, 0);
      if (wait == WAIT_TIMEOUT) {
        BOOST_LOG(warning) << "Display helper: terminating external instance (pid=" << pid << ").";
        if (!TerminateProcess(h, 1)) {
          DWORD err = GetLastError();
          BOOST_LOG(error) << "Display helper: TerminateProcess failed for pid=" << pid << " (winerr=" << err << ").";
        } else {
          DWORD wait_res = WaitForSingleObject(h, kHelperForceKillWaitMs);
          if (wait_res != WAIT_OBJECT_0) {
            BOOST_LOG(warning) << "Display helper: external instance pid=" << pid
                               << " did not exit within " << kHelperForceKillWaitMs << " ms.";
          }
        }
      }

      CloseHandle(h);
    }
  }

  struct session_dd_fields_t {
    int width = -1;
    int height = -1;
    int fps = -1;
    bool enable_hdr = false;
    bool enable_sops = false;
    bool virtual_display = false;
    std::string virtual_display_device_id;
    std::optional<int> framegen_refresh_rate;
    int framegen_refresh_multiplier = 1;
    bool gen1_framegen_fix = false;
    bool gen2_framegen_fix = false;
  };

  static std::mutex g_session_mutex;
  static std::optional<session_dd_fields_t> g_active_session_dd;

  // Tracks the exact helper REVERT generation expected to still be active. This
  // avoids spamming DISARM and prevents stale acknowledgements from clearing a
  // newer restore request.
  // Non-zero identifies the exact REVERT generation that is still unconfirmed.
  // Compare/exchange prevents a late DISARM/APPLY result for generation N from
  // clearing a newer generation N+1.
  static display_helper::PendingRestoreTracker g_restore_tracker;
  // Orders host-side REVERT publication/dispatch against DISARM probes. Without
  // this lock, DISARM could overtake the gap between publishing generation N and
  // writing its REVERT frame, then falsely clear N before the helper sees it.
  static std::recursive_timed_mutex g_restore_handoff_mutex;
  static std::atomic<bool> g_external_helper_recovery_requested {false};

  struct DisplayLifecycleState {
    std::mutex mutex;
    std::condition_variable cv;
    bool start_in_progress {false};
    bool teardown_in_progress {false};
    std::uint32_t start_waiters {0};
    std::uint64_t next_generation {0};
    std::uint64_t start_generation {0};
    std::uint64_t teardown_generation {0};
    std::function<void()> deferred_teardown;
    bool platform_streaming_claimed {false};
    bool platform_streaming_started {false};
    std::uint64_t platform_streaming_generation {0};
  };

  DisplayLifecycleState &display_lifecycle_state() {
    // Deliberately process-lifetime: reservation destructors can run from late
    // RTSP timeout callbacks while other function-local statics are unwinding.
    static auto *state = new DisplayLifecycleState();
    return *state;
  }

  std::mutex &platform_streaming_hook_mutex() {
    static auto *mutex = new std::mutex();
    return *mutex;
  }

  struct DisplayCleanupDispatcher {
    std::mutex mutex;
    thread_pool_util::ThreadPool pool;
    bool running {false};
    bool accepting {false};
    bool stopped {false};
  };

  DisplayCleanupDispatcher &display_cleanup_dispatcher() {
    // Process-lifetime for the same late-callback reason as the lifecycle
    // state. Explicit shutdown below stops and joins its workers before the
    // virtual-display singleton is destroyed.
    static auto *dispatcher = new DisplayCleanupDispatcher();
    return *dispatcher;
  }

  bool start_display_cleanup_dispatcher_locked(DisplayCleanupDispatcher &dispatcher) noexcept {
    if (dispatcher.accepting) {
      return true;
    }
    if (dispatcher.stopped) {
      return false;
    }
    try {
      // Verification can wait for up to six seconds while rollback/retry work
      // may need a display handoff. Keep those rare operations independent so
      // a verifier cannot delay a safety cleanup.
      dispatcher.pool.start(2);
      dispatcher.running = true;
      dispatcher.accepting = true;
      return true;
    } catch (const std::exception &e) {
      BOOST_LOG(error) << "Display cleanup dispatcher could not start: " << e.what();
    } catch (...) {
      BOOST_LOG(error) << "Display cleanup dispatcher could not start.";
    }
    dispatcher.pool.stop();
    dispatcher.stopped = true;
    return false;
  }

  template<typename Submit>
  bool enqueue_display_cleanup_task_impl(Submit &&submit) noexcept {
    try {
      auto &dispatcher = display_cleanup_dispatcher();
      std::lock_guard lock(dispatcher.mutex);
      if (!start_display_cleanup_dispatcher_locked(dispatcher)) {
        BOOST_LOG(warning) << "Display cleanup task rejected because the dispatcher is stopped.";
        return false;
      }
      submit(dispatcher.pool);
      return true;
    } catch (const std::exception &e) {
      BOOST_LOG(error) << "Unable to enqueue display cleanup task: " << e.what();
    } catch (...) {
      BOOST_LOG(error) << "Unable to enqueue display cleanup task.";
    }
    return false;
  }

  std::function<void()> guard_display_cleanup_task(std::function<void()> task) {
    return [task = std::move(task)]() mutable {
      try {
        if (task) {
          task();
        }
      } catch (const std::exception &e) {
        BOOST_LOG(error) << "Deferred display cleanup task failed: " << e.what();
      } catch (...) {
        BOOST_LOG(error) << "Deferred display cleanup task failed with an unknown exception.";
      }
    };
  }

  void append_deferred_teardown_locked(
    DisplayLifecycleState &state,
    std::function<void()> teardown
  ) {
    if (!teardown) {
      return;
    }
    if (!state.deferred_teardown) {
      state.deferred_teardown = std::move(teardown);
      return;
    }
    auto earlier = std::move(state.deferred_teardown);
    state.deferred_teardown = [earlier = std::move(earlier),
                               later = std::move(teardown)]() mutable {
      try {
        earlier();
      } catch (...) {
        BOOST_LOG(warning) << "Display lifecycle: one retained teardown callback threw; continuing remaining cleanup.";
      }
      try {
        later();
      } catch (...) {
        BOOST_LOG(warning) << "Display lifecycle: one retained teardown callback threw.";
      }
    };
  }

  void release_display_teardown_lease(std::uint64_t generation);

  void dispatch_retained_display_teardown(
    std::function<void()> teardown,
    std::uint64_t teardown_generation
  ) noexcept {
    bool queued = false;
    try {
      queued = display_helper_integration::enqueue_display_cleanup_task([teardown = std::move(teardown),
                                                                         teardown_generation]() mutable {
        try {
          teardown();
        } catch (...) {
          BOOST_LOG(warning) << "Display lifecycle: deferred abandoned-start teardown threw.";
        }
        release_display_teardown_lease(teardown_generation);
      });
    } catch (const std::exception &e) {
      BOOST_LOG(error) << "Display lifecycle: unable to enqueue retained teardown: " << e.what();
    } catch (...) {
      BOOST_LOG(error) << "Display lifecycle: unable to enqueue retained teardown.";
    }
    if (!queued) {
      // Reservation destructors are noexcept. If shutdown or resource
      // exhaustion rejects cleanup work, at least release the logical gate so
      // later starts cannot remain blocked forever.
      release_display_teardown_lease(teardown_generation);
    }
  }

  void release_display_start_reservation(std::uint64_t generation, bool published_active) {
    auto &state = display_lifecycle_state();
    std::function<void()> deferred_teardown;
    std::function<void()> discarded_teardown;
    std::uint64_t deferred_teardown_generation = 0;
    {
      std::lock_guard lock(state.mutex);
      if (!state.start_in_progress || state.start_generation != generation) {
        return;
      }
      state.start_in_progress = false;
      state.start_generation = 0;
      if (published_active) {
        // The new active counter now owns final teardown responsibility.
        discarded_teardown = std::move(state.deferred_teardown);
      } else if (state.deferred_teardown) {
        // Convert the abandoned start directly into a logical teardown so a
        // waiting start cannot overtake the retained last-session cleanup.
        deferred_teardown = std::move(state.deferred_teardown);
        state.teardown_in_progress = true;
        state.teardown_generation = ++state.next_generation;
        deferred_teardown_generation = state.teardown_generation;
      }
    }
    if (deferred_teardown) {
      // Token destruction frequently occurs while RTSP/WebRTC owner locks are
      // held. Keep the logical teardown gate, but execute out of line to avoid
      // re-entering those locks on the destructor thread.
      dispatch_retained_display_teardown(
        std::move(deferred_teardown),
        deferred_teardown_generation
      );
      return;
    }
    state.cv.notify_all();
  }

  void release_display_teardown_lease(std::uint64_t generation) {
    auto &state = display_lifecycle_state();
    std::function<void()> followup_teardown;
    std::uint64_t followup_generation = 0;
    {
      std::lock_guard lock(state.mutex);
      if (!state.teardown_in_progress || state.teardown_generation != generation) {
        return;
      }
      if (state.deferred_teardown) {
        followup_teardown = std::move(state.deferred_teardown);
        state.teardown_generation = ++state.next_generation;
        followup_generation = state.teardown_generation;
      } else {
        state.teardown_in_progress = false;
        state.teardown_generation = 0;
      }
    }
    if (followup_teardown) {
      dispatch_retained_display_teardown(
        std::move(followup_teardown),
        followup_generation
      );
      return;
    }
    state.cv.notify_all();
  }

  void reset_helper_connection_serialized() {
    std::lock_guard<std::recursive_timed_mutex> handoff_lock(g_restore_handoff_mutex);
    platf::display_helper_client::reset_connection();
  }

  // Resolve the effective display helper engine. In automatic mode the v2 engine
  // only rides pre-release builds; stable releases keep the legacy engine until
  // v2 has soaked, and users opt in explicitly via dd_display_helper_engine.
  static bool use_legacy_helper_engine() {
    using engine_e = config::video_t::dd_t::helper_engine_e;
    switch (config::video.dd.display_helper_engine) {
      case engine_e::legacy:
        return true;
      case engine_e::v2:
        return false;
      case engine_e::automatic:
      default:
        break;
    }
  #ifdef PROJECT_VERSION_PRERELEASE
    return std::string_view(PROJECT_VERSION_PRERELEASE).empty();
  #else
    return true;
  #endif
  }

  static std::atomic<std::uint64_t> g_disarm_generation_sent {0};
  static std::atomic<std::int64_t> g_last_disarm_attempt_us {0};
  static std::atomic<std::int64_t> g_last_disarm_success_us {0};
  static std::atomic<std::int64_t> g_last_helper_start_failure_us {0};

  // Tracks when the most recent successful APPLY completed, so the capture thread
  // can add a stabilization delay before attempting to reinit after topology changes.
  static std::atomic<std::int64_t> g_last_apply_completed_us {0};

  static std::int64_t now_steady_us() {
    using namespace std::chrono;
    return duration_cast<microseconds>(steady_clock::now().time_since_epoch()).count();
  }

  bool helper_start_failure_cooldown_active() {
    const auto last_us = g_last_helper_start_failure_us.load(std::memory_order_relaxed);
    if (last_us <= 0) {
      return false;
    }

    const auto elapsed_us = now_steady_us() - last_us;
    const auto cooldown_us = std::chrono::duration_cast<std::chrono::microseconds>(kHelperStartFailureCooldown).count();
    if (elapsed_us >= cooldown_us) {
      return false;
    }

    const auto remaining_ms = (cooldown_us - elapsed_us) / 1000;
    BOOST_LOG(warning) << "Display helper: skipping helper start during failure cooldown ("
                       << remaining_ms << "ms remaining).";
    return true;
  }

  void note_helper_start_failure(const char *reason) {
    g_last_helper_start_failure_us.store(now_steady_us(), std::memory_order_relaxed);
    BOOST_LOG(warning) << "Display helper: helper start failure cooldown armed after " << reason << ".";
  }

  bool restore_expected_with_live_helper() {
    const auto restore_generation = g_restore_tracker.current();
    if (restore_generation == 0) {
      return false;
    }
    if (helper_process_running()) {
      return true;
    }
    if (g_restore_tracker.clear_if(restore_generation)) {
      return false;
    }
    return g_restore_tracker.current() != 0 && helper_process_running();
  }

  // Active session display parameters snapshot for re-apply on reconnect.
  // We do NOT cache serialized JSON, only the subset of session fields that
  // affect display configuration. On reconnect, we rebuild the full
  // SingleDisplayConfiguration from current Sunshine config + these fields.

  bool dd_feature_enabled() {
    using config_option_e = config::video_t::dd_t::config_option_e;
    if (config::video.dd.configuration_option != config_option_e::disabled) {
      return true;
    }

    const bool virtual_display_selected =
      (config::video.virtual_display_mode == config::video_t::virtual_display_mode_e::per_client ||
       config::video.virtual_display_mode == config::video_t::virtual_display_mode_e::shared);
    if (virtual_display_selected) {
      return true;
    }

    std::lock_guard<std::mutex> lg(g_session_mutex);
    return g_active_session_dd && g_active_session_dd->virtual_display;
  }

  bool shutdown_requested() {
    if (!mail::man) {
      return false;
    }
    try {
      auto shutdown_event = mail::man->event<bool>(mail::shutdown);
      return shutdown_event && shutdown_event->peek();
    } catch (...) {
      return false;
    }
  }

  platf::display_helper_client::DisarmResult disarm_helper_restore_if_running_locked() {
    const auto observed_restore_generation = g_restore_tracker.current();
    const bool helper_running = helper_process_running();
    if (!helper_running) {
      if (observed_restore_generation == 0 || g_restore_tracker.clear_if(observed_restore_generation)) {
        return platf::display_helper_client::DisarmResult::Disarmed;
      }
      return platf::display_helper_client::DisarmResult::Busy;
    }

    const auto pending_restore_generation = g_restore_tracker.current();
    const bool restore_expected = pending_restore_generation != 0;
    const auto now_us = now_steady_us();
    const auto last_attempt_us = g_last_disarm_attempt_us.load(std::memory_order_relaxed);

    // Don't spam DISARM frames (they share the helper's job/message queues with APPLY/REVERT).
    // A cached success is valid only for the restore generation it acknowledged.
    // A newly-published REVERT must bypass the throttle and receive its own ACK.
    if ((now_us - last_attempt_us) < (kDisarmRetryThrottle.count() * 1000)) {
      const auto last_success_us = g_last_disarm_success_us.load(std::memory_order_relaxed);
      const auto disarmed_generation = g_disarm_generation_sent.load(std::memory_order_acquire);
      const bool cached_success_covers_restore =
        restore_expected && disarmed_generation >= pending_restore_generation;
      if (cached_success_covers_restore &&
          (now_us - last_success_us) < (kDisarmRetryThrottle.count() * 1000)) {
        return platf::display_helper_client::DisarmResult::Disarmed;
      }
      if (restore_expected && disarmed_generation >= pending_restore_generation) {
        return platf::display_helper_client::DisarmResult::Busy;
      }
      // With no tracked generation, the helper may have autonomously armed a
      // restore after the last ACK. A newer tracked restore has the same rule:
      // fall through and probe current helper state rather than using cache.
    }

    // If we believe a restore loop is active, ensure we only issue one DISARM per restore generation unless it fails
    // and the throttle allows a retry.
    const auto restore_generation = pending_restore_generation;
    if (restore_expected) {
      const auto disarmed_generation = g_disarm_generation_sent.load(std::memory_order_relaxed);
      if (disarmed_generation >= restore_generation) {
        const auto last_success_us = g_last_disarm_success_us.load(std::memory_order_relaxed);
        return (now_us - last_success_us) < (kDisarmRetryThrottle.count() * 1000) ?
                 platf::display_helper_client::DisarmResult::Disarmed :
                 platf::display_helper_client::DisarmResult::Busy;
      }
    }

    // The client enforces one total deadline across connect, send, and acknowledgement.
    auto try_send_fast = [&](int max_total_ms) -> platf::display_helper_client::DisarmResult {
      return platf::display_helper_client::send_disarm_restore_fast(std::max(10, max_total_ms));
    };

    g_last_disarm_attempt_us.store(now_us, std::memory_order_relaxed);
    auto result = try_send_fast(static_cast<int>(kDisarmRestoreBudget.count()));

    if (result == platf::display_helper_client::DisarmResult::Disarmed) {
      g_last_disarm_success_us.store(now_us, std::memory_order_relaxed);
      g_disarm_generation_sent.store(restore_generation, std::memory_order_relaxed);
      // A late acknowledgement for an older restore generation must never clear
      // a newer REVERT request.
      if (restore_generation != 0) {
        (void) g_restore_tracker.clear_if(restore_generation);
      }
      BOOST_LOG(info) << "Display helper: DISARM confirmed before display mutation.";
      return platf::display_helper_client::DisarmResult::Disarmed;
    }

    if (result == platf::display_helper_client::DisarmResult::Busy) {
      if (!restore_expected) {
        (void) g_restore_tracker.discover_restore();
      }
      BOOST_LOG(info) << "Display helper: DISARM rejected because a restore mutation is unconfirmed; preserving helper and restore state.";
      return platf::display_helper_client::DisarmResult::Busy;
    }

    // A missing acknowledgement is not proof that SetDisplayConfig stopped.
    // Never kill or disconnect a helper that the host believes may be restoring.
    if (restore_expected) {
      BOOST_LOG(warning) << "Display helper: DISARM was not acknowledged; preserving unconfirmed restore state.";
    }
    return platf::display_helper_client::DisarmResult::Unavailable;
  }

  platf::display_helper_client::DisarmResult disarm_helper_restore_if_running() {
    // This is a fast probe on the stream-start path. Contention means a REVERT
    // dispatch is currently being linearized, so report unavailable and let a
    // bounded caller retry instead of waiting here.
    std::unique_lock<std::recursive_timed_mutex> handoff_lock(g_restore_handoff_mutex, std::try_to_lock);
    if (!handoff_lock.owns_lock()) {
      return platf::display_helper_client::DisarmResult::Unavailable;
    }
    return disarm_helper_restore_if_running_locked();
  }

  bool ensure_helper_started(bool force_restart, bool force_enable) {
    // Connection reset/replacement can make the helper arm an autonomous
    // disconnect restore. It must not occur while a start/cleanup lease relies
    // on a prior DISARM acknowledgement.
    std::lock_guard<std::recursive_timed_mutex> handoff_lock(g_restore_handoff_mutex);
    if (!force_enable && !dd_feature_enabled()) {
      return false;
    }
    const bool shutting_down = shutdown_requested();
    std::lock_guard<std::mutex> lg(helper_mutex());
    const HANDLE tracked_helper = helper_proc().get_process_handle();
    const bool tracked_helper_is_live =
      tracked_helper != nullptr && WaitForSingleObject(tracked_helper, 0) == WAIT_TIMEOUT;
    if (!tracked_helper_is_live && external_helper_process_running()) {
      // A restore-task helper or a helper surviving a prior Sunshine process is
      // not ours to terminate. Reuse it when reachable; otherwise preserve it
      // until the handoff protocol or process exit proves mutation has drained.
      if (platf::display_helper_client::send_ping_fast(100)) {
        BOOST_LOG(info) << "Display helper: reusing externally-owned helper instance.";
        return true;
      }
      BOOST_LOG(warning) << "Display helper: external instance is not reachable; refusing unsafe replacement.";
      return false;
    }
    // Already started? Verify liveness to avoid stale or wedged state
    if (HANDLE h = helper_proc().get_process_handle(); h != nullptr) {
      BOOST_LOG(debug) << "Display helper: checking existing process handle...";
      DWORD wait = WaitForSingleObject(h, 0);
      if (wait == WAIT_TIMEOUT) {
        DWORD pid = GetProcessId(h);
        BOOST_LOG(debug) << "Display helper already running (pid=" << pid << ")";
        if (!force_restart) {
          // Check IPC liveness with a lightweight ping; if responsive, reuse existing helper
          bool ping_ok = false;
          for (int i = 0; i < 2 && !ping_ok; ++i) {
            ping_ok = platf::display_helper_client::send_ping();
            if (!ping_ok) {
              std::this_thread::sleep_for(std::chrono::milliseconds(200));
            }
          }
          if (ping_ok) {
            return true;
          }
          platf::display_helper_client::reset_connection();
          BOOST_LOG(warning) << "Display helper process ping failed; keeping existing instance and deferring restart.";
          note_helper_start_failure("failed ping");
          return false;
        }

        if (platf::display_helper_client::send_ping_fast(100)) {
          BOOST_LOG(debug) << "Display helper hard restart skipped because existing helper accepted a fast ping.";
          return true;
        }
        platf::display_helper_client::reset_connection();
        BOOST_LOG(warning) << "Display helper hard restart requested because existing helper did not accept a fast ping.";

        BOOST_LOG(warning) << "Display helper: hard restart requested; terminating existing instance (pid=" << pid
                           << ") with no grace period.";
        platf::display_helper_client::reset_connection();
        helper_proc().terminate();

        DWORD wait_result = WaitForSingleObject(h, kHelperForceKillWaitMs);
        if (wait_result == WAIT_OBJECT_0) {
          DWORD exit_code = 0;
          GetExitCodeProcess(h, &exit_code);
          BOOST_LOG(info) << "Display helper exited after forced termination (code=" << exit_code << ").";
        } else if (wait_result == WAIT_TIMEOUT) {
          BOOST_LOG(warning) << "Display helper: process did not exit within " << kHelperForceKillWaitMs
                             << " ms after termination request; continuing with cleanup.";
        } else {
          DWORD wait_err = GetLastError();
          BOOST_LOG(warning) << "Display helper: wait after termination failed (winerr=" << wait_err
                             << "); continuing with cleanup.";
        }

        // Small delay to reduce the chance of named pipe / mutex conflicts during rapid restart.
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
      } else {
        // Process exited; fall through to restart
        DWORD exit_code = 0;
        GetExitCodeProcess(h, &exit_code);
        BOOST_LOG(debug) << "Display helper process detected as exited (code=" << exit_code << "); preparing restart.";
      }
    }
    if (shutting_down) {
      return false;
    }

    if (helper_start_failure_cooldown_active()) {
      return false;
    }

    if (external_helper_process_running()) {
      BOOST_LOG(warning) << "Display helper: external instance appeared before launch; deferring replacement.";
      return false;
    }
    kill_all_helper_processes();

    // Compute path to sunshine_display_helper.exe inside the tools subdirectory next to Sunshine.exe
    wchar_t module_path[MAX_PATH] = {};
    if (!GetModuleFileNameW(nullptr, module_path, _countof(module_path))) {
      BOOST_LOG(error) << "Failed to resolve Sunshine module path; cannot launch display helper.";
      return false;
    }
    std::filesystem::path exe_path(module_path);
    std::filesystem::path dir = exe_path.parent_path();
    std::filesystem::path helper = dir / L"tools" / L"sunshine_display_helper.exe";

    if (!std::filesystem::exists(helper)) {
      BOOST_LOG(warning) << "Display helper not found at: " << platf::to_utf8(helper.wstring())
                         << ". Ensure the tools subdirectory is present and contains sunshine_display_helper.exe.";
      return false;
    }

    const bool allow_system_fallback = platf::is_running_as_system() && !user_session_ready();
    // Select the helper engine (legacy fallback vs v2) and propagate the log level.
    const bool legacy_engine = use_legacy_helper_engine();
    std::wstring helper_args = legacy_engine ? L"--engine=legacy" : L"--engine=v2";
    helper_args += L" --log-level=";
    helper_args += std::to_wstring(std::clamp(config::sunshine.min_log_level, 0, 6));
    statefile::save_display_helper_engine(legacy_engine ? "legacy" : "v2");
    BOOST_LOG(debug) << "Starting display helper: " << platf::to_utf8(helper.wstring())
                     << " " << platf::to_utf8(helper_args);
    bool started = helper_proc().start(helper.wstring(), helper_args, allow_system_fallback);
    if (!started && force_restart) {
      // If we were asked to hard-restart, tolerate a brief overlap window where the old
      // instance is still tearing down and retry quickly.
      for (int attempt = 0; attempt < 5 && !started; ++attempt) {
        std::this_thread::sleep_for(std::chrono::milliseconds(150));
        started = helper_proc().start(helper.wstring(), helper_args, allow_system_fallback);
      }
    }
    if (!started) {
      BOOST_LOG(error) << "Failed to start display helper: " << platf::to_utf8(helper.wstring());
      note_helper_start_failure("process launch failure");
      return false;
    }

    HANDLE h = helper_proc().get_process_handle();
    if (!h) {
      BOOST_LOG(error) << "Display helper started but no process handle available";
      note_helper_start_failure("missing process handle");
      return false;
    }

    DWORD pid = GetProcessId(h);
    BOOST_LOG(info) << "Display helper successfully started (pid=" << pid << ")";

    // Give the helper process time to initialize and create its named pipe server
    // Check if it exits early (e.g., singleton mutex conflict from incomplete cleanup)
    for (int check = 0; check < 6; ++check) {
      DWORD wait = WaitForSingleObject(h, 50);
      if (wait == WAIT_OBJECT_0) {
        DWORD exit_code = 0;
        GetExitCodeProcess(h, &exit_code);
        if (exit_code == 3) {
          BOOST_LOG(warning) << "Display helper exited immediately with code 3 (singleton conflict). "
                             << "Retrying after extended cleanup delay...";
          std::this_thread::sleep_for(std::chrono::milliseconds(1000));

          const bool retry_started = helper_proc().start(helper.wstring(), helper_args, allow_system_fallback);
          if (!retry_started) {
            BOOST_LOG(error) << "Display helper retry start failed";
            note_helper_start_failure("singleton retry launch failure");
            return false;
          }
          h = helper_proc().get_process_handle();
          if (h) {
            pid = GetProcessId(h);
            BOOST_LOG(info) << "Display helper retry succeeded (pid=" << pid << ")";
            std::this_thread::sleep_for(std::chrono::milliseconds(300));
          }
          break;
        } else {
          BOOST_LOG(error) << "Display helper exited unexpectedly with code " << exit_code;
          note_helper_start_failure("unexpected process exit");
          return false;
        }
      }
    }

    // Final initialization delay for pipe server creation
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    const bool ipc_ready = wait_for_helper_ipc_ready_locked();
    if (!ipc_ready) {
      note_helper_start_failure("IPC readiness timeout");
    } else if (!legacy_engine) {
      // Keep the v2 helper's log verbosity in sync with Sunshine (legacy would
      // log "Unknown message type" for this frame).
      (void) platf::display_helper_client::send_log_level(std::clamp(config::sunshine.min_log_level, 0, 6));
    }
    return ipc_ready;
  }

  // Watchdog state for helper liveness during active streams.
  // g_watchdog_mutex guards g_watchdog_running and g_watchdog_thread together:
  // start/stop are called from many threads (rtsp/webrtc session end, app
  // termination, paused-session cleanup, hotkeys, shutdown), and an
  // unsynchronized jthread move-assign racing joinable()/join() can make
  // join() throw std::system_error, which escapes to std::terminate.
  static std::mutex g_watchdog_mutex;
  static bool g_watchdog_running = false;
  static std::jthread g_watchdog_thread;
  static std::chrono::steady_clock::time_point g_last_vd_reenable {};

  constexpr auto kVirtualDisplayReenableCooldown = std::chrono::seconds(3);

  bool recently_reenabled_virtual_display() {
    if (g_last_vd_reenable.time_since_epoch().count() == 0) {
      return false;
    }
    return (std::chrono::steady_clock::now() - g_last_vd_reenable) < kVirtualDisplayReenableCooldown;
  }

  [[maybe_unused]] void explicit_virtual_display_reset_and_apply(
    display_helper_integration::DisplayApplyBuilder &builder,
    const rtsp_stream::launch_session_t &session,
    std::function<bool(const display_helper_integration::DisplayApplyRequest &)> apply_fn
  ) {
    // Only act if virtual display is in play.
    if (!session.virtual_display && !builder.build().session_overrides.virtual_display_override.value_or(false)) {
      return;
    }

    // Debounce to avoid hammering the driver.
    if (recently_reenabled_virtual_display()) {
      return;
    }

    // First send a "blank" request to detach virtual display.
    display_helper_integration::DisplayApplyBuilder disable_builder;
    disable_builder.set_session(session);
    auto &overrides = disable_builder.mutable_session_overrides();
    overrides.virtual_display_override = false;
    disable_builder.set_action(display_helper_integration::DisplayApplyAction::Apply);
    auto disable_req = disable_builder.build();

    BOOST_LOG(info) << "Display helper: explicit virtual display disable before re-enable.";
    (void) apply_fn(disable_req);

    std::this_thread::sleep_for(std::chrono::milliseconds(1500));

    // Re-enable with the original builder intent.
    BOOST_LOG(info) << "Display helper: explicit virtual display re-enable after disappearance.";
    auto enable_req = builder.build();
    if (apply_fn(enable_req)) {
      g_last_vd_reenable = std::chrono::steady_clock::now();
    }
  }

  static void set_active_session(
    const rtsp_stream::launch_session_t &session,
    std::optional<std::string> device_id_override = std::nullopt,
    std::optional<int> fps_override = std::nullopt,
    std::optional<int> width_override = std::nullopt,
    std::optional<int> height_override = std::nullopt,
    std::optional<bool> virtual_display_override = std::nullopt,
    std::optional<int> framegen_refresh_override = std::nullopt
  ) {
    std::lock_guard<std::mutex> lg(g_session_mutex);
    const int effective_fps = fps_override ? *fps_override : (session.framegen_refresh_rate && *session.framegen_refresh_rate > 0 ? *session.framegen_refresh_rate : session.fps);
    g_active_session_dd = session_dd_fields_t {
      .width = width_override ? *width_override : session.width,
      .height = height_override ? *height_override : session.height,
      .fps = effective_fps,
      .enable_hdr = rtsp_stream::effective_hdr_requested(session),
      .enable_sops = session.enable_sops,
      .virtual_display = virtual_display_override ? *virtual_display_override : session.virtual_display,
      .virtual_display_device_id = device_id_override ? *device_id_override : session.virtual_display_device_id,
      .framegen_refresh_rate = framegen_refresh_override ? framegen_refresh_override : session.framegen_refresh_rate,
      .framegen_refresh_multiplier = session.framegen_refresh_multiplier,
      .gen1_framegen_fix = session.gen1_framegen_fix,
      .gen2_framegen_fix = session.gen2_framegen_fix,
    };
    if (!g_active_session_dd->virtual_display_device_id.empty()) {
      // Persist so the helper (including the boot-time restore task) can exclude this
      // device from snapshots/baselines even when its EDID is not classifiable.
      statefile::remember_virtual_display_device(g_active_session_dd->virtual_display_device_id);
    }
  }

  [[maybe_unused]] static std::optional<session_dd_fields_t> get_active_session_copy() {
    std::lock_guard<std::mutex> lg(g_session_mutex);
    return g_active_session_dd;
  }

  static void clear_active_session() {
    std::lock_guard<std::mutex> lg(g_session_mutex);
    g_active_session_dd.reset();
  }

  std::optional<std::string> build_helper_apply_payload(const display_helper_integration::DisplayApplyRequest &request) {
    if (!request.configuration) {
      BOOST_LOG(error) << "Display helper: no configuration provided for APPLY payload.";
      return std::nullopt;
    }

    bool ok = true;
    std::string json = display_device::toJson(*request.configuration, 0u, &ok);
    if (!ok) {
      BOOST_LOG(error) << "Display helper: failed to serialize configuration for helper APPLY payload.";
      return std::nullopt;
    }

    nlohmann::json j = nlohmann::json::parse(json, nullptr, false);
    if (j.is_discarded()) {
      BOOST_LOG(error) << "Display helper: failed to parse serialized configuration JSON for helper APPLY payload.";
      return std::nullopt;
    }

    if (request.attach_hdr_toggle_flag) {
      j["wa_hdr_toggle"] = true;
    }

    if (request.virtual_display_arrangement) {
      j["sunshine_virtual_layout"] = virtual_layout_to_string(*request.virtual_display_arrangement);
    }

    if (!request.topology.topology.empty()) {
      nlohmann::json topo = nlohmann::json::array();
      for (const auto &grp : request.topology.topology) {
        nlohmann::json group = nlohmann::json::array();
        for (const auto &id : grp) {
          group.push_back(id);
        }
        topo.push_back(std::move(group));
      }
      j["sunshine_topology"] = std::move(topo);
    }

    if (!request.topology.monitor_positions.empty()) {
      nlohmann::json positions = nlohmann::json::object();
      for (const auto &[device_id, point] : request.topology.monitor_positions) {
        positions[device_id] = {{"x", point.m_x}, {"y", point.m_y}};
      }
      j["sunshine_monitor_positions"] = std::move(positions);
    }

    if (!request.topology.device_refresh_rate_overrides.empty()) {
      nlohmann::json overrides = nlohmann::json::object();
      for (const auto &[device_id, rate] : request.topology.device_refresh_rate_overrides) {
        overrides[device_id] = {{"num", rate.first}, {"den", rate.second}};
      }
      j["sunshine_device_refresh_rate_overrides"] = std::move(overrides);
    }

    // Pass golden-first restore preference to helper
    if (config::video.dd.always_restore_from_golden) {
      j["sunshine_always_restore_from_golden"] = true;
    }
    j["sunshine_restore_on_disconnect"] = config::video.dd.config_revert_on_disconnect;

    // Always carry the exclusion list: a hard-restarted helper has no SNAPSHOT_CURRENT
    // context and would otherwise capture virtual displays into its pre-apply baseline.
    try {
      j["sunshine_snapshot_exclude_devices"] = effective_snapshot_exclude_devices();
    } catch (...) {
    }

    return j.dump();
  }

  std::string build_revert_payload(bool prefer_golden_if_current_missing) {
    nlohmann::json j = nlohmann::json::object();
    j["sunshine_prefer_golden_if_current_missing"] = prefer_golden_if_current_missing;
    j["sunshine_always_restore_from_golden"] = config::video.dd.always_restore_from_golden;
    return j.dump();
  }

  static void watchdog_proc(std::stop_token st) {
    using namespace std::chrono_literals;
    constexpr auto kActiveInterval = 5s;
    constexpr auto kSuspendedInterval = 20s;
    bool helper_ready = false;

    auto sleep_interruptible = [&st](std::chrono::milliseconds interval) {
      for (auto slept = 0ms; slept < interval && !st.stop_requested(); slept += 100ms) {
        std::this_thread::sleep_for(100ms);
      }
    };
    auto reset_connection_noexcept = []() noexcept {
      try {
        reset_helper_connection_serialized();
      } catch (...) {
      }
    };

    while (!st.stop_requested()) {
      try {
        if (!dd_feature_enabled()) {
          if (helper_ready) {
            reset_helper_connection_serialized();
            helper_ready = false;
          }
          sleep_interruptible(kActiveInterval);
          continue;
        }

        if (!helper_ready) {
          helper_ready = ensure_helper_started();
          if (!helper_ready) {
            sleep_interruptible(kActiveInterval);
            continue;
          }
          (void) platf::display_helper_client::send_ping();
        }

        const bool suspended =
          stream::session::running_sessions.load(std::memory_order_acquire) == 0 &&
          proc::proc.running() > 0;
        const auto interval = suspended ? kSuspendedInterval : kActiveInterval;
        sleep_interruptible(interval);
        if (st.stop_requested()) {
          break;
        }

        if (!platf::display_helper_client::send_ping()) {
          // Avoid logging ping failures to reduce log spam; proceed to reconnect
          reset_helper_connection_serialized();
          helper_ready = ensure_helper_started();
          if (!helper_ready) {
            continue;
          }
          // Do not re-apply automatically on reconnect; just confirm IPC is reachable.
          helper_ready = platf::display_helper_client::send_ping();
        }
      } catch (const std::exception &e) {
        BOOST_LOG(error) << "Display helper watchdog failed: " << e.what();
        reset_connection_noexcept();
        helper_ready = false;
        sleep_interruptible(kActiveInterval);
      } catch (...) {
        BOOST_LOG(error) << "Display helper watchdog failed with an unknown exception.";
        reset_connection_noexcept();
        helper_ready = false;
        sleep_interruptible(kActiveInterval);
      }
    }
  }

}  // namespace

namespace display_helper_integration {
  void start_display_cleanup_dispatcher() noexcept {
    auto &dispatcher = display_cleanup_dispatcher();
    std::lock_guard lock(dispatcher.mutex);
    (void) start_display_cleanup_dispatcher_locked(dispatcher);
  }

  void stop_display_cleanup_dispatcher() noexcept {
    auto &dispatcher = display_cleanup_dispatcher();
    bool should_join = false;
    {
      std::lock_guard lock(dispatcher.mutex);
      dispatcher.accepting = false;
      dispatcher.stopped = true;
      if (dispatcher.running) {
        dispatcher.pool.stop();
        dispatcher.running = false;
        should_join = true;
      }
    }
    if (should_join) {
      try {
        dispatcher.pool.join();
      } catch (const std::exception &e) {
        BOOST_LOG(error) << "Display cleanup dispatcher join failed: " << e.what();
      } catch (...) {
        BOOST_LOG(error) << "Display cleanup dispatcher join failed.";
      }
    }
  }

  bool enqueue_display_cleanup_task(std::function<void()> task) noexcept {
    try {
      if (!task) {
        return false;
      }
      return enqueue_display_cleanup_task_impl([task = guard_display_cleanup_task(std::move(task))](thread_pool_util::ThreadPool &pool) mutable {
        pool.push(std::move(task));
      });
    } catch (const std::exception &e) {
      BOOST_LOG(error) << "Unable to prepare display cleanup task: " << e.what();
    } catch (...) {
      BOOST_LOG(error) << "Unable to prepare display cleanup task.";
    }
    return false;
  }

  bool enqueue_delayed_display_cleanup_task(
    std::chrono::milliseconds delay,
    std::function<void()> task
  ) noexcept {
    try {
      if (!task) {
        return false;
      }
      return enqueue_display_cleanup_task_impl([delay,
                                                task = guard_display_cleanup_task(std::move(task))](thread_pool_util::ThreadPool &pool) mutable {
        (void) pool.pushDelayed(std::move(task), delay);
      });
    } catch (const std::exception &e) {
      BOOST_LOG(error) << "Unable to prepare delayed display cleanup task: " << e.what();
    } catch (...) {
      BOOST_LOG(error) << "Unable to prepare delayed display cleanup task.";
    }
    return false;
  }

  struct DisplayTeardownLease::Impl {
    explicit Impl(std::uint64_t generation_in, std::function<void()> on_release_in = {}):
        generation(generation_in),
        on_release(std::move(on_release_in)) {}

    ~Impl() {
      if (on_release) {
        dispatch_retained_display_teardown(std::move(on_release), generation);
        return;
      }
      release_display_teardown_lease(generation);
    }

    std::uint64_t generation;
    std::function<void()> on_release;
  };

  struct DisplayStartReservation::Impl {
    explicit Impl(std::uint64_t generation_in):
        generation(generation_in) {}

    ~Impl() {
      release(false);
    }

    void release(bool published_active) {
      bool should_release = false;
      {
        std::lock_guard lock(release_mutex);
        if (!released) {
          released = true;
          should_release = true;
        }
      }
      if (should_release) {
        release_display_start_reservation(generation, published_active);
      }
    }

    std::shared_ptr<DisplayTeardownLease> convert_to_teardown() {
      std::unique_lock release_lock(release_mutex);
      if (released) {
        return {};
      }

      auto &state = display_lifecycle_state();
      std::unique_lock state_lock(state.mutex);
      if (!state.start_in_progress ||
          state.start_generation != generation ||
          state.teardown_in_progress) {
        // Match the normal reservation release behavior if a competing path
        // already changed lifecycle state. Do the external release after
        // dropping our local lock.
        released = true;
        state_lock.unlock();
        release_lock.unlock();
        release_display_start_reservation(generation, false);
        return {};
      }

      const auto teardown_generation = state.next_generation + 1;
      // All fallible allocation/copying happens before publishing the state
      // transition. An exception therefore leaves the start reservation live
      // and retryable instead of wedging the lifecycle gate.
      auto deferred_teardown = state.deferred_teardown;
      auto teardown_impl = std::make_unique<DisplayTeardownLease::Impl>(
        teardown_generation,
        std::move(deferred_teardown)
      );
      auto teardown = std::shared_ptr<DisplayTeardownLease>(
        new DisplayTeardownLease(std::move(teardown_impl))
      );

      state.start_in_progress = false;
      state.start_generation = 0;
      state.teardown_in_progress = true;
      state.teardown_generation = teardown_generation;
      state.next_generation = teardown_generation;
      state.deferred_teardown = {};
      released = true;
      return teardown;
    }

    std::uint64_t generation;
    std::mutex release_mutex;
    bool released {false};
  };

  DisplayStartReservation::DisplayStartReservation(std::unique_ptr<Impl> impl):
      impl_(std::move(impl)) {}

  DisplayStartReservation::~DisplayStartReservation() = default;
  DisplayStartReservation::DisplayStartReservation(DisplayStartReservation &&) noexcept = default;
  DisplayStartReservation &DisplayStartReservation::operator=(DisplayStartReservation &&) noexcept = default;

  void DisplayStartReservation::publish_active() {
    if (impl_) {
      impl_->release(true);
    }
  }

  std::shared_ptr<DisplayTeardownLease> DisplayStartReservation::begin_abort_cleanup() noexcept {
    try {
      return impl_ ? impl_->convert_to_teardown() : std::shared_ptr<DisplayTeardownLease> {};
    } catch (const std::exception &e) {
      // Impl::convert_to_teardown deliberately leaves lifecycle state untouched
      // on allocation/copy failure. The reservation's later destruction will
      // still release its start gate; callers must not terminate while already
      // handling an aborted launch.
      BOOST_LOG(error) << "Display lifecycle: unable to prepare abort cleanup lease: " << e.what();
    } catch (...) {
      BOOST_LOG(error) << "Display lifecycle: unable to prepare abort cleanup lease.";
    }
    return {};
  }

  std::shared_ptr<DisplayStartReservation> acquire_display_start_reservation(
    std::chrono::milliseconds timeout
  ) {
    using namespace std::chrono;
    auto &state = display_lifecycle_state();
    const auto deadline = steady_clock::now() + std::max(timeout, milliseconds(0));

    std::unique_lock lock(state.mutex);
    ++state.start_waiters;
    state.cv.notify_all();
    const auto available = [&]() {
      return !state.start_in_progress && !state.teardown_in_progress;
    };
    if (!state.cv.wait_until(lock, deadline, available)) {
      --state.start_waiters;
      BOOST_LOG(warning) << "Display lifecycle: timed out waiting for another start/teardown transaction.";
      return {};
    }

    --state.start_waiters;
    const auto generation = state.next_generation + 1;
    try {
      // Allocate before publishing the state transition. If memory allocation
      // fails, availability remains true and another start can proceed.
      auto impl = std::make_unique<DisplayStartReservation::Impl>(generation);
      auto reservation = std::shared_ptr<DisplayStartReservation>(
        new DisplayStartReservation(std::move(impl))
      );
      state.start_in_progress = true;
      state.start_generation = generation;
      state.next_generation = generation;
      return reservation;
    } catch (...) {
      state.cv.notify_all();
      throw;
    }
  }

  bool display_start_waiting() {
    auto &state = display_lifecycle_state();
    std::lock_guard lock(state.mutex);
    return state.start_waiters != 0;
  }

  bool display_start_in_progress() {
    auto &state = display_lifecycle_state();
    std::lock_guard lock(state.mutex);
    return state.start_in_progress;
  }

  DisplayTeardownLease::DisplayTeardownLease(std::unique_ptr<Impl> impl):
      impl_(std::move(impl)) {}

  DisplayTeardownLease::~DisplayTeardownLease() = default;
  DisplayTeardownLease::DisplayTeardownLease(DisplayTeardownLease &&) noexcept = default;
  DisplayTeardownLease &DisplayTeardownLease::operator=(DisplayTeardownLease &&) noexcept = default;

  std::shared_ptr<DisplayTeardownLease> try_acquire_display_teardown(
    std::function<bool()> still_last_capture_user,
    std::function<void()> retry_if_start_abandoned
  ) {
    auto &state = display_lifecycle_state();
    std::lock_guard lock(state.mutex);
    if (state.teardown_in_progress) {
      append_deferred_teardown_locked(state, std::move(retry_if_start_abandoned));
      return {};
    }
    if (state.start_in_progress || state.start_waiters != 0) {
      append_deferred_teardown_locked(state, std::move(retry_if_start_abandoned));
      return {};
    }
    try {
      if (still_last_capture_user && !still_last_capture_user()) {
        return {};
      }
    } catch (...) {
      BOOST_LOG(warning) << "Display lifecycle: final-capture predicate threw; suppressing teardown.";
      return {};
    }

    const auto generation = state.next_generation + 1;
    // As with start reservations, construct all ownership before setting the
    // global in-progress bit so allocation failure cannot strand the gate.
    auto impl = std::make_unique<DisplayTeardownLease::Impl>(generation);
    auto lease = std::shared_ptr<DisplayTeardownLease>(
      new DisplayTeardownLease(std::move(impl))
    );
    state.teardown_in_progress = true;
    state.teardown_generation = generation;
    state.next_generation = generation;
    return lease;
  }

  std::optional<PlatformStreamingClaim> claim_platform_streaming_lifecycle(
    std::function<bool()> still_needed
  ) {
    std::lock_guard hook_lock(platform_streaming_hook_mutex());
    auto &state = display_lifecycle_state();
    std::lock_guard lock(state.mutex);
    try {
      if (still_needed && !still_needed()) {
        return std::nullopt;
      }
    } catch (...) {
      BOOST_LOG(warning) << "Display lifecycle: platform start predicate threw; suppressing stale start hook.";
      return std::nullopt;
    }
    if (state.platform_streaming_claimed) {
      return PlatformStreamingClaim {
        .generation = state.platform_streaming_generation,
        .start_required = !state.platform_streaming_started,
      };
    }
    state.platform_streaming_claimed = true;
    state.platform_streaming_started = false;
    state.platform_streaming_generation = ++state.next_generation;
    return PlatformStreamingClaim {
      .generation = state.platform_streaming_generation,
      .start_required = true,
    };
  }

  bool run_platform_streaming_start(
    std::uint64_t generation,
    std::function<void()> start_hook,
    std::function<bool()> still_needed,
    std::function<void()> rollback_hook
  ) {
    std::lock_guard hook_lock(platform_streaming_hook_mutex());
    auto &state = display_lifecycle_state();
    {
      std::lock_guard lock(state.mutex);
      if (!state.platform_streaming_claimed ||
          state.platform_streaming_generation != generation ||
          state.platform_streaming_started) {
        return false;
      }
    }
    try {
      if (still_needed && !still_needed()) {
        return false;
      }
    } catch (...) {
      BOOST_LOG(warning) << "Display lifecycle: platform start predicate threw; suppressing stale start hook.";
      return false;
    }
    try {
      if (start_hook) {
        start_hook();
      }
    } catch (...) {
      // The callback can consist of multiple non-idempotent platform steps.
      // Leave the lifecycle unstarted, but give it a precise rollback for any
      // step that completed before the exception.
      if (rollback_hook) {
        try {
          rollback_hook();
        } catch (const std::exception &e) {
          BOOST_LOG(error) << "Display lifecycle: platform start rollback failed: " << e.what();
        } catch (...) {
          BOOST_LOG(error) << "Display lifecycle: platform start rollback failed.";
        }
      }
      throw;
    }
    {
      std::lock_guard lock(state.mutex);
      // Final release is serialized by hook_lock and cannot revoke the claim
      // between the callback and this publication.
      if (state.platform_streaming_claimed &&
          state.platform_streaming_generation == generation) {
        state.platform_streaming_started = true;
        return true;
      }
    }
    return false;
  }

  bool run_platform_streaming_update(
    std::uint64_t generation,
    std::function<void()> update_hook
  ) {
    std::lock_guard hook_lock(platform_streaming_hook_mutex());
    auto &state = display_lifecycle_state();
    {
      std::lock_guard lock(state.mutex);
      if (!state.platform_streaming_claimed ||
          state.platform_streaming_generation != generation ||
          !state.platform_streaming_started) {
        return false;
      }
    }
    if (update_hook) {
      update_hook();
    }
    return true;
  }

  bool release_platform_streaming_lifecycle(
    std::uint64_t generation,
    std::function<void(bool platform_started)> stop_hook
  ) {
    std::lock_guard hook_lock(platform_streaming_hook_mutex());
    auto &state = display_lifecycle_state();
    bool should_stop = false;
    {
      std::lock_guard lock(state.mutex);
      if (!state.platform_streaming_claimed ||
          state.platform_streaming_generation != generation) {
        return false;
      }
      should_stop = state.platform_streaming_started;
      state.platform_streaming_claimed = false;
      state.platform_streaming_started = false;
      state.platform_streaming_generation = 0;
    }
    if (stop_hook) {
      stop_hook(should_stop);
    }
    return true;
  }

  std::optional<std::uint64_t> current_platform_streaming_generation() {
    std::lock_guard hook_lock(platform_streaming_hook_mutex());
    auto &state = display_lifecycle_state();
    std::lock_guard lock(state.mutex);
    if (!state.platform_streaming_claimed) {
      return std::nullopt;
    }
    return state.platform_streaming_generation;
  }

  struct DisplayHandoffLease::Impl {
    explicit Impl(std::unique_lock<std::recursive_timed_mutex> lock_in):
        lock(std::move(lock_in)) {}

    std::unique_lock<std::recursive_timed_mutex> lock;
  };

  DisplayHandoffLease::DisplayHandoffLease(std::unique_ptr<Impl> impl):
      impl_(std::move(impl)) {}

  DisplayHandoffLease::~DisplayHandoffLease() = default;
  DisplayHandoffLease::DisplayHandoffLease(DisplayHandoffLease &&) noexcept = default;
  DisplayHandoffLease &DisplayHandoffLease::operator=(DisplayHandoffLease &&) noexcept = default;

  std::shared_ptr<DisplayHandoffLease> acquire_safe_display_handoff(
    std::chrono::milliseconds timeout,
    std::function<bool()> still_allowed
  ) {
    using namespace std::chrono;
    const auto deadline = steady_clock::now() + std::max(timeout, milliseconds(0));

    std::unique_lock<std::recursive_timed_mutex> handoff_lock(g_restore_handoff_mutex, std::defer_lock);
    // Do not let a canceled recovery monitor consume its whole handoff budget
    // while another display transaction owns the mutex. The uncontended path
    // still acquires immediately; callers without a freshness predicate keep
    // the original one-shot timed-lock behavior.
    constexpr auto kSerializationPollInterval = milliseconds(50);
    bool serialized = false;
    if (!still_allowed) {
      serialized = handoff_lock.try_lock_until(deadline);
    } else {
      while (!serialized) {
        if (!still_allowed()) {
          BOOST_LOG(info) << "Display helper: handoff transaction canceled while waiting for serialization because it is stale.";
          return {};
        }

        const auto now = steady_clock::now();
        if (now >= deadline) {
          // Preserve the prior zero-timeout behavior: make one immediate lock
          // attempt before reporting that serialization timed out.
          serialized = handoff_lock.try_lock();
          break;
        }

        serialized = handoff_lock.try_lock_until(std::min(deadline, now + kSerializationPollInterval));
      }
    }
    if (!serialized) {
      BOOST_LOG(warning) << "Display helper: timed out waiting to serialize the restore handoff.";
      return {};
    }
    if (still_allowed && !still_allowed()) {
      BOOST_LOG(info) << "Display helper: handoff transaction canceled after serialization because it is stale.";
      return {};
    }

    do {
      if (still_allowed && !still_allowed()) {
        BOOST_LOG(info) << "Display helper: handoff transaction canceled while waiting because it became stale.";
        return {};
      }
      const auto disarm_result = disarm_helper_restore_if_running_locked();
      if (disarm_result == platf::display_helper_client::DisarmResult::Disarmed) {
        // Pair the ACK with a final generation read while retaining the lease.
        // A published REVERT cannot overtake the caller's first display action.
        if (g_restore_tracker.current() == 0) {
          if (still_allowed && !still_allowed()) {
            return {};
          }
          g_external_helper_recovery_requested.store(false, std::memory_order_release);
          auto impl = std::make_unique<DisplayHandoffLease::Impl>(std::move(handoff_lock));
          return std::shared_ptr<DisplayHandoffLease>(new DisplayHandoffLease(std::move(impl)));
        }
        continue;
      }
      if (disarm_result == platf::display_helper_client::DisarmResult::Unavailable &&
          helper_process_running()) {
        // An unresponsive helper may be serialized behind a blocking APPLY or
        // may predate the acknowledgement protocol. Treat it as unsafe until a
        // positive acknowledgement or process exit proves otherwise.
        (void) g_restore_tracker.discover_restore();
        if (helper_process_is_external() &&
            !g_external_helper_recovery_requested.exchange(true, std::memory_order_acq_rel)) {
          // An older helper cannot positively acknowledge DISARM. Let its own
          // strict restore engine finish and exit before this process upgrades
          // it; never terminate it in a blocking display call.
          BOOST_LOG(warning) << "Display helper: requesting strict restore from external helper before upgrade.";
          if (still_allowed && !still_allowed()) {
            g_external_helper_recovery_requested.store(false, std::memory_order_release);
            return {};
          }
          const auto remaining = duration_cast<milliseconds>(deadline - steady_clock::now());
          const bool sent = remaining > milliseconds(0) &&
                            platf::display_helper_client::send_revert_fast(
                              {},
                              static_cast<int>(remaining.count())
                            );
          if (!sent) {
            g_external_helper_recovery_requested.store(false, std::memory_order_release);
          }
        }
      }
      if (!restore_expected_with_live_helper()) {
        if (still_allowed && !still_allowed()) {
          return {};
        }
        g_external_helper_recovery_requested.store(false, std::memory_order_release);
        auto impl = std::make_unique<DisplayHandoffLease::Impl>(std::move(handoff_lock));
        return std::shared_ptr<DisplayHandoffLease>(new DisplayHandoffLease(std::move(impl)));
      }
      if (steady_clock::now() >= deadline) {
        break;
      }
      std::this_thread::sleep_for(milliseconds(100));
    } while (true);

    g_external_helper_recovery_requested.store(false, std::memory_order_release);
    BOOST_LOG(warning) << "Display helper: restore handoff remained unsafe for "
                       << timeout.count() << "ms; suppressing stream-start display changes.";
    return {};
  }

  namespace {
    bool apply_internal(
      const DisplayApplyRequest &request,
      bool allow_resolution_deferral,
      std::optional<std::uint64_t> *verification_token = nullptr,
      bool *handoff_safe = nullptr
    ) {
      // All host-side APPLY paths (including deferred/hot recovery) participate
      // in the same transaction order as stream start, cleanup, and REVERT.
      // Start/recovery callers may already own the recursive lease.
      std::lock_guard<std::recursive_timed_mutex> transaction_lock(g_restore_handoff_mutex);
      if (verification_token) {
        verification_token->reset();
      }
      if (handoff_safe) {
        *handoff_safe = true;
      }
      if (request.action == DisplayApplyAction::Skip) {
        BOOST_LOG(info) << "Display helper: configuration parse failed; not dispatching.";
        return false;
      }

      if (request.action == DisplayApplyAction::Preserve) {
        BOOST_LOG(info) << "Display helper: display configuration is disabled; preserving current state.";
        if (request.session) {
          // Preserve means "no topology Apply", not "no active display
          // session". Recording a per-session VD keeps helper/watchdog policy
          // alive when the global DD and VD settings are disabled by overrides.
          set_active_session(
            *request.session,
            request.session_overrides.device_id_override,
            request.session_overrides.fps_override,
            request.session_overrides.width_override,
            request.session_overrides.height_override,
            request.session_overrides.virtual_display_override,
            request.session_overrides.framegen_refresh_override
          );
          if (request.enable_virtual_display_watchdog) {
            platf::display_helper::Coordinator::instance().set_virtual_display_watchdog_enabled(true);
          }
        }
        return true;
      }

      if (request.action == DisplayApplyAction::Revert) {
        // REVERT has its own generation-tracked API and must never be smuggled
        // through an Apply request (which may already hold a handoff lease).
        BOOST_LOG(error) << "Display helper: refusing untracked REVERT through apply(); use revert() instead.";
        return false;
      }

      if (request.action != DisplayApplyAction::Apply) {
        return false;
      }

      // Prefer the helper for APPLY, even when running as SYSTEM without an interactive user session.
      // In-process display APIs frequently return ERROR_ACCESS_DENIED in that context.
      const bool system_no_user_session = platf::is_running_as_system() && !user_session_ready();
      if (system_no_user_session) {
        BOOST_LOG(debug) << "Display helper: SYSTEM context without user session; preferring helper dispatch.";
      }

      // Stream-start policy: probe the existing helper with a correlated
      // DISARM before deciding whether it can be reused. An acknowledged helper
      // is both faster and safer to reuse; an unconfirmed restore is preserved
      // because killing it could strand a partially restored topology.
      // A helper can autonomously arm its disconnect restore without a host
      // generation (for example after a prior process or pipe died). Fence that
      // state before force-restart decisions on every Apply path, including
      // deferred/background applies that do not already own a start lease.
      const bool helper_was_running = helper_process_running();
      const auto pre_apply_disarm = disarm_helper_restore_if_running_locked();
      if (pre_apply_disarm != platf::display_helper_client::DisarmResult::Disarmed &&
          helper_process_running()) {
        (void) g_restore_tracker.discover_restore();
      }
      const bool restore_expected = restore_expected_with_live_helper();
      const auto restore_generation = restore_expected ?
                                        g_restore_tracker.current() :
                                        0;
      // A correlated DISARM ACK is a positive drained/live barrier. Reuse that
      // helper instead of killing and relaunching the instance that just took
      // the start snapshot. This keeps the normal start path fast; restart is
      // reserved for a live helper that could not provide the barrier.
      const bool reusable_helper_barrier =
        helper_was_running &&
        pre_apply_disarm == platf::display_helper_client::DisarmResult::Disarmed;
      const bool hard_restart =
        (request.session != nullptr) && !restore_expected && helper_was_running && !reusable_helper_barrier;
      if (request.session && restore_expected) {
        BOOST_LOG(info) << "Display helper: reusing existing helper because an unconfirmed restore is pending; APPLY will supersede it.";
      }

      bool helper_ready = ensure_helper_started(hard_restart, true);
      if (!helper_ready && hard_restart) {
        BOOST_LOG(warning) << "Display helper: hard restart path unavailable; retrying helper start without restart.";
        helper_ready = ensure_helper_started(false, true);
      }
      if (!helper_ready) {
        helper_ready = ensure_helper_started(hard_restart, true);
      }

      if (helper_ready) {
        auto payload = build_helper_apply_payload(request);
        if (!payload) {
          BOOST_LOG(error) << "Display helper: failed to build APPLY payload for helper dispatch.";
          return false;
        }

        BOOST_LOG(info) << "Display helper: sending APPLY request via helper.";
        // A restore that is already inside SetDisplayConfig cannot stop until the
        // Windows call returns. Give the serialized APPLY enough time to run after
        // that handoff without penalizing the normal fast path.
        platf::display_helper_client::ApplyResult ipc_result;
        if (restore_expected) {
          ipc_result = platf::display_helper_client::send_apply_json(*payload, kRestoreHandoffApplyResultTimeoutMs);
        } else {
          ipc_result = platf::display_helper_client::send_apply_json(*payload);
        }
        const bool ok = ipc_result.succeeded;
        if (!ok) {
          // Even an acknowledged Expired/Invalid result can be emitted before
          // an older restore or post-Apply worker has drained. Force every
          // failed Apply through the positive DISARM barrier before the caller
          // probes/captures; success is the only cross-engine drain contract.
          (void) g_restore_tracker.discover_restore();
          if (handoff_safe) {
            *handoff_safe = false;
          }
        }
        BOOST_LOG(info) << "Display helper: APPLY dispatch result=" << (ok ? "true" : "false");
        if (ok && verification_token && !use_legacy_helper_engine()) {
          *verification_token = ipc_result.request_id;
        }
        if (ok && request.session) {
          if (restore_generation != 0) {
            (void) g_restore_tracker.clear_if(restore_generation);
          }
          g_last_apply_completed_us.store(now_steady_us(), std::memory_order_relaxed);
          set_active_session(
            *request.session,
            request.session_overrides.device_id_override,
            request.session_overrides.fps_override,
            request.session_overrides.width_override,
            request.session_overrides.height_override,
            request.session_overrides.virtual_display_override,
            request.session_overrides.framegen_refresh_override
          );
          if (request.enable_virtual_display_watchdog) {
            platf::display_helper::Coordinator::instance().set_virtual_display_watchdog_enabled(true);
          }
        }
        if (!ok && allow_resolution_deferral && request.session && platf::is_lock_screen_active()) {
          BOOST_LOG(info) << "Display helper: APPLY failed during lock screen; queuing deferred apply for retry after unlock.";
          queue_deferred_resolution_apply(request);
        }
        return ok;
      }

      if (system_no_user_session) {
        BOOST_LOG(warning) << "Display helper: helper unavailable in SYSTEM context without user session; skipping in-process APPLY fallback.";
        maybe_queue_deferred_resolution_apply(request, allow_resolution_deferral);
        return false;
      }

      BOOST_LOG(warning) << "Display helper: helper unavailable; falling back to in-process APPLY.";

      if (!request.session) {
        BOOST_LOG(error) << "Display helper: missing session context for in-process APPLY.";
        return false;
      }

      const auto apply_result = apply_in_process(request);
      if (apply_result != display_device::SettingsManagerInterface::ApplyResult::Ok) {
        if (apply_result == display_device::SettingsManagerInterface::ApplyResult::ApiTemporarilyUnavailable) {
          maybe_queue_deferred_resolution_apply_on_api_unavailable(request);
        }
        BOOST_LOG(warning) << "Display helper: in-process APPLY failed.";
        return false;
      }

      const auto device_id = request.configuration ? request.configuration->m_device_id : std::string {};
      if (!verify_helper_topology(*request.session, device_id)) {
        BOOST_LOG(warning) << "Display helper: topology verification failed after in-process APPLY.";
      }
      (void) apply_topology_definition(request.topology, "in-process");

      g_last_apply_completed_us.store(now_steady_us(), std::memory_order_relaxed);
      set_active_session(
        *request.session,
        request.session_overrides.device_id_override,
        request.session_overrides.fps_override,
        request.session_overrides.width_override,
        request.session_overrides.height_override,
        request.session_overrides.virtual_display_override,
        request.session_overrides.framegen_refresh_override
      );
      if (request.enable_virtual_display_watchdog) {
        platf::display_helper::Coordinator::instance().set_virtual_display_watchdog_enabled(true);
      }
      maybe_queue_deferred_resolution_apply(request, allow_resolution_deferral);
      return true;
    }
  }  // namespace

  ApplyVerificationStatus wait_for_apply_verification(
    std::uint64_t verification_token,
    std::chrono::milliseconds timeout
  ) {
    if (verification_token == 0) {
      return ApplyVerificationStatus::Unknown;
    }

    const int timeout_ms = static_cast<int>(std::max<long long>(timeout.count(), 0LL));
    const auto result = platf::display_helper_client::wait_for_verification_result(verification_token, timeout_ms);
    if (!result.has_value()) {
      BOOST_LOG(warning) << "Display helper: verification result unavailable; proceeding with stream.";
      return ApplyVerificationStatus::Unknown;
    }

    return *result ? ApplyVerificationStatus::Verified : ApplyVerificationStatus::Failed;
  }

  ApplyDispatchResult apply_with_verification(const DisplayApplyRequest &request) {
    // A new direct Apply supersedes any lock-screen/API retry retained for an
    // older session. Deferred retries call apply_internal directly and keep
    // their captured epoch.
    clear_pending_apply();
    // Remember the session's virtual display before the APPLY payload is built so the
    // helper can exclude it from the pre-apply baseline it may capture.
    if (request.session) {
      const auto &vd_id = request.session_overrides.device_id_override ?
                            *request.session_overrides.device_id_override :
                            request.session->virtual_display_device_id;
      if (!vd_id.empty()) {
        statefile::remember_virtual_display_device(vd_id);
      }
    }
    ApplyDispatchResult result;
    result.accepted = apply_internal(
      request,
      true,
      &result.verification_token,
      &result.handoff_safe
    );
    return result;
  }

  bool apply(const DisplayApplyRequest &request) {
    return apply_with_verification(request).accepted;
  }

  bool revert(bool prefer_golden_if_current_missing) {
    // Serialize helper inspection/start as well as publication/send. Otherwise
    // a new Sunshine process could kill an external restore helper before the
    // transaction lock was acquired.
    std::lock_guard<std::recursive_timed_mutex> handoff_lock(g_restore_handoff_mutex);
    // Queue creation happens under this same transaction. Clearing here makes
    // it impossible for a deferred Apply from the ended session to appear in
    // the pre-lock gap and run after REVERT.
    clear_pending_apply();
    if (!ensure_helper_started()) {
      BOOST_LOG(info) << "Display helper unavailable; cannot send revert.";
      return false;
    }
    BOOST_LOG(info) << "Display helper: sending REVERT request"
                    << (prefer_golden_if_current_missing ? " (prefer golden if current missing)." : ".");
    // Publish before the frame can be observed by the helper. A concurrent
    // stream-start barrier must never declare safety in the send->publish gap.
    const auto restore_generation = g_restore_tracker.begin_restore();
    const bool ok = platf::display_helper_client::send_revert(build_revert_payload(prefer_golden_if_current_missing));
    BOOST_LOG(info) << "Display helper: REVERT dispatch result=" << (ok ? "true" : "false");
    if (!ok) {
      // A failed/timeout send is ambiguous: the helper may already have the
      // frame. Retain this generation until process exit or an acknowledged
      // handoff proves that no restore can still mutate the topology.
      BOOST_LOG(warning) << "Display helper: preserving REVERT generation "
                         << restore_generation << " after unacknowledged dispatch.";
    }
    clear_active_session();
    return ok;
  }

  bool disarm_pending_restore() {
    const auto result = disarm_helper_restore_if_running();
    if (result == platf::display_helper_client::DisarmResult::Unavailable && helper_process_running()) {
      (void) g_restore_tracker.discover_restore();
    }
    return result == platf::display_helper_client::DisarmResult::Disarmed;
  }

  bool wait_for_safe_display_handoff(std::chrono::milliseconds timeout) {
    return static_cast<bool>(acquire_safe_display_handoff(timeout));
  }

  bool helper_or_restore_active() {
    std::lock_guard<std::recursive_timed_mutex> handoff_lock(g_restore_handoff_mutex);
    return g_restore_tracker.current() != 0 || helper_process_running();
  }

  bool export_golden_restore() {
    std::lock_guard<std::recursive_timed_mutex> handoff_lock(g_restore_handoff_mutex);
    if (!ensure_helper_started()) {
      BOOST_LOG(info) << "Display helper unavailable; cannot export golden snapshot.";
      return false;
    }
    BOOST_LOG(info) << "Display helper: sending EXPORT_GOLDEN request.";
    const bool ok = platf::display_helper_client::send_export_golden(build_snapshot_exclude_payload());
    BOOST_LOG(info) << "Display helper: EXPORT_GOLDEN dispatch result=" << (ok ? "true" : "false");
    return ok;
  }

  bool reset_persistence() {
    std::lock_guard<std::recursive_timed_mutex> handoff_lock(g_restore_handoff_mutex);
    if (!ensure_helper_started()) {
      BOOST_LOG(info) << "Display helper unavailable; cannot reset persistence.";
      return false;
    }
    BOOST_LOG(info) << "Display helper: sending RESET request.";
    const bool ok = platf::display_helper_client::send_reset();
    BOOST_LOG(info) << "Display helper: RESET dispatch result=" << (ok ? "true" : "false");
    return ok;
  }

  bool snapshot_current_display_state() {
    std::lock_guard<std::recursive_timed_mutex> handoff_lock(g_restore_handoff_mutex);
    if (restore_expected_with_live_helper()) {
      BOOST_LOG(info) << "Display helper: skipping SNAPSHOT_CURRENT while an unconfirmed restore is pending.";
      return false;
    }

    if (!ensure_helper_started()) {
      BOOST_LOG(info) << "Display helper unavailable; cannot snapshot current display state.";
      return false;
    }
    BOOST_LOG(info) << "Display helper: sending SNAPSHOT_CURRENT request.";
    const bool ok = platf::display_helper_client::send_snapshot_current(build_snapshot_exclude_payload());
    BOOST_LOG(info) << "Display helper: SNAPSHOT_CURRENT dispatch result=" << (ok ? "true" : "false");
    return ok;
  }

  bool apply_pending_if_ready() {
    {
      std::lock_guard<std::mutex> lock(pending_apply_mutex());
      if (!pending_apply_state()) {
        return false;
      }
    }

    if (platf::is_running_as_system() && !user_session_ready()) {
      return false;
    }

    const auto now = std::chrono::steady_clock::now();
    PendingApplyState pending;
    {
      std::lock_guard<std::mutex> lock(pending_apply_mutex());
      if (!pending_apply_state()) {
        return false;
      }
      auto &state = *pending_apply_state();
      if (!state.ready_since) {
        state.ready_since = now;
        state.next_attempt = now + kDeferredApplyInitialDelay;
        BOOST_LOG(info) << "Display helper: user session detected; delaying deferred APPLY for "
                        << kDeferredApplyInitialDelay.count() << "ms.";
        return false;
      }
      if (now < state.next_attempt) {
        return false;
      }
      if (state.attempts >= kMaxDeferredApplyAttempts) {
        BOOST_LOG(warning) << "Display helper: deferred APPLY exceeded retry limit; giving up on session "
                           << state.session_id << ".";
        pending_apply_state().reset();
        return false;
      }
      pending = state;
    }

    std::optional<rtsp_stream::launch_session_t> session;
    if (pending.has_session) {
      rtsp_stream::launch_session_t snapshot {};
      snapshot.width = pending.session_snapshot.width;
      snapshot.height = pending.session_snapshot.height;
      snapshot.fps = pending.session_snapshot.fps;
      snapshot.enable_hdr = pending.session_snapshot.enable_hdr;
      snapshot.enable_sops = pending.session_snapshot.enable_sops;
      snapshot.virtual_display = pending.session_snapshot.virtual_display;
      snapshot.virtual_display_device_id = pending.session_snapshot.virtual_display_device_id;
      snapshot.virtual_display_ready_since = pending.session_snapshot.virtual_display_ready_since;
      snapshot.framegen_refresh_rate = pending.session_snapshot.framegen_refresh_rate;
      snapshot.framegen_refresh_multiplier = pending.session_snapshot.framegen_refresh_multiplier;
      snapshot.gen1_framegen_fix = pending.session_snapshot.gen1_framegen_fix;
      snapshot.gen2_framegen_fix = pending.session_snapshot.gen2_framegen_fix;
      session = std::move(snapshot);
      pending.request.session = &*session;
    } else {
      pending.request.session = nullptr;
    }

    // Linearize the popped request with REVERT/clear and revalidate the source
    // session after the potentially long readiness delay. apply_internal takes
    // the same recursive transaction, so no ended-session retry can overtake a
    // restore after this point.
    std::lock_guard<std::recursive_timed_mutex> handoff_lock(g_restore_handoff_mutex);
    {
      std::lock_guard<std::mutex> lock(pending_apply_mutex());
      if (!pending_apply_state() || pending_apply_state()->epoch != pending.epoch ||
          pending_apply_epoch().load(std::memory_order_acquire) != pending.epoch) {
        BOOST_LOG(info) << "Display helper: dropping stale deferred APPLY before dispatch.";
        return false;
      }
      pending_apply_state().reset();
    }
    if (pending.has_session &&
        stream::session::running_sessions.load(std::memory_order_acquire) == 0 &&
        !webrtc_stream::has_active_sessions()) {
      BOOST_LOG(info) << "Display helper: dropping deferred APPLY because its stream session ended.";
      return false;
    }

    BOOST_LOG(info) << "Display helper: applying deferred configuration for session " << pending.session_id << ".";
    const bool ok = apply_internal(pending.request, false);
    if (!ok) {
      pending.attempts += 1;
      pending.request.session = nullptr;
      const auto delay = deferred_apply_retry_delay(pending.attempts);
      pending.next_attempt = std::chrono::steady_clock::now() + delay;
      std::lock_guard<std::mutex> lock(pending_apply_mutex());
      if (!pending_apply_state() &&
          pending_apply_epoch().load(std::memory_order_acquire) == pending.epoch) {
        pending_apply_state() = pending;
        BOOST_LOG(warning) << "Display helper: deferred APPLY failed; retrying in "
                           << delay.count() << "ms (attempt " << pending.attempts
                           << "/" << kMaxDeferredApplyAttempts << ").";
      } else {
        BOOST_LOG(info) << "Display helper: deferred APPLY failed but a newer pending configuration is queued; dropping retry.";
      }
    }
    return ok;
  }

  bool has_pending_apply() {
    std::lock_guard<std::mutex> lock(pending_apply_mutex());
    return pending_apply_state().has_value();
  }

  void clear_pending_apply() {
    std::lock_guard<std::mutex> lock(pending_apply_mutex());
    (void) pending_apply_epoch().fetch_add(1, std::memory_order_acq_rel);
    pending_apply_state().reset();
  }

  int64_t ms_since_last_apply() {
    const auto last_us = g_last_apply_completed_us.load(std::memory_order_relaxed);
    if (last_us == 0) {
      return std::numeric_limits<int64_t>::max();
    }
    const auto elapsed_us = now_steady_us() - last_us;
    return elapsed_us / 1000;
  }

  namespace {
    constexpr double kEdidRefreshToleranceHz = 0.5;

    struct ParsedEdidRefreshInfo {
      bool present {false};
      std::optional<int> max_vertical_hz;
      double max_timing_hz {0.0};
    };

    void consider_timing(double hz, ParsedEdidRefreshInfo &out) {
      if (!std::isfinite(hz) || hz <= 0.0) {
        return;
      }
      if (hz > out.max_timing_hz) {
        out.max_timing_hz = hz;
      }
    }

    void parse_detailed_descriptor(const uint8_t *descriptor, ParsedEdidRefreshInfo &out) {
      if (!descriptor) {
        return;
      }

      const uint16_t pixel_clock = static_cast<uint16_t>(descriptor[0] | (static_cast<uint16_t>(descriptor[1]) << 8));
      if (pixel_clock == 0) {
        if (descriptor[3] == 0xFD) {
          const int max_vertical = static_cast<int>(descriptor[6]);
          if (max_vertical > 0 && max_vertical < 2000) {
            if (!out.max_vertical_hz || max_vertical > *out.max_vertical_hz) {
              out.max_vertical_hz = max_vertical;
            }
          }
        }
        return;
      }

      const uint16_t h_active = static_cast<uint16_t>(descriptor[2] | (static_cast<uint16_t>(descriptor[4] & 0xF0) << 4));
      const uint16_t h_blanking = static_cast<uint16_t>(descriptor[3] | (static_cast<uint16_t>(descriptor[4] & 0x0F) << 8));
      const uint16_t v_active = static_cast<uint16_t>(descriptor[5] | (static_cast<uint16_t>(descriptor[7] & 0xF0) << 4));
      const uint16_t v_blanking = static_cast<uint16_t>(descriptor[6] | (static_cast<uint16_t>(descriptor[7] & 0x0F) << 8));
      const uint32_t h_total = static_cast<uint32_t>(h_active) + static_cast<uint32_t>(h_blanking);
      const uint32_t v_total = static_cast<uint32_t>(v_active) + static_cast<uint32_t>(v_blanking);
      if (h_total == 0 || v_total == 0) {
        return;
      }

      const double pixel_clock_hz = static_cast<double>(pixel_clock) * 10000.0;
      double refresh_hz = pixel_clock_hz / (static_cast<double>(h_total) * static_cast<double>(v_total));
      if ((descriptor[17] & 0x80) != 0) {
        refresh_hz *= 2.0;
      }

      consider_timing(refresh_hz, out);
    }

    ParsedEdidRefreshInfo parse_edid_refresh(const std::vector<std::byte> &edid) {
      ParsedEdidRefreshInfo info;
      if (edid.empty()) {
        return info;
      }
      info.present = true;
      if (edid.size() < 128) {
        return info;
      }

      const auto *bytes = reinterpret_cast<const uint8_t *>(edid.data());
      const auto parse_block_descriptors = [&](const uint8_t *block, std::size_t start, std::size_t end) {
        if (!block || start >= end) {
          return;
        }
        for (std::size_t offset = start; offset + 17 < end; offset += 18) {
          parse_detailed_descriptor(block + offset, info);
        }
      };

      parse_block_descriptors(bytes, 54, 126);

      const std::size_t block_count = edid.size() / 128;
      const uint8_t extension_count = bytes[126];
      const std::size_t max_extensions = std::min<std::size_t>(extension_count, block_count > 0 ? block_count - 1 : 0);
      for (std::size_t idx = 0; idx < max_extensions; ++idx) {
        const std::size_t block_start = (idx + 1) * 128;
        if (block_start + 128 > edid.size()) {
          break;
        }
        const auto *ext = bytes + block_start;
        if (ext[0] == 0x02) {
          const uint8_t dtd_offset = ext[2];
          if (dtd_offset >= 4 && dtd_offset < 127) {
            const std::size_t start = block_start + dtd_offset;
            const std::size_t end = block_start + 127;
            for (std::size_t offset = start; offset + 17 < end; offset += 18) {
              parse_detailed_descriptor(bytes + offset, info);
            }
          }
        }
      }

      return info;
    }

    std::vector<std::byte> read_edid_for_device_id(const std::string &device_id) {
      if (device_id.empty()) {
        return {};
      }
      try {
        display_device::DisplayRecoveryBehaviorGuard guard(display_device::DisplayRecoveryBehavior::Skip);
        auto api = std::make_shared<display_device::WinApiLayer>();
        auto display_data = api->queryDisplayConfig(display_device::QueryType::All);
        if (!display_data) {
          return {};
        }

        auto source_data = display_device::win_utils::collectSourceDataForMatchingPaths(*api, display_data->m_paths);
        auto it = source_data.find(device_id);
        if (it == source_data.end()) {
          for (const auto &entry : source_data) {
            if (boost::iequals(entry.first, device_id)) {
              it = source_data.find(entry.first);
              break;
            }
          }
        }

        if (it == source_data.end() || it->second.m_source_id_to_path_index.empty()) {
          return {};
        }

        const UINT32 source_id = it->second.m_active_source.value_or(it->second.m_source_id_to_path_index.begin()->first);
        const auto path_it = it->second.m_source_id_to_path_index.find(source_id);
        if (path_it == it->second.m_source_id_to_path_index.end()) {
          return {};
        }

        const std::size_t path_index = path_it->second;
        if (path_index >= display_data->m_paths.size()) {
          return {};
        }

        const auto &path = display_data->m_paths[path_index];
        return api->getEdid(path);
      } catch (const std::exception &ex) {
        BOOST_LOG(warning) << "Display helper: failed to read EDID for device " << device_id << ": " << ex.what();
      } catch (...) {
        BOOST_LOG(warning) << "Display helper: failed to read EDID for device " << device_id << " due to unknown error.";
      }

      return {};
    }

    std::optional<display_device::EnumeratedDevice> find_device_for_hint(const std::string &hint) {
      if (hint.empty()) {
        return std::nullopt;
      }

      auto devices = enumerate_devices(display_device::DeviceEnumerationDetail::Minimal);
      if (!devices) {
        return std::nullopt;
      }

      for (const auto &device : *devices) {
        if (device_id_equals_ci(device.m_device_id, hint) || device_id_equals_ci(device.m_display_name, hint) ||
            device_id_equals_ci(device.m_friendly_name, hint)) {
          return device;
        }
      }

      return std::nullopt;
    }
  }  // namespace

  std::optional<FramegenEdidSupportResult> framegen_edid_refresh_support(
    const std::string &device_hint,
    const std::vector<int> &targets_hz
  ) {
    const auto resolved_device = find_device_for_hint(device_hint);
    if (!resolved_device) {
      return std::nullopt;
    }

    FramegenEdidSupportResult result;
    result.device_id = resolved_device->m_device_id;
    if (!resolved_device->m_friendly_name.empty()) {
      result.device_label = resolved_device->m_friendly_name;
    } else if (!resolved_device->m_display_name.empty()) {
      result.device_label = resolved_device->m_display_name;
    } else {
      result.device_label = resolved_device->m_device_id;
    }

    const auto edid_bytes = read_edid_for_device_id(result.device_id);
    const auto parsed = parse_edid_refresh(edid_bytes);
    result.edid_present = parsed.present;
    if (parsed.max_vertical_hz) {
      result.max_vertical_hz = parsed.max_vertical_hz;
    }
    if (parsed.max_timing_hz > 0.0) {
      result.max_timing_hz = parsed.max_timing_hz;
    }

    for (int hz : targets_hz) {
      FramegenEdidTargetSupport target {};
      target.hz = hz;
      if (!parsed.present || edid_bytes.empty()) {
        target.supported = std::nullopt;
        target.method = "unknown";
      } else if (parsed.max_vertical_hz && static_cast<double>(*parsed.max_vertical_hz) + kEdidRefreshToleranceHz >= static_cast<double>(hz)) {
        target.supported = true;
        target.method = "range";
      } else if (parsed.max_timing_hz > 0.0 && parsed.max_timing_hz + kEdidRefreshToleranceHz >= static_cast<double>(hz)) {
        target.supported = true;
        target.method = "timing";
      } else if (parsed.max_vertical_hz) {
        target.supported = false;
        target.method = "range";
      } else if (parsed.max_timing_hz > 0.0) {
        target.supported = false;
        target.method = "timing";
      } else {
        target.supported = std::nullopt;
        target.method = "unknown";
      }
      result.targets.push_back(std::move(target));
    }

    return result;
  }

  std::optional<display_device::EnumeratedDeviceList> enumerate_devices(
    display_device::DeviceEnumerationDetail detail
  ) {
    try {
      display_device::DisplayRecoveryBehaviorGuard guard(display_device::DisplayRecoveryBehavior::Skip);
      auto api = std::make_shared<display_device::WinApiLayer>();
      display_device::WinDisplayDevice dd(api);
      return dd.enumAvailableDevices(detail);
    } catch (...) {
      return std::nullopt;
    }
  }

  std::optional<std::vector<std::vector<std::string>>> capture_current_topology() {
    try {
      display_device::DisplayRecoveryBehaviorGuard guard(display_device::DisplayRecoveryBehavior::Skip);
      auto api = std::make_shared<display_device::WinApiLayer>();
      display_device::WinDisplayDevice dd(api);
      return dd.getCurrentTopology();
    } catch (...) {
      return std::nullopt;
    }
  }

  std::string enumerate_devices_json(display_device::DeviceEnumerationDetail detail) {
    auto devices = enumerate_devices(detail);
    if (!devices) {
      return "[]";
    }
    if (detail == display_device::DeviceEnumerationDetail::Minimal) {
      devices->erase(
        std::remove_if(
          devices->begin(),
          devices->end(),
          [](const display_device::EnumeratedDevice &device) {
            return !device.m_info.has_value();
          }
        ),
        devices->end()
      );
    }
    return display_device::toJson(*devices);
  }

  void start_watchdog() {
    std::scoped_lock lk(g_watchdog_mutex);
    if (g_watchdog_running) {
      return;  // already running
    }
    g_watchdog_running = true;
    g_watchdog_thread = std::jthread(watchdog_proc);
  }

  void stop_watchdog() {
    std::jthread thread;
    {
      std::scoped_lock lk(g_watchdog_mutex);
      if (!g_watchdog_running) {
        return;  // not running
      }
      g_watchdog_running = false;
      thread = std::move(g_watchdog_thread);
    }
    if (thread.joinable()) {
      thread.request_stop();
      try {
        thread.join();
      } catch (const std::system_error &e) {
        BOOST_LOG(warning) << "Display helper: failed to join watchdog thread: " << e.what();
      }
    }
    if (config::video.dd.config_revert_on_disconnect) {
      reset_helper_connection_serialized();
    }
    clear_active_session();
  }
}  // namespace display_helper_integration

#endif

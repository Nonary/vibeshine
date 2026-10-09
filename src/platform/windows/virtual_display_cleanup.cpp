#include "virtual_display_cleanup.h"

#ifdef _WIN32

  #include "display_helper_integration.h"
  #include "src/logging.h"
  #include "src/platform/windows/impersonating_display_device.h"
  #include "src/platform/windows/physical_display_recovery.h"
  #include "src/platform/windows/virtual_display.h"
  #include "src/process.h"
  #include "src/remote_display_topology.h"

  #include <algorithm>
  #include <array>
  #include <atomic>
  #include <chrono>
  #include <cstring>
  #include <display_device/windows/win_api_layer.h>
  #include <display_device/windows/win_api_utils.h>
  #include <display_device/windows/win_display_device.h>
  #include <exception>
  #include <memory>
  #include <mutex>
  #include <set>
  #include <string>
  #include <thread>

namespace platf::virtual_display_cleanup {
  namespace {
    std::atomic_uint g_cleanup_reservations {0};
    std::mutex g_terminal_cleanup_mutex;

    class cleanup_reservation_t {
    public:
      cleanup_reservation_t() {
        g_cleanup_reservations.fetch_add(1, std::memory_order_acq_rel);
      }

      ~cleanup_reservation_t() {
        g_cleanup_reservations.fetch_sub(1, std::memory_order_acq_rel);
      }

      cleanup_reservation_t(const cleanup_reservation_t &) = delete;
      cleanup_reservation_t &operator=(const cleanup_reservation_t &) = delete;
    };

    bool has_active_virtual_display() {
      const auto virtual_displays = VDISPLAY::enumerateVirtualDisplays();
      return std::any_of(
        virtual_displays.begin(),
        virtual_displays.end(),
        [](const VDISPLAY::VirtualDisplayInfo &info) {
          return info.is_active;
        }
      );
    }

    std::size_t active_virtual_display_count() {
      const auto virtual_displays = VDISPLAY::enumerateVirtualDisplays();
      return static_cast<std::size_t>(std::count_if(
        virtual_displays.begin(),
        virtual_displays.end(),
        [](const VDISPLAY::VirtualDisplayInfo &info) {
          return info.is_active;
        }
      ));
    }

    bool wait_for_virtual_display_teardown(std::chrono::steady_clock::duration timeout) {
      constexpr auto kPollInterval = std::chrono::milliseconds(100);

      const auto deadline = std::chrono::steady_clock::now() + timeout;
      while (true) {
        const auto remaining = active_virtual_display_count();
        if (remaining == 0) {
          return true;
        }

        if (std::chrono::steady_clock::now() >= deadline) {
          BOOST_LOG(warning) << "Virtual display cleanup: teardown wait expired with "
                             << remaining << " virtual display(s) still enumerated.";
          return false;
        }

        std::this_thread::sleep_for(kPollInterval);
      }
    }

    bool restore_windows_display_database() {
      try {
        auto api = std::make_shared<display_device::WinApiLayer>();
        auto win_dd = std::make_shared<display_device::WinDisplayDevice>(api);
        auto impersonating_dd = std::make_shared<display_device::ImpersonatingDisplayDevice>(win_dd);
        return impersonating_dd->restoreMonitorSettings();
      } catch (const std::exception &e) {
        BOOST_LOG(warning) << "Virtual display cleanup: direct database restore threw exception: " << e.what();
      } catch (...) {
        BOOST_LOG(warning) << "Virtual display cleanup: direct database restore threw unknown exception.";
      }
      return false;
    }

    bool guid_bytes_are_empty(const std::array<std::uint8_t, 16> &guid_bytes) {
      return std::all_of(guid_bytes.begin(), guid_bytes.end(), [](std::uint8_t byte) {
        return byte == 0;
      });
    }

    bool remove_specific_virtual_display(const std::optional<std::array<std::uint8_t, 16>> &guid_bytes) {
      if (!guid_bytes || guid_bytes_are_empty(*guid_bytes)) {
        return true;
      }

      GUID guid {};
      static_assert(sizeof(guid) == 16);
      std::memcpy(&guid, guid_bytes->data(), sizeof(guid));
      return VDISPLAY::removeVirtualDisplay(guid);
    }

    void disengage_recovery_monitors(const std::optional<std::array<std::uint8_t, 16>> &guid_bytes) {
      if (!guid_bytes || guid_bytes_are_empty(*guid_bytes)) {
        VDISPLAY::cancel_all_virtual_display_recovery_monitors();
        return;
      }

      GUID guid {};
      static_assert(sizeof(guid) == 16);
      std::memcpy(&guid, guid_bytes->data(), sizeof(guid));
      VDISPLAY::cancel_virtual_display_recovery_monitor(guid);
    }
  }  // namespace

  cleanup_result_t run(
    const std::string_view reason,
    const bool enforce_db_restore,
    const revert_order_t revert_order,
    const bool prefer_golden_if_current_missing,
    const std::optional<std::array<std::uint8_t, 16>> virtual_display_guid_bytes,
    const recovery_monitor_policy_t recovery_monitor_policy,
    const cleanup_admission_policy_t cleanup_admission_policy,
    const bool allow_disabled_recovery
  ) {
    cleanup_reservation_t cleanup_reservation;
    cleanup_result_t result;

    const std::string reason_text = reason.empty() ? "unspecified" : std::string(reason);
    if (recovery_monitor_policy == recovery_monitor_policy_t::disengage_before_admission) {
      // Terminal intent is authoritative even while a managed session still
      // owns the display. Cancel before the ownership guard so an intentionally
      // expired or externally removed lease can never be classified as a crash
      // and recreated by that ended session's recovery worker.
      disengage_recovery_monitors(virtual_display_guid_bytes);
      BOOST_LOG(info) << "Virtual display cleanup: recovery monitors disengaged before terminal cleanup admission (reason="
                      << reason_text << ").";
    }
    const bool managed_cleanup_allowed = remote_display_topology::instance().generic_virtual_display_cleanup_allowed();
    if (!cleanup_admitted(managed_cleanup_allowed, cleanup_admission_policy)) {
      if (enforce_db_restore) {
        proc::defer_display_revert();
      }
      BOOST_LOG(info) << "Virtual display cleanup: deferred (reason=" << reason_text
                      << ") until the remaining managed client display sessions release ownership.";
      return result;
    }
    if (!managed_cleanup_allowed) {
      BOOST_LOG(warning) << "Virtual display cleanup: overriding managed display ownership for terminal user action (reason="
                         << reason_text << ").";
    }

    BOOST_LOG(info) << "Virtual display cleanup: begin (reason=" << reason_text
                    << ", enforce_db_restore=" << (enforce_db_restore ? "true" : "false")
                    << ", revert_order="
                    << (revert_order == revert_order_t::restore_before_remove ? "restore_before_remove" : "remove_before_restore")
                    << ", prefer_golden_if_current_missing=" << (prefer_golden_if_current_missing ? "true" : "false")
                    << ")";

    const bool had_active_virtual_display = has_active_virtual_display();
    VDISPLAY::setWatchdogFeedingEnabled(false);

    const auto try_helper_revert = [&]() {
      if (!enforce_db_restore || result.helper_revert_dispatched) {
        return;
      }

      result.helper_revert_dispatched = display_helper_integration::revert(
        prefer_golden_if_current_missing,
        cleanup_admission_policy == cleanup_admission_policy_t::override_managed_owners,
        allow_disabled_recovery
      );
      // REVERT is asynchronous. A successful send does not establish that the
      // helper restored or verified the desktop; report dispatch separately.
    };

    bool teardown_completed = false;
    bool teardown_waited = false;
    const auto wait_for_teardown_before_restore = [&]() {
      if (teardown_waited || result.helper_revert_dispatched || !teardown_completed ||
          !had_active_virtual_display || !enforce_db_restore) {
        return;
      }
      constexpr auto kTeardownSettleTimeout = std::chrono::seconds(5);
      if (wait_for_virtual_display_teardown(kTeardownSettleTimeout)) {
        BOOST_LOG(debug) << "Virtual display cleanup: teardown settled before restore.";
      }
      teardown_waited = true;
    };

    // Keep the retained probe display alive for restore-before-remove callers,
    // but remove it in the normal remove-before-restore order with the other
    // virtual displays. This also covers a driver-accepted target that has
    // not yet appeared in Windows enumeration.
    for (const auto step : ordered_restore_steps(revert_order)) {
      switch (step) {
        case cleanup_step_t::helper_revert:
          wait_for_teardown_before_restore();
          if (enforce_db_restore) {
            try_helper_revert();
          }
          break;
        case cleanup_step_t::retained_probe_remove:
          VDISPLAY::cleanup_retained_ensure_display();
          break;
        case cleanup_step_t::explicit_display_remove: {
          const bool specific_display_removed = remove_specific_virtual_display(virtual_display_guid_bytes);
          const bool tracked_displays_removed = VDISPLAY::removeAllVirtualDisplays();
          result.virtual_displays_removed = specific_display_removed && tracked_displays_removed;
          teardown_completed = true;
          break;
        }
        case cleanup_step_t::database_restore:
          wait_for_teardown_before_restore();
          if (enforce_db_restore && !result.helper_revert_dispatched &&
              cleanup_admission_policy != cleanup_admission_policy_t::override_managed_owners) {
            // An unavailable pipe does not prove that the helper stopped. It
            // may already be recovering from that same disconnect; serialize
            // native fallback with helper exit just as terminal rescue does.
            result.database_restore_applied = display_helper_integration::run_terminal_physical_recovery(
              restore_windows_display_database);
          }
          break;
      }
    }

    BOOST_LOG(info) << "Virtual display cleanup: finished (reason=" << reason_text
                    << ", had_active_virtual_display=" << (had_active_virtual_display ? "true" : "false")
                    << ", virtual_displays_removed=" << (result.virtual_displays_removed ? "true" : "false")
                    << ", helper_revert_dispatched=" << (result.helper_revert_dispatched ? "true" : "false")
                    << ", database_restore_applied=" << (result.database_restore_applied ? "true" : "false")
                    << ")";
    return result;
  }

  static bool recover_emergency_physical_display(bool &database_restore_applied) {
    try {
      const auto recover = [&]() {
        namespace rescue = display_helper::physical_recovery;
        const display_device::DisplayRecoveryBehaviorGuard recovery_guard {display_device::DisplayRecoveryBehavior::Skip};
        auto api = std::make_shared<display_device::WinApiLayer>();
        display_device::WinDisplayDevice device {api};
        const auto capture_actual = [&]() -> std::optional<rescue::Topology> {
          const auto active = api->queryDisplayConfig(display_device::QueryType::Active);
          if (!active) return std::nullopt;
          std::set<std::string> active_ids;
          for (const auto &path : active->m_paths) {
            if ((path.flags & DISPLAYCONFIG_PATH_ACTIVE) == 0) continue;
            const auto id = api->getDeviceId(path);
            if (id.empty()) return std::nullopt;
            active_ids.insert(rescue::normalized_id(id));
          }
          if (active_ids.empty()) return rescue::Topology {};
          auto topology = device.getCurrentTopology();
          if (rescue::device_ids(topology) != active_ids) return std::nullopt;
          return topology;
        };
        const auto before = capture_actual();
        if (!before) return false;
        const auto outputs = rescue::enumerate_devices(*api);
        rescue::Topology candidates;
        for (const auto &output : outputs) {
          if (output.physical) candidates.push_back({output.id});
        }
        if (candidates.empty()) return false;

        // Explicit terminal recovery may use any connected physical target.
        // Keep the live desktop, including permanent virtual outputs, intact;
        // reloading the Windows database could silently replace those paths.
        (void) database_restore_applied;
        if (!before->empty()) {
          return rescue::ensure_visible(
            candidates,
            {},
            [&]() { return rescue::enumerate_devices(*api); },
            [&]() { return capture_actual().value_or(rescue::Topology {}); },
            [&](const rescue::Topology &topology) { return device.setTopology(topology); },
            []() { return false; },
            [](std::chrono::milliseconds duration) { std::this_thread::sleep_for(duration); return true; }).physical_available;
        }

        // A successful empty active query is different from a failed read.
        // WinDisplayDevice synthesizes a topology in that state, so use one
        // temporary CCD activation and require fresh physical readback.
        const auto all = api->queryDisplayConfig(display_device::QueryType::All);
        if (!all) return false;
        const rescue::Topology requested {candidates.front()};
        const auto sources = display_device::win_utils::collectSourceDataForMatchingPaths(*api, all->m_paths);
        const auto paths = display_device::win_utils::makePathsForNewTopology(requested, sources, all->m_paths);
        if (paths.empty()) return false;
        const auto still_empty = capture_actual();
        if (!still_empty || !still_empty->empty()) return false;
        // makePathsForNewTopology invalidates source/target/desktop mode
        // indexes; with an empty mode array this requests temporary best-mode
        // selection (the same supplied-config contract used by setTopology).
        const UINT32 flags = SDC_APPLY | SDC_USE_SUPPLIED_DISPLAY_CONFIG | SDC_ALLOW_CHANGES | SDC_VIRTUAL_MODE_AWARE;
        if (api->setDisplayConfig(paths, {}, flags) != ERROR_SUCCESS) return false;
        const auto actual = capture_actual();
        if (!actual) return false;
        const auto active_ids = rescue::device_ids(*actual);
        const auto current_outputs = rescue::enumerate_devices(*api);
        return std::ranges::any_of(current_outputs, [&](const auto &output) {
          return output.physical && output.active && active_ids.contains(rescue::normalized_id(output.id));
        });
      };

      // Raw CCD queries and setters must run in the same user context. The
      // enclosing terminal fence already owns helper/session mutation admission.
      if (!platf::is_running_as_system()) return recover();
      const HANDLE token = platf::retrieve_users_token(true);
      if (!token) return false;
      const auto close_token = util::fail_guard([&]() { CloseHandle(token); });
      bool recovered = false;
      std::exception_ptr failure;
      const auto error = platf::impersonate_current_user(token, [&]() {
        // The impersonation helper reverts after the callback returns. Keep
        // callback failures local until that user context has been released.
        try {
          recovered = recover();
        } catch (...) {
          failure = std::current_exception();
        }
      });
      if (failure) std::rethrow_exception(failure);
      return !error && recovered;
    } catch (const std::exception &e) {
      BOOST_LOG(warning) << "Virtual display cleanup: emergency physical recovery failed: " << e.what();
    } catch (...) {
      BOOST_LOG(warning) << "Virtual display cleanup: emergency physical recovery failed.";
    }
    return false;
  }

  cleanup_result_t terminate_all(const std::string_view reason) {
    std::lock_guard terminal_lock {g_terminal_cleanup_mutex};
    cleanup_reservation_t cleanup_reservation;
    // A previous ordinary cleanup may have queued a restore behind the same
    // managed-owner gate this terminal action intentionally overrides. This
    // action consumes that intent now, so it must not fire again later.
    proc::clear_deferred_display_revert();
    auto result = run(
      reason,
      true,
      revert_order_t::remove_before_restore,
      true,
      std::nullopt,
      recovery_monitor_policy_t::disengage_before_admission,
      cleanup_admission_policy_t::override_managed_owners
    );

    // A terminal user action must also end the helper restart loop. Forced
    // stop is safe here because run() has already completed the synchronous
    // REVERT attempt and display teardown. Closing the driver transport stops
    // its ping/watchdog worker; a later new session may open it again.
    VDISPLAY::closeVDisplayDevice();
    display_helper_integration::stop_watchdog(true);
    // A damaged driver journal can make aggregate removal fail after tracked
    // outputs were already removed. The explicit recovery action must still
    // try to make the local screen usable. The terminal fence first quiesces
    // helper mutations; this does not authorize removing unknown displays.
    result.physical_display_recovered = display_helper_integration::run_terminal_physical_recovery([&] {
      return recover_emergency_physical_display(result.database_restore_applied);
    });
    BOOST_LOG(info) << "Virtual display cleanup: terminal driver and helper watchdog shutdown completed (reason="
                    << (reason.empty() ? "unspecified" : std::string(reason)) << ").";
    return result;
  }

  bool in_progress() {
    return g_cleanup_reservations.load(std::memory_order_acquire) != 0;
  }
}  // namespace platf::virtual_display_cleanup

#endif  // _WIN32

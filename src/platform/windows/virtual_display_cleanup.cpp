#include "virtual_display_cleanup.h"

#ifdef _WIN32

  #include "display_helper_integration.h"
  #include "src/logging.h"
  #include "src/platform/windows/impersonating_display_device.h"
  #include "src/platform/windows/virtual_display.h"

  #include <algorithm>
  #include <array>
  #include <atomic>
  #include <chrono>
  #include <cstring>
  #include <display_device/windows/win_api_layer.h>
  #include <display_device/windows/win_display_device.h>
  #include <exception>
  #include <memory>
  #include <string>
  #include <thread>

namespace platf::virtual_display_cleanup {
  namespace {
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

        if (display_helper_integration::display_start_waiting() ||
            display_helper_integration::display_start_in_progress()) {
          BOOST_LOG(debug) << "Virtual display cleanup: a stream start is waiting; skipping the remaining teardown settle delay.";
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
  }  // namespace

  namespace {
    // Only a new display-owning start invalidates cleanup work. Independent
    // cleanup callers must not cancel each other: a weaker VD-only request
    // could otherwise supersede a restore-required last-session request.
    std::atomic<std::uint64_t> g_cleanup_cancel_epoch {0};

    bool cleanup_complete(const cleanup_result_t &result, bool enforce_restore) {
      const bool restore_complete =
        !enforce_restore ||
        result.helper_revert_dispatched ||
        result.database_restore_applied;
      return result.virtual_displays_removed && restore_complete;
    }
  }  // namespace

  void cancel_deferred() {
    (void) g_cleanup_cancel_epoch.fetch_add(1, std::memory_order_acq_rel);
  }

  static cleanup_result_t run_once(
    const std::string_view reason,
    const bool enforce_db_restore,
    const revert_order_t revert_order,
    const bool prefer_golden_if_current_missing,
    const std::optional<std::array<std::uint8_t, 16>> virtual_display_guid_bytes,
    const std::function<bool()> &still_allowed,
    const std::uint64_t cleanup_cancel_epoch,
    const bool remove_virtual_displays
  ) {
    cleanup_result_t result;

    const auto transaction_still_allowed = [&]() {
      return g_cleanup_cancel_epoch.load(std::memory_order_acquire) == cleanup_cancel_epoch &&
             !display_helper_integration::display_start_waiting() &&
             !display_helper_integration::display_start_in_progress() &&
             (!still_allowed || still_allowed());
    };

    // Serialize removal/direct DB restore/REVERT with every stream-start and
    // APPLY transaction. The mutex is recursive so the tracked revert below can
    // publish its generation while this lease remains held.
    auto display_handoff =
      display_helper_integration::acquire_safe_display_handoff(
        std::chrono::seconds(10),
        transaction_still_allowed
      );
    if (!display_handoff) {
      if (!transaction_still_allowed()) {
        BOOST_LOG(info) << "Virtual display cleanup canceled because a newer session is active.";
      } else {
        BOOST_LOG(warning) << "Virtual display cleanup deferred: display handoff remained unsafe.";
      }
      return result;
    }
    if (!transaction_still_allowed()) {
      BOOST_LOG(info) << "Virtual display cleanup canceled after acquiring handoff because a newer session is active.";
      return result;
    }

    const std::string reason_text = reason.empty() ? "unspecified" : std::string(reason);
    BOOST_LOG(info) << "Virtual display cleanup: begin (reason=" << reason_text
                    << ", enforce_db_restore=" << (enforce_db_restore ? "true" : "false")
                    << ", revert_order="
                    << (revert_order == revert_order_t::restore_before_remove ? "restore_before_remove" : "remove_before_restore")
                    << ", prefer_golden_if_current_missing=" << (prefer_golden_if_current_missing ? "true" : "false")
                    << ")";

    // Last pre-mutation yield point. Once VD removal or REVERT begins, finish
    // the requested restore so the waiting start never observes half-cleaned
    // topology.
    if (!transaction_still_allowed()) {
      BOOST_LOG(info) << "Virtual display cleanup yielded to a waiting stream start before mutation.";
      return result;
    }

    const bool had_active_virtual_display =
      remove_virtual_displays && has_active_virtual_display();
    if (remove_virtual_displays) {
      VDISPLAY::setWatchdogFeedingEnabled(false);
    }

    const auto try_helper_revert = [&]() {
      if (!enforce_db_restore || result.helper_revert_dispatched) {
        return;
      }

      result.helper_revert_dispatched = display_helper_integration::revert(prefer_golden_if_current_missing);
      if (result.helper_revert_dispatched) {
        result.database_restore_applied = true;
      }
    };

    if (enforce_db_restore && revert_order == revert_order_t::restore_before_remove) {
      try_helper_revert();
    }

    if (remove_virtual_displays) {
      const bool specific_display_removed = remove_specific_virtual_display(virtual_display_guid_bytes);
      const bool tracked_displays_removed = VDISPLAY::removeAllVirtualDisplays();
      result.virtual_displays_removed = specific_display_removed && tracked_displays_removed;
    } else {
      result.virtual_displays_removed = true;
    }
    const bool should_wait_for_teardown_before_restore = had_active_virtual_display;
    if (should_wait_for_teardown_before_restore) {
      constexpr auto kTeardownSettleTimeout = std::chrono::seconds(5);
      if (wait_for_virtual_display_teardown(kTeardownSettleTimeout)) {
        BOOST_LOG(debug) << "Virtual display cleanup: teardown settled before restore.";
      }
    }

    if (enforce_db_restore) {
      if (revert_order == revert_order_t::remove_before_restore) {
        try_helper_revert();
      }

      if (!result.helper_revert_dispatched) {
        result.database_restore_applied = restore_windows_display_database();
      }
    }

    if (!remove_virtual_displays) {
      VDISPLAY::setWatchdogFeedingEnabled(has_active_virtual_display());
    }

    BOOST_LOG(info) << "Virtual display cleanup: finished (reason=" << reason_text
                    << ", had_active_virtual_display=" << (had_active_virtual_display ? "true" : "false")
                    << ", virtual_displays_removed=" << (result.virtual_displays_removed ? "true" : "false")
                    << ", helper_revert_dispatched=" << (result.helper_revert_dispatched ? "true" : "false")
                    << ", database_restore_applied=" << (result.database_restore_applied ? "true" : "false")
                    << ")";
    return result;
  }

  struct DeferredCleanupRetryState {
    std::uint64_t cancel_epoch {0};
    std::string reason;
    bool enforce_db_restore {true};
    revert_order_t revert_order {revert_order_t::remove_before_restore};
    bool prefer_golden_if_current_missing {true};
    std::optional<std::array<std::uint8_t, 16>> virtual_display_guid_bytes;
    bool remove_virtual_displays {true};
    std::function<bool()> still_allowed;
  };

  void run_deferred_cleanup_retry(
    const std::shared_ptr<DeferredCleanupRetryState> &state,
    int attempt
  );

  void schedule_deferred_cleanup_retry(
    const std::shared_ptr<DeferredCleanupRetryState> &state,
    int attempt
  ) {
    try {
      const bool queued = display_helper_integration::enqueue_delayed_display_cleanup_task(
        std::chrono::milliseconds(250 * attempt),
        [state, attempt]() {
          run_deferred_cleanup_retry(state, attempt);
        }
      );
      if (!queued) {
        BOOST_LOG(warning) << "Virtual display cleanup retry was not queued (reason="
                           << state->reason << ").";
      }
    } catch (const std::exception &e) {
      BOOST_LOG(error) << "Virtual display cleanup retry could not be scheduled: " << e.what();
    } catch (...) {
      BOOST_LOG(error) << "Virtual display cleanup retry could not be scheduled.";
    }
  }

  void run_deferred_cleanup_retry(
    const std::shared_ptr<DeferredCleanupRetryState> &state,
    int attempt
  ) {
    constexpr int kMaxAttempts = 3;
    if (g_cleanup_cancel_epoch.load(std::memory_order_acquire) != state->cancel_epoch ||
        (state->still_allowed && !state->still_allowed())) {
      return;
    }
    if (display_helper_integration::display_start_in_progress() ||
        display_helper_integration::display_start_waiting()) {
      // A pending start preserves the cleanup intent without consuming retry
      // budget. Its publication cancels this work; an abandoned start lets it
      // resume after the same bounded delay.
      schedule_deferred_cleanup_retry(state, attempt);
      return;
    }

    const auto retry = run_once(
      state->reason,
      state->enforce_db_restore,
      state->revert_order,
      state->prefer_golden_if_current_missing,
      state->virtual_display_guid_bytes,
      state->still_allowed,
      state->cancel_epoch,
      state->remove_virtual_displays
    );
    if (cleanup_complete(retry, state->enforce_db_restore)) {
      BOOST_LOG(info) << "Virtual display cleanup retry succeeded (reason="
                      << state->reason << ", attempt=" << attempt << ").";
      return;
    }
    if (attempt >= kMaxAttempts) {
      BOOST_LOG(warning) << "Virtual display cleanup retry budget exhausted (reason="
                         << state->reason << ").";
      return;
    }
    schedule_deferred_cleanup_retry(state, attempt + 1);
  }

  static cleanup_result_t run_impl(
    const std::string_view reason,
    const bool enforce_db_restore,
    const revert_order_t revert_order,
    const bool prefer_golden_if_current_missing,
    const std::optional<std::array<std::uint8_t, 16>> virtual_display_guid_bytes,
    std::function<bool()> still_allowed,
    const bool remove_virtual_displays
  ) {
    if (still_allowed && !still_allowed()) {
      BOOST_LOG(info) << "Virtual display cleanup canceled before scheduling because it is stale.";
      return {};
    }
    const auto cancel_epoch = g_cleanup_cancel_epoch.load(std::memory_order_acquire);
    auto result = run_once(
      reason,
      enforce_db_restore,
      revert_order,
      prefer_golden_if_current_missing,
      virtual_display_guid_bytes,
      still_allowed,
      cancel_epoch,
      remove_virtual_displays
    );

    if (cleanup_complete(result, enforce_db_restore) ||
        g_cleanup_cancel_epoch.load(std::memory_order_acquire) != cancel_epoch ||
        (still_allowed && !still_allowed())) {
      return result;
    }

    // A blocking Windows display mutation can occasionally outlive the normal
    // handoff budget. Preserve the cleanup intent instead of orphaning a VD
    // when its session object is about to disappear. The session predicate and
    // cancellation epoch make the retry self-cancel as soon as a newer start
    // owns the display; sibling cleanup requests remain independently valid.
    const std::string deferred_reason = reason.empty() ? "unspecified" : std::string(reason);
    BOOST_LOG(warning) << "Virtual display cleanup scheduled for bounded retry (reason="
                       << deferred_reason << ").";
    try {
      auto retry = std::make_shared<DeferredCleanupRetryState>(DeferredCleanupRetryState {
        .cancel_epoch = cancel_epoch,
        .reason = deferred_reason,
        .enforce_db_restore = enforce_db_restore,
        .revert_order = revert_order,
        .prefer_golden_if_current_missing = prefer_golden_if_current_missing,
        .virtual_display_guid_bytes = virtual_display_guid_bytes,
        .remove_virtual_displays = remove_virtual_displays,
        .still_allowed = std::move(still_allowed),
      });
      schedule_deferred_cleanup_retry(retry, 1);
    } catch (const std::exception &e) {
      BOOST_LOG(error) << "Virtual display cleanup retry state could not be created: " << e.what();
    } catch (...) {
      BOOST_LOG(error) << "Virtual display cleanup retry state could not be created.";
    }

    return result;
  }

  cleanup_result_t run(
    const std::string_view reason,
    const bool enforce_db_restore,
    const revert_order_t revert_order,
    const bool prefer_golden_if_current_missing,
    const std::optional<std::array<std::uint8_t, 16>> virtual_display_guid_bytes,
    std::function<bool()> still_allowed
  ) {
    return run_impl(
      reason,
      enforce_db_restore,
      revert_order,
      prefer_golden_if_current_missing,
      virtual_display_guid_bytes,
      std::move(still_allowed),
      true
    );
  }

  cleanup_result_t restore_only(
    const std::string_view reason,
    const bool prefer_golden_if_current_missing,
    std::function<bool()> still_allowed
  ) {
    return run_impl(
      reason,
      true,
      revert_order_t::remove_before_restore,
      prefer_golden_if_current_missing,
      std::nullopt,
      std::move(still_allowed),
      false
    );
  }
}  // namespace platf::virtual_display_cleanup

#endif  // _WIN32

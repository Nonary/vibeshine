#include "virtual_display.h"

#include "src/config.h"
#include "src/logging.h"

#include <atomic>
#include <exception>
#include <mutex>
#include <string>
#include <unordered_map>

namespace VDISPLAY {
  HANDLE VIRTUAL_DISPLAY_DRIVER_HANDLE = INVALID_HANDLE_VALUE;
}

namespace VDISPLAY_SUNSHINE {
  using VDISPLAY::DRIVER_STATUS;
  using VDISPLAY::ensure_display_result;
  using VDISPLAY::VirtualDisplayCreationResult;
  using VDISPLAY::VirtualDisplayInfo;
  using VDISPLAY::VirtualDisplayRecoveryParams;

  void closeVDisplayDevice();
  DRIVER_STATUS openVDisplayDevice();
  bool ensure_driver_is_ready();
  bool startPingThread(std::function<void()> failCb);
  void setWatchdogFeedingEnabled(bool enable);
  bool setRenderAdapterByName(const std::wstring &adapterName);
  bool setRenderAdapterWithMostDedicatedMemory();
  void ensureVirtualDisplayRegistryDefaults();
  std::optional<VirtualDisplayCreationResult> createVirtualDisplay(
    const char *s_client_uid,
    const char *s_client_name,
    const char *s_hdr_profile,
    uint32_t width,
    uint32_t height,
    uint32_t fps,
    const GUID &guid,
    uint32_t base_fps_millihz,
    bool framegen_refresh_active,
    int framegen_refresh_multiplier,
    bool hdr_requested,
    bool allow_pending_enumeration,
    bool replace_existing
  );
  void applyHdrProfileToOutput(const char *s_client_name, const char *s_hdr_profile, const char *s_device_id);
  void restorePhysicalHdrProfiles();
  void request_recovery_monitor_shutdown();
  void wait_for_recovery_monitor_shutdown();
  bool removeVirtualDisplay(const GUID &guid);
  bool removeAllVirtualDisplays();
  void schedule_virtual_display_recovery_monitor(const VirtualDisplayRecoveryParams &params);
  bool is_virtual_display_guid_tracked(const GUID &guid);
  std::optional<std::uint64_t> virtual_display_tracking_generation(const GUID &guid);
  std::optional<std::string> resolveVirtualDisplayDeviceId(const std::wstring &display_name);
  std::optional<std::string> resolveVirtualDisplayDeviceIdForClient(const std::string &client_name);
  std::optional<std::string> resolveActiveVirtualDisplayDeviceId(const std::string &preferred_output_identifier, const std::string &client_name, bool allow_any_fallback);
  std::optional<std::string> resolveActiveVirtualDisplayDeviceIdForStableId(const std::string &stable_id, const std::string &preferred_output_identifier, const std::string &client_name, bool allow_any_fallback);
  std::optional<std::string> resolveAnyVirtualDisplayDeviceId();
  bool is_virtual_display_output(const std::string &output_identifier);
  bool is_virtual_display_selection(const std::string &output_identifier);
  uint64_t client_uuid_to_virtual_display_id(const GUID &client_guid);
  uuid_util::uuid_t virtualDisplayUuidFromStableId(const std::string &stable_id);
  GUID sharedVirtualDisplayGuid();
  bool is_sunshine_virtual_display_identity(const std::string &device_path, const std::string &friendly_name, const std::string &edid_manufacturer_id, const std::string &edid_product_code);
  bool isVirtualDisplayDriverInstalled();
  std::vector<VirtualDisplayInfo> enumerateVirtualDisplays();
  uuid_util::uuid_t persistentVirtualDisplayUuid();
  bool has_active_physical_display();
  bool should_auto_enable_virtual_display();
  ensure_display_result ensure_display();
  void cleanup_ensure_display(const ensure_display_result &result, bool probe_succeeded, bool allow_temporary_teardown);
  bool has_retained_ensure_display();
}  // namespace VDISPLAY_SUNSHINE

namespace VDISPLAY_SUDOVDA {
  using VDISPLAY::DRIVER_STATUS;
  using VDISPLAY::ensure_display_result;
  using VDISPLAY::VirtualDisplayCreationResult;
  using VDISPLAY::VirtualDisplayInfo;
  using VDISPLAY::VirtualDisplayRecoveryParams;

  void closeVDisplayDevice();
  DRIVER_STATUS openVDisplayDevice();
  bool ensure_driver_is_ready();
  bool startPingThread(std::function<void()> failCb);
  void setWatchdogFeedingEnabled(bool enable);
  bool setRenderAdapterByName(const std::wstring &adapterName);
  bool setRenderAdapterWithMostDedicatedMemory();
  void ensureVirtualDisplayRegistryDefaults();
  std::optional<VirtualDisplayCreationResult> createVirtualDisplay(
    const char *s_client_uid,
    const char *s_client_name,
    const char *s_hdr_profile,
    uint32_t width,
    uint32_t height,
    uint32_t fps,
    const GUID &guid,
    uint32_t base_fps_millihz,
    bool framegen_refresh_active,
    int framegen_refresh_multiplier,
    bool hdr_requested,
    bool replace_existing
  );
  void applyHdrProfileToOutput(const char *s_client_name, const char *s_hdr_profile, const char *s_device_id);
  void restorePhysicalHdrProfiles();
  void request_recovery_monitor_shutdown();
  void wait_for_recovery_monitor_shutdown();
  bool removeVirtualDisplay(const GUID &guid);
  bool removeAllVirtualDisplays();
  void schedule_virtual_display_recovery_monitor(const VirtualDisplayRecoveryParams &params);
  bool is_virtual_display_guid_tracked(const GUID &guid);
  std::optional<std::uint64_t> virtual_display_tracking_generation(const GUID &guid);
  std::optional<std::string> resolveVirtualDisplayDeviceId(const std::wstring &display_name);
  std::optional<std::string> resolveVirtualDisplayDeviceIdForClient(const std::string &client_name);
  std::optional<std::string> resolveActiveVirtualDisplayDeviceId(const std::string &preferred_output_identifier, const std::string &client_name, bool allow_any_fallback);
  std::optional<std::string> resolveAnyVirtualDisplayDeviceId();
  bool is_virtual_display_output(const std::string &output_identifier);
  bool is_virtual_display_selection(const std::string &output_identifier);
  uint64_t client_uuid_to_vdd_display_id(const GUID &client_guid);
  GUID sharedVirtualDisplayGuid();
  bool isSudaVDADriverInstalled();
  std::vector<VirtualDisplayInfo> enumerateSudaVDADisplays();
  uuid_util::uuid_t persistentVirtualDisplayUuid();
  bool has_active_physical_display();
  bool should_auto_enable_virtual_display();
  ensure_display_result ensure_display();
  void cleanup_ensure_display(const ensure_display_result &result, bool probe_succeeded, bool allow_temporary_teardown);
  bool has_retained_ensure_display();
}  // namespace VDISPLAY_SUDOVDA

namespace {
  std::string guid_key(const GUID &guid) {
    return std::string(
      reinterpret_cast<const char *>(&guid),
      reinterpret_cast<const char *>(&guid) + sizeof(guid)
    );
  }

  std::mutex g_recovery_owner_mutex;

  struct recovery_owner_t {
    std::uint64_t generation {0};
    std::optional<std::uint64_t> tracking_generation;
    // A recovery may need to remove and recreate the very GUID that owns its
    // monitor. During that transaction, accept only the expected interval
    // where that registration is temporarily absent; never accept a newly
    // registered generation in its place.
    bool recreating {false};
  };

  std::unordered_map<std::string, recovery_owner_t> g_recovery_owner_generations;
  std::uint64_t g_next_recovery_owner_generation = 0;

  // All recovery reopen/recreate sequences and watchdog-triggered closes pass
  // through this lock. The terminal flag is published before cancellation is
  // signaled; a final lock/unlock in beginVDisplayDeviceShutdown() forms a
  // barrier for a sequence that was already in flight.
  std::atomic_bool g_vdisplay_terminal_shutdown_requested {false};
  std::recursive_mutex g_virtual_display_recovery_operation_mutex;

  struct recovery_operation_lease_t {
    explicit recovery_operation_lease_t(std::unique_lock<std::recursive_mutex> lock_in):
        lock(std::move(lock_in)) {}

    std::unique_lock<std::recursive_mutex> lock;
  };

  std::mutex g_virtual_hdr_profile_operation_mutex;

  struct virtual_hdr_profile_target_t {
    std::mutex write_mutex;
    std::atomic<std::uint64_t> generation {0};
  };

  std::unordered_map<std::string, std::shared_ptr<virtual_hdr_profile_target_t>> g_virtual_hdr_profile_operation_targets;
  std::uint64_t g_next_virtual_hdr_profile_operation_generation = 0;

  struct virtual_hdr_profile_operation_lease_t {
    virtual_hdr_profile_operation_lease_t(
      std::shared_ptr<virtual_hdr_profile_target_t> target_in,
      std::unique_lock<std::mutex> lock_in
    ):
        target(std::move(target_in)),
        lock(std::move(lock_in)) {}

    std::shared_ptr<virtual_hdr_profile_target_t> target;
    std::unique_lock<std::mutex> lock;
  };

  bool terminal_shutdown_requested() {
    return g_vdisplay_terminal_shutdown_requested.load(std::memory_order_acquire);
  }

  bool use_sunshine_driver() {
    return config::video.dd.use_sunshine_virtual_display_driver;
  }
}  // namespace

namespace VDISPLAY {
  void closeVDisplayDevice() {
    std::lock_guard<std::recursive_mutex> lock(g_virtual_display_recovery_operation_mutex);
    VDISPLAY_SUNSHINE::closeVDisplayDevice();
    VDISPLAY_SUDOVDA::closeVDisplayDevice();
  }

  void beginVDisplayDeviceShutdown() {
    // Signal both backends before waiting on either. This lets cancellation
    // reach queued recovery retries while main drains the managed cleanup
    // executor, and prevents a stale owner from entering a new handoff.
    g_vdisplay_terminal_shutdown_requested.store(true, std::memory_order_release);
    invalidate_all_virtual_display_recovery_owners();
    VDISPLAY_SUNSHINE::request_recovery_monitor_shutdown();
    VDISPLAY_SUDOVDA::request_recovery_monitor_shutdown();
    invalidate_all_virtual_display_hdr_profile_operations();
    // A monitor that had already acquired its recovery operation lease sees
    // the driver abort flag between side effects and releases the lease before
    // this returns. New acquisitions recheck the terminal flag under the same
    // mutex and therefore cannot race beyond this barrier.
    std::lock_guard<std::recursive_mutex> barrier(g_virtual_display_recovery_operation_mutex);
  }

  void shutdownVDisplayDevice() {
    // Process-exit only: recovery monitors may otherwise reopen a driver after
    // its transport/handle is torn down. Ordinary close/reopen paths keep
    // using closeVDisplayDevice() so they do not self-cancel recovery.
    beginVDisplayDeviceShutdown();
    VDISPLAY_SUNSHINE::wait_for_recovery_monitor_shutdown();
    VDISPLAY_SUDOVDA::wait_for_recovery_monitor_shutdown();
    closeVDisplayDevice();
  }

  std::shared_ptr<void> acquire_virtual_display_recovery_operation() noexcept {
    if (g_vdisplay_terminal_shutdown_requested.load(std::memory_order_acquire)) {
      return {};
    }
    try {
      std::unique_lock<std::recursive_mutex> lock(g_virtual_display_recovery_operation_mutex);
      if (g_vdisplay_terminal_shutdown_requested.load(std::memory_order_acquire)) {
        return {};
      }
      auto impl = std::make_unique<recovery_operation_lease_t>(std::move(lock));
      std::shared_ptr<recovery_operation_lease_t> lease(std::move(impl));
      return lease;
    } catch (const std::exception &e) {
      BOOST_LOG(error) << "Virtual display recovery: unable to acquire operation lease: " << e.what();
    } catch (...) {
      BOOST_LOG(error) << "Virtual display recovery: unable to acquire operation lease.";
    }
    return {};
  }

  std::uint64_t begin_virtual_display_hdr_profile_operation(const GUID &guid) noexcept {
    if (terminal_shutdown_requested()) {
      return 0;
    }
    try {
      std::unique_lock<std::mutex> targets_lock(g_virtual_hdr_profile_operation_mutex);
      const auto key = guid_key(guid);
      auto &target = g_virtual_hdr_profile_operation_targets[key];
      if (!target) {
        target = std::make_shared<virtual_hdr_profile_target_t>();
      }
      // New creation waits only for a registry write already in progress, not
      // for the task's display enumeration/retry loop.
      std::unique_lock<std::mutex> write_lock(target->write_mutex);
      auto generation = ++g_next_virtual_hdr_profile_operation_generation;
      if (generation == 0) {
        generation = ++g_next_virtual_hdr_profile_operation_generation;
      }
      target->generation.store(generation, std::memory_order_release);
      return generation;
    } catch (const std::exception &e) {
      BOOST_LOG(error) << "Virtual display HDR: unable to begin profile operation: " << e.what();
    } catch (...) {
      BOOST_LOG(error) << "Virtual display HDR: unable to begin profile operation.";
    }
    return 0;
  }

  bool is_virtual_display_hdr_profile_operation_current(
    const GUID &guid,
    std::uint64_t generation
  ) noexcept {
    if (generation == 0 || terminal_shutdown_requested()) {
      return false;
    }
    try {
      std::lock_guard<std::mutex> targets_lock(g_virtual_hdr_profile_operation_mutex);
      const auto found = g_virtual_hdr_profile_operation_targets.find(guid_key(guid));
      return found != g_virtual_hdr_profile_operation_targets.end() &&
             found->second &&
             found->second->generation.load(std::memory_order_acquire) == generation &&
             !terminal_shutdown_requested();
    } catch (...) {
      return false;
    }
  }

  std::shared_ptr<void> acquire_virtual_display_hdr_profile_operation(
    const GUID &guid,
    std::uint64_t generation
  ) noexcept {
    if (generation == 0 || terminal_shutdown_requested()) {
      return {};
    }
    try {
      std::shared_ptr<virtual_hdr_profile_target_t> target;
      {
        std::lock_guard<std::mutex> targets_lock(g_virtual_hdr_profile_operation_mutex);
        const auto found = g_virtual_hdr_profile_operation_targets.find(guid_key(guid));
        if (found == g_virtual_hdr_profile_operation_targets.end()) {
          return {};
        }
        target = found->second;
      }
      if (!target) {
        return {};
      }
      std::unique_lock<std::mutex> write_lock(target->write_mutex);
      if (terminal_shutdown_requested() ||
          target->generation.load(std::memory_order_acquire) != generation) {
        return {};
      }
      auto impl = std::make_unique<virtual_hdr_profile_operation_lease_t>(
        std::move(target),
        std::move(write_lock)
      );
      std::shared_ptr<virtual_hdr_profile_operation_lease_t> lease(std::move(impl));
      return lease;
    } catch (const std::exception &e) {
      BOOST_LOG(error) << "Virtual display HDR: unable to acquire profile operation: " << e.what();
    } catch (...) {
      BOOST_LOG(error) << "Virtual display HDR: unable to acquire profile operation.";
    }
    return {};
  }

  void invalidate_virtual_display_hdr_profile_operation(const GUID &guid) noexcept {
    try {
      std::unique_lock<std::mutex> targets_lock(g_virtual_hdr_profile_operation_mutex);
      const auto found = g_virtual_hdr_profile_operation_targets.find(guid_key(guid));
      if (found == g_virtual_hdr_profile_operation_targets.end() || !found->second) {
        return;
      }
      std::unique_lock<std::mutex> write_lock(found->second->write_mutex);
      found->second->generation.store(0, std::memory_order_release);
    } catch (const std::exception &e) {
      BOOST_LOG(error) << "Virtual display HDR: unable to invalidate profile operation: " << e.what();
    } catch (...) {
      BOOST_LOG(error) << "Virtual display HDR: unable to invalidate profile operation.";
    }
  }

  void invalidate_all_virtual_display_hdr_profile_operations() noexcept {
    try {
      std::unique_lock<std::mutex> targets_lock(g_virtual_hdr_profile_operation_mutex);
      for (auto &[_, target] : g_virtual_hdr_profile_operation_targets) {
        if (!target) {
          continue;
        }
        std::unique_lock<std::mutex> write_lock(target->write_mutex);
        target->generation.store(0, std::memory_order_release);
      }
    } catch (const std::exception &e) {
      BOOST_LOG(error) << "Virtual display HDR: unable to invalidate profile operations: " << e.what();
    } catch (...) {
      BOOST_LOG(error) << "Virtual display HDR: unable to invalidate profile operations.";
    }
  }

  DRIVER_STATUS openVDisplayDevice() {
    std::lock_guard<std::recursive_mutex> lock(g_virtual_display_recovery_operation_mutex);
    if (terminal_shutdown_requested()) {
      return DRIVER_STATUS::FAILED;
    }
    return use_sunshine_driver() ? VDISPLAY_SUNSHINE::openVDisplayDevice() : VDISPLAY_SUDOVDA::openVDisplayDevice();
  }

  bool ensure_driver_is_ready() {
    std::lock_guard<std::recursive_mutex> lock(g_virtual_display_recovery_operation_mutex);
    if (terminal_shutdown_requested()) {
      return false;
    }
    return use_sunshine_driver() ? VDISPLAY_SUNSHINE::ensure_driver_is_ready() : VDISPLAY_SUDOVDA::ensure_driver_is_ready();
  }

  bool startPingThread(std::function<void()> failCb) {
    std::lock_guard<std::recursive_mutex> lock(g_virtual_display_recovery_operation_mutex);
    if (terminal_shutdown_requested()) {
      return false;
    }
    return use_sunshine_driver() ? VDISPLAY_SUNSHINE::startPingThread(std::move(failCb)) : VDISPLAY_SUDOVDA::startPingThread(std::move(failCb));
  }

  void setWatchdogFeedingEnabled(bool enable) {
    std::lock_guard<std::recursive_mutex> lock(g_virtual_display_recovery_operation_mutex);
    // Terminal shutdown may still need to stop a feeding watchdog while the
    // cleanup dispatcher drains. Only reject a new enable request.
    if (terminal_shutdown_requested() && enable) {
      return;
    }
    if (use_sunshine_driver()) {
      VDISPLAY_SUNSHINE::setWatchdogFeedingEnabled(enable);
    } else {
      VDISPLAY_SUDOVDA::setWatchdogFeedingEnabled(enable);
    }
  }

  bool setRenderAdapterByName(const std::wstring &adapterName) {
    std::lock_guard<std::recursive_mutex> lock(g_virtual_display_recovery_operation_mutex);
    if (terminal_shutdown_requested()) {
      return false;
    }
    return use_sunshine_driver() ? VDISPLAY_SUNSHINE::setRenderAdapterByName(adapterName) : VDISPLAY_SUDOVDA::setRenderAdapterByName(adapterName);
  }

  bool setRenderAdapterWithMostDedicatedMemory() {
    std::lock_guard<std::recursive_mutex> lock(g_virtual_display_recovery_operation_mutex);
    if (terminal_shutdown_requested()) {
      return false;
    }
    return use_sunshine_driver() ? VDISPLAY_SUNSHINE::setRenderAdapterWithMostDedicatedMemory() : VDISPLAY_SUDOVDA::setRenderAdapterWithMostDedicatedMemory();
  }

  void ensureVirtualDisplayRegistryDefaults() {
    std::lock_guard<std::recursive_mutex> lock(g_virtual_display_recovery_operation_mutex);
    if (terminal_shutdown_requested()) {
      return;
    }
    if (use_sunshine_driver()) {
      VDISPLAY_SUNSHINE::ensureVirtualDisplayRegistryDefaults();
    } else {
      VDISPLAY_SUDOVDA::ensureVirtualDisplayRegistryDefaults();
    }
  }

  std::optional<VirtualDisplayCreationResult> createVirtualDisplay(
    const char *s_client_uid,
    const char *s_client_name,
    const char *s_hdr_profile,
    uint32_t width,
    uint32_t height,
    uint32_t fps,
    const GUID &guid,
    uint32_t base_fps_millihz,
    bool framegen_refresh_active,
    int framegen_refresh_multiplier,
    bool hdr_requested,
    bool allow_pending_enumeration,
    bool replace_existing
  ) {
    std::lock_guard<std::recursive_mutex> lock(g_virtual_display_recovery_operation_mutex);
    if (terminal_shutdown_requested()) {
      return std::nullopt;
    }
    if (use_sunshine_driver()) {
      return VDISPLAY_SUNSHINE::createVirtualDisplay(s_client_uid, s_client_name, s_hdr_profile, width, height, fps, guid, base_fps_millihz, framegen_refresh_active, framegen_refresh_multiplier, hdr_requested, allow_pending_enumeration, replace_existing);
    }
    return VDISPLAY_SUDOVDA::createVirtualDisplay(s_client_uid, s_client_name, s_hdr_profile, width, height, fps, guid, base_fps_millihz, framegen_refresh_active, framegen_refresh_multiplier, hdr_requested, replace_existing);
  }

  void applyHdrProfileToOutput(const char *s_client_name, const char *s_hdr_profile, const char *s_device_id) {
    if (use_sunshine_driver()) {
      VDISPLAY_SUNSHINE::applyHdrProfileToOutput(s_client_name, s_hdr_profile, s_device_id);
    } else {
      VDISPLAY_SUDOVDA::applyHdrProfileToOutput(s_client_name, s_hdr_profile, s_device_id);
    }
  }

  void restorePhysicalHdrProfiles() noexcept {
    try {
      VDISPLAY_SUNSHINE::restorePhysicalHdrProfiles();
    } catch (const std::exception &e) {
      BOOST_LOG(error) << "Failed to schedule Sunshine physical HDR restoration: " << e.what();
    } catch (...) {
      BOOST_LOG(error) << "Failed to schedule Sunshine physical HDR restoration.";
    }
    try {
      VDISPLAY_SUDOVDA::restorePhysicalHdrProfiles();
    } catch (const std::exception &e) {
      BOOST_LOG(error) << "Failed to schedule SudoVDA physical HDR restoration: " << e.what();
    } catch (...) {
      BOOST_LOG(error) << "Failed to schedule SudoVDA physical HDR restoration.";
    }
  }

  bool removeVirtualDisplay(const GUID &guid) {
    // Any explicit removal ends the recovery lifetime for this GUID. A new
    // creation receives a fresh owner/tracker generation.
    invalidate_virtual_display_recovery_owner(guid);
    invalidate_virtual_display_hdr_profile_operation(guid);
    std::lock_guard<std::recursive_mutex> lock(g_virtual_display_recovery_operation_mutex);
    return use_sunshine_driver() ? VDISPLAY_SUNSHINE::removeVirtualDisplay(guid) : VDISPLAY_SUDOVDA::removeVirtualDisplay(guid);
  }

  bool removeAllVirtualDisplays() {
    invalidate_all_virtual_display_recovery_owners();
    invalidate_all_virtual_display_hdr_profile_operations();
    std::lock_guard<std::recursive_mutex> lock(g_virtual_display_recovery_operation_mutex);
    return use_sunshine_driver() ? VDISPLAY_SUNSHINE::removeAllVirtualDisplays() : VDISPLAY_SUDOVDA::removeAllVirtualDisplays();
  }

  void schedule_virtual_display_recovery_monitor(const VirtualDisplayRecoveryParams &params) {
    if (use_sunshine_driver()) {
      VDISPLAY_SUNSHINE::schedule_virtual_display_recovery_monitor(params);
    } else {
      VDISPLAY_SUDOVDA::schedule_virtual_display_recovery_monitor(params);
    }
  }

  bool is_virtual_display_guid_tracked(const GUID &guid) {
    return use_sunshine_driver() ? VDISPLAY_SUNSHINE::is_virtual_display_guid_tracked(guid) : VDISPLAY_SUDOVDA::is_virtual_display_guid_tracked(guid);
  }

  std::optional<std::uint64_t> virtual_display_tracking_generation(const GUID &guid) {
    return use_sunshine_driver() ?
             VDISPLAY_SUNSHINE::virtual_display_tracking_generation(guid) :
             VDISPLAY_SUDOVDA::virtual_display_tracking_generation(guid);
  }

  bool is_virtual_display_tracking_generation_current(const GUID &guid, std::uint64_t generation) {
    const auto current = virtual_display_tracking_generation(guid);
    return current && *current == generation;
  }

  std::uint64_t claim_virtual_display_recovery_owner(const GUID &guid) {
    const auto tracking_generation = virtual_display_tracking_generation(guid);
    std::lock_guard<std::mutex> lock(g_recovery_owner_mutex);
    auto generation = ++g_next_recovery_owner_generation;
    if (generation == 0) {
      generation = ++g_next_recovery_owner_generation;
    }
    g_recovery_owner_generations.insert_or_assign(
      guid_key(guid),
      recovery_owner_t {
        .generation = generation,
        .tracking_generation = tracking_generation,
        .recreating = false,
      }
    );
    return generation;
  }

  bool is_virtual_display_recovery_owner_current(const GUID &guid, std::uint64_t generation) {
    const auto key = guid_key(guid);
    std::optional<std::uint64_t> tracking_generation;
    bool recreating = false;
    {
      std::lock_guard<std::mutex> lock(g_recovery_owner_mutex);
      const auto found = g_recovery_owner_generations.find(key);
      if (found == g_recovery_owner_generations.end() || found->second.generation != generation) {
        return false;
      }
      tracking_generation = found->second.tracking_generation;
      recreating = found->second.recreating;
    }
    // A boolean tracked check is ABA-prone: the same GUID can be removed and
    // recreated while an old monitor/retry is winding down. Bind ownership to
    // the driver's per-registration generation instead. A recreation window
    // admits only the expected no-tracker interval; a different tracker
    // generation is always stale, even while recreating.
    const auto current_tracking_generation = virtual_display_tracking_generation(guid);
    // The tracker query runs outside the owner lock to avoid coupling it to a
    // driver's own synchronization. Revalidate the same owner snapshot
    // before answering so a superseding stream cannot race through this
    // predicate while the query is in flight.
    {
      std::lock_guard<std::mutex> lock(g_recovery_owner_mutex);
      const auto found = g_recovery_owner_generations.find(key);
      if (found == g_recovery_owner_generations.end() ||
          found->second.generation != generation ||
          found->second.tracking_generation != tracking_generation ||
          found->second.recreating != recreating) {
        return false;
      }
    }
    if (current_tracking_generation) {
      return tracking_generation && *tracking_generation == *current_tracking_generation;
    }
    return recreating && tracking_generation;
  }

  bool begin_virtual_display_recovery_recreation(const GUID &guid, std::uint64_t generation) {
    const auto tracking_generation = virtual_display_tracking_generation(guid);
    if (!tracking_generation) {
      return false;
    }
    std::lock_guard<std::mutex> lock(g_recovery_owner_mutex);
    const auto found = g_recovery_owner_generations.find(guid_key(guid));
    if (found == g_recovery_owner_generations.end() ||
        found->second.generation != generation ||
        !found->second.tracking_generation ||
        *found->second.tracking_generation != *tracking_generation ||
        found->second.recreating) {
      return false;
    }
    found->second.recreating = true;
    return true;
  }

  bool cancel_virtual_display_recovery_recreation(const GUID &guid, std::uint64_t generation) {
    std::lock_guard<std::mutex> lock(g_recovery_owner_mutex);
    const auto found = g_recovery_owner_generations.find(guid_key(guid));
    if (found == g_recovery_owner_generations.end() || found->second.generation != generation) {
      return false;
    }
    found->second.recreating = false;
    return true;
  }

  bool refresh_virtual_display_recovery_owner_tracking(const GUID &guid, std::uint64_t generation) {
    const auto tracking_generation = virtual_display_tracking_generation(guid);
    if (!tracking_generation) {
      return false;
    }
    std::lock_guard<std::mutex> lock(g_recovery_owner_mutex);
    const auto found = g_recovery_owner_generations.find(guid_key(guid));
    if (found == g_recovery_owner_generations.end() || found->second.generation != generation) {
      return false;
    }
    found->second.tracking_generation = *tracking_generation;
    found->second.recreating = false;
    return true;
  }

  bool release_virtual_display_recovery_owner(const GUID &guid, std::uint64_t generation) {
    std::lock_guard<std::mutex> lock(g_recovery_owner_mutex);
    const auto found = g_recovery_owner_generations.find(guid_key(guid));
    if (found == g_recovery_owner_generations.end() || found->second.generation != generation) {
      return false;
    }
    g_recovery_owner_generations.erase(found);
    return true;
  }

  void invalidate_virtual_display_recovery_owner(const GUID &guid) {
    std::lock_guard<std::mutex> lock(g_recovery_owner_mutex);
    g_recovery_owner_generations.erase(guid_key(guid));
  }

  void invalidate_all_virtual_display_recovery_owners() {
    std::lock_guard<std::mutex> lock(g_recovery_owner_mutex);
    g_recovery_owner_generations.clear();
  }

  std::optional<std::string> resolveVirtualDisplayDeviceId(const std::wstring &display_name) {
    return use_sunshine_driver() ? VDISPLAY_SUNSHINE::resolveVirtualDisplayDeviceId(display_name) : VDISPLAY_SUDOVDA::resolveVirtualDisplayDeviceId(display_name);
  }

  std::optional<std::string> resolveVirtualDisplayDeviceIdForClient(const std::string &client_name) {
    return use_sunshine_driver() ? VDISPLAY_SUNSHINE::resolveVirtualDisplayDeviceIdForClient(client_name) : VDISPLAY_SUDOVDA::resolveVirtualDisplayDeviceIdForClient(client_name);
  }

  std::optional<std::string> resolveActiveVirtualDisplayDeviceId(const std::string &preferred_output_identifier, const std::string &client_name, bool allow_any_fallback) {
    return use_sunshine_driver() ? VDISPLAY_SUNSHINE::resolveActiveVirtualDisplayDeviceId(preferred_output_identifier, client_name, allow_any_fallback) : VDISPLAY_SUDOVDA::resolveActiveVirtualDisplayDeviceId(preferred_output_identifier, client_name, allow_any_fallback);
  }

  std::optional<std::string> resolveActiveVirtualDisplayDeviceIdForStableId(
    const std::string &stable_id,
    const std::string &preferred_output_identifier,
    const std::string &client_name,
    bool allow_any_fallback
  ) {
    if (use_sunshine_driver()) {
      return VDISPLAY_SUNSHINE::resolveActiveVirtualDisplayDeviceIdForStableId(stable_id, preferred_output_identifier, client_name, allow_any_fallback);
    }
    return VDISPLAY_SUDOVDA::resolveActiveVirtualDisplayDeviceId(preferred_output_identifier, client_name, allow_any_fallback);
  }

  std::optional<std::string> resolveAnyVirtualDisplayDeviceId() {
    return use_sunshine_driver() ? VDISPLAY_SUNSHINE::resolveAnyVirtualDisplayDeviceId() : VDISPLAY_SUDOVDA::resolveAnyVirtualDisplayDeviceId();
  }

  bool is_virtual_display_output(const std::string &output_identifier) {
    return use_sunshine_driver() ? VDISPLAY_SUNSHINE::is_virtual_display_output(output_identifier) : VDISPLAY_SUDOVDA::is_virtual_display_output(output_identifier);
  }

  bool is_virtual_display_selection(const std::string &output_identifier) {
    return use_sunshine_driver() ? VDISPLAY_SUNSHINE::is_virtual_display_selection(output_identifier) : VDISPLAY_SUDOVDA::is_virtual_display_selection(output_identifier);
  }

  uint64_t client_uuid_to_virtual_display_id(const GUID &client_guid) {
    return use_sunshine_driver() ? VDISPLAY_SUNSHINE::client_uuid_to_virtual_display_id(client_guid) : VDISPLAY_SUDOVDA::client_uuid_to_vdd_display_id(client_guid);
  }

  uuid_util::uuid_t virtualDisplayUuidFromStableId(const std::string &stable_id) {
    return VDISPLAY_SUNSHINE::virtualDisplayUuidFromStableId(stable_id);
  }

  GUID sharedVirtualDisplayGuid() {
    return use_sunshine_driver() ? VDISPLAY_SUNSHINE::sharedVirtualDisplayGuid() : VDISPLAY_SUDOVDA::sharedVirtualDisplayGuid();
  }

  bool is_sunshine_virtual_display_identity(
    const std::string &device_path,
    const std::string &friendly_name,
    const std::string &edid_manufacturer_id,
    const std::string &edid_product_code
  ) {
    return VDISPLAY_SUNSHINE::is_sunshine_virtual_display_identity(device_path, friendly_name, edid_manufacturer_id, edid_product_code);
  }

  std::vector<std::wstring> matchDisplay(std::wstring sMatch) {
    const auto displays = enumerateVirtualDisplays();
    std::vector<std::wstring> matches;
    for (const auto &display : displays) {
      if (display.device_name.find(sMatch) != std::wstring::npos) {
        matches.push_back(display.device_name);
      } else if (display.friendly_name.find(sMatch) != std::wstring::npos) {
        matches.push_back(display.friendly_name);
      }
    }
    return matches;
  }

  bool isVirtualDisplayDriverInstalled() {
    return use_sunshine_driver() ? VDISPLAY_SUNSHINE::isVirtualDisplayDriverInstalled() : VDISPLAY_SUDOVDA::isSudaVDADriverInstalled();
  }

  std::vector<VirtualDisplayInfo> enumerateVirtualDisplays() {
    return use_sunshine_driver() ? VDISPLAY_SUNSHINE::enumerateVirtualDisplays() : VDISPLAY_SUDOVDA::enumerateSudaVDADisplays();
  }

  uuid_util::uuid_t persistentVirtualDisplayUuid() {
    return use_sunshine_driver() ? VDISPLAY_SUNSHINE::persistentVirtualDisplayUuid() : VDISPLAY_SUDOVDA::persistentVirtualDisplayUuid();
  }

  bool has_active_physical_display() {
    return use_sunshine_driver() ? VDISPLAY_SUNSHINE::has_active_physical_display() : VDISPLAY_SUDOVDA::has_active_physical_display();
  }

  bool should_auto_enable_virtual_display() {
    return use_sunshine_driver() ? VDISPLAY_SUNSHINE::should_auto_enable_virtual_display() : VDISPLAY_SUDOVDA::should_auto_enable_virtual_display();
  }

  ensure_display_result ensure_display() {
    std::lock_guard<std::recursive_mutex> lock(g_virtual_display_recovery_operation_mutex);
    if (terminal_shutdown_requested()) {
      return ensure_display_result {false, false, false, {}};
    }
    return use_sunshine_driver() ? VDISPLAY_SUNSHINE::ensure_display() : VDISPLAY_SUDOVDA::ensure_display();
  }

  void cleanup_ensure_display(const ensure_display_result &result, bool probe_succeeded, bool allow_temporary_teardown) {
    // Cleanup is destructive and may be needed while terminal shutdown drains
    // already-started work, so serialize it but do not reject it on terminal.
    std::lock_guard<std::recursive_mutex> lock(g_virtual_display_recovery_operation_mutex);
    if (use_sunshine_driver()) {
      VDISPLAY_SUNSHINE::cleanup_ensure_display(result, probe_succeeded, allow_temporary_teardown);
    } else {
      VDISPLAY_SUDOVDA::cleanup_ensure_display(result, probe_succeeded, allow_temporary_teardown);
    }
  }

  bool has_retained_ensure_display() {
    return use_sunshine_driver() ? VDISPLAY_SUNSHINE::has_retained_ensure_display() : VDISPLAY_SUDOVDA::has_retained_ensure_display();
  }
}  // namespace VDISPLAY

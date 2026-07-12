#pragma once

#include "src/utility.h"
#include "src/uuid.h"

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>
// Windows.h includes the obsolete Winsock.h unless Winsock2 is included first.
// clang-format off
#include <winsock2.h>
#include <windows.h>
// clang-format on

namespace VDISPLAY {
  inline constexpr const char *VIRTUAL_DISPLAY_SELECTION = "sunshine:virtual_display";

  enum class DRIVER_STATUS {
    UNKNOWN = 1,
    OK = 0,
    FAILED = -1,
    VERSION_INCOMPATIBLE = -2,
    WATCHDOG_FAILED = -3
  };

  extern HANDLE VIRTUAL_DISPLAY_DRIVER_HANDLE;

  void closeVDisplayDevice();
  // First phase of process-exit shutdown. It invalidates recovery ownership
  // and asks both driver backends to stop recovery before cleanup workers are
  // drained. Do not use for normal operational driver reopens.
  void beginVDisplayDeviceShutdown();
  // Process-exit variant: aborts and quiesces recovery monitors before closing
  // driver handles. Do not use for normal operational driver reopens.
  void shutdownVDisplayDevice();
  // Internal recovery-operation serialization. A nonempty lease excludes a
  // watchdog-triggered close while a monitor is reopening/recreating a
  // display. Terminal shutdown makes future acquisitions fail, signals both
  // drivers, then waits for an already-held lease to drain.
  std::shared_ptr<void> acquire_virtual_display_recovery_operation() noexcept;
  // Virtual-display HDR profile work is asynchronous. These generation-bound
  // checks and write leases prevent an old profile task from applying to a
  // removed/reused virtual display after a newer create or teardown wins.
  std::uint64_t begin_virtual_display_hdr_profile_operation(const GUID &guid) noexcept;
  bool is_virtual_display_hdr_profile_operation_current(
    const GUID &guid,
    std::uint64_t generation
  ) noexcept;
  // Acquire immediately before registry mutation, not while resolving a
  // display path. This preserves a fast stream-start path while still
  // serializing a profile write against invalidation/new creation.
  std::shared_ptr<void> acquire_virtual_display_hdr_profile_operation(
    const GUID &guid,
    std::uint64_t generation
  ) noexcept;
  void invalidate_virtual_display_hdr_profile_operation(const GUID &guid) noexcept;
  void invalidate_all_virtual_display_hdr_profile_operations() noexcept;
  DRIVER_STATUS openVDisplayDevice();
  bool ensure_driver_is_ready();
  bool startPingThread(std::function<void()> failCb);
  void setWatchdogFeedingEnabled(bool enable);
  bool setRenderAdapterByName(const std::wstring &adapterName);
  bool setRenderAdapterWithMostDedicatedMemory();
  void ensureVirtualDisplayRegistryDefaults();

  struct VirtualDisplayCreationResult {
    std::optional<std::wstring> display_name;
    std::optional<std::string> device_id;
    std::optional<std::string> client_name;
    std::optional<std::wstring> monitor_device_path;
    bool reused_existing;
    std::chrono::steady_clock::time_point ready_since;
  };

  struct VirtualDisplayRecoveryParams {
    GUID guid;
    uint32_t width;
    uint32_t height;
    uint32_t fps;
    uint32_t base_fps_millihz = 0;
    bool framegen_refresh_active = false;
    int framegen_refresh_multiplier = 1;
    bool hdr_requested = false;
    std::string client_uid;
    std::string client_name;
    std::optional<std::string> hdr_profile;
    std::optional<std::wstring> display_name;
    std::optional<std::string> device_id;
    std::optional<std::wstring> monitor_device_path;
    unsigned int max_attempts = 3;
    // Acquires a lease immediately before recovery touches the driver/display
    // stack. The returned token is held through recreation; an empty token
    // defers the attempt.
    std::function<std::shared_ptr<void>()> acquire_display_handoff;
    // A successful recovery recreation registers the same GUID with a fresh
    // driver-tracker generation. The owner refreshes that expected generation
    // before continuing, without accepting a different stream's claim.
    std::function<bool()> refresh_recovery_owner_after_recreation;
    // Opens a narrow owner window immediately before a recovery removes and
    // recreates its own tracked GUID. During that window only the expected
    // no-tracker gap is accepted; a different tracker generation still aborts
    // the stale monitor. A failed recreation must cancel this window.
    std::function<bool()> begin_recovery_owner_recreation;
    std::function<void()> cancel_recovery_owner_recreation;
    std::function<void(const VirtualDisplayCreationResult &)> on_recovery_success;
    // Releases the generation-specific recovery owner when this monitor exits.
    // It must tolerate being superseded by a newer monitor for the same GUID.
    std::function<void()> on_monitor_exit;
    std::function<bool()> should_abort;
  };

  std::optional<VirtualDisplayCreationResult> createVirtualDisplay(
    const char *s_client_uid,
    const char *s_client_name,
    const char *s_hdr_profile,
    uint32_t width,
    uint32_t height,
    uint32_t fps,
    const GUID &guid,
    uint32_t base_fps_millihz = 0,
    bool framegen_refresh_active = false,
    int framegen_refresh_multiplier = 1,
    bool hdr_requested = false,
    bool allow_pending_enumeration = false,
    bool replace_existing = true
  );

  // Apply an HDR color profile to a physical output (best-effort).
  // If s_hdr_profile is null/empty, we fall back to matching by client name.
  void applyHdrProfileToOutput(
    const char *s_client_name,
    const char *s_hdr_profile,
    const char *s_device_id
  );

  // Restore any physical display color profiles that Sunshine overrode for streaming.
  // Virtual display associations are not restored.
  void restorePhysicalHdrProfiles() noexcept;
  bool removeVirtualDisplay(const GUID &guid);
  bool removeAllVirtualDisplays();
  void schedule_virtual_display_recovery_monitor(const VirtualDisplayRecoveryParams &params);
  bool is_virtual_display_guid_tracked(const GUID &guid);
  std::optional<std::uint64_t> virtual_display_tracking_generation(const GUID &guid);
  bool is_virtual_display_tracking_generation_current(const GUID &guid, std::uint64_t generation);
  // Distinct from driver tracking: every session/monitor claim increments even
  // when shared mode reuses an already-tracked GUID.
  std::uint64_t claim_virtual_display_recovery_owner(const GUID &guid);
  bool is_virtual_display_recovery_owner_current(const GUID &guid, std::uint64_t generation);
  bool begin_virtual_display_recovery_recreation(const GUID &guid, std::uint64_t generation);
  bool cancel_virtual_display_recovery_recreation(const GUID &guid, std::uint64_t generation);
  bool refresh_virtual_display_recovery_owner_tracking(const GUID &guid, std::uint64_t generation);
  // Release only the matching claimant so an older monitor cannot erase a
  // newer stream's owner. Whole-process and terminal cleanup use invalidate.
  bool release_virtual_display_recovery_owner(const GUID &guid, std::uint64_t generation);
  void invalidate_virtual_display_recovery_owner(const GUID &guid);
  void invalidate_all_virtual_display_recovery_owners();

  std::optional<std::string> resolveVirtualDisplayDeviceId(const std::wstring &display_name);
  std::optional<std::string> resolveVirtualDisplayDeviceIdForClient(const std::string &client_name);
  std::optional<std::string> resolveActiveVirtualDisplayDeviceId(
    const std::string &preferred_output_identifier,
    const std::string &client_name,
    bool allow_any_fallback = true
  );
  std::optional<std::string> resolveActiveVirtualDisplayDeviceIdForStableId(
    const std::string &stable_id,
    const std::string &preferred_output_identifier,
    const std::string &client_name,
    bool allow_any_fallback = true
  );
  std::optional<std::string> resolveAnyVirtualDisplayDeviceId();
  bool is_virtual_display_output(const std::string &output_identifier);
  bool is_virtual_display_selection(const std::string &output_identifier);

  uint64_t client_uuid_to_virtual_display_id(const GUID &client_guid);
  uuid_util::uuid_t virtualDisplayUuidFromStableId(const std::string &stable_id);
  GUID sharedVirtualDisplayGuid();
  bool is_sunshine_virtual_display_identity(
    const std::string &device_path,
    const std::string &friendly_name,
    const std::string &edid_manufacturer_id,
    const std::string &edid_product_code
  );

  std::vector<std::wstring> matchDisplay(std::wstring sMatch);

  struct VirtualDisplayInfo {
    std::wstring device_name;
    std::wstring friendly_name;
    bool is_active;
    int width;
    int height;
  };

  bool isVirtualDisplayDriverInstalled();
  std::vector<VirtualDisplayInfo> enumerateVirtualDisplays();

  uuid_util::uuid_t persistentVirtualDisplayUuid();
  bool has_active_physical_display();
  bool should_auto_enable_virtual_display();

  struct ensure_display_result {
    bool success;
    bool created_temporary;
    bool tracks_temporary_for_probe;
    GUID temporary_guid;
  };

  /**
   * @brief Ensures a display is available for capture/encoding.
   * If no active physical displays exist, automatically creates a temporary virtual display.
   * @return Result indicating success and whether a temporary display was created.
   */
  ensure_display_result ensure_display();

  /**
   * @brief Cleans up temporary display created by ensure_display().
   * @param result The result from ensure_display() call.
   * @param probe_succeeded True when probe finished successfully.
   * @param allow_temporary_teardown False keeps the temporary display retained.
   */
  void cleanup_ensure_display(const ensure_display_result &result, bool probe_succeeded, bool allow_temporary_teardown = true);

  /**
   * @brief Returns true when ensure_display() is currently retaining a temporary display for probe retries.
   */
  bool has_retained_ensure_display();
}  // namespace VDISPLAY

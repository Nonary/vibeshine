#pragma once

#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#ifdef _WIN32
  #include <map>
  #include <tuple>

  #include <display_device/windows/win_api_layer_interface.h>
  #include <display_device/windows/win_api_recovery.h>
#endif

namespace display_recovery_safety {
  enum class PhysicalDisplayState {
    unknown,
    none_connected,
    connected_inactive,
    active,
  };

  enum class DisplayTargetPresence {
    missing,
    present_inactive,
    present_active,
    unknown,
  };

  struct DisplayTargetObservation {
    bool available {false};
    bool active {false};
    bool virtual_output {false};
  };

  // An empty, successful CCD query proves that no physical target is connected.
  // An empty high-level device enumeration does not: that API also returns an
  // empty list when querying the OS or resolving monitor identities fails.
  inline PhysicalDisplayState classify_physical_displays(
    const bool query_succeeded,
    const std::span<const DisplayTargetObservation> targets
  ) {
    if (!query_succeeded) {
      return PhysicalDisplayState::unknown;
    }

    auto result = PhysicalDisplayState::none_connected;
    for (const auto &target : targets) {
      if (target.virtual_output) {
        continue;
      }
      // Windows can retain ACTIVE after unplugging a monitor. Do not authorize
      // a headless exemption while the display stack reports this transition.
      // https://learn.microsoft.com/windows/win32/api/wingdi/ns-wingdi-displayconfig_path_target_info
      if (target.active && !target.available) {
        return PhysicalDisplayState::unknown;
      }
      if (!target.available) {
        continue;
      }
      if (target.active) {
        result = PhysicalDisplayState::active;
      } else if (result != PhysicalDisplayState::active) {
        result = PhysicalDisplayState::connected_inactive;
      }
    }
    return result;
  }

  // A temporarily unplugged physical monitor does not erase its recovery
  // obligation. Callers must retain that baseline and its durable guard.
  inline bool is_headless_without_baseline(
    const PhysicalDisplayState state,
    const bool has_retained_baseline
  ) {
    return state == PhysicalDisplayState::none_connected && !has_retained_baseline;
  }

  namespace detail {
    constexpr char ascii_lower(const char value) {
      return value >= 'A' && value <= 'Z' ? static_cast<char>(value + ('a' - 'A')) : value;
    }

    inline bool starts_with_ci(const std::string_view value, const std::string_view prefix) {
      if (value.size() < prefix.size()) {
        return false;
      }
      for (std::size_t i = 0; i < prefix.size(); ++i) {
        if (ascii_lower(value[i]) != ascii_lower(prefix[i])) {
          return false;
        }
      }
      return true;
    }

    inline std::size_t find_ci(const std::string_view value, const std::string_view needle) {
      if (value.size() >= needle.size()) {
        for (std::size_t i = 0; i <= value.size() - needle.size(); ++i) {
          if (starts_with_ci(value.substr(i), needle)) {
            return i;
          }
        }
      }
      return std::string_view::npos;
    }
  }  // namespace detail

  // These are the managed driver identities used by the existing helper and
  // VDISPLAY classifiers. Keep the probe independent of driver control and
  // high-level enumeration, both of which can initiate display recovery.
  inline bool is_managed_virtual_identity(
    const std::string_view monitor_path,
    const std::string_view edid_manufacturer = {}
  ) {
    using detail::find_ci;
    using detail::starts_with_ci;
    if (edid_manufacturer.size() == 3 &&
        (starts_with_ci(edid_manufacturer, "SDD") || starts_with_ci(edid_manufacturer, "SMK"))) {
      return true;
    }
    for (const auto marker : {"SunshineVirtualDisplay", "Sunshine Virtual Display", "SUDOVDA", "SUDOMAKER"}) {
      if (find_ci(monitor_path, marker) != std::string_view::npos) {
        return true;
      }
    }

    constexpr std::string_view display_prefix = "DISPLAY#";
    const auto prefix = find_ci(monitor_path, display_prefix);
    if (prefix == std::string_view::npos) {
      return false;
    }
    const auto begin = prefix + display_prefix.size();
    const auto end = monitor_path.find('#', begin);
    if (end == std::string_view::npos) {
      return false;
    }
    const auto hardware_id = monitor_path.substr(begin, end - begin);
    return starts_with_ci(hardware_id, "SMK") ||
           starts_with_ci(hardware_id, "SDD4") ||
           starts_with_ci(hardware_id, "SDD5");
  }

#ifdef _WIN32
  // A partial high-level enumeration can silently omit the virtual target
  // after a per-target identity/name failure. Before destructive recovery,
  // resolve its stable identity directly from all CCD paths. Unidentified
  // connected paths prevent a negative result from proving target absence.
  inline DisplayTargetPresence probe_display_target_presence(
    display_device::WinApiLayerInterface &api,
    const std::string_view expected_device_id,
    const std::string_view expected_monitor_path
  ) {
    try {
      const display_device::DisplayRecoveryBehaviorGuard recovery_guard {
        display_device::DisplayRecoveryBehavior::Skip
      };
      const auto data = api.queryDisplayConfig(display_device::QueryType::All);
      if (!data) {
        return DisplayTargetPresence::unknown;
      }
      const auto matches = [](const std::string_view expected, const std::string_view actual) {
        return !expected.empty() && expected.size() == actual.size() && detail::starts_with_ci(actual, expected);
      };
      std::map<std::tuple<LONG, DWORD, UINT32>, std::pair<std::string, std::string>> identities;
      bool uncertain = false;
      bool matched_inactive = false;
      for (const auto &path : data->m_paths) {
        const bool available = path.targetInfo.targetAvailable != FALSE;
        const bool active = (path.flags & DISPLAYCONFIG_PATH_ACTIVE) != 0;
        if (!available) {
          // ACTIVE can linger across a switch or unplug. It is not proof of
          // a usable target or of a genuinely absent one.
          uncertain |= active;
          continue;
        }
        const auto key = std::tuple {path.targetInfo.adapterId.HighPart, path.targetInfo.adapterId.LowPart, path.targetInfo.id};
        auto identity = identities.find(key);
        if (identity == identities.end()) {
          identity = identities.emplace(key, std::pair {api.getDeviceId(path), api.getMonitorDevicePath(path)}).first;
        }
        const auto &[device_id, monitor_path] = identity->second;
        if (matches(expected_device_id, device_id) || matches(expected_monitor_path, monitor_path)) {
          if (active) {
            return DisplayTargetPresence::present_active;
          }
          matched_inactive = true;
          continue;
        }
        // Both identity fields must be trustworthy before excluding this
        // connected target. An ID may fall back to a different hash when
        // EDID/path lookup fails, so one nonmatching field alone is not proof.
        // With only the expected hash, a different nonempty ID could be the
        // same target's fallback hash after a transient EDID/SetupAPI failure.
        // A retained monitor path is needed to exclude that possibility.
        uncertain |= device_id.empty() || monitor_path.empty() || expected_monitor_path.empty();
      }
      if (uncertain) {
        return DisplayTargetPresence::unknown;
      }
      return matched_inactive ? DisplayTargetPresence::present_inactive : DisplayTargetPresence::missing;
    } catch (...) {
      return DisplayTargetPresence::unknown;
    }
  }

  // Read only. In particular, never use enumAvailableDevices or recovery APIs
  // to prove headlessness: they can hide query errors or activate topology.
  inline PhysicalDisplayState probe_physical_displays(display_device::WinApiLayerInterface &api) {
    try {
      const display_device::DisplayRecoveryBehaviorGuard recovery_guard {
        display_device::DisplayRecoveryBehavior::Skip
      };
      const auto display_data = api.queryDisplayConfig(display_device::QueryType::All);
      if (!display_data) {
        return PhysicalDisplayState::unknown;
      }

      std::vector<DisplayTargetObservation> targets;
      targets.reserve(display_data->m_paths.size());
      // QDC_ALL_PATHS contains alternative sources for the same target. Read
      // identity once per target rather than reopening its registry data for
      // every possible source/target combination.
      std::map<std::tuple<LONG, DWORD, UINT32>, bool> virtual_targets;
      for (const auto &path : display_data->m_paths) {
        DisplayTargetObservation target {
          .available = path.targetInfo.targetAvailable != FALSE,
          .active = (path.flags & DISPLAYCONFIG_PATH_ACTIVE) != 0,
        };
        if (target.available || target.active) {
          const auto key = std::tuple {path.targetInfo.adapterId.HighPart, path.targetInfo.adapterId.LowPart, path.targetInfo.id};
          if (const auto known = virtual_targets.find(key); known != virtual_targets.end()) {
            target.virtual_output = known->second;
            targets.push_back(target);
            continue;
          }
          // INDIRECT_WIRED includes real USB monitors and must remain physical.
          // Only INDIRECT_VIRTUAL is sufficient connector evidence by itself.
          target.virtual_output = path.targetInfo.outputTechnology == DISPLAYCONFIG_OUTPUT_TECHNOLOGY_INDIRECT_VIRTUAL;
          if (!target.virtual_output) {
            target.virtual_output = is_managed_virtual_identity(api.getMonitorDevicePath(path));
          }
          if (!target.virtual_output) {
            if (const auto edid = display_device::EdidData::parse(api.getEdid(path))) {
              target.virtual_output = is_managed_virtual_identity({}, edid->m_manufacturer_id);
            }
          }
          // Missing path/EDID is deliberately treated as possibly physical.
          // It cannot grant permission to discard physical recovery protection.
          virtual_targets.emplace(key, target.virtual_output);
        }
        targets.push_back(target);
      }
      return classify_physical_displays(true, targets);
    } catch (...) {
      return PhysicalDisplayState::unknown;
    }
  }
#endif
}  // namespace display_recovery_safety

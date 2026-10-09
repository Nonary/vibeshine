#pragma once

#include <algorithm>
#include <chrono>
#include <cctype>
#include <set>
#include <string>
#include <utility>
#include <vector>

#ifdef _WIN32
  #include "src/platform/windows/display_recovery_safety.h"

  #include <map>
#endif

namespace display_helper::physical_recovery {
  using Topology = std::vector<std::vector<std::string>>;

  struct Device {
    std::string id;
    bool physical = false;
    bool active = false;
  };

  struct Outcome {
    bool physical_available = false;
    bool mutation_attempted = false;
  };

  inline std::string normalized_id(std::string id) {
    id.erase(id.begin(), std::find_if(id.begin(), id.end(), [](unsigned char ch) { return !std::isspace(ch); }));
    id.erase(std::find_if(id.rbegin(), id.rend(), [](unsigned char ch) { return !std::isspace(ch); }).base(), id.end());
    std::transform(id.begin(), id.end(), id.begin(), [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    return id;
  }

  inline std::set<std::string> device_ids(const Topology &topology) {
    std::set<std::string> ids;
    for (const auto &group : topology) {
      for (const auto &id : group) {
        if (auto normalized = normalized_id(id); !normalized.empty()) {
          ids.insert(std::move(normalized));
        }
      }
    }
    return ids;
  }

#ifdef _WIN32
  /// Unlike the snapshot codec's managed-driver classification, physical
  /// rescue also excludes other vendors' indirect virtual targets. Require
  /// current CCD connection evidence before activating or reporting a screen.
  inline std::vector<Device> enumerate_devices(display_device::WinApiLayerInterface &api) {
    try {
      const display_device::DisplayRecoveryBehaviorGuard recovery_guard {display_device::DisplayRecoveryBehavior::Skip};
      const auto data = api.queryDisplayConfig(display_device::QueryType::All);
      if (!data) {
        return {};
      }
      std::map<std::string, Device> unique;
      for (const auto &path : data->m_paths) {
        if (!path.targetInfo.targetAvailable) {
          continue;
        }
        const auto id = api.getDeviceId(path);
        if (id.empty()) {
          continue;
        }
        const auto key = normalized_id(id);
        if (const auto known = unique.find(key); known != unique.end()) {
          known->second.active |= (path.flags & DISPLAYCONFIG_PATH_ACTIVE) != 0;
          continue;
        }
        bool virtual_output = path.targetInfo.outputTechnology == DISPLAYCONFIG_OUTPUT_TECHNOLOGY_INDIRECT_VIRTUAL ||
                              display_recovery_safety::is_managed_virtual_identity(api.getMonitorDevicePath(path));
        if (!virtual_output) {
          if (const auto edid = display_device::EdidData::parse(api.getEdid(path))) {
            virtual_output = display_recovery_safety::is_managed_virtual_identity({}, edid->m_manufacturer_id);
          }
        }
        unique.emplace(key, Device {id, !virtual_output, (path.flags & DISPLAYCONFIG_PATH_ACTIVE) != 0});
      }
      std::vector<Device> result;
      result.reserve(unique.size());
      for (auto &[id, device] : unique) {
        result.push_back(std::move(device));
      }
      return result;
    } catch (...) {
      return {};
    }
  }
#endif

  /// A visibility rescue is an extension of the live desktop, never a partial
  /// snapshot restore. Only connected physical IDs from the authoritative raw
  /// baseline are eligible; absent/empty baselines do not authorize enabling a
  /// deliberately disabled monitor. The caller must serialize this with APPLY
  /// and supply the same cancellation fence used by its recovery transaction.
  /// No snapshot or durable recovery evidence is modified by this operation.
  template <typename Enumerate, typename CaptureTopology, typename ApplyTopology, typename Cancelled, typename Wait>
  Outcome ensure_visible(
    const Topology &baseline,
    const std::vector<std::string> &exclusions,
    Enumerate enumerate,
    CaptureTopology capture_topology,
    ApplyTopology apply_topology,
    Cancelled cancelled,
    Wait wait) {
    Outcome outcome;
    if (cancelled()) {
      return outcome;
    }
    const auto devices = enumerate();
    if (cancelled()) {
      return outcome;
    }
    const auto before = capture_topology();
    if (cancelled()) {
      return outcome;
    }
    const auto before_ids = device_ids(before);
    const auto visible = [&](const auto &enumerated, const auto &active_ids) {
      return std::any_of(enumerated.begin(), enumerated.end(), [&](const auto &device) {
        return device.physical && device.active && active_ids.contains(normalized_id(device.id));
      });
    };
    if (visible(devices, before_ids)) {
      outcome.physical_available = true;
      return outcome;
    }
    // A failed topology read must not turn an additive rescue into replacement
    // of an unknown desktop. An inconsistent active-physical read also waits
    // for the next recovery attempt instead of making a gratuitous change.
    if (before_ids.empty() || std::any_of(devices.begin(), devices.end(), [](const auto &device) {
          return device.physical && device.active;
        })) {
      return outcome;
    }

    auto allowed = device_ids(baseline);
    for (const auto &id : exclusions) {
      allowed.erase(normalized_id(id));
    }
    const auto candidate = std::find_if(devices.begin(), devices.end(), [&](const auto &device) {
      return device.physical && allowed.contains(normalized_id(device.id)) && !before_ids.contains(normalized_id(device.id));
    });
    if (candidate == devices.end() || cancelled()) {
      return outcome;
    }

    auto extended = before;
    extended.push_back({candidate->id});
    if (cancelled()) {
      return outcome;
    }
    outcome.mutation_attempted = true;
    const bool accepted = apply_topology(extended);
    if (cancelled()) {
      return outcome;
    }
    // A setter acknowledgment is insufficient: Windows may select a different
    // topology. Fresh reads must show a physical output and all previous paths.
    for (int poll = 0; poll < (accepted ? 20 : 1); ++poll) {
      if (cancelled()) {
        return outcome;
      }
      const auto current_devices = enumerate();
      if (cancelled()) {
        return outcome;
      }
      const auto current = capture_topology();
      if (cancelled()) {
        return outcome;
      }
      const auto current_ids = device_ids(current);
      if (!std::includes(current_ids.begin(), current_ids.end(), before_ids.begin(), before_ids.end())) {
        // If Windows dropped an existing path, repair additively as well. Keep
        // newly active paths, including permanent virtual displays, and fence
        // this second mutation against a superseding session.
        if (!current_ids.empty() && !cancelled()) {
          auto repaired = current;
          for (const auto &group : before) {
            std::vector<std::string> missing;
            for (const auto &id : group) {
              if (!current_ids.contains(normalized_id(id))) {
                missing.push_back(id);
              }
            }
            if (!missing.empty()) {
              repaired.push_back(std::move(missing));
            }
          }
          if (!cancelled()) {
            (void) apply_topology(repaired);
          }
        }
        return outcome;
      }
      if (visible(current_devices, current_ids)) {
        outcome.physical_available = true;
        return outcome;
      }
      if (poll + 1 < (accepted ? 20 : 1) && !wait(std::chrono::milliseconds(100))) {
        return outcome;
      }
    }
    return outcome;
  }
}  // namespace display_helper::physical_recovery

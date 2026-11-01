/**
 * @file src/platform/windows/display_coordinator_restore.h
 * @brief Exact physical refresh capture and coordinator restore ordering.
 */
#pragma once

#include "src/display_helper_builder.h"
#include "src/remote_display_topology.h"

#include <limits>
#include <set>
#include <string>

namespace display_helper {
  inline std::optional<std::pair<unsigned int, unsigned int>> exact_baseline_refresh(
    const display_device::FloatingPoint &refresh
  ) {
    const auto *rate = std::get_if<display_device::Rational>(&refresh);
    if (!rate || rate->m_numerator == 0 || rate->m_denominator == 0) {
      return std::nullopt;
    }
    return std::pair {rate->m_numerator, rate->m_denominator};
  }

  inline std::optional<std::pair<unsigned int, unsigned int>> exact_baseline_refresh_from_json(
    const nlohmann::json &refresh
  ) {
    if (!refresh.is_object()) {
      return std::nullopt;
    }
    const auto valid_part = [&](const char *name) {
      const auto value = refresh.find(name);
      return value != refresh.end() && value->is_number_integer() && *value > 0 &&
             *value <= std::numeric_limits<unsigned int>::max();
    };
    if (!valid_part("numerator") || !valid_part("denominator")) {
      return std::nullopt;
    }
    return std::pair {refresh["numerator"].get<unsigned int>(), refresh["denominator"].get<unsigned int>()};
  }

  inline void append_physical_refresh_override(
    const remote_display_topology::node_t &node,
    const std::string &device_id,
    display_helper_integration::DisplayTopologyDefinition &topology
  ) {
    // Never derive a restore rate from rounded UI Hz, encoder FPS, or a virtual
    // client's configured/requested mode. Missing exact metadata means no override.
    if (node.physical && node.baseline_refresh_rate) {
      topology.device_refresh_rate_overrides[device_id] = *node.baseline_refresh_rate;
    }
  }

  // Coordinator geometry must be restored before its best-effort refresh stage.
  // Use the same temporary exact-mode operation as snapshot restoration.
  // Return geometry success so a refresh-only failure cannot latch the baseline guard.
  template<typename Device, typename Topology, typename ReportFailure>
  bool restore_composed_topology(Device &device, const Topology &layout, ReportFailure report_failure) {
    if (!layout.topology.empty() && !device.isTopologyTheSame(device.getCurrentTopology(), layout.topology) && !device.setTopology(layout.topology)) {
      report_failure("topology", "");
      return false;
    }
    if (layout.primary_device && !layout.primary_device->empty() && !device.setAsPrimary(*layout.primary_device)) {
      report_failure("primary", *layout.primary_device);
      return false;
    }
    for (const auto &[id, origin] : layout.monitor_positions) {
      if (!device.setDisplayOrigin(id, origin)) {
        report_failure("origin", id);
        return false;
      }
    }
    if (layout.device_refresh_rate_overrides.empty()) {
      return true;
    }
    std::set<std::string> requested;
    for (const auto &[id, rate] : layout.device_refresh_rate_overrides) {
      requested.insert(id);
    }
    const auto report_refresh_failure = [&](const char *reason) {
      decltype(device.getCurrentDisplayModes(requested)) readback;
      try {
        readback = device.getCurrentDisplayModes(requested);
      } catch (...) {
        // Failure to read back is diagnostic, not a geometry failure.
      }
      for (const auto &id : requested) {
        const auto &rate = layout.device_refresh_rate_overrides.at(id);
        const auto found = readback.find(id);
        const auto actual = found == readback.end() ? std::string {"unavailable"} :
                                                      std::to_string(found->second.m_refresh_rate.m_numerator) + "/" +
                                                        std::to_string(found->second.m_refresh_rate.m_denominator);
        report_failure(std::string {reason} + " (requested " + std::to_string(rate.first) + "/" + std::to_string(rate.second) + ", readback " + actual + ")", id);
      }
    };
    try {
      std::set<std::string> active;
      for (const auto &group : device.getCurrentTopology()) {
        active.insert(group.begin(), group.end());
      }
      std::erase_if(requested, [&](const auto &id) {
        return !active.contains(id);
      });
      // Exclusive APPLY deliberately keeps physical refresh targets inactive.
      if (requested.empty()) {
        return true;
      }
      auto modes = device.getCurrentDisplayModes(requested);
      for (const auto &id : requested) {
        const auto found = modes.find(id);
        if (found == modes.end()) {
          report_refresh_failure("refresh read");
          return true;
        }
        const auto &rate = layout.device_refresh_rate_overrides.at(id);
        found->second.m_refresh_rate = {rate.first, rate.second};
      }
      if (!device.setDisplayModesExactTemporary(modes)) {
        report_refresh_failure("exact refresh");
      }
    } catch (...) {
      report_refresh_failure("refresh exception");
    }
    return true;
  }

}  // namespace display_helper

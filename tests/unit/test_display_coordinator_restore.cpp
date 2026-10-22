/**
 * @file tests/unit/test_display_coordinator_restore.cpp
 * @brief Coordinator capture/compose/restore tests at the production boundaries.
 */
#include "src/platform/windows/display_coordinator_restore.h"

#include <gtest/gtest.h>
#include <stdexcept>

namespace {
  using display_device::Rational;
  using display_helper_integration::DisplayTopologyDefinition;
  using Topology = std::vector<std::vector<std::string>>;

  struct Mode {
    display_device::Resolution m_resolution;
    Rational m_refresh_rate;
  };

  using Modes = std::map<std::string, Mode>;

  DisplayTopologyDefinition saved_layout() {
    return {
      .topology = {{"MSI"}, {"AOC"}},
      .monitor_positions = {{"MSI", {0, 0}}, {"AOC", {3840, 0}}},
      .primary_device = "MSI",
      .device_refresh_rate_overrides = {{"MSI", {240, 4}}},
    };
  }

  struct Device {
    Topology topology {{"VDD"}};
    std::vector<std::string> calls;
    Modes applied;
    std::map<std::string, display_device::Point> origins {{"MSI", {1920, 0}}, {"AOC", {0, 0}}};
    bool topology_ok = true;
    bool primary_ok = true;
    bool origin_ok = true;
    bool refresh_ok = true;
    bool refresh_throws = false;
    bool modes_readable = true;

    auto getCurrentTopology() {
      return topology;
    }

    bool isTopologyTheSame(const Topology &a, const Topology &b) {
      return a == b;
    }

    bool setTopology(const Topology &value) {
      calls.push_back("topology");
      if (topology_ok) {
        topology = value;
      }
      return topology_ok;
    }

    bool setAsPrimary(const std::string &id) {
      calls.push_back("primary");
      if (primary_ok) {
        const auto origin = origins[id];
        for (auto &[_, point] : origins) {
          point.m_x -= origin.m_x;
          point.m_y -= origin.m_y;
        }
      }
      return primary_ok;
    }

    bool setDisplayOrigin(const std::string &id, const display_device::Point &point) {
      calls.push_back("origin:" + id);
      if (origin_ok) {
        origins[id] = point;
      }
      return origin_ok;
    }

    Modes getCurrentDisplayModes(const std::set<std::string> &ids) {
      calls.push_back("read modes");
      if (!modes_readable) {
        return {};
      }
      Modes modes;
      if (ids.contains("MSI")) {
        modes["MSI"] = {{3840, 2160}, {59997, 1000}};
      }
      if (ids.contains("AOC")) {
        modes["AOC"] = {{1920, 1080}, {60000, 1001}};
      }
      return modes;
    }

    bool setDisplayModesExactTemporary(const Modes &modes) {
      calls.push_back("refresh");
      if (refresh_throws) {
        throw std::runtime_error("refresh failed");
      }
      applied = modes;
      return refresh_ok;
    }
  };

  const auto ignore_failure = [](const auto &, const auto &) {
  };
}  // namespace

TEST(DisplayCoordinatorRestore, ReactivatesThenRestoresPrimaryOriginsAndVerbatimRefresh) {
  Device device;
  EXPECT_TRUE(display_helper::restore_composed_topology(device, saved_layout(), ignore_failure));
  EXPECT_EQ(device.calls, (std::vector<std::string> {"topology", "primary", "origin:AOC", "origin:MSI", "read modes", "refresh"}));
  ASSERT_TRUE(device.applied.contains("MSI"));
  EXPECT_EQ(device.applied.at("MSI").m_refresh_rate.m_numerator, 240u);
  EXPECT_EQ(device.applied.at("MSI").m_refresh_rate.m_denominator, 4u);
  EXPECT_EQ(device.applied.at("MSI").m_resolution.m_width, 3840u);
  EXPECT_EQ(device.applied.at("MSI").m_resolution.m_height, 2160u);
  EXPECT_EQ(device.origins.at("MSI").m_x, 0);
  EXPECT_EQ(device.origins.at("AOC").m_x, 3840);
}

TEST(DisplayCoordinatorRestore, RefreshFailureWarnsWithDeviceAndRequestedReadbackRationals) {
  Device device;
  device.refresh_ok = false;
  std::vector<std::pair<std::string, std::string>> warnings;
  EXPECT_TRUE(display_helper::restore_composed_topology(device, saved_layout(), [&](const auto &reason, const auto &id) {
    warnings.emplace_back(reason, id);
  }));
  ASSERT_EQ(warnings.size(), 1u);
  EXPECT_EQ(warnings.front().second, "MSI");
  EXPECT_NE(warnings.front().first.find("requested 240/4"), std::string::npos);
  EXPECT_NE(warnings.front().first.find("readback 59997/1000"), std::string::npos);
  EXPECT_EQ(device.origins.at("MSI").m_x, 0);
  EXPECT_EQ(device.origins.at("AOC").m_x, 3840);
}

TEST(DisplayCoordinatorRestore, RefreshReadFailureOrExceptionPreservesGeometrySuccess) {
  for (const bool missing_modes : {false, true}) {
    Device device;
    device.modes_readable = !missing_modes;
    device.refresh_throws = !missing_modes;
    std::vector<std::string> warnings;
    EXPECT_TRUE(display_helper::restore_composed_topology(device, saved_layout(), [&](const auto &reason, const auto &id) {
      EXPECT_EQ(id, "MSI");
      warnings.push_back(reason);
    }));
    ASSERT_EQ(warnings.size(), 1u);
    EXPECT_NE(warnings.front().find("requested 240/4"), std::string::npos);
    EXPECT_NE(warnings.front().find(missing_modes ? "readback unavailable" : "readback 59997/1000"), std::string::npos);
  }
}

TEST(DisplayCoordinatorRestore, GeometryFailureStopsBeforeRefresh) {
  for (const auto stage : {"topology", "primary", "origin"}) {
    SCOPED_TRACE(stage);
    Device device;
    device.topology_ok = std::string(stage) != "topology";
    device.primary_ok = std::string(stage) != "primary";
    device.origin_ok = std::string(stage) != "origin";
    EXPECT_FALSE(display_helper::restore_composed_topology(device, saved_layout(), ignore_failure));
    EXPECT_TRUE(device.applied.empty());
    EXPECT_EQ(device.calls.back(), std::string(stage) == "origin" ? "origin:AOC" : stage);
  }
}

TEST(DisplayCoordinatorRestore, ExclusiveApplySkipsInactivePhysicalRefreshTargets) {
  Device device;
  auto layout = saved_layout();
  layout.topology = {{"VDD"}};
  layout.primary_device = "VDD";
  layout.monitor_positions = {{"VDD", {0, 0}}};
  EXPECT_TRUE(display_helper::restore_composed_topology(device, layout, ignore_failure));
  EXPECT_EQ(device.calls, (std::vector<std::string> {"primary", "origin:VDD"}));
  EXPECT_TRUE(device.applied.empty());
}

TEST(DisplayCoordinatorRestore, RefreshOnlyRequestUsesCurrentActiveResolution) {
  Device device;
  device.topology = {{"MSI"}};
  DisplayTopologyDefinition layout;
  layout.device_refresh_rate_overrides = {{"MSI", {60000, 1001}}};
  ASSERT_TRUE(display_helper::restore_composed_topology(device, layout, ignore_failure));
  EXPECT_EQ(device.calls, (std::vector<std::string> {"read modes", "refresh"}));
  EXPECT_EQ(device.applied.at("MSI").m_refresh_rate.m_numerator, 60000u);
  EXPECT_EQ(device.applied.at("MSI").m_refresh_rate.m_denominator, 1001u);
  EXPECT_EQ(device.applied.at("MSI").m_resolution.m_width, 3840u);
}

TEST(DisplayCoordinatorRestore, CaptureKeepsRationalIdentityNotRoundedOrNormalizedHz) {
  for (const auto rate : {Rational {240, 4}, Rational {60000, 1001}, Rational {59997, 1000}}) {
    const auto typed = display_helper::exact_baseline_refresh(display_device::FloatingPoint {rate});
    const auto inventory = display_helper::exact_baseline_refresh_from_json({{"numerator", rate.m_numerator}, {"denominator", rate.m_denominator}});
    ASSERT_TRUE(typed);
    EXPECT_EQ(*typed, std::make_pair(rate.m_numerator, rate.m_denominator));
    EXPECT_EQ(inventory, typed);
  }
}

TEST(DisplayCoordinatorRestore, MissingOrInvalidExactMetadataDoesNotInventFallbackRefresh) {
  EXPECT_FALSE(display_helper::exact_baseline_refresh(display_device::FloatingPoint {59.94}));
  EXPECT_FALSE(display_helper::exact_baseline_refresh(display_device::FloatingPoint {Rational {0, 1}}));
  EXPECT_FALSE(display_helper::exact_baseline_refresh(display_device::FloatingPoint {Rational {60, 0}}));
  for (const auto &invalid : std::vector<nlohmann::json> {
         nullptr,
         60,
         59.94,
         {{"numerator", 60}},
         {{"numerator", 60}, {"denominator", 0}},
         {{"numerator", -1}, {"denominator", 1}},
         {{"numerator", 60.0}, {"denominator", 1}},
         {{"numerator", 1ull << 40}, {"denominator", 1}},
       }) {
    EXPECT_FALSE(display_helper::exact_baseline_refresh_from_json(invalid)) << invalid;
  }
  remote_display_topology::node_t node {.id = "MSI", .physical = true, .configured_mode = {.refresh_hz = 144}};
  node.last_requested_mode = remote_display_topology::mode_t {.refresh_hz = 120};
  node.current_mode = remote_display_topology::mode_t {.refresh_hz = 60};
  DisplayTopologyDefinition layout;
  display_helper::append_physical_refresh_override(node, "MSI", layout);
  EXPECT_TRUE(layout.device_refresh_rate_overrides.empty());
  node.baseline_refresh_rate = {240, 4};
  node.physical = false;
  display_helper::append_physical_refresh_override(node, "VDD", layout);
  EXPECT_TRUE(layout.device_refresh_rate_overrides.empty());
}

TEST(DisplayCoordinatorRestore, CoordinatorTransportsExactBaselineWithoutChangingClientMode) {
  remote_display_topology::coordinator_t coordinator;
  remote_display_topology::node_t physical {.id = "MSI", .device_id = "MSI", .physical = true, .active = true};
  physical.baseline_refresh_rate = display_helper::exact_baseline_refresh(display_device::FloatingPoint {Rational {240, 4}});
  coordinator.set_physical_baseline({physical});
  int compositions = 0;
  coordinator.set_runtime_callbacks({
    .create_or_reclaim = [](const auto &, const auto &, const auto &mode) {
      EXPECT_EQ(mode.refresh_hz, 120);
      return true;
    },
    .apply_composed_topology = [&](const auto &nodes) {
      ++compositions;
      DisplayTopologyDefinition topology;
      for (const auto &node : nodes) {
        display_helper::append_physical_refresh_override(node, node.device_id.empty() ? node.id : node.device_id, topology);
      }
      EXPECT_EQ(topology.device_refresh_rate_overrides.size(), 1u);
      EXPECT_EQ(topology.device_refresh_rate_overrides.at("MSI"), std::make_pair(240u, 4u));
      Device device;
      device.topology = {{"MSI"}};
      EXPECT_TRUE(display_helper::restore_composed_topology(device, topology, ignore_failure));
      EXPECT_EQ(device.applied.at("MSI").m_refresh_rate.m_denominator, 4u);
      return true;
    },
    .exact_target_has_current_mode_and_dxgi = [](const auto &, const auto &mode) {
      EXPECT_EQ(mode.refresh_hz, 120);
      return std::optional<std::string> {"DISPLAY7"};
    },
    .remove_owned_display = [](const auto &) {
      return true;
    },
  });
  ASSERT_TRUE(coordinator.activate_or_resume("client", "Client", {.refresh_hz = 120}, 1).ready);
  EXPECT_TRUE(coordinator.explicit_release("client", 1, "test"));
  EXPECT_GE(compositions, 2);
}

TEST(DisplayCoordinatorRestore, FailedGeometryRejectsBaselineRecaptureUntilShutdownRetry) {
  remote_display_topology::coordinator_t coordinator;
  remote_display_topology::node_t physical {.id = "MSI", .physical = true, .active = true, .primary = true};
  physical.baseline_refresh_rate = {240, 4};
  coordinator.set_physical_baseline({physical});
  bool geometry_ok = true;
  int applies = 0;
  int removals = 0;
  coordinator.set_runtime_callbacks({
    .create_or_reclaim = [](const auto &, const auto &, const auto &) {
      return true;
    },
    .apply_composed_topology = [&](const auto &nodes) {
      ++applies;
      EXPECT_EQ(nodes.front().id, "MSI");
      EXPECT_EQ(nodes.front().baseline_refresh_rate, physical.baseline_refresh_rate);
      return geometry_ok;
    },
    .exact_target_has_current_mode_and_dxgi = [](const auto &, const auto &) {
      return std::optional<std::string> {"DISPLAY7"};
    },
    .terminate_owned_display = [&](const auto &) {
      ++removals;
      return true;
    },
  });
  ASSERT_TRUE(coordinator.activate_or_resume("client", "Client", {}, 1).ready);
  geometry_ok = false;
  EXPECT_TRUE(coordinator.explicit_release("client", 1, "test"));
  ASSERT_EQ(coordinator.managed_client_identity_count(), 0u);
  physical.baseline_refresh_rate = {59997, 1000};
  coordinator.set_physical_baseline({physical});
  coordinator.refresh_idle_physical_baseline({physical});
  physical.baseline_refresh_rate = {240, 4};
  coordinator.shutdown();
  EXPECT_FALSE(coordinator.shutdown_complete());
  geometry_ok = true;
  coordinator.shutdown();
  EXPECT_TRUE(coordinator.shutdown_complete());
  const auto completed_applies = applies;
  coordinator.shutdown();
  EXPECT_EQ(applies, completed_applies);
  EXPECT_EQ(removals, 1);
}

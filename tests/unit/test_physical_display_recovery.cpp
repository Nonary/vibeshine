#include "src/platform/windows/physical_display_recovery.h"

#include <gtest/gtest.h>

#include <functional>

namespace {
  namespace recovery = display_helper::physical_recovery;

  struct Harness {
    recovery::Topology baseline {{"A"}, {"B"}};
    recovery::Topology current {{"permanent-virtual"}, {"session-virtual"}};
    std::vector<recovery::Device> devices {
      {"permanent-virtual", false, true}, {"session-virtual", false, true}, {"A", true, false}};
    std::vector<std::string> exclusions;
    std::vector<recovery::Topology> writes;
    bool cancelled = false;
    bool apply_succeeds = true;
    bool activate_candidate = true;
    int reads = 0;
    int topology_reads = 0;
    int waits = 0;
    std::function<void()> on_enumerate;
    std::function<void()> on_capture;
    std::function<void()> on_apply;
    std::function<void()> on_wait;

    recovery::Outcome run() {
      return recovery::ensure_visible(
        baseline,
        exclusions,
        [&]() {
          ++reads;
          if (on_enumerate) on_enumerate();
          return devices;
        },
        [&]() {
          ++topology_reads;
          if (on_capture) on_capture();
          return current;
        },
        [&](const recovery::Topology &topology) {
          writes.push_back(topology);
          if (apply_succeeds) {
            current = topology;
            if (activate_candidate) {
              const auto active_ids = recovery::device_ids(current);
              for (auto &device : devices) {
                device.active = active_ids.contains(recovery::normalized_id(device.id));
              }
            }
          }
          if (on_apply) on_apply();
          return apply_succeeds;
        },
        [&]() { return cancelled; },
        [&](std::chrono::milliseconds) {
          ++waits;
          if (on_wait) on_wait();
          return !cancelled;
        });
    }
  };
}  // namespace

TEST(PhysicalDisplayRecovery, MissingSecondMonitorExtendsDesktopAndPreservesCompleteBaseline) {
  Harness harness;
  const auto baseline = harness.baseline;
  const auto outcome = harness.run();

  EXPECT_TRUE(outcome.physical_available);
  EXPECT_TRUE(outcome.mutation_attempted);
  ASSERT_EQ(harness.writes.size(), 1u);
  EXPECT_EQ(harness.current, (recovery::Topology {{"permanent-virtual"}, {"session-virtual"}, {"A"}}));
  EXPECT_EQ(harness.baseline, baseline);
  EXPECT_GE(harness.reads, 2);
}

TEST(PhysicalDisplayRecovery, AlreadyVisiblePhysicalOutputAvoidsAllMutation) {
  Harness harness;
  harness.current.push_back({"unrelated-physical"});
  harness.devices.push_back({"unrelated-physical", true, true});

  EXPECT_TRUE(harness.run().physical_available);
  EXPECT_TRUE(harness.writes.empty());
}

TEST(PhysicalDisplayRecovery, ExclusionsAndDeliberatelyDisabledPhysicalDevicesAreHonored) {
  Harness harness;
  harness.exclusions = {" a "};
  harness.devices.push_back({"deliberately-disabled", true, false});

  EXPECT_FALSE(harness.run().physical_available);
  EXPECT_TRUE(harness.writes.empty());
}

TEST(PhysicalDisplayRecovery, EmptyPhysicalBaselineDoesNotEnableConnectedPhysicalMonitor) {
  Harness harness;
  harness.baseline.clear();

  EXPECT_FALSE(harness.run().physical_available);
  EXPECT_TRUE(harness.writes.empty());
}

TEST(PhysicalDisplayRecovery, VirtualCandidateIsNeverUsedAsPhysicalRescue) {
  Harness harness;
  harness.devices.back().physical = false;

  EXPECT_FALSE(harness.run().physical_available);
  EXPECT_TRUE(harness.writes.empty());
}

TEST(PhysicalDisplayRecovery, FailedActivationRetainsActiveTopologyAndBaseline) {
  Harness harness;
  const auto before = harness.current;
  const auto baseline = harness.baseline;
  harness.apply_succeeds = false;

  EXPECT_FALSE(harness.run().physical_available);
  EXPECT_EQ(harness.current, before);
  EXPECT_EQ(harness.baseline, baseline);
  EXPECT_EQ(harness.writes.size(), 1u);
}

TEST(PhysicalDisplayRecovery, SuccessfulAcknowledgmentWithoutVisiblePhysicalReadbackIsNotSuccess) {
  Harness harness;
  harness.activate_candidate = false;

  EXPECT_FALSE(harness.run().physical_available);
  EXPECT_EQ(harness.writes.size(), 1u);
  EXPECT_EQ(harness.waits, 19);
}

TEST(PhysicalDisplayRecovery, CancellationBeforeStartOrDuringPreflightPreventsMutation) {
  for (int step = 0; step < 3; ++step) {
    Harness harness;
    if (step == 0) harness.cancelled = true;
    if (step == 1) harness.on_enumerate = [&]() { harness.cancelled = true; };
    if (step == 2) harness.on_capture = [&]() { harness.cancelled = true; };

    EXPECT_FALSE(harness.run().physical_available);
    EXPECT_TRUE(harness.writes.empty());
  }
}

TEST(PhysicalDisplayRecovery, CancellationDuringApplyOrWaitStopsVerificationAndLaterWrites) {
  for (const bool cancel_in_apply : {true, false}) {
    Harness harness;
    harness.activate_candidate = false;
    if (cancel_in_apply) harness.on_apply = [&]() { harness.cancelled = true; };
    else harness.on_wait = [&]() { harness.cancelled = true; };

    EXPECT_FALSE(harness.run().physical_available);
    EXPECT_EQ(harness.writes.size(), 1u);
    EXPECT_EQ(harness.waits, cancel_in_apply ? 0 : 1);
  }
}

TEST(PhysicalDisplayRecovery, MissingTopologyReadPreventsReplacingUnknownActiveDesktop) {
  Harness harness;
  harness.current.clear();

  EXPECT_FALSE(harness.run().physical_available);
  EXPECT_TRUE(harness.writes.empty());
}

TEST(PhysicalDisplayRecovery, SubstitutedTopologyRepairsExistingPathsWithoutRemovingNewPaths) {
  Harness harness;
  harness.on_apply = [&]() {
    if (harness.writes.size() == 1u) {
      harness.current = {{"A"}, {"new-permanent-virtual"}};
    }
  };

  EXPECT_FALSE(harness.run().physical_available);
  ASSERT_EQ(harness.writes.size(), 2u);
  EXPECT_EQ(harness.current, (recovery::Topology {{"A"}, {"new-permanent-virtual"}, {"permanent-virtual"}, {"session-virtual"}}));
}

TEST(PhysicalDisplayRecovery, CancellationAfterSubstitutionPreventsRepairOfNewSessionTopology) {
  Harness harness;
  harness.on_apply = [&]() { harness.current = {{"new-session"}}; };
  harness.on_capture = [&]() {
    if (harness.topology_reads == 2) harness.cancelled = true;
  };

  EXPECT_FALSE(harness.run().physical_available);
  EXPECT_EQ(harness.writes.size(), 1u);
  EXPECT_EQ(harness.current, (recovery::Topology {{"new-session"}}));
}

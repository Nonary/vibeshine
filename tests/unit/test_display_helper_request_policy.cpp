#include "src/platform/windows/display_helper_request_policy.h"

#include <gtest/gtest.h>

namespace policy = display_helper_integration::request_policy;

TEST(DisplayHelperRequestPolicy, ExplicitPhysicalTargetWithAutomationDisabledIsCaptureOnly) {
  policy::Input input {
    .configuration_option = policy::ConfigurationOption::Disabled,
    .physical_output_override = true,
  };
  EXPECT_TRUE(policy::capture_only_physical_request(input));
  EXPECT_FALSE(policy::evaluate(input).dispatch);

  input.configuration_option = policy::ConfigurationOption::EnsureActive;
  EXPECT_FALSE(policy::capture_only_physical_request(input));
}

TEST(DisplayHelperRequestPolicy, VirtualDisplayAndPhysicalHdrProfileStillRequirePreparation) {
  policy::Input input {
    .configuration_option = policy::ConfigurationOption::Disabled,
    .physical_output_override = true,
  };
  input.virtual_display = true;
  EXPECT_FALSE(policy::capture_only_physical_request(input));
  input.virtual_display = false;
  input.hdr_profile_selected = true;
  EXPECT_FALSE(policy::capture_only_physical_request(input));
}

TEST(DisplayHelperRequestPolicy, DisabledAutomationWithoutExplicitTargetRetainsRevertPreparation) {
  EXPECT_FALSE(policy::capture_only_physical_request({
    .configuration_option = policy::ConfigurationOption::Disabled,
  }));
}

TEST(DisplayHelperRequestPolicy, BaselinePreflightSupersedesRestoreBeforeSnapshot) {
  bool restore_pending = true;
  std::vector<std::string> operations;
  const bool prepared = policy::prepare_virtual_display_baseline(
    [&] {
      operations.emplace_back("disarm");
      restore_pending = false;
    },
    [&] {
      operations.emplace_back("restore-status");
      return restore_pending;
    },
    [&] {
      operations.emplace_back("snapshot-acknowledged");
      return true;
    }
  );
  ASSERT_TRUE(prepared);
  EXPECT_EQ(operations, (std::vector<std::string> {"disarm", "restore-status", "snapshot-acknowledged"}));
}

TEST(DisplayHelperRequestPolicy, BaselinePreflightDoesNotSnapshotWhileRestoreStillOwnsDesktop) {
  int snapshot_requests = 0;
  EXPECT_FALSE(policy::prepare_virtual_display_baseline(
    [] {},
    [] { return true; },
    [&] {
      ++snapshot_requests;
      return true;
    }
  ));
  EXPECT_EQ(snapshot_requests, 0);
}

TEST(DisplayHelperRequestPolicy, BaselinePreflightRequiresSnapshotAcknowledgementAfterDisarm) {
  int snapshot_requests = 0;
  EXPECT_FALSE(policy::prepare_virtual_display_baseline(
    [] {},
    [] { return false; },
    [&] {
      ++snapshot_requests;
      // Covers unavailable helpers, failed persistence/task registration, and
      // indeterminate physical enumeration: none authorizes VD creation.
      return false;
    }
  ));
  EXPECT_EQ(snapshot_requests, 1);
}

TEST(DisplayHelperRequestPolicy, VerifyOnlyExtendedTargetsNeverPrepareTopologyOrPrimary) {
  for (const auto layout : {policy::VirtualDisplayLayout::Extended, policy::VirtualDisplayLayout::ExtendedPrimary, policy::VirtualDisplayLayout::ExtendedIsolated, policy::VirtualDisplayLayout::ExtendedPrimaryIsolated}) {
    const auto result = policy::evaluate({
      .configuration_option = policy::ConfigurationOption::VerifyOnly,
      .layout = layout,
      .virtual_display = true,
      .target_device_id = "tablet",
      .topology_snapshot = {{"physical"}},
    });
    EXPECT_TRUE(result.dispatch);
    EXPECT_EQ(result.device_preparation, policy::DevicePreparation::VerifyOnly);
    EXPECT_TRUE(result.topology.empty());
  }
}

TEST(DisplayHelperRequestPolicy, ExplicitExclusiveAndAutomaticExtendedKeepTheirPolicies) {
  const auto exclusive = policy::evaluate({
    .configuration_option = policy::ConfigurationOption::VerifyOnly,
    .virtual_display = true,
    .target_device_id = "tablet",
  });
  EXPECT_EQ(exclusive.device_preparation, policy::DevicePreparation::EnsureOnlyDisplay);
  EXPECT_EQ(exclusive.topology, (std::vector<std::vector<std::string>> {{"tablet"}}));
  const auto extended = policy::evaluate({
    .configuration_option = policy::ConfigurationOption::EnsureActive,
    .layout = policy::VirtualDisplayLayout::Extended,
    .virtual_display = true,
    .target_device_id = "tablet",
    .topology_snapshot = {{"physical"}, {"peer"}},
  });
  EXPECT_EQ(extended.device_preparation, policy::DevicePreparation::EnsureActive);
  EXPECT_EQ(extended.topology, (std::vector<std::vector<std::string>> {{"physical"}, {"peer"}, {"tablet"}}));
}

TEST(DisplayHelperRequestPolicy, ExtendedMergeRetainsPeersAndRestoresMissingCloneMembers) {
  const auto merged = policy::merge_extended_topology(
    {{"PHYSICAL"}, {"peer"}}, {{"physical", "cloned"}, {"second"}}, "tablet"
  );
  EXPECT_EQ(merged, (std::vector<std::vector<std::string>> {{"PHYSICAL", "cloned"}, {"peer"}, {"second"}, {"tablet"}}));
  EXPECT_EQ(policy::merge_extended_topology(merged, {{"physical", "cloned"}, {"second"}}, "TABLET"), merged);
}

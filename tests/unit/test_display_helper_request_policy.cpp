#include "src/platform/windows/display_helper_request_policy.h"

#include <gtest/gtest.h>
#include <utility>

namespace policy = display_helper_integration::request_policy;

TEST(DisplayHelperRequestPolicy, EnabledPreparationAppliesTheSelectedVirtualLayout) {
  using enum policy::VirtualDisplayLayout;
  using Prep = policy::DevicePreparation;
  using Option = policy::ConfigurationOption;
  using Topology = std::vector<std::vector<std::string>>;
  for (const auto &[layout, preparation] : {
         std::pair {Exclusive, Prep::EnsureOnlyDisplay},
         std::pair {Extended, Prep::EnsureActive},
         std::pair {ExtendedPrimary, Prep::EnsurePrimary},
         std::pair {ExtendedIsolated, Prep::EnsureActive},
         std::pair {ExtendedPrimaryIsolated, Prep::EnsurePrimary},
       }) {
    const auto expected_topology = layout == Exclusive ? Topology {{"tablet"}} : Topology {{"physical"}, {"tablet"}};
    for (const auto option : {Option::VerifyOnly, Option::EnsureActive, Option::EnsureOnlyDisplay}) {
      const auto result = policy::evaluate({
        .configuration_option = option,
        .layout = layout,
        .virtual_display = true,
        .target_device_id = "tablet",
        .topology_snapshot = {{"physical"}},
      });
      EXPECT_TRUE(result.dispatch);
      EXPECT_EQ(result.device_preparation, preparation);
      EXPECT_EQ(result.topology, expected_topology);
    }
  }
}

TEST(DisplayHelperRequestPolicy, DisabledPreparationSkipsEveryVirtualLayout) {
  using enum policy::VirtualDisplayLayout;
  for (const auto layout : {Exclusive, Extended, ExtendedPrimary, ExtendedIsolated, ExtendedPrimaryIsolated}) {
    const auto result = policy::evaluate({
      .configuration_option = policy::ConfigurationOption::Disabled,
      .layout = layout,
      .virtual_display = true,
      .target_device_id = "tablet",
      .topology_snapshot = {{"physical"}},
    });
    EXPECT_FALSE(result.dispatch);
    EXPECT_FALSE(result.device_preparation.has_value());
    EXPECT_TRUE(result.topology.empty());
  }
}

TEST(DisplayHelperRequestPolicy, ExtendedPreparationKeepsActivePeers) {
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

TEST(DisplayHelperRequestPolicy, ReturnedPhysicalOutputRequiresFreshSettingsBaseline) {
  const std::vector<std::vector<std::string>> baseline {{"PRIMARY"}};
  const std::vector<std::vector<std::string>> live {{"PRIMARY"}, {"SCEPTRE"}, {"PEER"}, {"VIRTUAL"}};
  EXPECT_TRUE(policy::physical_topology_extends_baseline(baseline, {{"primary"}, {"SCEPTRE"}}));
  EXPECT_TRUE(policy::physical_topology_extends_baseline(baseline, {{"primary", "SCEPTRE"}}));
  EXPECT_EQ(policy::merge_extended_topology(live, baseline, "virtual"), live);
  EXPECT_EQ(baseline, (std::vector<std::vector<std::string>> {{"PRIMARY"}}));
}

TEST(DisplayHelperRequestPolicy, PartialEmptyOrCaseOnlyPhysicalEnumerationKeepsSettingsBaseline) {
  const std::vector<std::vector<std::string>> baseline {{"PRIMARY", "CLONE"}, {"SECONDARY"}};
  EXPECT_FALSE(policy::physical_topology_extends_baseline(baseline, {}));
  EXPECT_FALSE(policy::physical_topology_extends_baseline(baseline, {{"primary"}, {"", "CLONE", "clone"}}));
  EXPECT_FALSE(policy::physical_topology_extends_baseline(baseline, {{"secondary"}}));
  EXPECT_TRUE(policy::physical_topology_extends_baseline(baseline, {{"NEW_A", "NEW_B", "new_a"}}));
}

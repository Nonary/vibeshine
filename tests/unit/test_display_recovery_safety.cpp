#include "../tests_common.h"

#include <src/platform/windows/display_recovery_safety.h>

#include <array>

namespace {
  using display_recovery_safety::classify_physical_displays;
  using display_recovery_safety::DisplayTargetObservation;
  using display_recovery_safety::is_headless_without_baseline;
  using display_recovery_safety::is_managed_virtual_identity;
  using display_recovery_safety::PhysicalDisplayState;

  constexpr DisplayTargetObservation physical_active {true, true, false};
  constexpr DisplayTargetObservation physical_disabled {true, false, false};
  constexpr DisplayTargetObservation physical_disconnected {false, false, false};
  constexpr DisplayTargetObservation virtual_active {true, true, true};
}  // namespace

TEST(DisplayRecoverySafety, EmptySuccessfulQueryAllowsFirstHeadlessDisplay) {
  const auto state = classify_physical_displays(true, {});
  EXPECT_EQ(state, PhysicalDisplayState::none_connected);
  EXPECT_TRUE(is_headless_without_baseline(state, false));
}

TEST(DisplayRecoverySafety, EmptyFailedQueryNeverProvesHeadlessness) {
  const auto state = classify_physical_displays(false, {});
  EXPECT_EQ(state, PhysicalDisplayState::unknown);
  EXPECT_FALSE(is_headless_without_baseline(state, false));
}

TEST(DisplayRecoverySafety, FailedQueryDoesNotUseStaleSuccessfulTargets) {
  const std::array targets {virtual_active};
  EXPECT_EQ(classify_physical_displays(false, targets), PhysicalDisplayState::unknown);
}

TEST(DisplayRecoverySafety, VirtualOnlyHeadlessHostDoesNotRequirePhysicalBaseline) {
  const std::array targets {virtual_active, physical_disconnected};
  EXPECT_EQ(classify_physical_displays(true, targets), PhysicalDisplayState::none_connected);
}

TEST(DisplayRecoverySafety, DisabledConnectedMonitorIsNotHeadless) {
  const std::array targets {virtual_active, physical_disabled};
  const auto state = classify_physical_displays(true, targets);
  EXPECT_EQ(state, PhysicalDisplayState::connected_inactive);
  EXPECT_FALSE(is_headless_without_baseline(state, false));
}

TEST(DisplayRecoverySafety, ActivePhysicalMonitorWinsOverItsInactiveAlternativePaths) {
  const std::array targets {physical_disabled, virtual_active, physical_active, physical_disabled};
  EXPECT_EQ(classify_physical_displays(true, targets), PhysicalDisplayState::active);
}

TEST(DisplayRecoverySafety, DisconnectionInProgressCannotGrantHeadlessExemption) {
  const std::array targets {virtual_active, DisplayTargetObservation {false, true, false}};
  EXPECT_EQ(classify_physical_displays(true, targets), PhysicalDisplayState::unknown);
}

TEST(DisplayRecoverySafety, KnownVirtualDepartureDoesNotInventAPhysicalRecoveryObligation) {
  const std::array targets {DisplayTargetObservation {false, true, true}};
  EXPECT_EQ(classify_physical_displays(true, targets), PhysicalDisplayState::none_connected);
}

TEST(DisplayRecoverySafety, TemporaryPhysicalAbsenceRetainsBaselineObligation) {
  const std::array targets {physical_disconnected, virtual_active};
  const auto state = classify_physical_displays(true, targets);
  EXPECT_EQ(state, PhysicalDisplayState::none_connected);
  EXPECT_FALSE(is_headless_without_baseline(state, true));
}

TEST(DisplayRecoverySafety, OnlyKnownHeadlessStateMaySkipRecoveryProtection) {
  for (const auto state : {PhysicalDisplayState::unknown, PhysicalDisplayState::connected_inactive, PhysicalDisplayState::active}) {
    EXPECT_FALSE(is_headless_without_baseline(state, false));
    EXPECT_FALSE(is_headless_without_baseline(state, true));
  }
}

TEST(DisplayRecoverySafety, RecognizesManagedDriverHardwarePathsWithoutEdid) {
  for (const auto path : {R"(\\?\DISPLAY#SDD4001#5&123)", R"(\\?\display#sdd5001#5&123)", R"(\\?\DISPLAY#SMK1234#5&123)", "SunshineVirtualDisplay", "sudovda"}) {
    EXPECT_TRUE(is_managed_virtual_identity(path)) << path;
  }
}

TEST(DisplayRecoverySafety, RecognizesManagedEdidWithoutDevicePath) {
  EXPECT_TRUE(is_managed_virtual_identity({}, "SDD"));
  EXPECT_TRUE(is_managed_virtual_identity({}, "smk"));
}

TEST(DisplayRecoverySafety, MissingOrUnrecognizedIdentityStaysPossiblyPhysical) {
  EXPECT_FALSE(is_managed_virtual_identity({}));
  EXPECT_FALSE(is_managed_virtual_identity(R"(\\?\DISPLAY#DEL1234#5&123)", "DEL"));
  EXPECT_FALSE(is_managed_virtual_identity(R"(\\?\DISPLAY#SDD1001#5&123)"));
  EXPECT_FALSE(is_managed_virtual_identity(R"(\\?\DISPLAY#SDD4001)"));
  EXPECT_FALSE(is_managed_virtual_identity({}, "SDD-like"));
}

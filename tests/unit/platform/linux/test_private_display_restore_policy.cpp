/**
 * @file tests/unit/platform/linux/test_private_display_restore_policy.cpp
 * @brief Tests for Linux private-display restore guards.
 */
#include <array>
#include <gtest/gtest.h>
#include <limits>
#include <map>
#include <nlohmann/json.hpp>
#include <src/platform/linux/private_display_restore_policy.h>
#include <vector>

namespace policy = platf::linux_private_display::restore_policy;

namespace {
  using json = nlohmann::json;

  json external_desktop() {
    return {{"outputs", {
      {{"name", "eDP-1"}, {"connected", true}, {"enabled", false}, {"rotation", 8}},
      {{"name", "DP-1"}, {"connected", true}, {"enabled", true}, {"currentModeId", "25"},
       {"priority", 1}, {"scale", 1.5}, {"rotation", 1}, {"hdr", true},
       {"pos", {{"x", 0}, {"y", 0}}}, {"size", {{"width", 2560}, {"height", 1440}}}}
    }}};
  }

  json saved_mode_output() {
    auto output = external_desktop()["outputs"][1];
    output["modes"] = {{{"id", "25"}, {"size", {{"width", 2560}, {"height", 1440}}}, {"refreshRate", 60.0}}};
    return output;
  }
}

TEST(LinuxPrivateDisplayRestorePolicy, SavesDesktopBeforeHotplugCanEnablePanelAndExtendMonitor) {
  auto desktop = external_desktop();
  const auto original = desktop;
  std::optional<json> snapshot;
  ASSERT_TRUE(policy::connect_with_snapshot(snapshot, [&] { return std::make_optional(desktop); }, [&] {
    desktop["outputs"][0]["enabled"] = true;
    desktop["outputs"][1]["pos"]["x"] = 1280;
    return true;
  }));
  ASSERT_TRUE(snapshot);
  EXPECT_EQ(*snapshot, original);
  EXPECT_NE(*snapshot, desktop);
}

TEST(LinuxPrivateDisplayRestorePolicy, ReconnectAndSecondClientKeepFirstSnapshot) {
  std::optional<json> snapshot = external_desktop();
  const auto original = *snapshot;
  int queries = 0;
  int connections = 0;
  for (int attempt = 0; attempt < 2; ++attempt) {
    EXPECT_TRUE(policy::connect_with_snapshot(snapshot, [&]() -> std::optional<json> {
      ++queries;
      return json {{"outputs", json::array()}};
    }, [&] { ++connections; return true; }));
  }
  EXPECT_EQ(queries, 0);
  EXPECT_EQ(connections, 2);
  EXPECT_EQ(*snapshot, original);
}

TEST(LinuxPrivateDisplayRestorePolicy, FailedSnapshotPreventsConnection) {
  std::optional<json> snapshot;
  bool connected = false;
  EXPECT_FALSE(policy::connect_with_snapshot(snapshot, []() -> std::optional<json> {
    return std::nullopt;
  }, [&] { connected = true; return true; }));
  EXPECT_FALSE(connected);
  EXPECT_FALSE(snapshot);
}

TEST(LinuxPrivateDisplayRestorePolicy, FailedConnectionRetainsOriginalForRollback) {
  std::optional<json> snapshot;
  EXPECT_FALSE(policy::connect_with_snapshot(snapshot, [] { return std::make_optional(external_desktop()); }, [] { return false; }));
  ASSERT_TRUE(snapshot);
  EXPECT_EQ(*snapshot, external_desktop());
}

TEST(LinuxPrivateDisplayRestorePolicy, FailedPublicationPreservesOnlyLiveAdmittedConnectorAndBaseline) {
  auto desktop = external_desktop();
  const auto original = desktop;
  std::optional<json> snapshot;
  std::vector<std::string> operations;

  EXPECT_EQ(policy::connect_with_snapshot_and_publication(snapshot, [&] {
    operations.emplace_back("snapshot");
    return std::make_optional(desktop);
  }, [&] {
    operations.emplace_back("connect");
    // Hotplug can load KWin's remembered exclusive streaming topology before
    // KScreen queries succeed. The private connector is already the scanout.
    desktop["outputs"][1]["enabled"] = false;
    desktop["outputs"].push_back({{"name", "Virtual-1"}, {"connected", true}, {"enabled", true}});
    return true;
  }, [&] {
    operations.emplace_back("publication failed");
    return false;
  }), policy::connection_result_e::publication_failed);

  EXPECT_EQ(operations, (std::vector<std::string> {"snapshot", "connect", "publication failed"}));
  ASSERT_TRUE(snapshot);
  EXPECT_EQ(*snapshot, original);
  EXPECT_FALSE(desktop["outputs"][1]["enabled"]);
  EXPECT_TRUE(desktop["outputs"][2]["connected"]);
  EXPECT_TRUE(desktop["outputs"][2]["enabled"]);
}

TEST(LinuxPrivateDisplayRestorePolicy, SnapshotFailurePreventsHotplugAndPublication) {
  std::optional<json> snapshot;
  unsigned connections = 0;
  unsigned publications = 0;
  EXPECT_EQ(policy::connect_with_snapshot_and_publication(snapshot, []() -> std::optional<json> {
    return std::nullopt;
  }, [&] {
    ++connections;
    return true;
  }, [&] {
    ++publications;
    return true;
  }), policy::connection_result_e::failed);
  EXPECT_FALSE(snapshot);
  EXPECT_EQ(connections, 0U);
  EXPECT_EQ(publications, 0U);
}

TEST(LinuxPrivateDisplayRestorePolicy, RejectedConnectionDoesNotWaitForPublicationOrDiscardBaseline) {
  std::optional<json> snapshot;
  unsigned publications = 0;
  EXPECT_EQ(policy::connect_with_snapshot_and_publication(snapshot, [] {
    return std::make_optional(external_desktop());
  }, [] {
    return false;
  }, [&] {
    ++publications;
    return true;
  }), policy::connection_result_e::failed);
  ASSERT_TRUE(snapshot);
  EXPECT_EQ(*snapshot, external_desktop());
  EXPECT_EQ(publications, 0U);
}

TEST(LinuxPrivateDisplayRestorePolicy, PublicationRetryPreservesDesktopFromBeforeAcknowledgedHotplug) {
  std::optional<json> snapshot;
  unsigned snapshots = 0;
  unsigned connections = 0;
  unsigned publications = 0;
  for (unsigned attempt = 0; attempt < 2; ++attempt) {
    EXPECT_EQ(policy::connect_with_snapshot_and_publication(snapshot, [&] {
      ++snapshots;
      return std::make_optional(external_desktop());
    }, [&] {
      ++connections;
      return true;
    }, [&] {
      ++publications;
      return attempt > 0;
    }), attempt > 0 ? policy::connection_result_e::published : policy::connection_result_e::publication_failed);
  }
  EXPECT_EQ(snapshots, 1U);
  EXPECT_EQ(connections, 2U);
  EXPECT_EQ(publications, 2U);
  ASSERT_TRUE(snapshot);
  EXPECT_EQ(*snapshot, external_desktop());
}

TEST(LinuxPrivateDisplayRestorePolicy, MissingDesiredCaptureGuardPreservesPreviousScanout) {
  bool previous_live = true;
  unsigned rechecks = 0;
  EXPECT_FALSE(policy::retire_with_capture_guard(true, []() -> std::optional<std::string> {
    return std::nullopt;
  }, [&](const std::string &) {
    ++rechecks;
    return true;
  }, [] {
    return true;
  }, [&] {
    previous_live = false;
    return true;
  }));
  EXPECT_TRUE(previous_live);
  EXPECT_EQ(rechecks, 0U);
}

TEST(LinuxPrivateDisplayRestorePolicy, CaptureGuardMustBeDesiredConnectedEnabledAndPublished) {
  auto current = external_desktop();
  current["outputs"].push_back({{"name", "Virtual-1"}, {"connected", true}, {"enabled", true}});
  const std::vector<std::string> old_capture {"Virtual-1"};
  EXPECT_FALSE(policy::select_capture_ready_guard(current, {"DP-1"}, old_capture));

  const std::vector<std::string> new_capture {"DP-1"};
  EXPECT_EQ(policy::select_capture_ready_guard(current, {"DP-1"}, new_capture), "DP-1");
  for (const auto &change : {json {{"enabled", false}}, json {{"connected", false}}}) {
    auto unavailable = current;
    unavailable["outputs"][1].update(change);
    EXPECT_FALSE(policy::select_capture_ready_guard(unavailable, {"DP-1"}, new_capture));
  }
  EXPECT_EQ(policy::select_capture_ready_guard(current, {"Virtual-1"}, old_capture), "Virtual-1");
}

TEST(LinuxPrivateDisplayRestorePolicy, PublishedPhysicalOrRetainedPrivateGuardPrecedesRetirement) {
  for (const auto &name : {std::string {"DP-1"}, std::string {"Virtual-2"}}) {
    std::vector<std::string> operations;
    EXPECT_TRUE(policy::retire_with_capture_guard(true, [&] {
      operations.emplace_back("capture guard");
      return std::make_optional(name);
    }, [&](const std::string &guard) {
      EXPECT_EQ(guard, name);
      operations.emplace_back("current guard active");
      return true;
    }, [&] {
      operations.emplace_back("permission");
      return true;
    }, [&] {
      operations.emplace_back("retire previous");
      return true;
    }));
    EXPECT_EQ(operations, (std::vector<std::string> {"permission", "capture guard", "permission",
                                                   "current guard active", "permission", "retire previous"}));
  }
}

TEST(LinuxPrivateDisplayRestorePolicy, GuardLostAfterPublicationCannotRetirePreviousScanout) {
  bool previous_live = true;
  EXPECT_FALSE(policy::retire_with_capture_guard(true, [] {
    return std::make_optional(std::string {"DP-1"});
  }, [](const std::string &) {
    // Capture registry publication alone cannot authorize retirement if the
    // immediate KScreen recheck reports a disconnected or disabled output.
    return false;
  }, [] {
    return true;
  }, [&] {
    previous_live = false;
    return true;
  }));
  EXPECT_TRUE(previous_live);
}

TEST(LinuxPrivateDisplayRestorePolicy, CancellationDuringGuardRecheckPreservesPreviousScanout) {
  bool permitted = true;
  bool previous_live = true;
  EXPECT_FALSE(policy::retire_with_capture_guard(true, [] {
    return std::make_optional(std::string {"DP-1"});
  }, [&](const std::string &) {
    permitted = false;
    return true;
  }, [&] {
    return permitted;
  }, [&] {
    previous_live = false;
    return true;
  }));
  EXPECT_TRUE(previous_live);
}

TEST(LinuxPrivateDisplayRestorePolicy, CancellationBeforeGuardSelectionSkipsQueriesAndRetirement) {
  unsigned probes = 0;
  unsigned retirements = 0;
  EXPECT_FALSE(policy::retire_with_capture_guard(true, [&] {
    ++probes;
    return std::make_optional(std::string {"DP-1"});
  }, [&](const std::string &) {
    ++probes;
    return true;
  }, [] {
    return false;
  }, [&] {
    ++retirements;
    return true;
  }));
  EXPECT_EQ(probes, 0U);
  EXPECT_EQ(retirements, 0U);
}

TEST(LinuxPrivateDisplayRestorePolicy, InactiveRetirementSkipsCaptureAndGuardProbes) {
  unsigned probes = 0;
  unsigned retirements = 0;
  EXPECT_TRUE(policy::retire_with_capture_guard(false, [&] {
    ++probes;
    return std::make_optional(std::string {"DP-1"});
  }, [&](const std::string &) {
    ++probes;
    return true;
  }, [] {
    return true;
  }, [&] {
    ++retirements;
    return true;
  }));
  EXPECT_EQ(probes, 0U);
  EXPECT_EQ(retirements, 1U);
}

TEST(LinuxPrivateDisplayRestorePolicy, EqualModeIdentifierStillRequiresSavedResolution) {
  const auto snapshot = external_desktop();
  auto current = snapshot;
  current["outputs"][1]["size"] = {{"width", 1280}, {"height", 720}};
  EXPECT_FALSE(policy::snapshot_matches(snapshot, current));
  EXPECT_FALSE(policy::snapshot_matches(snapshot, current, true));
}

TEST(LinuxPrivateDisplayRestorePolicy, EqualModeIdentifierStillRequiresKnownSavedRefresh) {
  auto snapshot = external_desktop();
  snapshot["outputs"][1]["modes"] = {{{"id", "25"}, {"refreshRate", 60.0}}};
  auto current = snapshot;
  current["outputs"][1]["modes"][0]["refreshRate"] = 30.0;
  EXPECT_FALSE(policy::snapshot_matches(snapshot, current));
  EXPECT_FALSE(policy::snapshot_matches(snapshot, current, true));
}

TEST(LinuxPrivateDisplayRestorePolicy, KnownSavedRefreshCannotBeVerifiedFromMissingCurrentMetadata) {
  auto snapshot = external_desktop();
  snapshot["outputs"][1]["modes"] = {{{"id", "25"}, {"refreshRate", 60.0}}};
  auto current = snapshot;
  current["outputs"][1].erase("modes");
  EXPECT_FALSE(policy::snapshot_matches(snapshot, current));
  EXPECT_FALSE(policy::snapshot_matches(snapshot, current, true));
}

TEST(LinuxPrivateDisplayRestorePolicy, DifferentModeIdentifiersAcceptEquivalentKnownResolutionAndRefresh) {
  auto snapshot = external_desktop();
  snapshot["outputs"][1]["modes"] = {{{"id", "25"}, {"refreshRate", 60.0}}};
  auto current = snapshot;
  current["outputs"][1]["currentModeId"] = "replacement";
  current["outputs"][1]["modes"] = {{{"id", "replacement"}, {"refreshRate", 60.01}}};
  EXPECT_TRUE(policy::snapshot_matches(snapshot, current));
  EXPECT_TRUE(policy::snapshot_matches(snapshot, current, true));
}

TEST(LinuxPrivateDisplayRestorePolicy, IncompleteSavedModeMetadataRequiresIdentifierAndAvailableConstraints) {
  auto snapshot = external_desktop();
  auto current = snapshot;
  EXPECT_TRUE(policy::snapshot_matches(snapshot, current, true));
  current["outputs"][1]["currentModeId"] = "replacement";
  // Equal sizes with unknown refresh do not establish equivalent modes.
  EXPECT_FALSE(policy::snapshot_matches(snapshot, current, true));

  snapshot["outputs"][1].erase("size");
  current = snapshot;
  current["outputs"][1]["size"] = {{"width", 1280}, {"height", 720}};
  current["outputs"][1]["modes"] = {{{"id", "25"}, {"refreshRate", 30.0}}};
  EXPECT_TRUE(policy::snapshot_matches(snapshot, current, true));
  current["outputs"][1]["currentModeId"] = "replacement";
  EXPECT_FALSE(policy::snapshot_matches(snapshot, current, true));
}

TEST(LinuxPrivateDisplayRestorePolicy, MissingSavedIdentifierAndSemanticModeCannotVerifyRestoration) {
  auto snapshot = external_desktop();
  snapshot["outputs"][1].erase("size");
  snapshot["outputs"][1].erase("currentModeId");
  EXPECT_FALSE(policy::snapshot_matches(snapshot, snapshot, true));
}

TEST(LinuxPrivateDisplayRestorePolicy, RestoreModeUsesEquivalentCurrentIdentifierWhenSavedIdentifierIsAbsent) {
  const auto saved = saved_mode_output();
  auto present = saved;
  present["currentModeId"] = "replacement";
  present["modes"][0]["id"] = "replacement";
  EXPECT_EQ(policy::select_restore_mode(saved, &present), "replacement");
}

TEST(LinuxPrivateDisplayRestorePolicy, RestoreModeRejectsReusedIdentifierAndSelectsSemanticReplacement) {
  const auto saved = saved_mode_output();
  for (const auto &change : {json {{"size", {{"width", 1280}, {"height", 720}}}},
                            json {{"refreshRate", 30.0}}}) {
    auto present = saved;
    present["modes"][0].update(change);
    auto replacement = saved["modes"][0];
    replacement["id"] = "replacement";
    present["modes"].push_back(replacement);
    EXPECT_EQ(policy::select_restore_mode(saved, &present), "replacement");
  }
}

TEST(LinuxPrivateDisplayRestorePolicy, RestoreModeWithNoValidCandidateCannotAuthorizeActivation) {
  const auto saved = saved_mode_output();
  auto present = saved;
  present["modes"][0]["size"] = {{"width", 1280}, {"height", 720}};
  EXPECT_FALSE(policy::select_restore_mode(saved, &present));
  EXPECT_FALSE(policy::select_restore_mode(saved, static_cast<const json *>(nullptr)));
}

TEST(LinuxPrivateDisplayRestorePolicy, RestoreModeUsesCatalogPixelsInsteadOfRotatedOutputGeometry) {
  auto saved = saved_mode_output();
  saved["rotation"] = 2;
  saved["size"] = {{"width", 1440}, {"height", 2560}};
  saved["scale"] = 1.5;
  auto present = saved;
  present["modes"][0]["id"] = "replacement";
  EXPECT_EQ(policy::select_restore_mode(saved, &present), "replacement");
  present["currentModeId"] = "replacement";
  const json snapshot {{"outputs", {saved}}};
  const json current {{"outputs", {present}}};
  EXPECT_TRUE(policy::snapshot_matches(snapshot, current));
  EXPECT_TRUE(policy::snapshot_matches(snapshot, current, true));
  present["modes"][0]["size"] = saved["size"];
  EXPECT_FALSE(policy::select_restore_mode(saved, &present));
}

TEST(LinuxPrivateDisplayRestorePolicy, RestoreModeIncompleteSavedMetadataRequiresCatalogIdentifierAndKnownSize) {
  auto saved = saved_mode_output();
  saved.erase("modes");
  auto present = saved_mode_output();
  EXPECT_EQ(policy::select_restore_mode(saved, &present), "25");
  present["modes"][0]["size"] = {{"width", 1280}, {"height", 720}};
  EXPECT_FALSE(policy::select_restore_mode(saved, &present));
  present = saved_mode_output();
  present["modes"][0]["id"] = "replacement";
  EXPECT_FALSE(policy::select_restore_mode(saved, &present));
}

TEST(LinuxPrivateDisplayRestorePolicy, RestoreModeKnownRefreshRequiresFiniteCurrentCatalogRefresh) {
  const auto saved = saved_mode_output();
  for (const auto rate : {0.0, std::numeric_limits<double>::quiet_NaN()}) {
    auto present = saved;
    present["modes"][0]["refreshRate"] = rate;
    EXPECT_FALSE(policy::select_restore_mode(saved, &present));
  }
  auto present = saved;
  present["modes"][0].erase("refreshRate");
  EXPECT_FALSE(policy::select_restore_mode(saved, &present));
}

TEST(LinuxPrivateDisplayRestorePolicy, RestoreModeMissingSavedIdentifierCannotSelectWildcardCandidate) {
  auto saved = saved_mode_output();
  saved.erase("currentModeId");
  const auto present = saved_mode_output();
  EXPECT_FALSE(policy::select_restore_mode(saved, &present));
}

TEST(LinuxPrivateDisplayRestorePolicy, RestoreModeMissingCurrentCatalogCannotAuthorizeModeCommand) {
  const auto saved = saved_mode_output();
  auto present = saved;
  present.erase("modes");
  EXPECT_FALSE(policy::select_restore_mode(saved, &present));
  present["modes"] = json::array();
  EXPECT_FALSE(policy::select_restore_mode(saved, &present));
}

TEST(LinuxPrivateDisplayRestorePolicy, FinalVerificationRejectsPanelReenabledByDisconnect) {
  const auto snapshot = external_desktop();
  auto current = snapshot;
  current["outputs"][0]["enabled"] = true;
  EXPECT_TRUE(policy::snapshot_matches(snapshot, current));
  EXPECT_FALSE(policy::snapshot_matches(snapshot, current, true));
  current["outputs"][0]["enabled"] = false;
  EXPECT_TRUE(policy::snapshot_matches(snapshot, current, true));
}

TEST(LinuxPrivateDisplayRestorePolicy, FinalVerificationRejectsShiftedDesktopAndChangedPrimary) {
  const auto snapshot = external_desktop();
  auto current = snapshot;
  current["outputs"][1]["pos"]["x"] = 1280;
  EXPECT_FALSE(policy::snapshot_matches(snapshot, current, true));
  current = snapshot;
  current["outputs"][1]["priority"] = 2;
  EXPECT_FALSE(policy::snapshot_matches(snapshot, current, true));
}

TEST(LinuxPrivateDisplayRestorePolicy, ExclusiveRestoreAllowsRetiringPrivatePriorityOnlyBeforeFinalVerification) {
  const auto snapshot = external_desktop();
  auto current = snapshot;
  current["outputs"][1]["priority"] = 2;
  current["outputs"].push_back({{"name", "Virtual-1"}, {"connected", true}, {"enabled", true}, {"priority", 1}});

  EXPECT_TRUE(policy::snapshot_matches(snapshot, current));
  EXPECT_FALSE(policy::snapshot_matches(snapshot, current, true));
  current["outputs"].erase(2);
  current["outputs"][1]["priority"] = 1;
  EXPECT_TRUE(policy::snapshot_matches(snapshot, current, true));
}

TEST(LinuxPrivateDisplayRestorePolicy, ActivationVerificationRejectsMismatchedPhysicalOutputs) {
  auto snapshot = external_desktop();
  auto second = snapshot["outputs"][1];
  second["name"] = "DP-2";
  second["priority"] = 2;
  second["rotation"] = 4;
  second["pos"]["x"] = 2560;
  snapshot["outputs"].push_back(second);
  auto current = snapshot;
  current["outputs"][1]["priority"] = 2;
  current["outputs"][2]["priority"] = 3;
  current["outputs"].push_back({{"name", "Virtual-1"}, {"connected", true}, {"enabled", true}, {"priority", 1}});
  EXPECT_TRUE(policy::snapshot_matches(snapshot, current));
  EXPECT_FALSE(policy::snapshot_matches(snapshot, current, true));

  // Priority may settle after hot-unplug. Completed restoration still requires
  // the saved mode, position, rotation, scale, HDR and activation.
  for (const auto &change : {json {{"enabled", false}}, json {{"connected", false}},
                            json {{"rotation", 1}}, json {{"scale", 1.0}}, json {{"hdr", false}},
                            json {{"pos", {{"x", 0}, {"y", 0}}}},
                            json {{"currentModeId", "different"}, {"size", {{"width", 1920}, {"height", 1080}}}}}) {
    auto mismatched = current;
    mismatched["outputs"][2].update(change);
    EXPECT_FALSE(policy::snapshot_matches(snapshot, mismatched)) << change;
  }
  current["outputs"].erase(3);
  current["outputs"][1]["priority"] = 1;
  current["outputs"][2]["priority"] = 2;
  EXPECT_TRUE(policy::snapshot_matches(snapshot, current, true));
}

TEST(LinuxPrivateDisplayRestorePolicy, FinalVerificationRejectsChangedOrientationScaleAndHdr) {
  const auto snapshot = external_desktop();
  for (const auto &change : {json {{"rotation", 8}}, json {{"scale", 1.0}}, json {{"hdr", false}}}) {
    auto current = snapshot;
    current["outputs"][1].update(change);
    EXPECT_FALSE(policy::snapshot_matches(snapshot, current, true));
  }
}

TEST(LinuxPrivateDisplayRestorePolicy, DisabledSavedOutputMayDisappearButActiveOutputMustSurvive) {
  const auto snapshot = external_desktop();
  auto current = snapshot;
  current["outputs"].erase(0);
  EXPECT_TRUE(policy::snapshot_matches(snapshot, current, true));
  current["outputs"] = json::array();
  EXPECT_FALSE(policy::snapshot_matches(snapshot, current, true));
}

TEST(LinuxPrivateDisplayRestorePolicy, RetiresReleasedPrivateOutputEvenAfterItsReservationWasErased) {
  const auto snapshot = external_desktop();
  auto current = snapshot;
  current["outputs"].push_back({{"name", "Virtual-1"}, {"connected", true}, {"enabled", true}});
  EXPECT_EQ(policy::retiring_outputs(snapshot, current, {"Virtual-1"}, {}),
            (std::set<std::string> {"Virtual-1"}));
}

TEST(LinuxPrivateDisplayRestorePolicy, RetiresDisabledConnectedPrivateOutputAndKeepsDisconnectedReservationForRetry) {
  auto snapshot = external_desktop();
  snapshot["outputs"].push_back({{"name", "Virtual-1"}, {"connected", false}, {"enabled", false}});
  auto current = snapshot;
  current["outputs"][2]["connected"] = true;
  current["outputs"].push_back({{"name", "Virtual-2"}, {"connected", false}, {"enabled", false}});
  EXPECT_EQ(policy::retiring_outputs(snapshot, current, {"Virtual-1", "Virtual-2"}, {"Virtual-2"}),
            (std::set<std::string> {"Virtual-1", "Virtual-2"}));
}

TEST(LinuxPrivateDisplayRestorePolicy, RetirementPreservesPreexistingActivePrivateDesktop) {
  const json snapshot {{"outputs", {{{"name", "Virtual-1"}, {"connected", true}, {"enabled", true}}}}};
  auto current = snapshot;
  current["outputs"].push_back({{"name", "Virtual-2"}, {"connected", true}, {"enabled", true}});
  EXPECT_EQ(policy::retiring_outputs(snapshot, current, {"Virtual-1", "Virtual-2"}, {"Virtual-1", "Virtual-2"}),
            (std::set<std::string> {"Virtual-2"}));
}

TEST(LinuxPrivateDisplayRestorePolicy, RetirementDoesNotDisconnectPhysicalOutputsOrUnreservedDisconnectedPrivateOutputs) {
  auto snapshot = external_desktop();
  auto current = snapshot;
  current["outputs"].push_back({{"name", "HDMI-A-1"}, {"connected", true}, {"enabled", true}});
  current["outputs"].push_back({{"name", "Virtual-1"}, {"connected", false}, {"enabled", false}});
  EXPECT_TRUE(policy::retiring_outputs(snapshot, current, {"Virtual-1"}, {}).empty());
  EXPECT_TRUE(policy::retiring_outputs(snapshot, current, {"Virtual-1"}, {"HDMI-A-1"}).empty());
}

TEST(LinuxPrivateDisplayRestorePolicy, MissingSecondaryDoesNotBlockActivationOfAvailableDesktop) {
  auto snapshot = external_desktop();
  auto second = snapshot["outputs"][1];
  second["name"] = "DP-2";
  second["priority"] = 2;
  snapshot["outputs"].push_back(second);
  auto current = snapshot;
  current["outputs"][2]["connected"] = false;
  current["outputs"][2]["enabled"] = false;
  current["outputs"][1]["priority"] = 2;
  current["outputs"].push_back({{"name", "Virtual-1"}, {"connected", true}, {"enabled", true}, {"priority", 1}});

  const auto available = policy::connected_activation_snapshot(snapshot, current);
  ASSERT_EQ(available["outputs"].size(), 2U);
  EXPECT_TRUE(policy::snapshot_matches(available, current));
  EXPECT_FALSE(policy::snapshot_matches(snapshot, current, true));
  EXPECT_EQ(snapshot["outputs"].size(), 3U); // Complete recovery baseline is retained.
  EXPECT_TRUE(snapshot["outputs"][2]["enabled"]);
}

TEST(LinuxPrivateDisplayRestorePolicy, AvailableActivationStillRequiresConnectedOutputsToActuallyEnable) {
  const auto snapshot = external_desktop();
  auto current = snapshot;
  current["outputs"][1]["enabled"] = false;
  const auto available = policy::connected_activation_snapshot(snapshot, current);
  EXPECT_EQ(available, snapshot);
  EXPECT_FALSE(policy::snapshot_matches(available, current));
}

TEST(LinuxPrivateDisplayRestorePolicy, MissingOutputCannotPassStrictGuardOrFinalRestoreVerification) {
  const auto snapshot = external_desktop();
  auto current = snapshot;
  current["outputs"].erase(1);
  EXPECT_FALSE(policy::snapshot_matches(snapshot, current));
  EXPECT_FALSE(policy::snapshot_matches(snapshot, current, true));
  const auto available = policy::connected_activation_snapshot(snapshot, current);
  ASSERT_EQ(available["outputs"].size(), 1U);
  EXPECT_FALSE(available["outputs"][0]["enabled"]);
}

TEST(LinuxPrivateDisplayRestorePolicy, PrefersConnectedPhysicalGuard) {
  const std::array candidates {
    policy::candidate_t {"Virtual-1", true, true, true},
    policy::candidate_t {"HDMI-A-1", true, true, false},
  };

  EXPECT_EQ(policy::select_guard(candidates), "HDMI-A-1");
}

TEST(LinuxPrivateDisplayRestorePolicy, FallsBackToPrivateGuardForPrivateBaseline) {
  const std::array candidates {
    policy::candidate_t {"Virtual-2", true, true, true},
  };

  EXPECT_EQ(policy::select_guard(candidates), "Virtual-2");
}

TEST(LinuxPrivateDisplayRestorePolicy, RetiringPrivateOutputCannotGuardItsOwnDisconnect) {
  const std::array candidates {
    policy::candidate_t {"Virtual-1", true, true, true, true},
  };

  EXPECT_FALSE(policy::select_guard(candidates).has_value());
}

TEST(LinuxPrivateDisplayRestorePolicy, DistinctPrivateOutputCanGuardRetirement) {
  const std::array candidates {
    policy::candidate_t {"Virtual-1", true, true, true, true},
    policy::candidate_t {"Virtual-2", true, true, true, false},
  };

  EXPECT_EQ(policy::select_guard(candidates), "Virtual-2");
}

TEST(LinuxPrivateDisplayRestorePolicy, RejectsDisabledAndDisconnectedGuards) {
  const std::array candidates {
    policy::candidate_t {"HDMI-A-1", false, true, false},
    policy::candidate_t {"DP-1", true, false, false},
  };

  EXPECT_FALSE(policy::select_guard(candidates).has_value());
}

TEST(LinuxPrivateDisplayRestorePolicy, EnabledDisconnectedBaselineHasNoUsableGuard) {
  const std::array candidates {
    policy::candidate_t {"HDMI-A-1", true, false, false},
  };

  EXPECT_FALSE(policy::select_guard(candidates).has_value());
}

TEST(LinuxPrivateDisplayRestorePolicy, HeadlessBaselineHasNoUsableGuard) {
  const std::array candidates {
    policy::candidate_t {"HDMI-A-1", false, true, false},
    policy::candidate_t {"Virtual-1", false, false, true},
  };

  EXPECT_FALSE(policy::select_guard(candidates).has_value());
}

TEST(LinuxPrivateDisplayRestorePolicy, MissingGuardActivationFailsSafely) {
  const std::map<std::string, std::vector<std::string>> activations {
    {"HDMI-A-1", {"output.HDMI-A-1.enable"}},
  };

  EXPECT_FALSE(policy::guard_activation(std::make_optional<std::string>("DP-1"), activations).has_value());
}

TEST(LinuxPrivateDisplayRestorePolicy, OwnsGuardNameWhenSnapshotNameStorageIsReused) {
  // Match restore_arguments: each JSON output supplies a copied local name.
  // Reusing that string must not turn the physical eDP-1 guard into "Virtu".
  std::string name = "eDP-1";
  std::vector<policy::candidate_t> candidates;
  candidates.push_back({name, true, true, false, false});
  name = "Virtual-1";
  candidates.push_back({name, true, true, true, true});
  const std::map<std::string, std::vector<std::string>> activations {
    {"eDP-1", {"output.eDP-1.enable"}},
    {"Virtual-1", {"output.Virtual-1.enable"}},
  };

  const auto guard = policy::select_guard(candidates);
  ASSERT_EQ(guard, "eDP-1");
  const auto activation = policy::guard_activation(guard, activations);
  ASSERT_TRUE(activation.has_value());
  EXPECT_EQ(*activation, activations.at("eDP-1"));
}

TEST(LinuxPrivateDisplayRestorePolicy, ResolvesKnownGuardActivation) {
  const std::map<std::string, std::vector<std::string>> activations {
    {"HDMI-A-1", {"output.HDMI-A-1.enable"}},
  };

  const auto activation = policy::guard_activation(std::make_optional<std::string>("HDMI-A-1"), activations);
  ASSERT_TRUE(activation.has_value());
  EXPECT_EQ(*activation, activations.at("HDMI-A-1"));
}

TEST(LinuxPrivateDisplayRestorePolicy, PreservesOnlyWorkingPrivateScanoutAtStartup) {
  EXPECT_TRUE(policy::preserve_private_scanout(false, true));
}

TEST(LinuxPrivateDisplayRestorePolicy, CleansPrivateScanoutWhenPhysicalCaptureIsReady) {
  EXPECT_FALSE(policy::preserve_private_scanout(true, true));
  EXPECT_FALSE(policy::preserve_private_scanout(true, false));
}

TEST(LinuxPrivateDisplayRestorePolicy, DoesNotPreserveUncapturablePrivateConnector) {
  EXPECT_FALSE(policy::preserve_private_scanout(false, false));
}

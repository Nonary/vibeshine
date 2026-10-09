#pragma once

#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace display_helper_integration::request_policy {
  enum class ConfigurationOption {
    Disabled,
    VerifyOnly,
    EnsureActive,
    EnsureOnlyDisplay,
  };

  enum class VirtualDisplayLayout {
    Exclusive,
    Extended,
    ExtendedPrimary,
    ExtendedIsolated,
    ExtendedPrimaryIsolated,
  };

  enum class DevicePreparation {
    VerifyOnly,
    EnsureActive,
    EnsurePrimary,
    EnsureOnlyDisplay,
  };

  struct Resolution {
    int width = 0;
    int height = 0;
  };

  struct Input {
    ConfigurationOption configuration_option {ConfigurationOption::Disabled};
    VirtualDisplayLayout layout {VirtualDisplayLayout::Exclusive};
    bool virtual_display = false;
    bool virtual_display_failed = false;
    bool physical_output_override = false;
    bool hdr_profile_selected = false;
    bool rtx_hdr_source_enabled = false;
    bool hdr_requested = false;
    std::string target_device_id;
    std::vector<std::vector<std::string>> topology_snapshot;
    std::optional<Resolution> remapped_resolution;
  };

  struct Result {
    bool dispatch = true;
    bool apply_hdr_profile_to_physical = false;
    std::optional<DevicePreparation> device_preparation;
    std::optional<bool> hdr_enabled;
    std::optional<Resolution> initial_resolution;
    std::optional<Resolution> applied_resolution;
    std::vector<std::vector<std::string>> topology;
  };

  [[nodiscard]] bool virtual_display_mutation_allowed(bool display_restore_in_progress);
  // An explicit physical capture target with automation disabled does not own
  // display recovery. HDR profile application still requires a baseline.
  [[nodiscard]] bool capture_only_physical_request(const Input &input);
  [[nodiscard]] bool supersede_restore_for_virtual_display(
    const std::function<void()> &disarm_restore,
    const std::function<bool()> &restore_in_progress
  );
  // A snapshot acknowledgement is required before enumeration or creation can
  // change the desktop. Supersede an older restore first, preserving its files.
  [[nodiscard]] bool prepare_virtual_display_baseline(
    const std::function<void()> &disarm_restore,
    const std::function<bool()> &restore_in_progress,
    const std::function<bool()> &snapshot_current
  );
  // Retain live peer outputs while restoring missing physical baseline members.
  [[nodiscard]] std::vector<std::vector<std::string>> merge_extended_topology(
    std::vector<std::vector<std::string>> current,
    const std::vector<std::vector<std::string>> &baseline,
    const std::string &target_device_id
  );

  [[nodiscard]] Result evaluate(const Input &input);
}  // namespace display_helper_integration::request_policy

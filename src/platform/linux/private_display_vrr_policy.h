/**
 * @file src/platform/linux/private_display_vrr_policy.h
 * @brief KScreen's optional VRR preference and its doctor command spelling.
 */
#pragma once

#include <optional>
#include <string_view>

namespace platf::linux_private_display::vrr_policy {
  // libkscreen serializes vrrPolicy only for VRR-capable outputs. Its enum is
  // Never=0, Always=1, Automatic=2; the doctor takes these lowercase names.
  template<typename Json>
  std::optional<std::string_view> argument(const Json &output) {
    if (!output.contains("vrrPolicy") || !output["vrrPolicy"].is_number_integer()) {
      return std::nullopt;
    }
    const auto &value = output["vrrPolicy"];
    if (value == 0) {
      return "never";
    }
    if (value == 1) {
      return "always";
    }
    if (value == 2) {
      return "automatic";
    }
    return std::nullopt;
  }

  template<typename Json>
  bool valid(const Json &output) {
    return !output.contains("vrrPolicy") || argument(output).has_value();
  }

  template<typename Json>
  bool matches(const Json &saved, const Json &current) {
    if (!saved.contains("vrrPolicy")) {
      return true;
    }
    const auto expected = argument(saved);
    return expected && expected == argument(current);
  }
}

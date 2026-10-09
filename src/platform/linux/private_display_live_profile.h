#pragma once

#include "src/remote_display_topology.h"

#include <algorithm>
#include <cmath>
#include <nlohmann/json.hpp>

namespace platf::linux_private_display::live_profile {
  // A healthy retained output belongs to its application. Only an explicit
  // profile request or a recreated/unusable output should receive a modeset.
  inline bool requires_apply(const remote_display_topology::node_t &node,
                             const nlohmann::json *output,
                             const bool newly_connected) {
    return node.apply_requested_mode || newly_connected || !output ||
           !output->value("connected", false) || !output->value("enabled", false) ||
           output->value("currentModeId", std::string {}).empty();
  }

  inline void observe(remote_display_topology::node_t &node,
                      const nlohmann::json *output,
                      const bool newly_connected) {
    node.apply_requested_mode = requires_apply(node, output, newly_connected);
    if (node.apply_requested_mode) return;
    const auto size = output->value("size", nlohmann::json::object());
    const int width = size.value("width", 0);
    const int height = size.value("height", 0);
    const double scale = output->value("scale", 1.0);
    double refresh = 0;
    for (const auto &mode : output->value("modes", nlohmann::json::array())) {
      if (mode.value("id", std::string {}) == output->value("currentModeId", std::string {})) {
        refresh = mode.value("refreshRate", 0.0);
        break;
      }
    }
    if (width <= 0 || height <= 0 || !std::isfinite(scale) || scale <= 0 ||
        !std::isfinite(refresh) || refresh <= 0) return;
    node.current_mode = remote_display_topology::mode_t {
      width, height, static_cast<int>(std::lround(refresh)), output->value("hdr", false)
    };
    node.layout_width = static_cast<int>(std::ceil(width / std::max(0.25, scale)));
    node.layout_height = static_cast<int>(std::ceil(height / std::max(0.25, scale)));
  }
}  // namespace platf::linux_private_display::live_profile

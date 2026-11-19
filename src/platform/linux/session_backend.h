/** @file Display capability boundary selected by the trusted machine session. */
#pragma once

#include <cstdlib>
#include <cstring>

namespace platf::linux_display {
  inline bool hyprland_kms_session() {
    const auto *machine = std::getenv("VIBESHINE_MACHINE_HOST");
    const auto *backend = std::getenv("VIBESHINE_SESSION_BACKEND");
    return machine && !std::strcmp(machine, "1") && backend && !std::strcmp(backend, "hyprland-kms");
  }
}  // namespace platf::linux_display

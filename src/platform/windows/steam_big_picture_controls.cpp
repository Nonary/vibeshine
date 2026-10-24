/**
 * @file src/platform/windows/steam_big_picture_controls.cpp
 * @brief Vibeshine stream controls inside Steam Big Picture.
 *
 * The helper launch, pipe, status pushes and actions arrive in later phases (see
 * docs/steam-big-picture-controls.md). Until then the module only records whether it is enabled
 * and reports that it is not implemented yet.
 */
#ifdef _WIN32
  #include "steam_big_picture_controls.h"

  #include <atomic>

namespace platf::steam::big_picture {
  namespace {
    std::atomic_bool enabled {false};
  }  // namespace

  void configure(bool value) {
    enabled = value;
  }

  void start() {
  }

  void stop() {
  }

  void notify_changed() {
  }

  nlohmann::json status() {
    return {
      {"status", true},
      {"supported", true},
      {"enabled", enabled.load()},
      {"implemented", false},
      {"state", "not_implemented"},
      {"features", nlohmann::json::object()},
    };
  }
}  // namespace platf::steam::big_picture
#endif

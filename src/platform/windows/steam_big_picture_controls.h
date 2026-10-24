/**
 * @file src/platform/windows/steam_big_picture_controls.h
 * @brief Vibeshine stream controls inside Steam Big Picture (Power menu entries and a header icon).
 */
#pragma once

#ifdef _WIN32
  #include "src/steam_big_picture_notify.h"

  #include <chrono>
  #include <nlohmann/json.hpp>
  #include <optional>

namespace platf::steam::big_picture {
  // Turn the feature on or off. Called from config parsing; cheap and safe from any thread.
  void configure(bool enabled);

  // Owned by nvhttp after initialization, like the end-stream hotkey: the actions end streams
  // through nvhttp. notify_changed() (src/steam_big_picture_notify.h) wakes the status updates.
  void start();
  void stop();

  // State for GET /api/steam/big_picture/status.
  nlohmann::json status();

  // Run the script's check mode in the running Steam and return the helper's "check_result"
  // message. nullopt when the helper isn't connected or doesn't answer in time.
  std::optional<nlohmann::json> check(std::chrono::milliseconds timeout = std::chrono::seconds(10));
}  // namespace platf::steam::big_picture
#endif

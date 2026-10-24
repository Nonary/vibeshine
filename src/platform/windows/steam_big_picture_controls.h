/**
 * @file src/platform/windows/steam_big_picture_controls.h
 * @brief Vibeshine stream controls inside Steam Big Picture (Power menu entries and a header icon).
 */
#pragma once

#ifdef _WIN32
  #include <nlohmann/json.hpp>

namespace platf::steam::big_picture {
  // Turn the feature on or off. Called from config parsing; cheap and safe from any thread.
  void configure(bool enabled);

  // Owned by nvhttp after initialization, like the end-stream hotkey.
  void start();
  void stop();

  // Wake the module after a stream, client or app lifecycle change. Never blocks.
  void notify_changed();

  // State for GET /api/steam/big_picture/status.
  nlohmann::json status();
}  // namespace platf::steam::big_picture
#endif

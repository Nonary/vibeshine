/**
 * @file src/platform/windows/steam_big_picture_debug_port.h
 * @brief Steam's debug-port flag file and Steam restarts, for the Big Picture controls' web UI.
 */
#pragma once

#ifdef _WIN32
  #include <filesystem>
  #include <optional>
  #include <stop_token>
  #include <string>

namespace platf::steam::big_picture::debug_port {
  struct steam_t {
    std::optional<std::filesystem::path> root;  ///< Steam's folder: the running steam.exe's, else the registered one.
    bool running = false;  ///< steam.exe runs in the console session.
    bool flag_file = false;  ///< The debug-port flag file exists in root.
    bool big_picture = false;  ///< Steam's Big Picture window is open.
  };

  // Look at Steam as the console user. Cheap enough for every status request.
  steam_t inspect();

  // Create (enabled) or delete the empty flag file in Steam's folder, as the console user so it can only
  // go where that user may write. Returns an error message on failure.
  std::optional<std::string> set_flag_file(bool enabled);

  // Close Steam and start it again as the console user, in Big Picture when big_picture is set. Blocks
  // while Steam closes (up to 40 s). Returns an error message on failure.
  std::optional<std::string> restart_steam(std::stop_token stop, bool big_picture);
}  // namespace platf::steam::big_picture::debug_port
#endif

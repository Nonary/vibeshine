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
  #include <system_error>

namespace platf::steam::big_picture::debug_port {
  // Windows error text in UTF-8. std::error_code::message() gives the ANSI code page, which
  // nlohmann::json refuses to serialize on non-English Windows.
  std::string error_text(const std::error_code &error);

  struct steam_t {
    std::optional<std::filesystem::path> root;  ///< Steam's folder: the running steam.exe's, else the registered one.
    bool running = false;  ///< steam.exe runs in the console session.
    bool flag_file = false;  ///< The debug-port flag file exists in root.
    bool flag_file_created = false;  ///< Vibeshine created that file, so turning the controls off may delete it.
    bool big_picture = false;  ///< Steam's Big Picture window is open.
  };

  // Look at Steam as the console user. Cheap enough for every status request.
  steam_t inspect();

  // Create (enabled) or delete the empty flag file in Steam's folder, as the console user so it can only
  // go where that user may write. A file Vibeshine creates is remembered for the uninstaller. Returns an
  // error message on failure.
  std::optional<std::string> set_flag_file(bool enabled);

  // For the uninstaller: delete the flag file if Vibeshine created it and it is still there, as the
  // console user. Steam keeps its debug port open until it next restarts.
  void remove_created_flag_file();

  // Close Steam and start it again as the console user, in Big Picture when big_picture is set. Blocks
  // while Steam closes; after 40 s, waits for steam.exe even if its web helpers are still running,
  // and gives up if it is still running 3 minutes after the shutdown request. A stop request cuts
  // that wait to 10 s more, but Steam is still started again. Returns an error message on failure.
  std::optional<std::string> restart_steam(std::stop_token stop, bool big_picture);
}  // namespace platf::steam::big_picture::debug_port
#endif

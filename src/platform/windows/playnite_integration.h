/**
 * @file src/platform/windows/playnite_integration.h
 * @brief Entry point for Playnite integration lifecycle on Windows.
 */
#pragma once

#include <memory>
#include <string>
#include <vector>

#include "src/platform/common.h"

namespace platf::playnite_integration {

  /**
   * @brief Starts Playnite integration (IPC server + handlers).
   * @return deinit guard that stops the integration on destruction.
   */
  [[nodiscard]] std::unique_ptr<::platf::deinit_t> start();

  // Query active IPC connection state (true if connected to Playnite)
  bool is_active();

  // Attempt to install the Playnite plugin. Returns true on success and sets error on failure.
  bool install_plugin(std::string &error);

  // Install the Playnite plugin to a specific destination directory (absolute path).
  // Returns true on success and sets error on failure.
  bool install_plugin_to(const std::string &dest_dir, std::string &error);

  // Compute the target extensions directory that would be used for installation.
  // Returns true on success and writes the absolute path to out.
  bool get_extension_target_dir(std::string &out);

  // Request Playnite to launch a game by its ID. Returns true if the
  // request was sent (or a best-effort fallback triggered), false otherwise.
  bool launch_game(const std::string &playnite_id);

  // Returns a JSON array string with minimal game info {id,name,categories}.
  // Returns false if data is unavailable.
  bool get_games_list_json(std::string &out_json);

}  // namespace platf::playnite_integration

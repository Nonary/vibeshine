/**
 * @file src/config_playnite.h
 * @brief Playnite integration configuration.
 */
#pragma once

#include <string>
#include <unordered_map>
#include <vector>

namespace config {

  struct playnite_t {
    bool enabled = false;                   // enable Playnite integration (IPC)
    bool auto_sync = false;                 // enable automatic sync from Playnite
    int recent_games = 10;                  // N most recent games
    std::vector<std::string> sync_categories; // categories to sync

    // Installation/paths
    std::string install_dir;                // Optional hint where Playnite is installed
    std::string extensions_dir;             // Playnite extensions dir; default: %AppData%/Playnite/Extensions/SunshinePlaynite
  };

  extern playnite_t playnite;

  // Consume Playnite-related config vars from the provided map.
  void apply_playnite(std::unordered_map<std::string, std::string> &vars);
}

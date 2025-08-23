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
    // Enabled flag removed; integration manager always runs (server up when plugin installed)
    bool auto_sync = true;  // enable automatic sync from Playnite
    int recent_games = 10;  // N most recent games
    // If > 0, only treat games whose last played time is within this many days
    // as "recent" for the purposes of recent-based auto-sync selection.
    int recent_max_age_days = 30;  // 0 = no age limit
    std::vector<std::string> sync_categories;  // categories to sync

    // Focus behavior
    int focus_attempts = 3;  // Count of confirmed re-applies of focus
    int focus_timeout_secs = 15;  // Total window to apply focus attempts
    bool focus_exit_on_first = false;  // Stop after first confirmed focus
    std::vector<std::string> exclude_games;  // Playnite game IDs to exclude from auto-sync

    // Auto-delete TTL for unplayed auto-synced apps (days). If > 0, an
    // auto-synced app that has not been launched from Playnite within this
    // many days of being added by auto-sync will be automatically removed.
    // 0 = remove immediately when it falls out of the selected set.
    int autosync_delete_after_days = 14;

    // When true, only purge auto-synced games that no longer qualify
    // if there is a qualifying replacement to fill the slot. When false,
    // always purge games that no longer qualify, even if it leaves fewer
    // than the configured recent slots.
    bool autosync_require_replacement = true;

    // Installation/paths (overrides removed)
  };

  extern playnite_t playnite;

  // Consume Playnite-related config vars from the provided map.
  void apply_playnite(std::unordered_map<std::string, std::string> &vars);
}  // namespace config

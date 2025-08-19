/**
 * @file src/platform/windows/playnite_integration.cpp
 * @brief Playnite integration lifecycle and message handling.
 */

#ifndef WIN32_LEAN_AND_MEAN
  #define WIN32_LEAN_AND_MEAN
#endif
#include "playnite_integration.h"

#include "src/confighttp.h"
#include "src/config.h"
#include "src/config_playnite.h"
#include "src/file_handler.h"
#include "src/logging.h"
#include "src/platform/windows/image_convert.h"
#include "src/platform/windows/ipc/misc_utils.h"
#include "src/platform/windows/playnite_ipc.h"
#include "src/platform/windows/playnite_protocol.h"
#include "src/process.h"

#include <algorithm>
#include <filesystem>
#include <KnownFolders.h>
#include <mutex>
#include <nlohmann/json.hpp>
#include <ShlObj.h>
#include <span>
#include <string>
#include <unordered_set>
#include <vector>
#include <Windows.h>
#include <winsock2.h>
#include <ctime>
#include <charconv>
#include <iomanip>
#include <sstream>

namespace platf::playnite_integration {

  class deinit_t_impl;  // forward
  static deinit_t_impl *g_instance = nullptr;

  class deinit_t_impl: public ::platf::deinit_t {
  public:
    deinit_t_impl() {
      if (!config::playnite.enabled) {
        BOOST_LOG(info) << "Playnite integration disabled";
        return;
      }
      BOOST_LOG(info) << "Playnite integration: enabled; starting IPC server";
      g_instance = this;
      server_ = std::make_unique<platf::playnite::IpcServer>();
      server_->set_message_handler([this](std::span<const uint8_t> bytes) {
        handle_message(bytes);
      });
      server_->start();
      // Start in a fresh snapshot state
      new_snapshot_ = true;
    }

    ~deinit_t_impl() override {
      if (server_) {
        server_->stop();
        server_.reset();
      }
      g_instance = nullptr;
    }

    bool is_server_active() const {
      return server_ && server_->is_active();
    }

    bool send_cmd_json_line(const std::string &s) {
      return server_ && server_->send_json_line(s);
    }

    void trigger_sync() {
      try {
        sync_apps_metadata();
      } catch (...) {}
    }

    void snapshot_games(std::vector<platf::playnite_protocol::Game> &out) {
      std::scoped_lock lk(mutex_);
      out = last_games_;
    }

  private:
    void handle_message(std::span<const uint8_t> bytes) {
      auto msg = platf::playnite_protocol::parse(bytes);
      using MT = platf::playnite_protocol::MessageType;
      if (msg.type == MT::Categories) {
        BOOST_LOG(info) << "Playnite: received " << msg.categories.size() << " categories";
        // Placeholder: could cache categories if needed
        // Treat a categories message as the beginning of a new full snapshot.
        {
          std::scoped_lock lk(mutex_);
          new_snapshot_ = true;
        }
      } else if (msg.type == MT::Games) {
        BOOST_LOG(info) << "Playnite: received " << msg.games.size() << " games";
        size_t added = 0;
        size_t skipped = 0;
        size_t before = 0;
        {
          std::scoped_lock lk(mutex_);
          if (new_snapshot_) {
            // Beginning a new snapshot accumulation.
            last_games_.clear();
            game_ids_.clear();
            new_snapshot_ = false;
          }
          before = last_games_.size();
          for (const auto &g : msg.games) {
            if (g.id.empty()) {
              skipped++;
              continue;
            }
            auto [it, ins] = game_ids_.insert(g.id);
            if (!ins) {
              skipped++;
              continue;
            }
            last_games_.push_back(g);
            added++;
          }
        }
        BOOST_LOG(info) << "Playnite: games batch processed added=" << added << " skipped=" << skipped
                        << " cumulative=" << (before + added) << " (unique IDs)";
        if (config::playnite.auto_sync) {
          BOOST_LOG(info) << "Playnite: auto_sync enabled; syncing apps metadata";
          try {
            sync_apps_metadata();
          } catch (const std::exception &e) {
            BOOST_LOG(error) << "Playnite sync failed: " << e.what();
          }
        }
      } else if (msg.type == MT::Status) {
        BOOST_LOG(info) << "Playnite: status '" << msg.statusName << "' id=" << msg.statusGameId;
        if (msg.statusName == "gameStopped") {
          proc::proc.terminate();
        }
      } else {
        BOOST_LOG(warning) << "Playnite: unrecognized message";
      }
    }

    static std::string to_lower_copy(std::string s) {
      std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) {
        return (char) std::tolower(c);
      });
      return s;
    }

    static std::string norm_path(const std::string &p) {
      std::string s = p;
      s.erase(std::remove(s.begin(), s.end(), '"'), s.end());
      for (auto &ch : s) {
        if (ch == '/') {
          ch = '\\';
        }
      }
      return to_lower_copy(s);
    }

    void sync_apps_metadata() {
      using nlohmann::json;
      const std::string path = config::stream.file_apps;
      std::string content = file_handler::read_file(path.c_str());
      json root = json::parse(content);
      if (!root.contains("apps") || !root["apps"].is_array()) {
        BOOST_LOG(warning) << "apps.json has no 'apps' array";
        return;
      }

      struct GRef {
        const platf::playnite_protocol::Game *g;
      };

      std::unordered_map<std::string, GRef> by_exe;
      std::unordered_map<std::string, GRef> by_dir;
      std::unordered_map<std::string, GRef> by_id;
      // Filter games according to config (categories and recent)
      std::vector<platf::playnite_protocol::Game> selected;
      std::unordered_map<std::string, int> source_flags;  // bit 1: recent, bit 2: category
      selected.reserve(last_games_.size());
      // Build installed-only list once and track install state for immediate purging
      std::vector<platf::playnite_protocol::Game> installed_games;
      std::unordered_set<std::string> uninstalled_ids;
      {
        std::scoped_lock lk(mutex_);
        installed_games = last_games_;
        for (const auto &g : last_games_) {
          if (!g.installed && !g.id.empty()) uninstalled_ids.insert(to_lower_copy(g.id));
        }
      }
      installed_games.erase(std::remove_if(installed_games.begin(), installed_games.end(), [](const auto &g) {
                              return !g.installed;
                            }),
                            installed_games.end());

      int recentN = std::max(0, config::playnite.recent_games);
      BOOST_LOG(info) << "Playnite sync: selecting recentN=" << recentN;

      // Time-based filters
      // - recent_age_days: limits what qualifies as "recent" (recent selection only)
      // - delete_after_days: TTL for unplayed auto-synced apps (purge logic)
      int recent_age_days = 0;
      int delete_after_days = 0;
      try { recent_age_days = std::max(0, config::playnite.recent_max_age_days); } catch (...) { recent_age_days = 0; }
      try { delete_after_days = std::max(0, config::playnite.autosync_delete_after_days); } catch (...) { delete_after_days = 0; }
      auto now_time = std::time(nullptr);
      std::time_t recent_cutoff_time = now_time - static_cast<long long>(recent_age_days) * 86400LL;

      // Helper: parse Playnite lastPlayed (ISO8601 variants) into time_t UTC.
      auto parse_time = [](const std::string &s, std::time_t &out) -> bool {
        if (s.empty()) return false;
        // Accept formats like:
        // YYYY-MM-DDTHH:MM:SSZ
        // YYYY-MM-DD HH:MM:SS
        // YYYY-MM-DDTHH:MM:SS.mmmmmmZ
        // YYYY-MM-DDTHH:MM:SS+/-HH:MM
        // We'll manually extract components.
        int Y=0,M=0,D=0,h=0,m=0,sec=0,off_sign=0,off_h=0,off_m=0;
        size_t pos = 0;
        auto read_int = [&](int &dst, size_t count) -> bool {
          if (pos + count > s.size()) return false;
          int val = 0;
          for (size_t i=0;i<count;++i) {
            char c = s[pos+i];
            if (c < '0' || c > '9') return false;
            val = val*10 + (c-'0');
          }
          pos += count;
          dst = val;
          return true;
        };
        if (!read_int(Y,4)) return false;
        if (pos>=s.size() || s[pos] != '-') return false; ++pos;
        if (!read_int(M,2)) return false;
        if (pos>=s.size() || s[pos] != '-') return false; ++pos;
        if (!read_int(D,2)) return false;
        if (pos>=s.size()) return false;
        if (s[pos] == 'T' || s[pos] == 't' || s[pos] == ' ') ++pos; else return false;
        if (!read_int(h,2)) return false;
        if (pos>=s.size() || s[pos] != ':') return false; ++pos;
        if (!read_int(m,2)) return false;
        if (pos>=s.size() || s[pos] != ':') return false; ++pos;
        if (!read_int(sec,2)) return false;
        // Optional fractional seconds
        if (pos < s.size() && s[pos] == '.') {
          ++pos;
          while (pos < s.size() && std::isdigit((unsigned char)s[pos])) ++pos; // skip
        }
        // Timezone: Z or +/-HH:MM or nothing (treat as local -> assume UTC to be conservative)
        if (pos < s.size()) {
          char c = s[pos];
          if (c == 'Z' || c == 'z') {
            ++pos;
          } else if (c=='+' || c=='-') {
            off_sign = (c=='+')?1:-1;
            ++pos;
            if (!read_int(off_h,2)) return false;
            if (pos>=s.size() || s[pos] != ':') return false; ++pos;
            if (!read_int(off_m,2)) return false;
          }
        }
        std::tm tm{};
        tm.tm_year = Y - 1900;
        tm.tm_mon = M - 1;
        tm.tm_mday = D;
        tm.tm_hour = h;
        tm.tm_min = m;
        tm.tm_sec = sec;
#if defined(_WIN32)
        // _mkgmtime converts tm (UTC) to time_t
        std::time_t t = _mkgmtime(&tm);
#else
        std::time_t t = timegm(&tm);
#endif
        if (t == (std::time_t)-1) return false;
        // Apply offset if present (convert to UTC). If +HH:MM in string, local time is ahead of UTC.
        if (off_sign != 0) {
          long offset_seconds = (off_h * 3600L + off_m * 60L) * off_sign;
          t -= offset_seconds; // convert to UTC
        }
        out = t;
        return true;
      };
      if (recent_age_days > 0) {
        BOOST_LOG(info) << "Playnite sync: recent_max_age_days=" << recent_age_days << " cutoff_unix=" << recent_cutoff_time;
      }

      // Build exclusions set (lowercased IDs) and apply during selection so they don't consume slots
      std::unordered_set<std::string> excl;
      if (!config::playnite.exclude_games.empty()) {
        for (auto s : config::playnite.exclude_games) {
          std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c){ return (char)std::tolower(c); });
          excl.insert(std::move(s));
        }
      }

  if (recentN > 0) {
        // Strict cap: take most recent installed within grace window (if grace configured)
        std::sort(installed_games.begin(), installed_games.end(), [](const auto &a, const auto &b) {
          return a.lastPlayed > b.lastPlayed;  // ISO8601 sorts lexicographically
        });
        size_t skipped_out_of_grace = 0;
        for (int i = 0, taken = 0; i < (int) installed_games.size() && taken < recentN; ++i) {
          const auto &g = installed_games[i];
          std::string id = to_lower_copy(g.id);
          if (!id.empty() && excl.contains(id)) continue; // skip excluded without consuming a slot
          if (recent_age_days > 0) {
            std::time_t gp = 0;
            bool okp = parse_time(g.lastPlayed, gp);
            if (!okp || gp < recent_cutoff_time) { skipped_out_of_grace++; continue; }
          }
          selected.push_back(g);
            source_flags[g.id] |= 0x1;
          ++taken;
        }
        if (recent_age_days > 0) {
          BOOST_LOG(info) << "Playnite sync: skipped " << skipped_out_of_grace << " games outside grace window";
        }
      } else if (!config::playnite.sync_categories.empty()) {
        // Categories only (no recent cap)
        auto catset = std::unordered_set<std::string>();
        for (const auto &c : config::playnite.sync_categories) {
          catset.insert(to_lower_copy(c));
        }
        BOOST_LOG(info) << "Playnite sync: category filter size=" << catset.size();
        for (const auto &g : installed_games) {
          bool any = false;
          for (const auto &cn : g.categories) {
            if (catset.contains(to_lower_copy(cn))) {
              any = true;
              break;
            }
          }
          if (any) {
            // Skip excluded ids
            std::string id = to_lower_copy(g.id);
            if (!id.empty() && excl.contains(id)) continue;
            // Do not apply recent_age_days to category-only selection; categories are independent of recency
            selected.push_back(g);
            source_flags[g.id] |= 0x2;
          }
        }
      } else {
        // No selection criteria -> select nothing
      }
      // Exclusions already applied during selection so they don't consume slots

      BOOST_LOG(info) << "Playnite sync: total selected games=" << selected.size();

      // Helper: current time as ISO8601 UTC string
      auto now_iso8601 = [](){
        std::time_t t = std::time(nullptr);
        std::tm tm{};
#if defined(_WIN32)
        gmtime_s(&tm, &t);
#else
        gmtime_r(&t, &tm);
#endif
        std::ostringstream oss;
        oss << std::put_time(&tm, "%Y-%m-%dT%H:%M:%SZ");
        return oss.str();
      };

      // Build indexes from selected games
      for (const auto &g : selected) {
        if (!g.exe.empty()) {
          by_exe[norm_path(g.exe)] = GRef {&g};
        }
        if (!g.workingDir.empty()) {
          by_dir[norm_path(g.workingDir)] = GRef {&g};
        }
        if (!g.id.empty()) {
          by_id[g.id] = GRef {&g};
        }
      }

      bool changed = false;
      size_t matched = 0;
      std::unordered_set<std::string> matched_ids;
      for (auto &app : root["apps"]) {
        std::string cmd = app.contains("cmd") && app["cmd"].is_string() ? app["cmd"].get<std::string>() : std::string();
        std::string wdir = app.contains("working-dir") && app["working-dir"].is_string() ? app["working-dir"].get<std::string>() : std::string();
        const platf::playnite_protocol::Game *match = nullptr;
        // Prefer explicit playnite-id if present (manual-added Playnite app)
        try {
          if (app.contains("playnite-id") && app["playnite-id"].is_string()) {
            std::string pid = app["playnite-id"].get<std::string>();
            auto iti = by_id.find(pid);
            if (iti != by_id.end()) {
              match = iti->second.g;
            }
          }
        } catch (...) {}
        if (!cmd.empty()) {
          auto it = by_exe.find(norm_path(cmd));
          if (it != by_exe.end()) {
            match = it->second.g;
          }
        }
        if (!match && !wdir.empty()) {
          auto it2 = by_dir.find(norm_path(wdir));
          if (it2 != by_dir.end()) {
            match = it2->second.g;
          }
        }
        if (!match) {
          continue;
        }
        ++matched;
        matched_ids.insert(match->id);

        try {
          if (!match->name.empty() && (!app.contains("name") || app["name"].get<std::string>() != match->name)) {
            app["name"] = match->name;
            changed = true;
          }
        } catch (...) {}

        try {
          if (!match->boxArtPath.empty()) {
            std::filesystem::path src = match->boxArtPath;
            std::filesystem::path dstDir = platf::appdata() / "covers";
            file_handler::make_directory(dstDir.string());
            // Use deterministic filename: playnite_<id>.png
            std::filesystem::path dst = dstDir / ("playnite_" + match->id + ".png");
            bool needConvert = true;
            try {
              auto lower = to_lower_copy(src.string());
              if (lower.size() >= 4 && lower.substr(lower.size() - 4) == ".png") {
                // Copy as PNG and normalize DPI to 96 via re-encode for consistency
                needConvert = true;
              }
            } catch (...) {}

            bool ok = false;
            if (std::filesystem::exists(dst)) {
              ok = true;  // reuse existing converted image
            } else {
              std::wstring wsrc = std::filesystem::path(src).wstring();
              std::wstring wdst = std::filesystem::path(dst).wstring();
              ok = platf::img::convert_to_png_96dpi(wsrc, wdst);
            }
            if (ok) {
              std::string out = dst.generic_string();
              if (!app.contains("image-path") || app["image-path"].get<std::string>() != out) {
                app["image-path"] = out;
                changed = true;
              }
            }
          }
        } catch (...) {}

        try {
          app["playnite-id"] = match->id;
          changed = true;
        } catch (...) {}

        // Do not expose command for Playnite-managed apps; erase if present
        try {
          if (app.contains("cmd")) {
            app.erase("cmd");
            changed = true;
          }
        } catch (...) {}
        // Clear working-dir since launcher manages its own context internally
        try {
          if (app.contains("working-dir")) {
            app.erase("working-dir");
            changed = true;
          }
        } catch (...) {}
        // Mark source for UX
        try {
          int flags = 0;
          auto itf = source_flags.find(match->id);
          if (itf != source_flags.end()) {
            flags = itf->second;
          }
          std::string src = flags == 0 ? std::string("unknown") : (flags == 3 ? std::string("recent+category") : (flags == 1 ? std::string("recent") : std::string("category")));
          app["playnite-source"] = src;
          changed = true;
          app["playnite-managed"] = "auto";
        } catch (...) {}
      }

      // Build set of selected IDs for purge/add decisions
      std::unordered_set<std::string> selected_ids;
      for (const auto &g : selected) {
        selected_ids.insert(g.id);
      }

      // Decide which auto-synced apps to keep or purge.
      // - For Recent mode: optionally require replacements to purge non-qualifying apps.
      // - For Category mode: strictly mirror category filter (no replacement concept).
      // - TTL: always purge unplayed apps after delete_after_days (if configured).
      try {
        // Build a quick lookup of lastPlayed for installed games (id -> time_t)
        std::unordered_map<std::string, std::time_t> last_played_map;
        last_played_map.reserve(installed_games.size());
        for (const auto &g : installed_games) {
          if (!g.id.empty() && !g.lastPlayed.empty()) {
            std::time_t tp=0; if (parse_time(g.lastPlayed, tp)) last_played_map.emplace(g.id, tp);
          }
        }
        // Collect current auto-managed list and track selected/new
        std::unordered_set<std::string> current_auto_ids;
        current_auto_ids.reserve(root["apps"].size());
        for (auto &app : root["apps"]) {
          try {
            if (app.contains("playnite-managed") && app["playnite-managed"].is_string() && app["playnite-managed"].get<std::string>() == "auto" && app.contains("playnite-id") && app["playnite-id"].is_string()) {
              current_auto_ids.insert(app["playnite-id"].get<std::string>());
            }
          } catch (...) {}
        }
        std::unordered_set<std::string> selected_ids;
        selected_ids.reserve(selected.size());
        for (const auto &g : selected) selected_ids.insert(g.id);

        const bool recent_mode = (recentN > 0);
        const bool require_repl = config::playnite.autosync_require_replacement;

        // Determine how many replacements we actually have available (recent mode only)
        size_t replacements_available = 0;
        if (recent_mode) {
          for (const auto &sid : selected_ids) if (!current_auto_ids.contains(sid)) ++replacements_available;
        }

        nlohmann::json kept = nlohmann::json::array();
        for (auto &app : root["apps"]) {
          bool is_auto = false;
          std::string pid;
          try {
            is_auto = app.contains("playnite-managed") && app["playnite-managed"].is_string() && app["playnite-managed"].get<std::string>() == "auto";
            if (app.contains("playnite-id") && app["playnite-id"].is_string()) {
              pid = app["playnite-id"].get<std::string>();
            }
          } catch (...) {}
          if (is_auto && !pid.empty()) {
            // Apply delete-after (days) for unplayed auto-synced apps: if configured and the
            // app hasn't been played since being auto-added and the deadline has passed, drop it.
            if (delete_after_days > 0) {
              std::time_t added_at = 0;
              bool has_added_at = false;
              try {
                if (app.contains("playnite-added-at") && app["playnite-added-at"].is_string()) {
                  std::string ad = app["playnite-added-at"].get<std::string>();
                  has_added_at = parse_time(ad, added_at);
                }
              } catch (...) {}
              if (!has_added_at) {
                // If we don't know when it was added, stamp now to avoid premature deletion
                added_at = now_time;
                try { app["playnite-added-at"] = nlohmann::json(now_iso8601()); changed = true; } catch (...) {}
              }
              std::time_t deadline = added_at + static_cast<long long>(delete_after_days) * 86400LL;
              bool past_deadline = now_time >= deadline;
              bool played_since_added = false;
              auto itlp = last_played_map.find(pid);
              if (itlp != last_played_map.end()) {
                played_since_added = itlp->second >= added_at;
              }
              if (past_deadline && !played_since_added) {
                // Delete unplayed auto-synced app after TTL
                changed = true;
                continue;
              }
            }

            // If the game is uninstalled, drop it immediately
            std::string lpid = to_lower_copy(pid);
            if (uninstalled_ids.contains(lpid)) { changed = true; continue; }

            // Selection purge policy
            if (!selected_ids.contains(pid)) {
              if (recent_mode && require_repl) {
                // Only purge if we have replacements to fill the slot
                if (replacements_available > 0) {
                  --replacements_available;
                  changed = true;
                  continue;
                }
                // Otherwise keep to preserve slot occupancy
              } else {
                // Category mode or permissive purge policy: remove non-qualifying entries
                changed = true;
                continue;
              }
            }
          }
          kept.push_back(app);
        }
        root["apps"] = kept;
      } catch (...) {}

      // Add new apps for selected Playnite games that were not matched to existing entries
      try {
        for (const auto &g : selected) {
          if (g.id.empty() || matched_ids.contains(g.id)) {
            continue;
          }

          nlohmann::json app;
          app["name"] = g.name;
          app["playnite-id"] = g.id;
          // Record when this auto-synced entry was added to support TTL for unplayed apps
          try { app["playnite-added-at"] = nlohmann::json(now_iso8601()); } catch (...) {}
          // Mark source for UX
          int flags = 0;
          auto itf = source_flags.find(g.id);
          if (itf != source_flags.end()) {
            flags = itf->second;
          }
          std::string src = flags == 0 ? std::string("unknown") : (flags == 3 ? std::string("recent+category") : (flags == 1 ? std::string("recent") : std::string("category")));
          app["playnite-source"] = src;
          app["playnite-managed"] = "auto";

          // Cover image -> config/covers/playnite_<id>.png
          try {
            if (!g.boxArtPath.empty()) {
              std::filesystem::path srcp = g.boxArtPath;
              std::filesystem::path dstDir = platf::appdata() / "covers";
              file_handler::make_directory(dstDir.string());
              std::filesystem::path dst = dstDir / ("playnite_" + g.id + ".png");
              bool ok = false;
              if (std::filesystem::exists(dst)) {
                ok = true;  // reuse if already converted
              } else {
                std::wstring wsrc = srcp.wstring();
                std::wstring wdst = dst.wstring();
                ok = platf::img::convert_to_png_96dpi(wsrc, wdst);
              }
              if (ok) {
                app["image-path"] = dst.generic_string();
              }
            }
          } catch (...) {}

          root["apps"].push_back(app);
          changed = true;
        }
      } catch (...) {}

      if (changed) {
        // Update apps file and refresh client cache
        confighttp::refresh_client_apps_cache(root);
        BOOST_LOG(info) << "Playnite sync: apps.json updated";
        BOOST_LOG(info) << "Playnite sync: matched apps updated count=" << matched;
      } else {
        BOOST_LOG(info) << "Playnite sync: no changes to apps.json (matched=" << matched << ")";
      }
    }

    std::vector<platf::playnite_protocol::Game> last_games_;
    std::mutex mutex_;

    std::unique_ptr<platf::playnite::IpcServer> server_;
    bool new_snapshot_ = true;  // Indicates next games message starts a new accumulation
    std::unordered_set<std::string> game_ids_;  // Track unique IDs during accumulation
  };

  std::unique_ptr<::platf::deinit_t> start() {
    return std::make_unique<deinit_t_impl>();
  }

  bool is_active() {
    auto inst = g_instance;
    if (inst) {
      return inst->is_server_active();
    }
    return false;
  }

  // Safe implementation to avoid problematic wide-char literal in older compilers
  static bool resolve_extensions_dir_safe(std::filesystem::path &destOut) {
    BOOST_LOG(info) << "Playnite installer: resolving extensions dir (begin)";
    BOOST_LOG(info) << "Playnite installer: resolving extensions dir";
    if (!config::playnite.extensions_dir.empty()) {
      destOut = std::filesystem::path(config::playnite.extensions_dir);
      BOOST_LOG(info) << "Playnite installer: using configured extensions_dir=" << destOut.string();
      return true;
    }
    auto pids1 = platf::dxgi::find_process_ids_by_name(L"Playnite.DesktopApp.exe");
    auto pids2 = platf::dxgi::find_process_ids_by_name(L"Playnite.FullscreenApp.exe");
    BOOST_LOG(info) << "Playnite installer: found Playnite PIDs desktop=" << pids1.size() << ", fullscreen=" << pids2.size();
    std::vector<DWORD> pids = pids1;
    pids.insert(pids.end(), pids2.begin(), pids2.end());
    for (DWORD pid : pids) {
      HANDLE hp = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
      if (!hp) {
        BOOST_LOG(info) << "Playnite installer: OpenProcess failed for PID=" << pid;
        continue;
      }
      std::wstring buf;
      buf.resize(32768);
      DWORD size = static_cast<DWORD>(buf.size());
      if (QueryFullProcessImageNameW(hp, 0, buf.data(), &size)) {
        buf.resize(size);
        std::filesystem::path exePath = buf;
        std::filesystem::path base = exePath.parent_path();
        std::filesystem::path extDir = base / L"Extensions" / L"SunshinePlaynite";
        BOOST_LOG(info) << "Playnite installer: found running Playnite pid=" << pid << ", base=" << base.string();
        CloseHandle(hp);
        destOut = extDir;
        return true;
      } else {
        BOOST_LOG(info) << "Playnite installer: QueryFullProcessImageNameW failed for PID=" << pid;
        CloseHandle(hp);
      }
    }
    auto userTok = platf::dxgi::retrieve_users_token(false);
    if (!userTok) {
      BOOST_LOG(info) << "Playnite installer: retrieve_users_token failed";
      return false;
    }
    PWSTR roaming = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, userTok, &roaming)) && roaming) {
      destOut = std::filesystem::path(roaming) / L"Playnite" / L"Extensions" / L"SunshinePlaynite";
      BOOST_LOG(info) << "Playnite installer: resolved via active user token to " << destOut.string();
      CoTaskMemFree(roaming);
      CloseHandle(userTok);
      return true;
    }
    CloseHandle(userTok);
    return false;
  }

  bool get_extension_target_dir(std::string &out) {
    std::filesystem::path dest;
    if (!resolve_extensions_dir_safe(dest)) {
      return false;
    }
    out = dest.string();
    return true;
  }

  bool launch_game(const std::string &playnite_id) {
#ifndef _WIN32
    (void) playnite_id;
    return false;
#else
    auto inst = g_instance;
    if (!inst) {
      return false;
    }
    // Build a simple command JSON that the plugin reads line-delimited
    nlohmann::json j;
    j["type"] = "command";
    j["command"] = "launch";
    j["id"] = playnite_id;
    std::string s = j.dump();
    return inst->send_cmd_json_line(s);
#endif
  }

  bool get_games_list_json(std::string &out_json) {
#ifndef _WIN32
    (void) out_json;
    return false;
#else
    auto inst = g_instance;
    if (!inst) {
      return false;
    }
    nlohmann::json arr = nlohmann::json::array();
    std::vector<platf::playnite_protocol::Game> copy;
    inst->snapshot_games(copy);
    for (const auto &g : copy) {
      nlohmann::json j;
      j["id"] = g.id;
      j["name"] = g.name;
      j["categories"] = g.categories;
      j["installed"] = g.installed;
      arr.push_back(std::move(j));
    }
    out_json = arr.dump();
    return true;
#endif
  }

  bool force_sync() {
#ifndef _WIN32
    return false;
#else
    auto inst = g_instance;
    if (!inst) {
      return false;
    }
    inst->trigger_sync();
    return true;
#endif
  }

  bool get_cover_png_for_playnite_game(const std::string &playnite_id, std::string &out_path) {
#ifndef _WIN32
    (void) playnite_id;
    (void) out_path;
    return false;
#else
    auto inst = g_instance;
    if (!inst) {
      return false;
    }
    // Snapshot games
    std::vector<platf::playnite_protocol::Game> copy;
    inst->snapshot_games(copy);
    const platf::playnite_protocol::Game *gptr = nullptr;
    for (const auto &g : copy) {
      if (g.id == playnite_id) {
        gptr = &g;
        break;
      }
    }
    if (!gptr || gptr->boxArtPath.empty()) {
      return false;
    }

    try {
      std::filesystem::path src = gptr->boxArtPath;
      std::filesystem::path dstDir = platf::appdata() / "covers";
      file_handler::make_directory(dstDir.string());
      std::filesystem::path dst = dstDir / ("playnite_" + playnite_id + ".png");
      bool ok = false;
      if (std::filesystem::exists(dst)) {
        ok = true;
      } else {
        std::wstring wsrc = src.wstring();
        std::wstring wdst = dst.wstring();
        ok = platf::img::convert_to_png_96dpi(wsrc, wdst);
      }
      if (ok) {
        out_path = dst.generic_string();
        return true;
      }
    } catch (...) {}
    return false;
#endif
  }

  static bool do_install_plugin_impl(const std::string &dest_override, std::string &error) {
#ifndef _WIN32
    error = "Playnite integration is only supported on Windows";
    return false;
#else
    try {
      // Determine source directory: alongside the Sunshine executable under plugins/playnite/SunshinePlaynite
      std::wstring exePath;
      exePath.resize(MAX_PATH);
      GetModuleFileNameW(nullptr, exePath.data(), (DWORD) exePath.size());
      exePath.resize(wcslen(exePath.c_str()));
      std::filesystem::path exeDir = std::filesystem::path(exePath).parent_path();
      std::filesystem::path srcDir = exeDir / L"plugins" / L"playnite" / L"SunshinePlaynite";
      BOOST_LOG(info) << "Playnite installer: srcDir=" << srcDir.string();
      BOOST_LOG(info) << "Playnite installer: src exists? " << (std::filesystem::exists(srcDir) ? "yes" : "no");
      BOOST_LOG(info) << "Playnite installer: src file(extension.yaml) exists? " << (std::filesystem::exists(srcDir / L"extension.yaml") ? "yes" : "no");
      BOOST_LOG(info) << "Playnite installer: src file(SunshinePlaynite.psm1) exists? " << (std::filesystem::exists(srcDir / L"SunshinePlaynite.psm1") ? "yes" : "no");
      if (!std::filesystem::exists(srcDir)) {
        error = "Plugin source not found: " + srcDir.string();
        return false;
      }

      // Determine destination directory (support SYSTEM context and running Playnite)
      std::filesystem::path destDir;
      if (!dest_override.empty()) {
        destDir = std::filesystem::path(dest_override);
        BOOST_LOG(info) << "Playnite installer: using API override destDir=" << destDir.string();
      } else if (!config::playnite.extensions_dir.empty()) {
        destDir = config::playnite.extensions_dir;
        BOOST_LOG(info) << "Playnite installer: using configured extensions_dir=" << destDir.string();
      } else {
        // Prefer the same resolution used by status API
        std::string resolved;
        if (platf::playnite_integration::get_extension_target_dir(resolved)) {
          destDir = std::filesystem::path(resolved);
          BOOST_LOG(info) << "Playnite installer: using resolved target dir from API=" << destDir.string();
        } else if (resolve_extensions_dir_safe(destDir)) {
          BOOST_LOG(info) << "Playnite installer: using resolved target dir via safe resolver=" << destDir.string();
        } else {
          // Fallback to current user's %AppData% if we cannot resolve via token/process
          wchar_t appdataPath[MAX_PATH] = {};
          if (FAILED(SHGetFolderPathW(nullptr, CSIDL_APPDATA, nullptr, SHGFP_TYPE_CURRENT, appdataPath))) {
            error = "Failed to resolve %AppData% path";
            return false;
          }
          destDir = std::filesystem::path(appdataPath) / L"Playnite" / L"Extensions" / L"SunshinePlaynite";
          BOOST_LOG(info) << "Playnite installer: fallback destDir=%AppData% -> " << destDir.string();
        }
      }
      std::error_code ec;
      std::filesystem::create_directories(destDir, ec);

      auto copy_one = [&](const wchar_t *name) {
        ec.clear();
        auto src = srcDir / name;
        auto dst = destDir / name;
        std::wstring wname(name);
        std::string sname(wname.begin(), wname.end());
        BOOST_LOG(info) << "Playnite installer: copying " << sname << " from " << src.string() << " to " << dst.string();
        std::filesystem::copy_file(src, dst, std::filesystem::copy_options::overwrite_existing, ec);
        return !ec;
      };

      if (!copy_one(L"extension.yaml") || !copy_one(L"SunshinePlaynite.psm1")) {
        error = "Failed to copy plugin files to " + destDir.string();
        return false;
      }
      BOOST_LOG(info) << "Playnite installer: installation complete";
      return true;
    } catch (const std::exception &e) {
      BOOST_LOG(info) << "Playnite installer: exception: " << e.what();
      error = e.what();
      return false;
    }
#endif
  }

  bool install_plugin(std::string &error) {
    return do_install_plugin_impl(std::string(), error);
  }

  bool install_plugin_to(const std::string &dest_dir, std::string &error) {
    return do_install_plugin_impl(dest_dir, error);
  }

  

}  // namespace platf::playnite_integration

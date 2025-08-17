/**
 * @file src/platform/windows/playnite_integration.cpp
 * @brief Playnite integration lifecycle and message handling.
 */

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>

#include "playnite_integration.h"

#include <span>
#include <string>
#include <vector>

#include "src/logging.h"
#include "src/platform/windows/playnite_ipc.h"
#include "src/platform/windows/playnite_protocol.h"
#include "src/config_playnite.h"
#include "src/process.h"
#include "src/file_handler.h"
#include "src/config.h"
#include <nlohmann/json.hpp>
#include <unordered_set>
#include <algorithm>
#include <filesystem>
#include <mutex>
#include "src/platform/windows/image_convert.h"
#include "src/platform/windows/ipc/misc_utils.h"
#include <Windows.h>
#include <KnownFolders.h>
#include <ShlObj.h>

namespace platf::playnite_integration {

  class deinit_t_impl; // forward
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
    }

    ~deinit_t_impl() override {
      if (server_) {
        server_->stop();
        server_.reset();
      }
      g_instance = nullptr;
    }

    bool is_server_active() const { return server_ && server_->is_active(); }
    bool send_cmd_json_line(const std::string &s) { return server_ && server_->send_json_line(s); }

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
      } else if (msg.type == MT::Games) {
        BOOST_LOG(info) << "Playnite: received " << msg.games.size() << " games";
        {
          std::scoped_lock lk(mutex_);
          last_games_ = msg.games;
        }
        if (config::playnite.auto_sync) {
          BOOST_LOG(info) << "Playnite: auto_sync enabled; syncing apps metadata";
          try { sync_apps_metadata(); }
          catch (const std::exception &e) { BOOST_LOG(error) << "Playnite sync failed: " << e.what(); }
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
      std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return (char) std::tolower(c); });
      return s;
    }

    static std::string norm_path(const std::string &p) {
      std::string s = p;
      s.erase(std::remove(s.begin(), s.end(), '"'), s.end());
      for (auto &ch : s) { if (ch == '/') ch = '\\'; }
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

      struct GRef { const platf::playnite_protocol::Game *g; };
      std::unordered_map<std::string, GRef> by_exe;
      std::unordered_map<std::string, GRef> by_dir;
      // Filter games according to config (categories and recent)
      std::vector<platf::playnite_protocol::Game> selected;
      std::unordered_map<std::string, int> source_flags; // bit 1: recent, bit 2: category
      selected.reserve(last_games_.size());
      // Recent N by lastPlayed
      std::vector<platf::playnite_protocol::Game> games_copy;
      {
        std::scoped_lock lk(mutex_);
        games_copy = last_games_;
      }
      std::sort(games_copy.begin(), games_copy.end(), [](const auto &a, const auto &b) {
        return a.lastPlayed > b.lastPlayed; // ISO8601 sorts lexicographically
      });
      int recentN = std::max(0, config::playnite.recent_games);
      BOOST_LOG(info) << "Playnite sync: selecting recentN=" << recentN;
      for (int i = 0; i < (int)games_copy.size() && i < recentN; ++i) {
        selected.push_back(games_copy[i]);
        source_flags[games_copy[i].id] |= 0x1;
      }
      // Category filter
      if (!config::playnite.sync_categories.empty()) {
        auto catset = std::unordered_set<std::string>();
        for (const auto &c : config::playnite.sync_categories) catset.insert(to_lower_copy(c));
        BOOST_LOG(info) << "Playnite sync: category filter size=" << catset.size();
        std::vector<platf::playnite_protocol::Game> last_copy;
        {
          std::scoped_lock lk(mutex_);
          last_copy = last_games_;
        }
        for (const auto &g : last_copy) {
          bool any = false;
          for (const auto &cn : g.categories) { if (catset.contains(to_lower_copy(cn))) { any = true; break; } }
          if (any) { selected.push_back(g); source_flags[g.id] |= 0x2; }
        }
      } else if (selected.empty()) {
        // If no filters, default to all
        std::scoped_lock lk(mutex_);
        selected = last_games_;
      }
      BOOST_LOG(info) << "Playnite sync: total selected games=" << selected.size();

      // Build indexes from selected games
      for (const auto &g : selected) {
        if (!g.exe.empty()) by_exe[norm_path(g.exe)] = GRef{&g};
        if (!g.workingDir.empty()) by_dir[norm_path(g.workingDir)] = GRef{&g};
      }

      bool changed = false;
      size_t matched = 0;
      for (auto &app : root["apps"]) {
        std::string cmd = app.contains("cmd") && app["cmd"].is_string() ? app["cmd"].get<std::string>() : std::string();
        std::string wdir = app.contains("working-dir") && app["working-dir"].is_string() ? app["working-dir"].get<std::string>() : std::string();
        const platf::playnite_protocol::Game *match = nullptr;
        if (!cmd.empty()) {
          auto it = by_exe.find(norm_path(cmd));
          if (it != by_exe.end()) match = it->second.g;
        }
        if (!match && !wdir.empty()) {
          auto it2 = by_dir.find(norm_path(wdir));
          if (it2 != by_dir.end()) match = it2->second.g;
        }
        if (!match) continue;
        ++matched;

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
              if (lower.size() >= 4 && lower.substr(lower.size()-4) == ".png") {
                // Copy as PNG and normalize DPI to 96 via re-encode for consistency
                needConvert = true;
              }
            } catch (...) {}

            bool ok = false;
            if (std::filesystem::exists(dst)) {
              ok = true; // reuse existing converted image
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

        try { app["playnite-id"] = match->id; changed = true; } catch (...) {}
        // Mark source for UX
        try {
          int flags = 0;
          auto itf = source_flags.find(match->id);
          if (itf != source_flags.end()) flags = itf->second;
          std::string src = flags == 0 ? std::string("unknown") : (flags == 3 ? std::string("recent+category") : (flags == 1 ? std::string("recent") : std::string("category")));
          app["playnite-source"] = src; changed = true;
          app["playnite-managed"] = "auto";
        } catch (...) {}
      }

      if (changed) {
        file_handler::write_file(path.c_str(), root.dump(4));
        proc::refresh(config::stream.file_apps);
        BOOST_LOG(info) << "Playnite sync: apps.json updated";
        BOOST_LOG(info) << "Playnite sync: matched apps updated count=" << matched;
      } else {
        BOOST_LOG(info) << "Playnite sync: no changes to apps.json (matched=" << matched << ")";
      }
    }

    std::vector<platf::playnite_protocol::Game> last_games_;
    std::mutex mutex_;

    std::unique_ptr<platf::playnite::IpcServer> server_;
  };

  std::unique_ptr<::platf::deinit_t> start() {
    return std::make_unique<deinit_t_impl>();
  }

  bool is_active() {
    auto inst = g_instance;
    if (inst) return inst->is_server_active();
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
    std::vector<DWORD> pids = pids1; pids.insert(pids.end(), pids2.begin(), pids2.end());
    for (DWORD pid : pids) {
      HANDLE hp = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
      if (!hp) { BOOST_LOG(info) << "Playnite installer: OpenProcess failed for PID=" << pid; continue; }
      std::wstring buf; buf.resize(32768);
      DWORD size = static_cast<DWORD>(buf.size());
      if (QueryFullProcessImageNameW(hp, 0, buf.data(), &size)) {
        buf.resize(size);
        std::filesystem::path exePath = buf;
        std::filesystem::path base = exePath.parent_path();
        std::filesystem::path extDir = base / L"Extensions" / L"SunshinePlaynite";
        BOOST_LOG(info) << "Playnite installer: found running Playnite pid=" << pid << ", base=" << base.string();
        CloseHandle(hp);
        destOut = extDir; return true;
      } else {
        BOOST_LOG(info) << "Playnite installer: QueryFullProcessImageNameW failed for PID=" << pid;
        CloseHandle(hp);
      }
    }
    auto userTok = platf::dxgi::retrieve_users_token(false);
    if (!userTok) { BOOST_LOG(info) << "Playnite installer: retrieve_users_token failed"; return false; }
    PWSTR roaming = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, userTok, &roaming)) && roaming) {
      destOut = std::filesystem::path(roaming) / L"Playnite" / L"Extensions" / L"SunshinePlaynite";
      BOOST_LOG(info) << "Playnite installer: resolved via active user token to " << destOut.string();
      CoTaskMemFree(roaming); CloseHandle(userTok); return true;
    }
    CloseHandle(userTok); return false;
  }

  bool get_extension_target_dir(std::string &out) {
    std::filesystem::path dest; if (!resolve_extensions_dir_safe(dest)) return false; out = dest.string(); return true;
  }

  bool launch_game(const std::string &playnite_id) {
#ifndef _WIN32
    (void) playnite_id;
    return false;
#else
    auto inst = g_instance;
    if (!inst) return false;
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
    (void) out_json; return false;
#else
    auto inst = g_instance; if (!inst) return false;
    nlohmann::json arr = nlohmann::json::array();
    std::vector<platf::playnite_protocol::Game> copy;
    inst->snapshot_games(copy);
    for (const auto &g : copy) {
      nlohmann::json j;
      j["id"] = g.id;
      j["name"] = g.name;
      j["categories"] = g.categories;
      arr.push_back(std::move(j));
    }
    out_json = arr.dump();
    return true;
#endif
  }


  static bool do_install_plugin_impl(const std::string &dest_override, std::string &error) {
#ifndef _WIN32
    error = "Playnite integration is only supported on Windows";
    return false;
#else
    try {
      // Determine source directory: alongside the Sunshine executable under plugins/playnite/SunshinePlaynite
      std::wstring exePath; exePath.resize(MAX_PATH);
      GetModuleFileNameW(nullptr, exePath.data(), (DWORD)exePath.size());
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
        BOOST_LOG(info) << "Playnite installer: copying "  << sname <<  " from "  << src.string() << " to "  << dst.string();
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

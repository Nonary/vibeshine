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
#include "src/platform/windows/playnite_sync.h"
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
// boost env/filesystem for process launch helpers
#include <boost/process/environment.hpp>
#include <boost/filesystem.hpp>

namespace platf::playnite {

  // Time parsing helper moved to platf::playnite::sync

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

    void snapshot_games(std::vector<platf::playnite::Game> &out) {
      std::scoped_lock lk(mutex_);
      out = last_games_;
    }

    void snapshot_categories(std::vector<std::string> &out) {
      std::scoped_lock lk(mutex_);
      out = last_categories_;
    }

  private:
    void handle_message(std::span<const uint8_t> bytes) {
      auto msg = platf::playnite::parse(bytes);
      using MT = platf::playnite::MessageType;
      if (msg.type == MT::Categories) {
        BOOST_LOG(info) << "Playnite: received " << msg.categories.size() << " categories";
        // Cache distinct category names and treat as the start of a new snapshot for games
        {
          std::scoped_lock lk(mutex_);
          std::unordered_set<std::string> uniq;
          last_categories_.clear();
          for (const auto &c : msg.categories) {
            if (!c.name.empty() && uniq.insert(c.name).second) {
              last_categories_.push_back(c.name);
            }
          }
          std::sort(last_categories_.begin(), last_categories_.end(), [](const std::string &a, const std::string &b){ return a < b; });
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
  BOOST_LOG(info) << "Playnite: status '" << msg.status_name << "' id=" << msg.status_game_id;
  if (msg.status_name == "gameStopped") {
          proc::proc.terminate();
        }
      } else {
        BOOST_LOG(warning) << "Playnite: unrecognized message";
      }
    }

    // Use helpers in playnite_sync for string normalization

    static std::string norm_path(const std::string &p) { return platf::playnite::sync::normalize_path_for_match(p); }

    void sync_apps_metadata() {
      using nlohmann::json;
      const std::string path = config::stream.file_apps;
      std::string content = file_handler::read_file(path.c_str());
      json root = json::parse(content);
      if (!root.contains("apps") || !root["apps"].is_array()) {
        BOOST_LOG(warning) << "apps.json has no 'apps' array";
        return;
      }

      using GRef = platf::playnite::sync::GameRef;

      std::unordered_map<std::string, GRef> by_exe;
      std::unordered_map<std::string, GRef> by_dir;
      std::unordered_map<std::string, GRef> by_id;
      // Build all games snapshot and reconcile with apps.json via helper
      std::vector<platf::playnite::Game> all;
      { std::scoped_lock lk(mutex_); all = last_games_; }
      int recentN = std::max(0, config::playnite.recent_games);
      int recent_age_days = std::max(0, config::playnite.recent_max_age_days);
      int delete_after_days = std::max(0, config::playnite.autosync_delete_after_days);
      bool changed = false; std::size_t matched = 0;
      platf::playnite::sync::autosync_reconcile(root, all, recentN, recent_age_days, delete_after_days, config::playnite.autosync_require_replacement, config::playnite.sync_categories, config::playnite.exclude_games, changed, matched);
      if (changed) {
        platf::playnite::sync::write_and_refresh_apps(root, config::stream.file_apps);
        BOOST_LOG(info) << "Playnite sync: apps.json updated";
        BOOST_LOG(info) << "Playnite sync: matched apps updated count=" << matched;
      } else {
        BOOST_LOG(info) << "Playnite sync: no changes to apps.json (matched=" << matched << ")";
      }
    }

    std::vector<platf::playnite::Game> last_games_;
    std::mutex mutex_;

    std::unique_ptr<platf::playnite::IpcServer> server_;
    bool new_snapshot_ = true;  // Indicates next games message starts a new accumulation
    std::unordered_set<std::string> game_ids_;  // Track unique IDs during accumulation
    std::vector<std::string> last_categories_;   // Last known categories (names)
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
  }

  bool get_games_list_json(std::string &out_json) {
    auto inst = g_instance;
    if (!inst) {
      return false;
    }
    nlohmann::json arr = nlohmann::json::array();
    std::vector<platf::playnite::Game> copy;
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
  }

  bool get_categories_list_json(std::string &out_json) {
    auto inst = g_instance;
    if (!inst) {
      return false;
    }
    std::vector<std::string> cats;
    inst->snapshot_categories(cats);
    if (cats.empty()) {
      std::vector<platf::playnite::Game> copy;
      inst->snapshot_games(copy);
      std::unordered_set<std::string> uniq;
      for (const auto &g : copy) {
        for (const auto &c : g.categories) if (!c.empty()) uniq.insert(c);
      }
      cats.assign(uniq.begin(), uniq.end());
      std::sort(cats.begin(), cats.end(), [](const std::string &a, const std::string &b){ return a < b; });
    }
    nlohmann::json arr = nlohmann::json::array();
    for (const auto &c : cats) arr.push_back(c);
    out_json = arr.dump();
    return true;
  }

  bool stop_game(const std::string &playnite_id) {
    auto inst = g_instance;
    if (!inst) return false;
    nlohmann::json j;
    j["type"] = "command";
    j["command"] = "stop";
    if (!playnite_id.empty()) j["id"] = playnite_id;
    return inst->send_cmd_json_line(j.dump());
  }

  bool force_sync() {
    auto inst = g_instance;
    if (!inst) {
      return false;
    }
    inst->trigger_sync();
    return true;
  }

  bool get_cover_png_for_playnite_game(const std::string &playnite_id, std::string &out_path) {
    auto inst = g_instance;
    if (!inst) {
      return false;
    }
    // Snapshot games
    std::vector<platf::playnite::Game> copy;
    inst->snapshot_games(copy);
    const platf::playnite::Game *gptr = nullptr;
    for (const auto &g : copy) {
      if (g.id == playnite_id) {
        gptr = &g;
        break;
      }
    }
  if (!gptr || gptr->box_art_path.empty()) {
      return false;
    }

    try {
  std::filesystem::path src = gptr->box_art_path;
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
  }

  // Helper: gather running Playnite PIDs and capture any discovered exe path
  static void collect_playnite_state(std::vector<DWORD> &pids, std::wstring &exe_path_out) {
    try {
      auto d = platf::dxgi::find_process_ids_by_name(L"Playnite.DesktopApp.exe");
      auto f = platf::dxgi::find_process_ids_by_name(L"Playnite.FullscreenApp.exe");
      pids = d;
      pids.insert(pids.end(), f.begin(), f.end());
      for (DWORD pid : pids) {
        HANDLE hp = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
        if (!hp) continue;
        std::wstring buf;
        buf.resize(32768);
        DWORD size = static_cast<DWORD>(buf.size());
        if (QueryFullProcessImageNameW(hp, 0, buf.data(), &size)) {
          buf.resize(size);
          exe_path_out = buf;
          CloseHandle(hp);
          break;
        }
        CloseHandle(hp);
      }
    } catch (...) {
      // best-effort
    }
  }

  // Helper: attempt to resolve a Playnite Desktop exe path if not running
  static bool resolve_playnite_exe_path(std::wstring &exe_path_out) {
    // Prefer configured install_dir
    try {
      if (!config::playnite.install_dir.empty()) {
        std::filesystem::path p = std::filesystem::path(config::playnite.install_dir) / L"Playnite.DesktopApp.exe";
        if (std::filesystem::exists(p)) { exe_path_out = p.wstring(); return true; }
      }
    } catch (...) {}

    // Try the active user's LocalAppData path
    try {
      platf::dxgi::safe_token user_token; user_token.reset(platf::dxgi::retrieve_users_token(false));
      if (user_token.get()) {
        PWSTR local = nullptr;
        if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, user_token.get(), &local)) && local) {
          std::filesystem::path p = std::filesystem::path(local) / L"Playnite" / L"Playnite.DesktopApp.exe";
          CoTaskMemFree(local);
          if (std::filesystem::exists(p)) { exe_path_out = p.wstring(); return true; }
        }
      }
    } catch (...) {}

    // Fall back to current process' LocalAppData
    try {
      wchar_t buf[MAX_PATH] = {};
      if (SUCCEEDED(SHGetFolderPathW(nullptr, CSIDL_LOCAL_APPDATA, nullptr, SHGFP_TYPE_CURRENT, buf))) {
        std::filesystem::path p = std::filesystem::path(buf) / L"Playnite" / L"Playnite.DesktopApp.exe";
        if (std::filesystem::exists(p)) { exe_path_out = p.wstring(); return true; }
      }
    } catch (...) {}

    return false;
  }

  bool restart_playnite() {
    // 1) Collect current state and attempt graceful-then-force close from the user's session
    std::vector<DWORD> pids; std::wstring running_exe;
    collect_playnite_state(pids, running_exe);

    // Build a small PowerShell to close then kill leftover processes by name
    std::string ps =
      "powershell -NoProfile -WindowStyle Hidden -Command \""
      "try {"
      "  $procs = @(Get-Process Playnite.DesktopApp,Playnite.FullscreenApp -ErrorAction SilentlyContinue);"
      "  foreach ($p in $procs) { try { $null = $p.CloseMainWindow(); } catch {} }"
      "  Start-Sleep -Milliseconds 1200;"
      "  $procs = @(Get-Process Playnite.DesktopApp,Playnite.FullscreenApp -ErrorAction SilentlyContinue);"
      "  foreach ($p in $procs) { try { if (-not $p.HasExited) { $p.Kill() } } catch {} }"
      "} catch {}\"";

    std::error_code ec_close;
    auto env = boost::this_process::environment();
    boost::filesystem::path wd;
    auto child = platf::run_command(false, false, ps, wd, env, nullptr, ec_close, nullptr);
    if (!ec_close) {
      try { child.wait_for(std::chrono::milliseconds(2500)); } catch (...) {}
    }

    // 2) Determine exe path to start
    std::wstring exe;
    if (!running_exe.empty()) {
      exe = running_exe;
    } else {
      if (!resolve_playnite_exe_path(exe)) {
        BOOST_LOG(warning) << "Playnite restart: could not resolve Playnite executable path";
        // Even if we couldn't resolve path, treat close attempt as success
        return false;
      }
    }

    // 3) Launch Playnite (impersonates active user when running as SYSTEM)
    std::filesystem::path exePath = exe; std::filesystem::path startDir = exePath.parent_path();
    std::string cmd = platf::to_utf8(exe);
    std::error_code ec_launch;
    auto child2 = platf::run_command(false, true, cmd, startDir, env, nullptr, ec_launch, nullptr);
    if (ec_launch) {
      BOOST_LOG(warning) << "Playnite restart: launch failed: " << ec_launch.message();
      return false;
    }
    child2.detach();
    BOOST_LOG(info) << "Playnite restart: launched " << cmd;
    return true;
  }

  static bool do_install_plugin_impl(const std::string &dest_override, std::string &error) {
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
        if (platf::playnite::get_extension_target_dir(resolved)) {
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
  }

  bool install_plugin(std::string &error) {
    return do_install_plugin_impl(std::string(), error);
  }

  bool install_plugin_to(const std::string &dest_dir, std::string &error) {
    return do_install_plugin_impl(dest_dir, error);
  }

  

}  // namespace platf::playnite

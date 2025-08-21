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
#include "src/platform/windows/misc.h"
#include "src/utility.h"
#include "src/process.h"

#include <algorithm>
#include <filesystem>
#include <KnownFolders.h>
#include <mutex>
#include <nlohmann/json.hpp>
#include <ShlObj.h>
#include <Shlwapi.h>
#include <shellapi.h>
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

  // Resolve the Playnite Extensions/SunshinePlaynite directory via the "playnite" URL association.
  // Uses per-user registry views and impersonates the active user before calling AssocQueryString.
  static bool resolve_extensions_dir_via_assoc(std::filesystem::path &destOut) {
    BOOST_LOG(info) << "Playnite: resolving extensions dir via AssocQueryString";

    // Acquire user token when running as SYSTEM so AssocQueryString consults the user's HKCU/HKCR
    HANDLE user_token = nullptr;
    if (platf::dxgi::is_running_as_system()) {
      user_token = platf::dxgi::retrieve_users_token(false);
      if (!user_token) {
        BOOST_LOG(info) << "Playnite: no active user token available for association lookup";
        return false;
      }
    }

    HRESULT res = E_FAIL;
    std::wstring cmd_string;
    {
      static std::mutex per_user_key_mutex;
      auto lg = std::lock_guard(per_user_key_mutex);

      if (!platf::override_per_user_predefined_keys(user_token)) {
        if (user_token) CloseHandle(user_token);
        return false;
      }

      std::array<WCHAR, 4096> shell_cmd{};
      DWORD out_len = static_cast<DWORD>(shell_cmd.size());

      // Impersonate the user while querying associations to ensure resolution uses their profile
      if (user_token) {
        auto ec = platf::impersonate_current_user(user_token, [&]() {
          res = AssocQueryStringW(ASSOCF_NOTRUNCATE,
                                  ASSOCSTR_COMMAND,
                                  L"playnite",
                                  L"open",
                                  shell_cmd.data(),
                                  &out_len);
        });
        (void) ec; // best-effort; error code already logged by helper if impersonation fails
      } else {
        res = AssocQueryStringW(ASSOCF_NOTRUNCATE,
                                ASSOCSTR_COMMAND,
                                L"playnite",
                                L"open",
                                shell_cmd.data(),
                                &out_len);
      }

      // Restore original predefined keys regardless of success
      platf::override_per_user_predefined_keys(nullptr);

      if (res == S_OK) {
        cmd_string.assign(shell_cmd.data());
      }
    }

    if (user_token) CloseHandle(user_token);

    if (res != S_OK || cmd_string.empty()) {
      BOOST_LOG(info) << "Playnite: association query for 'playnite' failed (res=0x" << util::hex(res).to_string_view() << ")";
      return false;
    }

    // Parse the command line to extract the executable path
    int argc = 0;
    std::wstring exe_path_w;
    auto argv = CommandLineToArgvW(cmd_string.c_str(), &argc);
    if (argv && argc >= 1) {
      exe_path_w.assign(argv[0]);
      LocalFree(argv);
    } else {
      // Best-effort fallback parser: quoted or first token until space
      if (!cmd_string.empty() && cmd_string.front() == L'"') {
        auto pos = cmd_string.find(L'"', 1);
        if (pos != std::wstring::npos) {
          exe_path_w = cmd_string.substr(1, pos - 1);
        }
      } else {
        auto pos = cmd_string.find(L' ');
        exe_path_w = (pos == std::wstring::npos) ? cmd_string : cmd_string.substr(0, pos);
      }
    }

    if (exe_path_w.empty()) {
      BOOST_LOG(info) << "Playnite: failed to parse executable path from association command";
      return false;
    }

    std::filesystem::path exePath = exe_path_w;
    std::filesystem::path base = exePath.parent_path();
    destOut = base / L"Extensions" / L"SunshinePlaynite";
    BOOST_LOG(info) << "Playnite: resolved extensions dir at " << destOut.string();
    return true;
  }

  // Resolve Playnite executable via 'playnite' URL association (per-user), falling back to command parsing.
  static bool resolve_playnite_exe_via_assoc(std::wstring &exe_out) {
    HANDLE user_token = nullptr;
    if (platf::dxgi::is_running_as_system()) {
      user_token = platf::dxgi::retrieve_users_token(false);
      if (!user_token) return false;
    }
    HRESULT res = E_FAIL;
    std::wstring exe;
    {
      static std::mutex per_user_key_mutex;
      auto lg = std::lock_guard(per_user_key_mutex);
      if (!platf::override_per_user_predefined_keys(user_token)) {
        if (user_token) CloseHandle(user_token);
        return false;
      }

      // Try to get the executable directly
      std::array<WCHAR, 4096> exe_buf{};
      DWORD out_len = static_cast<DWORD>(exe_buf.size());
      res = AssocQueryStringW(ASSOCF_NOTRUNCATE, ASSOCSTR_EXECUTABLE, L"playnite", nullptr, exe_buf.data(), &out_len);
      if (res == S_OK) {
        exe.assign(exe_buf.data());
      }

      // If EXECUTABLE not available, parse COMMAND
      if (exe.empty()) {
        std::array<WCHAR, 4096> cmd_buf{};
        out_len = static_cast<DWORD>(cmd_buf.size());
        HRESULT res2 = AssocQueryStringW(ASSOCF_NOTRUNCATE, ASSOCSTR_COMMAND, L"playnite", L"open", cmd_buf.data(), &out_len);
        if (res2 == S_OK) {
          int argc = 0; auto argv = CommandLineToArgvW(cmd_buf.data(), &argc);
          if (argv && argc >= 1) { exe.assign(argv[0]); LocalFree(argv); }
          else {
            std::wstring s{cmd_buf.data()};
            if (!s.empty() && s.front() == L'"') { auto p = s.find(L'"', 1); if (p != std::wstring::npos) exe = s.substr(1, p - 1); }
            else { auto p = s.find(L' '); exe = (p == std::wstring::npos) ? s : s.substr(0, p); }
          }
        }
      }

      platf::override_per_user_predefined_keys(nullptr);
    }
    if (user_token) CloseHandle(user_token);
    if (exe.empty() || !std::filesystem::exists(exe)) return false;
    exe_out = exe;
    return true;
  }

  bool get_extension_target_dir(std::string &out) {
    std::filesystem::path dest;
    if (!resolve_extensions_dir_via_assoc(dest)) {
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
      // Prefer URL association (per-user) to determine the Playnite executable
      if (!resolve_playnite_exe_via_assoc(exe) && !resolve_playnite_exe_path(exe)) {
        BOOST_LOG(warning) << "Playnite restart: could not resolve Playnite executable path";
        // Even if we couldn't resolve path, treat close attempt as success
        return false;
      }
    }

    // 3) Launch Playnite (impersonates active user when running as SYSTEM)
    std::filesystem::path exePath = exe; std::filesystem::path startDir = exePath.parent_path();
    std::string cmd = platf::to_utf8(exe);
    std::error_code ec_launch;
    // platf::run_command expects a boost::filesystem::path&
    boost::filesystem::path boostStartDir = boost::filesystem::path(startDir.wstring());
    auto child2 = platf::run_command(false, true, cmd, boostStartDir, env, nullptr, ec_launch, nullptr);
    if (ec_launch) {
      BOOST_LOG(warning) << "Playnite restart: launch failed: " << ec_launch.message();
      return false;
    }
    child2.detach();
    BOOST_LOG(info) << "Playnite restart: launched " << cmd;
    return true;
  }

  // explicit launch-only helper removed; use restart_playnite()

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
      } else {
        // Prefer the same resolution used by status API
        std::string resolved;
        if (platf::playnite::get_extension_target_dir(resolved)) {
          destDir = std::filesystem::path(resolved);
          BOOST_LOG(info) << "Playnite installer: using resolved target dir from API=" << destDir.string();
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

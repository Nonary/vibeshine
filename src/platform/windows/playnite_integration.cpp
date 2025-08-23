/**
 * @file src/platform/windows/playnite_integration.cpp
 * @brief Playnite integration lifecycle and message handling.
 */

#ifndef WIN32_LEAN_AND_MEAN
  #define WIN32_LEAN_AND_MEAN
#endif
#include "playnite_integration.h"

#include "src/config.h"
#include "src/config_playnite.h"
#include "src/confighttp.h"
#include "src/file_handler.h"
#include "src/logging.h"
#include "src/platform/windows/image_convert.h"
#include "src/platform/windows/ipc/misc_utils.h"
#include "src/platform/windows/misc.h"
#include "src/platform/windows/playnite_ipc.h"
#include "src/platform/windows/playnite_protocol.h"
#include "src/platform/windows/playnite_sync.h"
#include "src/process.h"
#include "src/utility.h"

#include <algorithm>
#include <atomic>
#include <charconv>
#include <chrono>
#include <ctime>
#include <filesystem>
#include <iomanip>
#include <KnownFolders.h>
#include <mutex>
#include <nlohmann/json.hpp>
#include <shellapi.h>
#include <ShlObj.h>
#include <Shlwapi.h>
#include <span>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_set>
#include <vector>
#include <Windows.h>
// boost env/filesystem for process launch helpers
#include <boost/filesystem.hpp>
#include <boost/process/environment.hpp>

namespace platf::playnite {

  // Time parsing helper moved to platf::playnite::sync

  class deinit_t_impl;  // forward
  static std::atomic<deinit_t_impl *> g_instance {nullptr};

  static bool is_plugin_installed() {
    try {
      std::string dir;
      if (!get_extension_target_dir(dir)) {
        return false;
      }
      std::filesystem::path d(dir);
      return std::filesystem::exists(d / "extension.yaml") && std::filesystem::exists(d / "SunshinePlaynite.psm1");
    } catch (...) {
      return false;
    }
  }

  class deinit_t_impl: public ::platf::deinit_t {
  public:
    deinit_t_impl() {
      BOOST_LOG(info) << "Playnite integration: manager starting";
      g_instance.store(this, std::memory_order_release);
      // If plugin installed at startup, start immediately; otherwise wait for manager loop
      if (is_plugin_installed()) {
        BOOST_LOG(info) << "Playnite integration: plugin installed; starting IPC server";
        server_ = std::make_unique<platf::playnite::IpcServer>();
        server_->set_message_handler([this](std::span<const uint8_t> bytes) {
          handle_message(bytes);
        });
        server_->start();
        new_snapshot_ = true;
      } else {
        BOOST_LOG(info) << "Playnite integration: plugin not installed; server idle";
      }
      // Start manager loop to hot-apply enablement state
      stop_flag_.store(false, std::memory_order_release);
      manager_ = std::thread([this]() {
        this->manager_loop();
      });
    }

    ~deinit_t_impl() override {
      // Stop manager thread
      try {
        stop_flag_.store(true, std::memory_order_release);
        if (manager_.joinable()) {
          manager_.join();
        }
      } catch (...) {}
      if (server_) {
        server_->stop();
        server_.reset();
      }
      g_instance.store(nullptr, std::memory_order_release);
    }

    bool is_server_active() const {
      return server_ && server_->is_active();
    }

    bool send_cmd_json_line(const std::string &s) {
      return server_ && server_->send_json_line(s);
    }

    void trigger_sync() {
      (void) sync_apps_metadata();
    }

    void snapshot_games(std::vector<platf::playnite::Game> &out) {
      std::scoped_lock lk(mutex_);
      out = last_games_;
    }

    void snapshot_categories(std::vector<std::string> &out) {
      std::scoped_lock lk(mutex_);
      out = last_categories_;
    }

    // Hot-toggle helpers: stop or start the IPC server without destroying the instance
    void stop_server() {
      try {
        if (server_) {
          BOOST_LOG(info) << "Playnite: stopping IPC server (hot-toggle)";
          server_->stop();
          server_.reset();
        }
        // Clear cached snapshots so UI doesn't falsely show data as connected
        try {
          std::scoped_lock lk(mutex_);
          last_games_.clear();
          game_ids_.clear();
          last_categories_.clear();
          new_snapshot_ = true;
        } catch (...) {}
      } catch (...) {}
    }

    void ensure_started() {
      if (server_ && server_->is_active()) {
        return;
      }
      BOOST_LOG(info) << "Playnite: starting IPC server (hot-toggle)";
      server_ = std::make_unique<platf::playnite::IpcServer>();
      server_->set_message_handler([this](std::span<const uint8_t> bytes) {
        handle_message(bytes);
      });
      server_->start();
      new_snapshot_ = true;
    }

    void manager_loop() {
      using namespace std::chrono_literals;
      // simple periodic reconciliation loop
      while (!stop_flag_.load(std::memory_order_acquire)) {
        const bool want = is_plugin_installed();
        if (want) {
          ensure_started();
        } else {
          stop_server();
        }
        std::this_thread::sleep_for(1500ms);
      }
    }

  private:
    void handle_message(std::span<const uint8_t> bytes) {
      BOOST_LOG(debug) << "Playnite: handling message, bytes=" << bytes.size();
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
          std::sort(last_categories_.begin(), last_categories_.end(), [](const std::string &a, const std::string &b) {
            return a < b;
          });
          new_snapshot_ = true;
        }
      } else if (msg.type == MT::Games) {
        BOOST_LOG(debug) << "Playnite: received " << msg.games.size() << " games";
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
        BOOST_LOG(debug) << "Playnite: games batch processed added=" << added << " skipped=" << skipped
                         << " cumulative=" << (before + added) << " (unique IDs)";
        if (config::playnite.auto_sync) {
          BOOST_LOG(debug) << "Playnite: auto_sync enabled; syncing apps metadata";
          (void) sync_apps_metadata();
        }
      } else if (msg.type == MT::Status) {
        BOOST_LOG(info) << "Playnite: status '" << msg.status_name
                        << "' id='" << msg.status_game_id
                        << "' exe='" << msg.status_exe
                        << "' installDir='" << msg.status_install_dir << "'";
        if (msg.status_name == "gameStopped") {
          BOOST_LOG(info) << "Playnite: received gameStopped; terminating active process";
          proc::proc.terminate();
        }
      } else {
        // Truncate and log a preview of the raw message for diagnostics
        std::string preview;
        preview.assign((const char *) bytes.data(), std::min<size_t>(bytes.size(), 256));
        for (auto &ch : preview) {
          if (ch == '\n' || ch == '\r') {
            ch = ' ';
          }
        }
        BOOST_LOG(warning) << "Playnite: unrecognized message; size=" << bytes.size() << " preview='" << preview << "'";
      }
    }

    bool sync_apps_metadata() try {
      using nlohmann::json;
      const std::string path = config::stream.file_apps;
      BOOST_LOG(debug) << "Playnite sync: reading apps file '" << path << "'";
      std::string content = file_handler::read_file(path.c_str());
      BOOST_LOG(debug) << "Playnite sync: apps file size=" << content.size() << " bytes";
      json root = json::parse(content);
      if (!root.contains("apps") || !root["apps"].is_array()) {
        BOOST_LOG(warning) << "apps.json has no 'apps' array";
        return false;
      }

      using GRef = platf::playnite::sync::GameRef;
      // Build all games snapshot and reconcile with apps.json via helper
      std::vector<platf::playnite::Game> all;
      {
        std::scoped_lock lk(mutex_);
        all = last_games_;
      }
      int recentN = std::max(0, config::playnite.recent_games);
      int recent_age_days = std::max(0, config::playnite.recent_max_age_days);
      int delete_after_days = std::max(0, config::playnite.autosync_delete_after_days);
      bool changed = false;
      std::size_t matched = 0;
      platf::playnite::sync::autosync_reconcile(root, all, recentN, recent_age_days, delete_after_days, config::playnite.autosync_require_replacement, config::playnite.sync_categories, config::playnite.exclude_games, changed, matched);
      if (changed) {
        platf::playnite::sync::write_and_refresh_apps(root, config::stream.file_apps);
        BOOST_LOG(info) << "Playnite sync: apps.json updated";
        BOOST_LOG(info) << "Playnite sync: matched apps updated count=" << matched;
      } else {
        BOOST_LOG(debug) << "Playnite sync: no changes to apps.json (matched=" << matched << ")";
      }
      return true;
    } catch (const std::exception &e) {
      BOOST_LOG(error) << "Playnite sync failed for '" << config::stream.file_apps << "': " << e.what();
      return false;
    } catch (...) {
      BOOST_LOG(error) << "Playnite sync failed: unknown error reading/parsing '" << config::stream.file_apps << "'";
      return false;
    }

    std::vector<platf::playnite::Game> last_games_;
    std::mutex mutex_;

    std::unique_ptr<platf::playnite::IpcServer> server_;
    std::atomic<bool> stop_flag_ {false};
    std::thread manager_;
    bool new_snapshot_ = true;  // Indicates next games message starts a new accumulation
    std::unordered_set<std::string> game_ids_;  // Track unique IDs during accumulation
    std::vector<std::string> last_categories_;  // Last known categories (names)
  };

  std::unique_ptr<::platf::deinit_t> start() {
    return std::make_unique<deinit_t_impl>();
  }

  bool is_active() {
    auto inst = g_instance.load(std::memory_order_acquire);
    if (inst) {
      return inst->is_server_active();
    }
    return false;
  }

  // Consolidated helper: query association for 'playnite' to resolve executable path
  static bool query_assoc_for_playnite(std::wstring &outExe) {
    HANDLE user_token = nullptr;
    if (platf::dxgi::is_running_as_system()) {
      user_token = platf::dxgi::retrieve_users_token(false);
    }
    std::wstring exe;
    {
      static std::mutex per_user_key_mutex;
      auto lg = std::lock_guard(per_user_key_mutex);
      if (!platf::override_per_user_predefined_keys(user_token)) {
        BOOST_LOG(debug) << "Playnite: per-user registry override failed (no active session?)";
        if (user_token) {
          CloseHandle(user_token);
        }
        return false;
      }

      // Try ASSOCSTR_EXECUTABLE first
      std::array<WCHAR, 4096> exe_buf {};
      DWORD out_len = static_cast<DWORD>(exe_buf.size());
      HRESULT hr = AssocQueryStringW(ASSOCF_NOTRUNCATE, ASSOCSTR_EXECUTABLE, L"playnite", nullptr, exe_buf.data(), &out_len);
      if (hr == S_OK) {
        exe.assign(exe_buf.data());
      }

      // Fallback to ASSOCSTR_COMMAND and parse
      if (exe.empty()) {
        std::array<WCHAR, 4096> cmd_buf {};
        out_len = static_cast<DWORD>(cmd_buf.size());
        hr = AssocQueryStringW(ASSOCF_NOTRUNCATE, ASSOCSTR_COMMAND, L"playnite", L"open", cmd_buf.data(), &out_len);
        if (hr == S_OK) {
          int argc = 0;
          auto argv = CommandLineToArgvW(cmd_buf.data(), &argc);
          if (argv && argc >= 1) {
            exe.assign(argv[0]);
            LocalFree(argv);
          } else {
            std::wstring s {cmd_buf.data()};
            if (!s.empty() && s.front() == L'"') {
              auto p = s.find(L'"', 1);
              if (p != std::wstring::npos) {
                exe = s.substr(1, p - 1);
              }
            } else {
              auto p = s.find(L' ');
              exe = (p == std::wstring::npos) ? s : s.substr(0, p);
            }
          }
        }
      }

      platf::override_per_user_predefined_keys(nullptr);
    }
    if (user_token) {
      CloseHandle(user_token);
    }
    if (exe.empty() || !std::filesystem::exists(exe)) {
      return false;
    }
    outExe = exe;
    return true;
  }

  // Resolve the Playnite Extensions/SunshinePlaynite directory via the "playnite" URL association.
  // Uses per-user registry views and impersonates the active user before calling AssocQueryString.
  static bool resolve_extensions_dir_via_assoc(std::filesystem::path &destOut) {
    std::wstring exe_path_w;
    if (!query_assoc_for_playnite(exe_path_w)) {
      return false;
    }
    std::filesystem::path exePath = exe_path_w;
    std::filesystem::path base = exePath.parent_path();
    destOut = base / L"Extensions" / L"SunshinePlaynite";
    return true;
  }

  // Resolve Playnite executable via 'playnite' URL association (per-user), falling back to command parsing.
  static bool resolve_playnite_exe_via_assoc(std::wstring &exe_out) {
    return query_assoc_for_playnite(exe_out);
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
    auto inst = g_instance.load(std::memory_order_acquire);
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
    auto inst = g_instance.load(std::memory_order_acquire);
    if (!inst) {
      return false;
    }
    nlohmann::json arr = nlohmann::json::array();
    std::vector<platf::playnite::Game> copy;
    inst->snapshot_games(copy);
    try {
      auto &vec = arr.get_ref<nlohmann::json::array_t &>();
      vec.reserve(copy.size());
    } catch (...) {}
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
    auto inst = g_instance.load(std::memory_order_acquire);
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
        for (const auto &c : g.categories) {
          if (!c.empty()) {
            uniq.insert(c);
          }
        }
      }
      cats.assign(uniq.begin(), uniq.end());
      std::sort(cats.begin(), cats.end(), [](const std::string &a, const std::string &b) {
        return a < b;
      });
    }
    nlohmann::json arr = nlohmann::json::array();
    for (const auto &c : cats) {
      arr.push_back(c);
    }
    out_json = arr.dump();
    return true;
  }

  bool stop_game(const std::string &playnite_id) {
    auto inst = g_instance.load(std::memory_order_acquire);
    if (!inst) {
      return false;
    }
    nlohmann::json j;
    j["type"] = "command";
    j["command"] = "stop";
    if (!playnite_id.empty()) {
      j["id"] = playnite_id;
    }
    return inst->send_cmd_json_line(j.dump());
  }

  bool force_sync() {
    auto inst = g_instance.load(std::memory_order_acquire);
    if (!inst) {
      return false;
    }
    inst->trigger_sync();
    return true;
  }

  bool get_cover_png_for_playnite_game(const std::string &playnite_id, std::string &out_path) {
    auto inst = g_instance.load(std::memory_order_acquire);
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
      std::error_code ec1, ec2;
      if (std::filesystem::exists(dst)) {
        auto dstTime = std::filesystem::last_write_time(dst, ec1);
        auto srcTime = std::filesystem::last_write_time(src, ec2);
        if (!ec1 && !ec2 && dstTime >= srcTime) {
          ok = true;
        }
      }
      if (!ok) {
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
        if (!hp) {
          continue;
        }
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
      platf::dxgi::safe_token user_token;
      user_token.reset(platf::dxgi::retrieve_users_token(false));
      if (user_token.get()) {
        PWSTR local = nullptr;
        if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, user_token.get(), &local)) && local) {
          std::filesystem::path p = std::filesystem::path(local) / L"Playnite" / L"Playnite.DesktopApp.exe";
          CoTaskMemFree(local);
          if (std::filesystem::exists(p)) {
            exe_path_out = p.wstring();
            return true;
          }
        }
      }
    } catch (...) {}

    // Fall back to current process' LocalAppData
    try {
      wchar_t buf[MAX_PATH] = {};
      if (SUCCEEDED(SHGetFolderPathW(nullptr, CSIDL_LOCAL_APPDATA, nullptr, SHGFP_TYPE_CURRENT, buf))) {
        std::filesystem::path p = std::filesystem::path(buf) / L"Playnite" / L"Playnite.DesktopApp.exe";
        if (std::filesystem::exists(p)) {
          exe_path_out = p.wstring();
          return true;
        }
      }
    } catch (...) {}

    return false;
  }

  // Close by posting WM_CLOSE to windows owned by process name, then kill leftovers
  static void close_then_kill_by_name(const wchar_t *exeName) {
    struct Ctx {
      const wchar_t *name;
      std::vector<DWORD> pids;
    } ctx {exeName, {}};

    // Build a set of target PIDs by name
    try {
      auto ids = platf::dxgi::find_process_ids_by_name(exeName);
      ctx.pids.insert(ctx.pids.end(), ids.begin(), ids.end());
    } catch (...) {}

    if (!ctx.pids.empty()) {
      BOOST_LOG(info) << "Playnite: posting WM_CLOSE to " << ctx.pids.size() << " window(s) for '" << platf::to_utf8(std::wstring(exeName)) << "'";
      EnumWindows([](HWND hwnd, LPARAM lparam) -> BOOL {
        auto *c = reinterpret_cast<Ctx *>(lparam);
        DWORD pid = 0;
        GetWindowThreadProcessId(hwnd, &pid);
        if (!pid) {
          return TRUE;
        }
        for (DWORD p : c->pids) {
          if (p == pid) {
            PostMessageW(hwnd, WM_CLOSE, 0, 0);
            break;
          }
        }
        return TRUE;
      },
                  reinterpret_cast<LPARAM>(&ctx));
    }

    Sleep(1200);

    // Kill any remaining processes by name
    try {
      auto ids = platf::dxgi::find_process_ids_by_name(exeName);
      if (!ids.empty()) {
        BOOST_LOG(info) << "Playnite: terminating remaining processes for '" << platf::to_utf8(std::wstring(exeName)) << "' count=" << ids.size();
      }
      for (DWORD pid : ids) {
        HANDLE hp = OpenProcess(PROCESS_TERMINATE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
        if (!hp) {
          continue;
        }
        DWORD code = 0;
        if (!GetExitCodeProcess(hp, &code) || code == STILL_ACTIVE) {
          TerminateProcess(hp, 1);
        }
        CloseHandle(hp);
      }
    } catch (...) {}
  }

  bool restart_playnite() {
    // 1) Collect current state and attempt graceful-then-force close from the user's session
    std::vector<DWORD> pids;
    std::wstring running_exe;
    collect_playnite_state(pids, running_exe);
    // Gracefully request close then kill stragglers, native
    close_then_kill_by_name(L"Playnite.DesktopApp.exe");
    close_then_kill_by_name(L"Playnite.FullscreenApp.exe");

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
    std::filesystem::path exePath = exe;
    std::filesystem::path startDir = exePath.parent_path();
    std::string cmd = platf::to_utf8(exe);
    std::error_code ec_launch;
    // platf::run_command expects a boost::filesystem::path&
    boost::filesystem::path boostStartDir = boost::filesystem::path(startDir.wstring());
    auto env = boost::this_process::environment();
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
        if (!platf::playnite::get_extension_target_dir(resolved)) {
          error = "Could not resolve Playnite Extensions directory (and no override provided).";
          return false;
        }
        destDir = std::filesystem::path(resolved);
        BOOST_LOG(info) << "Playnite installer: using resolved target dir from API=" << destDir.string();
      }
      std::error_code ec;
      if (!std::filesystem::create_directories(destDir, ec) && ec) {
        error = std::string("Failed to create destination directory: ") + destDir.string() + " (" + ec.message() + ")";
        return false;
      }

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

  static bool do_uninstall_plugin_impl(std::string &error) {
    try {
      std::string target;
      if (!platf::playnite::get_extension_target_dir(target)) {
        // If we cannot resolve the directory, consider it already uninstalled.
        BOOST_LOG(warning) << "Playnite uninstaller: could not resolve Extensions directory; assuming uninstalled";
        return true;
      }
      std::filesystem::path destDir = std::filesystem::path(target);
      if (!std::filesystem::exists(destDir)) {
        BOOST_LOG(info) << "Playnite uninstaller: target does not exist; nothing to do";
        return true;
      }
      std::error_code ec;
      auto removed = std::filesystem::remove_all(destDir, ec);
      if (ec) {
        error = std::string("Failed to remove plugin directory: ") + destDir.string() + " (" + ec.message() + ")";
        BOOST_LOG(warning) << "Playnite uninstaller: remove_all failed: " << ec.message();
        return false;
      }
      BOOST_LOG(info) << "Playnite uninstaller: removed files count=" << removed << " path=" << destDir.string();
      return true;
    } catch (const std::exception &e) {
      error = e.what();
      BOOST_LOG(warning) << "Playnite uninstaller: exception: " << e.what();
      return false;
    } catch (...) {
      error = "Unknown error";
      BOOST_LOG(warning) << "Playnite uninstaller: unknown exception";
      return false;
    }
  }

  bool uninstall_plugin(std::string &error) {
    return do_uninstall_plugin_impl(error);
  }

}  // namespace platf::playnite

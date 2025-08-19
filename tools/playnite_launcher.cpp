/**
 * @file tools/playnite_launcher.cpp
 * @brief Standalone Playnite launcher helper. Hosts a per-launch IPC server with a
 *        GUID-derived control pipe name and commands Playnite to start a specific game.
 *
 * Usage:
 *   playnite-launcher --game-id <GUID> [--public-guid <GUID>] [--timeout <seconds>]
 *   playnite-launcher --fullscreen [--focus-attempts N] [--focus-timeout S] [--focus-exit-on-first]
 *
 * Behavior:
 *   - Initializes logging to sunshine_playnite_launcher.log in appdata
 *   - Chooses a public GUID (either provided or generated) and hosts a control pipe named
 *     "sunshine_playnite_ctl_<GUID>". The Playnite plugin connects to this, performs our
 *     anonymous handshake, then switches to a private data pipe.
 *   - Once the data pipe is active, sends a launch command for the requested Playnite game id.
 *   - Remains alive, listening for status messages, and exits when it receives
 *     status.gameStopped for the same game id (or on timeout).
 */

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <shellapi.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <string_view>
#include <vector>
#include <thread>
#include <filesystem>
#include <functional>

#include "src/logging.h"
#include "src/platform/windows/playnite_ipc.h"
#include "src/platform/windows/playnite_protocol.h"
#include "src/platform/windows/ipc/misc_utils.h"
#include <nlohmann/json.hpp>
#include <shlobj.h>
#include <fstream>
#include <cwctype>
#include <psapi.h>

using namespace std::chrono_literals;

// Fallback declaration for CommandLineToArgvW if headers don't provide it
#ifdef _WIN32
#ifndef HAVE_CommandLineToArgvW_DECL
extern "C" __declspec(dllimport) LPWSTR* WINAPI CommandLineToArgvW(LPCWSTR lpCmdLine, int *pNumArgs);
#endif
#endif

namespace {
  
  // Simple GUID utilities
  std::string guid_to_string(const GUID &g) {
    char buf[64];
    std::snprintf(buf, sizeof(buf),
                  "%08lX-%04hX-%04hX-%02hhX%02hhX-%02hhX%02hhX%02hhX%02hhX%02hhX%02hhX",
                  g.Data1, g.Data2, g.Data3,
                  g.Data4[0], g.Data4[1], g.Data4[2], g.Data4[3],
                  g.Data4[4], g.Data4[5], g.Data4[6], g.Data4[7]);
    return std::string(buf);
  }

  bool parse_arg(std::span<char*> args, std::string_view name, std::string &out) {
    for (size_t i = 0; i + 1 < args.size(); ++i) {
      if (name == args[i]) { out = args[i+1]; return true; }
      // Support --key=value
      if (std::string_view(args[i]).rfind(std::string(name) + "=", 0) == 0) {
        out = std::string(args[i] + name.size() + 1);
        return true;
      }
    }
    return false;
  }

  bool parse_flag(std::span<char*> args, std::string_view name) {
    for (size_t i = 0; i < args.size(); ++i) {
      if (name == args[i]) return true;
      if (std::string_view(args[i]) == (std::string(name) + "=true")) return true;
    }
    return false;
  }

  // Enumerate top-level windows and return the first HWND whose owning PID matches
  static HWND find_main_window_for_pid(DWORD pid) {
    struct Ctx { DWORD pid; HWND hwnd; } ctx{pid, nullptr};
    auto enum_proc = [](HWND hwnd, LPARAM lparam) -> BOOL {
      auto *c = reinterpret_cast<Ctx*>(lparam);
      DWORD wpid = 0;
      GetWindowThreadProcessId(hwnd, &wpid);
      if (wpid != c->pid) return TRUE;
      if (!IsWindowVisible(hwnd)) return TRUE;
      HWND owner = GetWindow(hwnd, GW_OWNER);
      if (owner != nullptr) return TRUE; // skip owned tool windows
      c->hwnd = hwnd;
      return FALSE;
    };
    EnumWindows(enum_proc, reinterpret_cast<LPARAM>(&ctx));
    return ctx.hwnd;
  }

  static bool try_focus_hwnd(HWND hwnd) {
    if (!hwnd) return false;
    // Try to restore and bring to foreground using a more robust approach
    HWND fg = GetForegroundWindow();
    DWORD fg_tid = 0;
    if (fg) {
      fg_tid = GetWindowThreadProcessId(fg, nullptr);
    }
    DWORD cur_tid = GetCurrentThreadId();

    // Attach input to the foreground thread to increase SetForegroundWindow success
    if (fg && fg_tid != 0 && fg_tid != cur_tid) {
      AttachThreadInput(cur_tid, fg_tid, TRUE);
    }

    // Restore if minimized and bring to top-most temporarily
    ShowWindow(hwnd, SW_RESTORE);
    SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
    BOOL ok = SetForegroundWindow(hwnd);
    // Undo top-most
    SetWindowPos(hwnd, HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);

    if (fg && fg_tid != 0 && fg_tid != cur_tid) {
      AttachThreadInput(cur_tid, fg_tid, FALSE);
    }
    return ok != FALSE;
  }

  static bool confirm_foreground_pid(DWORD pid) {
    HWND fg = GetForegroundWindow();
    DWORD fpid = 0; if (fg) GetWindowThreadProcessId(fg, &fpid);
    return fpid == pid;
  }

  static bool focus_process_by_name_extended(const wchar_t *exe_name_w,
                                             int max_successes,
                                             int timeout_secs,
                                             bool exit_on_first,
                                             std::function<bool()> cancel = {}) {
    if (timeout_secs <= 0 || max_successes < 0) return false;
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(timeout_secs);
    int successes = 0;
    bool any = false;
    while (std::chrono::steady_clock::now() < deadline) {
      if (cancel && cancel()) break;
      auto pids = platf::dxgi::find_process_ids_by_name(exe_name_w);
      for (auto pid : pids) {
        if (cancel && cancel()) break;
        if (confirm_foreground_pid(pid)) {
          std::this_thread::sleep_for(200ms);
          continue;
        }
        HWND hwnd = find_main_window_for_pid(pid);
        if (hwnd && try_focus_hwnd(hwnd)) {
          std::this_thread::sleep_for(100ms);
          if (confirm_foreground_pid(pid)) {
            successes++; any = true;
            BOOST_LOG(info) << "Confirmed focus for PID=" << pid << ", successes=" << successes;
            if (exit_on_first) return true;
            if (max_successes > 0 && successes >= max_successes) return true;
          }
        }
      }
      std::this_thread::sleep_for(250ms);
    }
    return any;
  }

  // Forward declarations for helpers defined later in this file
  static bool get_process_image_path(DWORD pid, std::wstring &out);
  static bool path_starts_with(const std::wstring &path, const std::wstring &dir);

  // Enumerate all running processes whose image path begins with install_dir,
  // return sorted by working set (descending)
  static std::vector<DWORD> find_pids_under_install_dir_sorted(const std::wstring &install_dir) {
    std::vector<DWORD> result;
    if (install_dir.empty()) return result;

    // Gather PIDs via EnumProcesses
    DWORD needed = 0; std::vector<DWORD> pids(1024);
    if (!EnumProcesses(pids.data(), (DWORD)(pids.size() * sizeof(DWORD)), &needed)) {
      return result;
    }
    if (needed > pids.size() * sizeof(DWORD)) {
      pids.resize((needed / sizeof(DWORD)) + 256);
      if (!EnumProcesses(pids.data(), (DWORD)(pids.size() * sizeof(DWORD)), &needed)) {
        return result;
      }
    }
    size_t count = needed / sizeof(DWORD);

    struct Item { DWORD pid; SIZE_T wset; };
    std::vector<Item> items;
    items.reserve(count);

    for (size_t i = 0; i < count; ++i) {
      DWORD pid = pids[i];
      if (pid == 0) continue;
      std::wstring img;
      if (!get_process_image_path(pid, img)) continue;
      if (!path_starts_with(img, install_dir)) continue;

      // Check if there is a focusable top-level window
      HWND hwnd = find_main_window_for_pid(pid);
      if (!hwnd) continue;

      HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_VM_READ, FALSE, pid);
      SIZE_T wset = 0;
      if (h) {
        PROCESS_MEMORY_COUNTERS_EX pmc{};
        if (GetProcessMemoryInfo(h, (PROCESS_MEMORY_COUNTERS*)&pmc, sizeof(pmc))) {
          wset = pmc.WorkingSetSize;
        }
        CloseHandle(h);
      }
      items.push_back({pid, wset});
    }

    std::sort(items.begin(), items.end(), [](const Item &a, const Item &b){ return a.wset > b.wset; });
    result.reserve(items.size());
    for (auto &it : items) result.push_back(it.pid);
    return result;
  }

  // Try to focus any process under install_dir, preferring largest working set.
  // Waits up to total_wait_sec for a matching process to appear. For each candidate,
  // attempts SetForegroundWindow 'attempts' times with 1s delay.
  static bool focus_by_install_dir_extended(const std::wstring &install_dir,
                                            int max_successes,
                                            int total_wait_sec,
                                            bool exit_on_first,
                                            std::function<bool()> cancel = {}) {
    if (install_dir.empty() || total_wait_sec <= 0 || max_successes < 0) return false;
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(std::max(1, total_wait_sec));
    int successes = 0; bool any = false;
    while (std::chrono::steady_clock::now() < deadline) {
      if (cancel && cancel()) break;
      auto candidates = find_pids_under_install_dir_sorted(install_dir);
      if (!candidates.empty()) {
        for (auto pid : candidates) {
          if (cancel && cancel()) break;
          if (confirm_foreground_pid(pid)) continue;
          HWND hwnd = find_main_window_for_pid(pid);
          if (hwnd && try_focus_hwnd(hwnd)) {
            std::this_thread::sleep_for(100ms);
            if (confirm_foreground_pid(pid)) {
              successes++; any = true;
              BOOST_LOG(info) << "Confirmed focus (installDir) for PID=" << pid << ", successes=" << successes;
              if (exit_on_first) return true;
              if (max_successes > 0 && successes >= max_successes) return true;
            }
          }
        }
      } else {
        // No candidates yet; wait a bit and retry within the total window
        std::this_thread::sleep_for(250ms);
      }
      std::this_thread::sleep_for(250ms);
    }
    return any;
  }

  static std::wstring to_lower_copy(std::wstring s) {
    for (auto &ch : s) ch = (wchar_t)std::towlower(ch);
    return s;
  }

  static bool path_starts_with(const std::wstring &path, const std::wstring &dir) {
    if (dir.empty()) return false;
    auto p = to_lower_copy(path);
    auto d = to_lower_copy(dir);
    // Normalize separators
    for (auto &ch : p) if (ch == L'/') ch = L'\\';
    for (auto &ch : d) if (ch == L'/') ch = L'\\';
    if (p.size() < d.size()) return false;
    if (p.rfind(d, 0) != 0) return false;
    // Ensure boundary (dir match ends on separator or exact)
    if (p.size() == d.size()) return true;
    return p[d.size()] == L'\\';
  }

  static bool get_process_image_path(DWORD pid, std::wstring &out) {
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!h) return false;
    wchar_t buf[MAX_PATH]; DWORD sz = ARRAYSIZE(buf);
    BOOL ok = QueryFullProcessImageNameW(h, 0, buf, &sz);
    CloseHandle(h);
    if (!ok) return false;
    out.assign(buf, sz);
    return true;
  }

  static void terminate_pid(DWORD pid) {
    HANDLE h = OpenProcess(PROCESS_TERMINATE, FALSE, pid);
    if (!h) return;
    TerminateProcess(h, 1);
    CloseHandle(h);
  }

  static void cleanup_kill_exes_in_dir(const std::wstring &install_dir) {
    if (install_dir.empty()) return;
    std::error_code ec;
    std::vector<std::wstring> exe_basenames;
    try {
      for (auto it = std::filesystem::recursive_directory_iterator(install_dir, ec);
           it != std::filesystem::recursive_directory_iterator(); ++it) {
        if (ec) break;
        if (!it->is_regular_file(ec)) continue;
        auto p = it->path();
        if (p.has_extension() && to_lower_copy(p.extension().wstring()) == L".exe") {
          exe_basenames.push_back(p.stem().wstring());
        }
      }
    } catch (...) {}
    if (exe_basenames.empty()) return;
    // Deduplicate names
    std::sort(exe_basenames.begin(), exe_basenames.end());
    exe_basenames.erase(std::unique(exe_basenames.begin(), exe_basenames.end()), exe_basenames.end());

    // For each basename, find PIDs and terminate if path under install_dir
    for (const auto &name : exe_basenames) {
      std::wstring exe_name = name + L".exe";
      auto pids = platf::dxgi::find_process_ids_by_name(exe_name);
      for (auto pid : pids) {
        std::wstring img;
        if (get_process_image_path(pid, img) && path_starts_with(img, install_dir)) {
          BOOST_LOG(info) << "Cleanup: terminating PID=" << pid << " path=" << platf::dxgi::wide_to_utf8(img);
          terminate_pid(pid);
        }
      }
    }
  }

  static void graceful_shutdown_playnite_desktop() {
    std::wstring desktop = L"C:\\Program Files\\Playnite\\Playnite.DesktopApp.exe";
    std::wstring cmd = L"\"" + desktop + L"\" --shutdown";
    STARTUPINFOW si{}; si.cb = sizeof(si);
    si.dwFlags |= STARTF_USESHOWWINDOW; si.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION pi{};
    std::wstring mutable_cmd = cmd;
    BOOL ok = CreateProcessW(desktop.c_str(), mutable_cmd.data(), nullptr, nullptr, FALSE,
                             CREATE_UNICODE_ENVIRONMENT | CREATE_NEW_PROCESS_GROUP | CREATE_NO_WINDOW | DETACHED_PROCESS,
                             nullptr, nullptr, &si, &pi);
    if (ok) { CloseHandle(pi.hThread); CloseHandle(pi.hProcess); }
  }
}

// Shared launcher logic; invoked by both main and WinMain wrappers
static int launcher_run(int argc, char **argv) {
  // Minimal arg parsing
  std::string game_id;
  std::string public_guid;
  std::string timeout_s;
  std::string focus_attempts_s;
  std::string focus_timeout_s;
  bool focus_exit_on_first_flag = false;
  std::string cleanup_flag;
  std::string install_dir_arg;
  std::string wait_for_pid_s;
  std::span<char*> argspan(argv, (size_t)argc);
  parse_arg(argspan, "--game-id", game_id);
  parse_arg(argspan, "--public-guid", public_guid);
  parse_arg(argspan, "--timeout", timeout_s);
  parse_arg(argspan, "--focus-attempts", focus_attempts_s);
  parse_arg(argspan, "--focus-timeout", focus_timeout_s);
  focus_exit_on_first_flag = parse_flag(argspan, "--focus-exit-on-first");
  bool fullscreen = parse_flag(argspan, "--fullscreen");
  bool do_cleanup = parse_flag(argspan, "--do-cleanup");
  parse_arg(argspan, "--install-dir", install_dir_arg);
  parse_arg(argspan, "--wait-for-pid", wait_for_pid_s);

  if (!fullscreen && !do_cleanup && game_id.empty()) {
    std::fprintf(stderr, "playnite-launcher: missing --game-id <GUID> or --fullscreen\n");
    return 2;
  }

  int timeout_sec = 600; // default 10 minutes safety timeout
  if (!timeout_s.empty()) {
    try { timeout_sec = std::max(1, std::stoi(timeout_s)); } catch (...) {}
  }
  int focus_attempts = 3;
  if (!focus_attempts_s.empty()) {
    try { focus_attempts = std::max(0, std::stoi(focus_attempts_s)); } catch (...) {}
  }
  int focus_timeout_secs = 15;
  if (!focus_timeout_s.empty()) {
    try { focus_timeout_secs = std::max(0, std::stoi(focus_timeout_s)); } catch (...) {}
  }

  // Best effort: do not keep/attach a console if started from one
  FreeConsole();

  // Initialize logging to a dedicated launcher log file
  // Resolve log path under %AppData%\Sunshine
  std::wstring appdataW; appdataW.resize(MAX_PATH);
  if (FAILED(SHGetFolderPathW(nullptr, CSIDL_APPDATA, nullptr, SHGFP_TYPE_CURRENT, appdataW.data()))) {
    appdataW = L"."; // fallback to current dir
  }
  appdataW.resize(wcslen(appdataW.c_str()));
  std::filesystem::path logdir = std::filesystem::path(appdataW) / L"Sunshine";
  std::error_code ec; std::filesystem::create_directories(logdir, ec);
  auto logfile = (logdir / L"sunshine_playnite_launcher.log");
  auto log_path = logfile.string();
  auto _log_guard = logging::init(2 /*info*/, log_path);
  BOOST_LOG(info) << "Playnite launcher starting; pid=" << GetCurrentProcessId();

  // Cleanup-only mode: optionally wait for a specific PID to exit, then run cleanup
  if (do_cleanup) {
    BOOST_LOG(info) << "Cleanup mode: starting";
    // Optional: wait for a specific PID (typically parent launcher) to exit
    if (!wait_for_pid_s.empty()) {
      try {
        DWORD wpid = (DWORD) std::stoul(wait_for_pid_s);
        if (wpid != 0 && wpid != GetCurrentProcessId()) {
          HANDLE hp = OpenProcess(SYNCHRONIZE, FALSE, wpid);
          if (hp) {
            BOOST_LOG(info) << "Cleanup mode: waiting for PID=" << wpid << " to exit";
            // Wait up to 30 minutes for the process to exit; early-exit if handle signals sooner
            DWORD wr = WaitForSingleObject(hp, 30 * 60 * 1000);
            CloseHandle(hp);
            BOOST_LOG(info) << "Cleanup mode: wait result=" << wr;
          } else {
            BOOST_LOG(warning) << "Cleanup mode: unable to open PID for wait: " << wpid;
          }
        }
      } catch (...) {
        BOOST_LOG(warning) << "Cleanup mode: invalid --wait-for-pid value: '" << wait_for_pid_s << "'";
      }
    }
    std::wstring install_dir_w = platf::dxgi::utf8_to_wide(install_dir_arg);
    if (!install_dir_w.empty()) {
      cleanup_kill_exes_in_dir(install_dir_w);
    }
    if (fullscreen) {
      std::this_thread::sleep_for(6s);
      graceful_shutdown_playnite_desktop();
    }
    BOOST_LOG(info) << "Cleanup mode: done";
    return 0;
  }

  // Fullscreen mode: start Playnite.FullscreenApp and apply focus attempts, then schedule cleanup and exit
  if (fullscreen) {
    BOOST_LOG(info) << "Fullscreen mode requested; launching Playnite.FullscreenApp";
    // Default path, matches prior script example
    std::wstring fullpath = L"C:\\Program Files\\Playnite\\Playnite.FullscreenApp.exe";
    STARTUPINFOW si{}; si.cb = sizeof(si);
    si.dwFlags |= STARTF_USESHOWWINDOW; si.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION pi{};
    std::wstring cmdline = L"\"" + fullpath + L"\""; // no extra args
    BOOL ok = CreateProcessW(fullpath.c_str(), cmdline.data(), nullptr, nullptr, FALSE,
                             CREATE_UNICODE_ENVIRONMENT | CREATE_NEW_PROCESS_GROUP | CREATE_NO_WINDOW | DETACHED_PROCESS,
                             nullptr, nullptr, &si, &pi);
    if (ok) {
      CloseHandle(pi.hThread);
      CloseHandle(pi.hProcess);
    } else {
      BOOST_LOG(error) << "Failed to start Playnite.FullscreenApp.exe";
    }
    // Wait briefly for process to appear then try to focus
    auto deadline = std::chrono::steady_clock::now() + 5s;
    while (std::chrono::steady_clock::now() < deadline) {
      auto pids = platf::dxgi::find_process_ids_by_name(L"Playnite.FullscreenApp.exe");
      if (!pids.empty()) break;
      std::this_thread::sleep_for(300ms);
    }
    bool focused = focus_process_by_name_extended(L"Playnite.FullscreenApp.exe", focus_attempts, focus_timeout_secs, focus_exit_on_first_flag);
    BOOST_LOG(info) << (focused ? "Fullscreen focus applied" : "Fullscreen focus not confirmed");
    // Spawn background cleanup to gracefully shutdown desktop in ~6s
    WCHAR selfPath[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, selfPath, ARRAYSIZE(selfPath));
    std::wstring wexe(selfPath);
    std::wstring wcmd = L"\"" + wexe + L"\" --do-cleanup --fullscreen";
    STARTUPINFOW si2{}; si2.cb = sizeof(si2);
    si2.dwFlags |= STARTF_USESHOWWINDOW; si2.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION pi2{}; std::wstring cmd2 = wcmd;
    CreateProcessW(selfPath, cmd2.data(), nullptr, nullptr, FALSE,
                   CREATE_UNICODE_ENVIRONMENT | CREATE_NEW_PROCESS_GROUP | CREATE_NO_WINDOW | DETACHED_PROCESS,
                   nullptr, nullptr, &si2, &pi2);
    if (pi2.hThread) CloseHandle(pi2.hThread);
    if (pi2.hProcess) CloseHandle(pi2.hProcess);
    return 0;
  }

  // If no public GUID was supplied, relaunch self with one so the GUID is visible
  if (public_guid.empty()) {
    try {
      public_guid = platf::dxgi::generate_guid();
    } catch (...) {
      public_guid.clear();
    }
    // Build command line: "exe" --game-id <id> --public-guid <guid> [--timeout N]
    WCHAR selfPath[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, selfPath, ARRAYSIZE(selfPath));
    std::wstring wexe(selfPath);
    std::wstring wcmd = L"\"" + wexe + L"\" --game-id " + std::wstring(game_id.begin(), game_id.end()) +
                        L" --public-guid " + std::wstring(public_guid.begin(), public_guid.end()) +
                        (timeout_s.empty() ? L"" : (L" --timeout " + std::wstring(timeout_s.begin(), timeout_s.end())));
    STARTUPINFOW si{}; si.cb = sizeof(si);
    si.dwFlags |= STARTF_USESHOWWINDOW; // ensure hidden window
    si.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION pi{};
    std::wstring cmdline = wcmd; // mutable buffer for CreateProcess
    BOOL ok = CreateProcessW(selfPath, cmdline.data(), nullptr, nullptr, FALSE,
                             CREATE_UNICODE_ENVIRONMENT | CREATE_NEW_PROCESS_GROUP | CREATE_NO_WINDOW | DETACHED_PROCESS,
                             nullptr, nullptr, &si, &pi);
    if (ok) {
      CloseHandle(pi.hThread);
      CloseHandle(pi.hProcess);
      BOOST_LOG(info) << "Spawned child with public GUID: " << public_guid;
      return 0; // Parent exits; child continues with server
    } else {
      BOOST_LOG(error) << "Failed to spawn child process with GUID; continuing in current process";
    }
  }
  BOOST_LOG(info) << "Using public GUID: " << public_guid;

  // Build dynamic control pipe name
  std::string control_name = std::string("sunshine_playnite_ctl_") + public_guid;

  // Discovery marker removed: do not write JSON files under %AppData%/Sunshine/playnite_launcher

  // Host IPC server and watch for status updates
  platf::playnite::IpcServer server(control_name);
  std::atomic<bool> got_started{false};
  std::atomic<bool> should_exit{false};
  std::string last_install_dir;
  std::string last_game_exe;
  std::atomic<bool> watcher_spawned{false};

  auto spawn_cleanup_watcher = [&](const std::string &install_dir_utf8) {
    if (install_dir_utf8.empty()) return;
    bool expected = false;
    if (!watcher_spawned.compare_exchange_strong(expected, true)) return; // spawn once
    try {
      WCHAR selfPath[MAX_PATH] = {};
      GetModuleFileNameW(nullptr, selfPath, ARRAYSIZE(selfPath));
      std::wstring wexe(selfPath);
      std::wstring wcmd = L"\"" + wexe + L"\" --do-cleanup --install-dir \"" + platf::dxgi::utf8_to_wide(install_dir_utf8) + L"\" --wait-for-pid " + std::to_wstring(GetCurrentProcessId());
      STARTUPINFOW si{}; si.cb = sizeof(si);
      si.dwFlags |= STARTF_USESHOWWINDOW; si.wShowWindow = SW_HIDE;
      PROCESS_INFORMATION pi{}; std::wstring cmd2 = wcmd;
      DWORD flags_base = CREATE_UNICODE_ENVIRONMENT | CREATE_NEW_PROCESS_GROUP | CREATE_NO_WINDOW | DETACHED_PROCESS;
      // Best-effort: attempt to break away from a potential job object so cleanup can survive group termination
      DWORD flags_try = flags_base | CREATE_BREAKAWAY_FROM_JOB;
      BOOL ok = CreateProcessW(selfPath, cmd2.data(), nullptr, nullptr, FALSE,
                               flags_try,
                               nullptr, nullptr, &si, &pi);
      if (!ok) {
        // Fallback: try without breakaway flag
        ok = CreateProcessW(selfPath, cmd2.data(), nullptr, nullptr, FALSE,
                            flags_base,
                            nullptr, nullptr, &si, &pi);
      }
      if (ok) {
        BOOST_LOG(info) << "Spawned cleanup watcher for installDir; pid=" << pi.dwProcessId;
        if (pi.hThread) CloseHandle(pi.hThread);
        if (pi.hProcess) CloseHandle(pi.hProcess);
      } else {
        BOOST_LOG(warning) << "Failed to spawn cleanup watcher";
        watcher_spawned.store(false);
      }
    } catch (...) {
      BOOST_LOG(warning) << "Exception while spawning cleanup watcher";
      watcher_spawned.store(false);
    }
  };

  server.set_message_handler([&](std::span<const uint8_t> bytes) {
    // Incoming messages are JSON objects from the plugin; parse and watch for status
    auto msg = platf::playnite::parse(bytes);
    using MT = platf::playnite::MessageType;
    if (msg.type == MT::Status) {
  BOOST_LOG(info) << "Status: name=" << msg.status_name << " id=" << msg.status_game_id;
      auto norm = [](std::string s) {
        s.erase(std::remove_if(s.begin(), s.end(), [](char c){ return c=='{' || c=='}'; }), s.end());
        std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c){ return (char)std::tolower(c); });
        return s;
      };
      if (!msg.status_game_id.empty() && norm(msg.status_game_id) == norm(game_id)) {
        if (msg.status_name == "gameStarted") { got_started.store(true); }
        if (msg.status_name == "gameStopped") { should_exit.store(true); }
        if (!msg.status_install_dir.empty()) {
          last_install_dir = msg.status_install_dir;
          // As soon as we learn the install dir, spawn a watcher to handle Moonlight-side termination
          spawn_cleanup_watcher(last_install_dir);
        }
  if (!msg.status_exe.empty()) { last_game_exe = msg.status_exe; }
      }
    }
  });

  server.start();

  // Wait for data pipe active then send launch command
  auto start_deadline = std::chrono::steady_clock::now() + std::chrono::minutes(2);
  while (!server.is_active() && std::chrono::steady_clock::now() < start_deadline) {
    std::this_thread::sleep_for(50ms);
  }
  if (!server.is_active()) {
    BOOST_LOG(error) << "IPC did not become active; exiting";
    return 3;
  }

  // Send command: {type:"command", command:"launch", id:"<GUID>"}
  {
    nlohmann::json j;
    j["type"] = "command";
    j["command"] = "launch";
    j["id"] = game_id;
    server.send_json_line(j.dump());
    BOOST_LOG(info) << "Launch command sent for id=" << game_id;
  }

  // Best-effort: shortly after we observe game start, attempt to bring the game (or Playnite) to foreground
  // (helps controller navigation start). Prefer processes under the game's install directory by working set;
  // then try the specific game EXE by name; else fall back to Playnite windows.
  if (focus_attempts > 0 && focus_timeout_secs > 0) {
    auto start_wait = std::chrono::steady_clock::now() + 5s;
    while (!got_started.load() && std::chrono::steady_clock::now() < start_wait) {
      std::this_thread::sleep_for(200ms);
    }
    bool focused = false;
    auto overall_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(std::max(1, focus_timeout_secs));
    if (!focused && !last_install_dir.empty()) {
      try {
        std::wstring wdir = platf::dxgi::utf8_to_wide(last_install_dir);
        BOOST_LOG(info) << "Autofocus: trying installDir=" << last_install_dir;
        int remaining = (int) std::chrono::duration_cast<std::chrono::seconds>(overall_deadline - std::chrono::steady_clock::now()).count();
        if (remaining > 0)
          focused = focus_by_install_dir_extended(wdir, focus_attempts, remaining, focus_exit_on_first_flag, [&](){ return should_exit.load(); });
      } catch (...) {}
    }
    if (!last_game_exe.empty()) {
      try {
        std::wstring wexe = platf::dxgi::utf8_to_wide(last_game_exe);
        // Strip path and focus by basename
        std::filesystem::path p = wexe;
        std::wstring base = p.filename().wstring();
        if (!base.empty()) {
          int remaining = (int) std::chrono::duration_cast<std::chrono::seconds>(overall_deadline - std::chrono::steady_clock::now()).count();
          if (remaining > 0)
            focused = focus_process_by_name_extended(base.c_str(), focus_attempts, remaining, focus_exit_on_first_flag, [&](){ return should_exit.load(); });
        }
      } catch (...) {}
    }
    if (!focused) {
      int remaining = (int) std::chrono::duration_cast<std::chrono::seconds>(overall_deadline - std::chrono::steady_clock::now()).count();
      if (remaining > 0)
        focused = focus_process_by_name_extended(L"Playnite.FullscreenApp.exe", focus_attempts, remaining, focus_exit_on_first_flag, [&](){ return should_exit.load(); });
    }
    if (!focused) {
      int remaining = (int) std::chrono::duration_cast<std::chrono::seconds>(overall_deadline - std::chrono::steady_clock::now()).count();
      if (remaining > 0)
        focused = focus_process_by_name_extended(L"Playnite.DesktopApp.exe", focus_attempts, remaining, focus_exit_on_first_flag, [&](){ return should_exit.load(); });
    }
    BOOST_LOG(info) << (focused ? "Applied focus after launch" : "Focus not applied after launch");
  }

  // Wait for stop or timeout
  auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(timeout_sec);
  while (!should_exit.load() && std::chrono::steady_clock::now() < deadline) {
    // If Playnite has exited but we saw the game start, proceed to cleanup immediately
    if (got_started.load()) {
      auto d = platf::dxgi::find_process_ids_by_name(L"Playnite.DesktopApp.exe");
      auto f = platf::dxgi::find_process_ids_by_name(L"Playnite.FullscreenApp.exe");
      if (d.empty() && f.empty()) {
        BOOST_LOG(warning) << "Playnite process appears to have exited; proceeding to cleanup";
        should_exit.store(true);
        break;
      }
    }
    std::this_thread::sleep_for(250ms);
  }

  if (!should_exit.load()) {
    BOOST_LOG(warning) << "Timeout waiting for gameStopped; exiting";
    // Best-effort cleanup: remove marker file
  // Discovery marker removal skipped (marker not created)
    // Fall through to schedule cleanup anyway
  }

  BOOST_LOG(info) << "Playnite reported gameStopped or timeout; scheduling cleanup and exiting";
  // Spawn background cleanup to terminate lingering game executables under install dir
  if (!last_install_dir.empty()) {
    WCHAR selfPath[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, selfPath, ARRAYSIZE(selfPath));
    std::wstring wexe(selfPath);
    std::wstring wcmd = L"\"" + wexe + L"\" --do-cleanup --install-dir \"" + platf::dxgi::utf8_to_wide(last_install_dir) + L"\"";
    STARTUPINFOW si2{}; si2.cb = sizeof(si2);
    si2.dwFlags |= STARTF_USESHOWWINDOW; si2.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION pi2{}; std::wstring cmd2 = wcmd;
    CreateProcessW(selfPath, cmd2.data(), nullptr, nullptr, FALSE,
                   CREATE_UNICODE_ENVIRONMENT | CREATE_NEW_PROCESS_GROUP | CREATE_NO_WINDOW | DETACHED_PROCESS,
                   nullptr, nullptr, &si2, &pi2);
    if (pi2.hThread) CloseHandle(pi2.hThread);
    if (pi2.hProcess) CloseHandle(pi2.hProcess);
  }
  return should_exit.load() ? 0 : 4;
}

// Console entry point (used by non-WIN32 subsystem builds and tests)
int main(int argc, char **argv) {
  return launcher_run(argc, argv);
}

#ifdef _WIN32
// GUI subsystem entry point: avoid console window
int APIENTRY wWinMain(HINSTANCE, HINSTANCE, LPWSTR, int) {
  int argc = 0;
  LPWSTR *wargv = CommandLineToArgvW(GetCommandLineW(), &argc);
  std::vector<std::string> utf8args;
  utf8args.reserve(argc);
  for (int i = 0; i < argc; ++i) {
    int need = WideCharToMultiByte(CP_UTF8, 0, wargv[i], -1, nullptr, 0, nullptr, nullptr);
    std::string s; s.resize(need > 0 ? (size_t)need - 1 : 0);
    if (need > 0) WideCharToMultiByte(CP_UTF8, 0, wargv[i], -1, s.data(), need, nullptr, nullptr);
    utf8args.emplace_back(std::move(s));
  }
  std::vector<char*> argv;
  argv.reserve(utf8args.size());
  for (auto &s : utf8args) argv.push_back(s.data());
  int rc = launcher_run((int)argv.size(), argv.data());
  if (wargv) LocalFree(wargv);
  return rc;
}
#endif

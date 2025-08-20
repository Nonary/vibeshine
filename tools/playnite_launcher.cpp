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
#include <unordered_map>
#include <unordered_set>

#include "src/logging.h"
#include "src/platform/windows/playnite_ipc.h"
#include "src/platform/windows/playnite_protocol.h"
#include "src/platform/windows/ipc/misc_utils.h"
#include <nlohmann/json.hpp>
#include <shlobj.h>
#include <fstream>
#include <cwctype>
#include <psapi.h>
#include <tlhelp32.h>

using namespace std::chrono_literals;

// Fallback declaration for CommandLineToArgvW if headers don't provide it
#ifdef _WIN32
#ifndef HAVE_CommandLineToArgvW_DECL
extern "C" __declspec(dllimport) LPWSTR* WINAPI CommandLineToArgvW(LPCWSTR lpCmdLine, int *pNumArgs);
#endif
#endif

namespace {
  static std::wstring get_env(const wchar_t *name) {
    wchar_t buf[32768];
    DWORD n = GetEnvironmentVariableW(name, buf, ARRAYSIZE(buf));
    if (n == 0 || n >= ARRAYSIZE(buf)) return L"";
    return std::wstring(buf, n);
  }

  static bool file_exists(const std::wstring &p) {
    std::error_code ec; return std::filesystem::exists(p, ec);
  }

  static bool read_reg_string(HKEY root, const wchar_t *subkey, const wchar_t *value, std::wstring &out, REGSAM view = KEY_READ) {
    HKEY h = nullptr;
    if (RegOpenKeyExW(root, subkey, 0, view | KEY_QUERY_VALUE, &h) != ERROR_SUCCESS) return false;
    DWORD type = 0; DWORD cb = 0;
    if (RegQueryValueExW(h, value, nullptr, &type, nullptr, &cb) != ERROR_SUCCESS || type != REG_SZ) { RegCloseKey(h); return false; }
    std::wstring tmp; tmp.resize(cb / sizeof(wchar_t));
    if (RegQueryValueExW(h, value, nullptr, &type, reinterpret_cast<LPBYTE>(tmp.data()), &cb) == ERROR_SUCCESS && type == REG_SZ) {
      // Ensure null termination and trim
      tmp.resize(wcsnlen(tmp.c_str(), tmp.size()));
      out = std::move(tmp);
      RegCloseKey(h); return true;
    }
    RegCloseKey(h); return false;
  }

  // Try to find Playnite.FullscreenApp.exe across common install locations and registry.
  static std::wstring find_playnite_fullscreen_path() {
    std::vector<std::wstring> candidates;
    // Common program locations
    auto pf = get_env(L"ProgramFiles");
    if (!pf.empty()) candidates.push_back(pf + L"\\Playnite\\Playnite.FullscreenApp.exe");
    auto pf86 = get_env(L"ProgramFiles(x86)");
    if (!pf86.empty()) candidates.push_back(pf86 + L"\\Playnite\\Playnite.FullscreenApp.exe");
    auto lapp = get_env(L"LOCALAPPDATA");
    if (!lapp.empty()) {
      candidates.push_back(lapp + L"\\Programs\\Playnite\\Playnite.FullscreenApp.exe");
      candidates.push_back(lapp + L"\\Playnite\\Playnite.FullscreenApp.exe");
    }

    for (auto &c : candidates) if (file_exists(c)) return c;

    // Registry: try InstallLocation or DisplayIcon for Playnite
    std::wstring install;
    if (!install.empty());
    auto try_from_install = [&](const std::wstring &dir) -> std::wstring {
      if (dir.empty()) return L"";
      std::filesystem::path p(dir);
      std::error_code ec;
      auto fs = (p / L"Playnite.FullscreenApp.exe").wstring();
      if (file_exists(fs)) return fs;
      return L"";
    };

    // HKCU and HKLM, 64 and 32-bit views
    const wchar_t *sub = L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\Playnite";
    std::wstring val;
    if (read_reg_string(HKEY_CURRENT_USER, sub, L"InstallLocation", val, KEY_READ)) {
      auto fs = try_from_install(val); if (!fs.empty()) return fs;
    }
    if (read_reg_string(HKEY_LOCAL_MACHINE, sub, L"InstallLocation", val, KEY_READ)) {
      auto fs = try_from_install(val); if (!fs.empty()) return fs;
    }
    if (read_reg_string(HKEY_LOCAL_MACHINE, L"SOFTWARE\\WOW6432Node\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\Playnite", L"InstallLocation", val, KEY_READ)) {
      auto fs = try_from_install(val); if (!fs.empty()) return fs;
    }
    // DisplayIcon sometimes points to DesktopApp; derive sibling FullscreenApp
    if (read_reg_string(HKEY_CURRENT_USER, sub, L"DisplayIcon", val, KEY_READ)) {
      std::filesystem::path p(val); auto dir = p.parent_path(); auto fs = (dir / L"Playnite.FullscreenApp.exe").wstring(); if (file_exists(fs)) return fs;
    }
    if (read_reg_string(HKEY_LOCAL_MACHINE, sub, L"DisplayIcon", val, KEY_READ)) {
      std::filesystem::path p(val); auto dir = p.parent_path(); auto fs = (dir / L"Playnite.FullscreenApp.exe").wstring(); if (file_exists(fs)) return fs;
    }
    if (read_reg_string(HKEY_LOCAL_MACHINE, L"SOFTWARE\\WOW6432Node\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\Playnite", L"DisplayIcon", val, KEY_READ)) {
      std::filesystem::path p(val); auto dir = p.parent_path(); auto fs = (dir / L"Playnite.FullscreenApp.exe").wstring(); if (file_exists(fs)) return fs;
    }

    // Search PATH (rare)
    wchar_t out[MAX_PATH]; DWORD rc = SearchPathW(nullptr, L"Playnite.FullscreenApp.exe", nullptr, ARRAYSIZE(out), out, nullptr);
    if (rc > 0 && rc < ARRAYSIZE(out) && file_exists(out)) return std::wstring(out);

    return L"";
  }
  
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

  // Enumerate all top-level windows belonging to PID and invoke cb(hwnd)
  template <typename F>
  static void for_each_top_level_window_of_pid(DWORD pid, F &&cb) {
    using Fn = std::decay_t<F>;
    Fn *fn = &cb;
    struct Ctx { DWORD pid; Fn *fn; } ctx{pid, fn};
    auto enum_proc = [](HWND hwnd, LPARAM lparam) -> BOOL {
      auto *c = reinterpret_cast<Ctx*>(lparam);
      DWORD wpid = 0; GetWindowThreadProcessId(hwnd, &wpid);
      if (wpid != c->pid) return TRUE;
      // Consider all top-level windows (owner may be null); don't require IsWindowVisible
      if (GetWindow(hwnd, GW_OWNER) != nullptr) return TRUE;
      (*c->fn)(hwnd);
      return TRUE;
    };
    EnumWindows(enum_proc, reinterpret_cast<LPARAM>(&ctx));
  }

  // Enumerate all thread windows of PID and invoke cb(hwnd)
  template <typename F>
  static void for_each_thread_window_of_pid(DWORD pid, F &&cb) {
    using Fn = std::decay_t<F>;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap == INVALID_HANDLE_VALUE) return;
    THREADENTRY32 te{}; te.dwSize = sizeof(te);
    if (Thread32First(snap, &te)) {
      do {
        if (te.th32OwnerProcessID != pid) continue;
        EnumThreadWindows(te.th32ThreadID, [](HWND hwnd, LPARAM lp) -> BOOL {
          auto *f = reinterpret_cast<Fn*>(lp);
          (*f)(hwnd); return TRUE;
        }, reinterpret_cast<LPARAM>(&cb));
      } while (Thread32Next(snap, &te));
    }
    CloseHandle(snap);
  }

  static void send_message_timeout(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    DWORD_PTR result = 0;
    SendMessageTimeoutW(hwnd, msg, wParam, lParam, SMTO_ABORTIFHUNG | SMTO_NORMAL, 200, &result);
  }

  // Stage 1: Polite close (SC_CLOSE + WM_CLOSE) to all windows of PID
  static void stage_close_windows_for_pid(DWORD pid) {
    int top_count = 0, thread_count = 0;
    auto send = [&](HWND hwnd){
      send_message_timeout(hwnd, WM_SYSCOMMAND, SC_CLOSE, 0);
      send_message_timeout(hwnd, WM_CLOSE, 0, 0);
      // We can't tell from here if this is top-level or thread window; counts are approximate
    };
    for_each_top_level_window_of_pid(pid, [&](HWND hwnd){ ++top_count; send(hwnd); });
    for_each_thread_window_of_pid(pid, [&](HWND hwnd){ ++thread_count; send(hwnd); });
    BOOST_LOG(info) << "Cleanup: stage1 sent SC_CLOSE/WM_CLOSE to PID=" << pid
                    << " topWindows=" << top_count << " threadWindows=" << thread_count;
  }

  // Stage 2: Logoff-style close (QUERYENDSESSION/ENDSESSION)
  static void stage_logoff_for_pid(DWORD pid) {
    int top_count = 0, thread_count = 0;
    auto send = [&](HWND hwnd){
      send_message_timeout(hwnd, WM_QUERYENDSESSION, TRUE, 0);
      // Per request: follow with WM_ENDSESSION (FALSE) to hint a close without full logoff
      send_message_timeout(hwnd, WM_ENDSESSION, FALSE, 0);
    };
    for_each_top_level_window_of_pid(pid, [&](HWND hwnd){ ++top_count; send(hwnd); });
    for_each_thread_window_of_pid(pid, [&](HWND hwnd){ ++thread_count; send(hwnd); });
    BOOST_LOG(info) << "Cleanup: stage2 sent QUERY/ENDSESSION to PID=" << pid
                    << " topWindows=" << top_count << " threadWindows=" << thread_count;
  }

  // Stage 3: Post WM_QUIT to (approx) main thread and try console CTRL events
  static void stage_quit_thread_or_console(DWORD pid) {
    // Choose the smallest thread ID for the process as an approximation for the main thread
    DWORD main_tid = 0xFFFFFFFF;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap != INVALID_HANDLE_VALUE) {
      THREADENTRY32 te{}; te.dwSize = sizeof(te);
      if (Thread32First(snap, &te)) {
        do {
          if (te.th32OwnerProcessID != pid) continue;
          if (te.th32ThreadID < main_tid) main_tid = te.th32ThreadID;
        } while (Thread32Next(snap, &te));
      }
      CloseHandle(snap);
    }
    if (main_tid != 0xFFFFFFFF) {
      BOOST_LOG(info) << "Cleanup: stage3 posting WM_QUIT to TID=" << main_tid << " (PID=" << pid << ")";
      PostThreadMessageW(main_tid, WM_QUIT, 0, 0);
    } else {
      BOOST_LOG(info) << "Cleanup: stage3 no thread found to post WM_QUIT (PID=" << pid << ")";
    }
    // Best-effort console CTRL event
    if (AttachConsole(pid)) {
      BOOST_LOG(info) << "Cleanup: stage3 attached console; sending CTRL_BREAK (PID=" << pid << ")";
      SetConsoleCtrlHandler(nullptr, TRUE);
      GenerateConsoleCtrlEvent(CTRL_BREAK_EVENT, 0);
      FreeConsole();
    }
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
  static bool pid_alive(DWORD pid);

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

  // Enumerate top-level windows of a PID and send WM_CLOSE
  static void send_wm_close_to_pid(DWORD pid) {
    struct Ctx { DWORD pid; } ctx{pid};
    auto enum_proc = [](HWND hwnd, LPARAM lp) -> BOOL {
      auto *c = reinterpret_cast<Ctx*>(lp);
      DWORD wpid = 0; GetWindowThreadProcessId(hwnd, &wpid);
      if (wpid != c->pid) return TRUE;
      // Only target visible top-level windows (no owners)
      if (!IsWindowVisible(hwnd)) return TRUE;
      if (GetWindow(hwnd, GW_OWNER) != nullptr) return TRUE;
      SendNotifyMessageW(hwnd, WM_CLOSE, 0, 0);
      return TRUE;
    };
    EnumWindows(enum_proc, reinterpret_cast<LPARAM>(&ctx));
  }

  // Graceful-then-forceful cleanup of processes whose image path is under install_dir
  static void cleanup_graceful_then_forceful_in_dir(const std::wstring &install_dir, int exit_timeout_secs) {
    if (install_dir.empty()) return;
    BOOST_LOG(info) << "Cleanup: begin for install_dir='" << platf::dxgi::wide_to_utf8(install_dir) << "' timeout=" << exit_timeout_secs << "s";
    // Gather initial candidate PIDs
    auto collect = [&]() {
      return find_pids_under_install_dir_sorted(install_dir);
    };

    // Time-sliced escalation: 0-40% close; 40-70% endsession; 70-100% quit/console; then force
    auto t_total = std::max(1, exit_timeout_secs);
    auto t_start = std::chrono::steady_clock::now();
    bool sent_close = false, sent_endsession = false, sent_quit = false;
    while (true) {
      auto remaining = collect();
      static bool logged_initial = false;
      if (!logged_initial) {
        // Log discovered PIDs and their image paths
        BOOST_LOG(info) << "Cleanup: initial candidates count=" << remaining.size();
        for (auto pid : remaining) {
          std::wstring img; get_process_image_path(pid, img);
          BOOST_LOG(info) << "Cleanup: candidate PID=" << pid << " path='" << platf::dxgi::wide_to_utf8(img) << "'";
        }
        logged_initial = true;
      }
      if (remaining.empty()) {
        BOOST_LOG(info) << "Cleanup: all processes exited gracefully";
        return;
      }
      auto now = std::chrono::steady_clock::now();
      auto elapsed_ms = (int) std::chrono::duration_cast<std::chrono::milliseconds>(now - t_start).count();
      double frac = std::min(1.0, (double)elapsed_ms / (double)(t_total * 1000));

      if (!sent_close) {
        BOOST_LOG(info) << "Cleanup: stage 1 (SC_CLOSE/WM_CLOSE) for " << remaining.size() << " processes";
        for (auto pid : remaining) stage_close_windows_for_pid(pid);
        sent_close = true;
      } else if (frac >= 0.4 && !sent_endsession) {
        BOOST_LOG(info) << "Cleanup: stage 2 (QUERYENDSESSION/ENDSESSION)";
        for (auto pid : remaining) stage_logoff_for_pid(pid);
        sent_endsession = true;
      } else if (frac >= 0.7 && !sent_quit) {
        BOOST_LOG(info) << "Cleanup: stage 3 (WM_QUIT + console CTRL)";
        for (auto pid : remaining) stage_quit_thread_or_console(pid);
        sent_quit = true;
      }

      if (frac >= 1.0) break;
      std::this_thread::sleep_for(std::chrono::milliseconds(250));
    }

    // Force terminate any remaining
    auto remaining = collect();
    for (auto pid : remaining) {
      std::wstring img; get_process_image_path(pid, img);
      BOOST_LOG(warning) << "Cleanup: forcing termination of PID=" << pid << (img.empty() ? "" : (" path=" + platf::dxgi::wide_to_utf8(img)));
      terminate_pid(pid);
    }
  }

  static void graceful_shutdown_playnite(bool fullscreen, int exit_timeout_secs) {
    std::vector<DWORD> pids;
    try {
      if (fullscreen) {
        pids = platf::dxgi::find_process_ids_by_name(L"Playnite.FullscreenApp.exe");
      } else {
        pids = platf::dxgi::find_process_ids_by_name(L"Playnite.DesktopApp.exe");
      }
    } catch (...) {}
    if (pids.empty()) return;
    for (DWORD pid : pids) send_wm_close_to_pid(pid);
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(std::max(1, exit_timeout_secs));
    while (std::chrono::steady_clock::now() < deadline) {
      bool any = false;
      for (DWORD pid : pids) { if (pid_alive(pid)) { any = true; break; } }
      if (!any) return;
      std::this_thread::sleep_for(250ms);
    }
    for (DWORD pid : pids) if (pid_alive(pid)) terminate_pid(pid);
  }

  // Helpers for targeted descendant cleanup
  static std::vector<DWORD> find_playnite_root_pids() {
    std::vector<DWORD> roots;
    try {
      auto d = platf::dxgi::find_process_ids_by_name(L"Playnite.DesktopApp.exe");
      auto f = platf::dxgi::find_process_ids_by_name(L"Playnite.FullscreenApp.exe");
      roots.reserve(d.size() + f.size());
      roots.insert(roots.end(), d.begin(), d.end());
      roots.insert(roots.end(), f.begin(), f.end());
    } catch (...) {}
    return roots;
  }

  struct ProcSnapshot {
    std::unordered_map<DWORD, std::vector<DWORD>> children;
    std::unordered_map<DWORD, std::wstring> exe_basename;
    std::unordered_map<DWORD, std::wstring> img_path;
  };

  static ProcSnapshot build_process_snapshot() {
    ProcSnapshot snap;
    HANDLE h = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (h == INVALID_HANDLE_VALUE) return snap;
    PROCESSENTRY32W pe{}; pe.dwSize = sizeof(pe);
    if (Process32FirstW(h, &pe)) {
      do {
        DWORD pid = pe.th32ProcessID;
        DWORD ppid = pe.th32ParentProcessID;
        snap.children[ppid].push_back(pid);
        snap.exe_basename[pid] = pe.szExeFile;
      } while (Process32NextW(h, &pe));
    }
    CloseHandle(h);
    // Optionally populate full image path for more accurate filtering
    for (const auto &kv : snap.exe_basename) {
      DWORD pid = kv.first;
      std::wstring img;
      if (get_process_image_path(pid, img)) snap.img_path[pid] = std::move(img);
    }
    return snap;
  }

  static std::unordered_set<DWORD> compute_descendants(const std::vector<DWORD> &roots, const ProcSnapshot &snap) {
    std::unordered_set<DWORD> out;
    std::vector<DWORD> stack = roots;
    while (!stack.empty()) {
      DWORD cur = stack.back(); stack.pop_back();
      auto it = snap.children.find(cur);
      if (it == snap.children.end()) continue;
      for (DWORD child : it->second) {
        if (out.insert(child).second) stack.push_back(child);
      }
    }
    return out;
  }

  static bool is_playnite_exe_basename(const std::wstring &base) {
    auto b = to_lower_copy(base);
    return b == L"playnite.desktopapp.exe" || b == L"playnite.fullscreenapp.exe";
  }

  static bool pid_alive(DWORD pid) {
    HANDLE h = OpenProcess(SYNCHRONIZE, FALSE, pid);
    if (!h) return false;
    DWORD wr = WaitForSingleObject(h, 0);
    CloseHandle(h);
    return wr == WAIT_TIMEOUT;
  }

  // Targeted cleanup: prefer descendants of Playnite roots; filter to exe basename or install_dir
  static void cleanup_descendants_targeted(const std::wstring &install_dir,
                                           const std::wstring &exe_basename,
                                           int exit_timeout_secs) {
    auto roots = find_playnite_root_pids();
    ProcSnapshot snap = build_process_snapshot();
    std::unordered_set<DWORD> candidates;
    if (!roots.empty()) {
      candidates = compute_descendants(roots, snap);
    }
    // If we have an exe basename, also consider processes by name globally (launcher/Steam descendants, etc.)
    std::vector<DWORD> by_name_extra;
    if (!exe_basename.empty()) {
      try {
        by_name_extra = platf::dxgi::find_process_ids_by_name(exe_basename);
      } catch (...) {}
    }
    auto matches_filters = [&](DWORD pid) {
      if (pid == 0) return false;
      auto itb = snap.exe_basename.find(pid);
      std::wstring base = (itb != snap.exe_basename.end()) ? itb->second : L"";
      if (is_playnite_exe_basename(base)) return false; // never target Playnite itself
      if (!exe_basename.empty()) {
        std::wstring bb = to_lower_copy(base);
        std::wstring eb = to_lower_copy(exe_basename);
        if (bb == eb) return true;
      }
      if (!install_dir.empty()) {
        std::wstring img;
        auto iti = snap.img_path.find(pid);
        if (iti != snap.img_path.end()) img = iti->second;
        if (!img.empty() && path_starts_with(img, install_dir)) return true;
      }
      return false;
    };

    std::vector<DWORD> targets;
    if (!candidates.empty()) {
      for (DWORD pid : candidates) if (matches_filters(pid)) targets.push_back(pid);
    } else {
      // Fallback: no roots found; use directory scan and optional exe basename
      auto bydir = find_pids_under_install_dir_sorted(install_dir);
      // Include any known-by-name PIDs even when install_dir is empty or unrelated
      if (!exe_basename.empty()) {
        for (DWORD pid : by_name_extra) {
          // populate snapshot info if missing (image path may be needed for excluding Playnite)
          if (!snap.exe_basename.contains(pid)) {
            std::wstring base;
            get_process_image_path(pid, base); // base is full path; we'll fill exe name below from it if possible
            std::filesystem::path p = base;
            snap.exe_basename[pid] = p.filename().wstring();
            snap.img_path[pid] = std::move(base);
          }
          targets.push_back(pid);
        }
        // Also include dir-matched entries if any
        for (DWORD pid : bydir) targets.push_back(pid);
      } else {
        targets = std::move(bydir);
      }
      // Ensure Playnite itself is not a target
      targets.erase(std::remove_if(targets.begin(), targets.end(), [&](DWORD pid){
        auto itb = snap.exe_basename.find(pid);
        return itb != snap.exe_basename.end() && is_playnite_exe_basename(itb->second);
      }), targets.end());
    }

    // De-duplicate target list
    std::sort(targets.begin(), targets.end());
    targets.erase(std::unique(targets.begin(), targets.end()), targets.end());

    if (targets.empty()) return;

    // Send WM_CLOSE
    for (DWORD pid : targets) send_wm_close_to_pid(pid);

    // Wait up to timeout
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(std::max(0, exit_timeout_secs));
    while (std::chrono::steady_clock::now() < deadline) {
      bool any_alive = false;
      for (DWORD pid : targets) { if (pid_alive(pid)) { any_alive = true; break; } }
      if (!any_alive) return; // all done
      std::this_thread::sleep_for(std::chrono::milliseconds(250));
    }

    // Force remaining
    for (DWORD pid : targets) if (pid_alive(pid)) terminate_pid(pid);
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
  std::string exit_timeout_s;
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
  parse_arg(argspan, "--exit-timeout", exit_timeout_s);
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

  int exit_timeout_secs = 10; // default graceful-exit window for cleanup
  if (!exit_timeout_s.empty()) {
    try { exit_timeout_secs = std::max(0, std::stoi(exit_timeout_s)); } catch (...) {}
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
  // Use append-mode logging to avoid cross-process truncation races with the cleanup watcher
  auto _log_guard = logging::init_append(2 /*info*/, log_path);
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
      cleanup_graceful_then_forceful_in_dir(install_dir_w, exit_timeout_secs);
    }
    if (fullscreen) {
      graceful_shutdown_playnite(true, std::max(3, exit_timeout_secs));
    }
    BOOST_LOG(info) << "Cleanup mode: done";
    return 0;
  }

  // Fullscreen mode: start Playnite.FullscreenApp and apply focus attempts, then schedule cleanup and exit
  if (fullscreen) {
    BOOST_LOG(info) << "Fullscreen mode requested; launching Playnite.FullscreenApp";
    std::wstring fullpath = find_playnite_fullscreen_path();
    if (fullpath.empty()) {
      // Last resort: try the legacy default path
      fullpath = L"C:\\Program Files\\Playnite\\Playnite.FullscreenApp.exe";
    }
    bool launched = false;
    if (!fullpath.empty() && file_exists(fullpath)) {
      // Prefer ShellExecuteW to honor app's own show behavior; ensure it is visible.
      HINSTANCE h = ShellExecuteW(nullptr, L"open", fullpath.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
      if ((INT_PTR)h > 32) {
        launched = true;
        BOOST_LOG(info) << "Launched Playnite.FullscreenApp at '" << platf::dxgi::wide_to_utf8(fullpath) << "'";
      } else {
        BOOST_LOG(warning) << "ShellExecuteW failed for Playnite.FullscreenApp (code=" << (INT_PTR)h << ")";
      }
    } else {
      BOOST_LOG(error) << "Playnite.FullscreenApp.exe not found in common locations";
    }
    if (!launched) {
      // Fallback: CreateProcessW without hiding flags
      STARTUPINFOW si{}; si.cb = sizeof(si);
      PROCESS_INFORMATION pi{};
      std::wstring cmdline = L"\"" + fullpath + L"\"";
      BOOL ok = CreateProcessW(fullpath.c_str(), cmdline.data(), nullptr, nullptr, FALSE,
                               CREATE_UNICODE_ENVIRONMENT | CREATE_NEW_PROCESS_GROUP,
                               nullptr, nullptr, &si, &pi);
      if (ok) {
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        launched = true;
        BOOST_LOG(info) << "CreateProcessW fallback launched Playnite.FullscreenApp";
      } else {
        BOOST_LOG(error) << "Failed to start Playnite.FullscreenApp.exe via CreateProcessW";
      }
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
    // Do not spawn cleanup here; fullscreen UI should remain active until user exits
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
      if (exit_timeout_secs > 0) {
        wcmd += L" --exit-timeout " + std::to_wstring(exit_timeout_secs);
      }
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
  if (exit_timeout_secs > 0) {
    wcmd += L" --exit-timeout " + std::to_wstring(exit_timeout_secs);
  }
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

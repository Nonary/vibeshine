/**
 * @file src/platform/windows/steam_big_picture_debug_port.cpp
 * @brief Steam's debug-port flag file and Steam restarts, for the Big Picture controls' web UI.
 *
 * Steam opens its CEF debug port at start when an empty .cef-enable-remote-debugging file is in its
 * folder. Everything here acts as the console user: the file is created with that user's access
 * checks and Steam is started under that user's token, never as SYSTEM.
 */
#ifdef _WIN32
  #include "steam_big_picture_debug_port.h"

  // misc.h brings in Boost.Asio, which needs winsock2.h before windows.h.
  #include "misc.h"
  #include "src/logging.h"
  #include "src/state_storage.h"
  #include "src/steam_big_picture_controls_policy.h"
  #include "src/utility.h"
  #include "steam_discovery.h"

  #include <tlhelp32.h>

  #include <algorithm>
  #include <array>
  #include <chrono>
  #include <condition_variable>
  #include <cwctype>
  #include <mutex>
  #include <system_error>
  #include <vector>

namespace {
  using namespace std::chrono_literals;
  namespace fs = std::filesystem;
  namespace discovery = platf::steam::windows_discovery;
  namespace policy = platf::steam::big_picture::policy;

  constexpr auto steam_close_timeout = 40s;

  std::string last_error_text() {
    return platf::steam::big_picture::debug_port::error_text({static_cast<int>(GetLastError()), std::system_category()});
  }

  std::optional<DWORD> console_session() {
    const DWORD session = WTSGetActiveConsoleSessionId();
    return session == 0 || session == 0xFFFFFFFF ? std::nullopt : std::optional<DWORD> {session};
  }

  // Processes with this executable name in the given session.
  std::vector<DWORD> session_processes(const wchar_t *executable, DWORD session) {
    std::vector<DWORD> found;
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) {
      return found;
    }
    auto close_snapshot = util::fail_guard([&] {
      CloseHandle(snapshot);
    });
    PROCESSENTRY32W entry {};
    entry.dwSize = sizeof(entry);
    for (BOOL more = Process32FirstW(snapshot, &entry); more; more = Process32NextW(snapshot, &entry)) {
      DWORD process_session = 0;
      if (_wcsicmp(entry.szExeFile, executable) == 0 && ProcessIdToSessionId(entry.th32ProcessID, &process_session) && process_session == session) {
        found.push_back(entry.th32ProcessID);
      }
    }
    return found;
  }

  std::optional<fs::path> process_path(DWORD pid) {
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!process) {
      return std::nullopt;
    }
    auto close_process = util::fail_guard([&] {
      CloseHandle(process);
    });
    std::wstring path(32768, L'\0');
    DWORD length = static_cast<DWORD>(path.size());
    if (!QueryFullProcessImageNameW(process, 0, path.data(), &length)) {
      return std::nullopt;
    }
    path.resize(length);
    return fs::path(path);
  }

  // Steam reads the flag file from the folder it runs from, so a running steam.exe wins over the
  // registered paths.
  std::optional<fs::path> steam_root(const std::vector<DWORD> &steam, const discovery::discovery_scope &scope) {
    for (const auto pid : steam) {
      if (auto path = process_path(pid)) {
        return path->parent_path();
      }
    }
    for (auto root : discovery::registry_roots(scope)) {
      std::error_code error;
      if (fs::is_regular_file(root / L"steam.exe", error)) {
        return root.make_preferred();
      }
    }
    return std::nullopt;
  }

  bool big_picture_open(DWORD session) {
    struct context_t {
      std::vector<DWORD> web_helpers;
      bool found = false;
    } context {session_processes(L"steamwebhelper.exe", session)};
    if (context.web_helpers.empty()) {
      return false;
    }
    EnumWindows(
      [](HWND window, LPARAM parameter) -> BOOL {
        auto &context = *reinterpret_cast<context_t *>(parameter);
        DWORD pid = 0;
        GetWindowThreadProcessId(window, &pid);
        if (!IsWindowVisible(window) || std::find(context.web_helpers.begin(), context.web_helpers.end(), pid) == context.web_helpers.end()) {
          return TRUE;
        }
        // Reads the stored caption without sending a message, so a hung Steam window can't block us.
        std::array<wchar_t, 256> title {};
        const int length = GetWindowTextW(window, title.data(), static_cast<int>(title.size()));
        if (length > 0 && policy::is_big_picture_window_title({title.data(), static_cast<std::size_t>(length)})) {
          context.found = true;
          return FALSE;
        }
        return TRUE;
      },
      reinterpret_cast<LPARAM>(&context)
    );
    return context.found;
  }

  // Start steam.exe as the console user, outside Vibeshine's job so Steam outlives Vibeshine.
  std::optional<std::string> run_steam(const fs::path &root, const std::wstring &arguments) {
    std::wstring command = L"\"" + (root / L"steam.exe").wstring() + L"\"";
    if (!arguments.empty()) {
      command += L" " + arguments;
    }
    std::error_code error;
    auto startup = platf::create_startup_info(nullptr, nullptr, error);
    if (error) {
      return error.message();
    }
    auto free_startup = util::fail_guard([&] {
      platf::free_proc_thread_attr_list(startup.lpAttributeList);
    });
    wchar_t desktop[] = L"winsta0\\default";
    startup.StartupInfo.lpDesktop = desktop;
    PROCESS_INFORMATION child {};
    constexpr DWORD flags = EXTENDED_STARTUPINFO_PRESENT | CREATE_UNICODE_ENVIRONMENT | CREATE_BREAKAWAY_FROM_JOB;
    const bool launched = platf::is_running_as_system() ?
                            platf::launch_process_with_impersonation(false, platf::to_utf8(command), root.wstring(), flags, startup, child, error) :
                            platf::launch_process_without_impersonation(platf::to_utf8(command), root.wstring(), flags, startup, child, error);
    if (!launched) {
      return error ? platf::steam::big_picture::debug_port::error_text(error) : std::string("the process could not be started");
    }
    CloseHandle(child.hThread);
    CloseHandle(child.hProcess);
    return std::nullopt;
  }

  // Wait until no process with this name runs in the session. False on timeout or stop.
  bool wait_for_exit(std::stop_token stop, const wchar_t *executable, DWORD session, std::chrono::steady_clock::time_point deadline) {
    std::mutex mutex;
    std::condition_variable_any wake;
    std::unique_lock lock(mutex);
    while (!session_processes(executable, session).empty()) {
      if (std::chrono::steady_clock::now() >= deadline) {
        return false;
      }
      wake.wait_for(lock, stop, 500ms, [] {
        return false;
      });
      if (stop.stop_requested()) {
        return false;
      }
    }
    return true;
  }
}  // namespace

namespace platf::steam::big_picture::debug_port {
  std::string error_text(const std::error_code &error) {
    if (error.category() == std::system_category()) {
      wchar_t *buffer = nullptr;
      const DWORD length = FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr, static_cast<DWORD>(error.value()), 0, reinterpret_cast<LPWSTR>(&buffer), 0, nullptr);
      if (length == 0 || !buffer) {
        return "Windows error " + std::to_string(error.value());
      }
      std::wstring text(buffer, length);
      LocalFree(buffer);
      while (!text.empty() && std::iswspace(text.back())) {
        text.pop_back();
      }
      return platf::to_utf8(text);
    }
    // Other categories come from the C runtime, in the ANSI code page.
    const auto narrow = error.message();
    const int length = MultiByteToWideChar(CP_ACP, 0, narrow.data(), static_cast<int>(narrow.size()), nullptr, 0);
    std::wstring wide(std::max(length, 0), L'\0');
    MultiByteToWideChar(CP_ACP, 0, narrow.data(), static_cast<int>(narrow.size()), wide.data(), length);
    return platf::to_utf8(wide);
  }

  steam_t inspect() {
    steam_t steam;
    const auto session = console_session();
    if (!session) {
      return steam;
    }
    const auto running = session_processes(L"steam.exe", *session);
    steam.running = !running.empty();
    discovery::discovery_scope scope;
    if (!scope.user_hive()) {
      return steam;
    }
    steam.root = steam_root(running, scope);
    if (steam.root) {
      std::error_code error;
      steam.flag_file = fs::is_regular_file(*steam.root / policy::debug_port_flag_file, error);
    }
    steam.big_picture = steam.running && big_picture_open(*session);
    return steam;
  }

  std::optional<std::string> set_flag_file(bool enabled) {
    const auto session = console_session();
    if (!session) {
      return "No user is signed in on this PC.";
    }
    fs::path path;
    bool created = false;
    {
      // Only the file itself is touched as the console user; the state file below is ours.
      discovery::discovery_scope scope;
      if (!scope.user_hive()) {
        return "No user is signed in on this PC.";
      }
      const auto root = steam_root(session_processes(L"steam.exe", *session), scope);
      if (!root) {
        return "Steam's folder was not found.";
      }
      path = *root / policy::debug_port_flag_file;
      if (enabled) {
        HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE) {
          const auto error = last_error_text();
          return "Could not create " + platf::to_utf8(path.wstring()) + ": " + error;
        }
        // OPEN_ALWAYS reports ERROR_ALREADY_EXISTS when the file was there before us.
        created = GetLastError() != ERROR_ALREADY_EXISTS;
        CloseHandle(file);
      } else if (!DeleteFileW(path.c_str()) && GetLastError() != ERROR_FILE_NOT_FOUND) {
        const auto error = last_error_text();
        return "Could not delete " + platf::to_utf8(path.wstring()) + ": " + error;
      }
    }
    const auto path_text = platf::to_utf8(path.wstring());
    // Remember a file we created so uninstalling Vibeshine removes it. One that was already there
    // belongs to the user or another Steam mod, and is left alone.
    if (created) {
      statefile::save_steam_debug_port_flag(path_text);
    } else if (const auto remembered = statefile::load_steam_debug_port_flag(); !enabled && remembered && _wcsicmp(platf::from_utf8(*remembered).c_str(), path.c_str()) == 0) {
      statefile::save_steam_debug_port_flag(std::nullopt);
    }
    BOOST_LOG(info) << "Steam Big Picture controls: Steam's debug port turned " << (enabled ? "on" : "off") << " (" << path_text << ").";
    return std::nullopt;
  }

  void remove_created_flag_file() {
    const auto remembered = statefile::load_steam_debug_port_flag();
    if (!remembered) {
      return;
    }
    const fs::path path = platf::from_utf8(*remembered);
    // Whatever the state file says, never delete anything but the flag file.
    if (path.filename() != fs::path(policy::debug_port_flag_file)) {
      statefile::save_steam_debug_port_flag(std::nullopt);
      return;
    }
    std::optional<std::string> error;
    {
      discovery::discovery_scope scope;
      if (!scope.user_hive()) {
        BOOST_LOG(warning) << "Steam Big Picture controls: no user is signed in, so Steam's debug-port flag file was left in place (" << *remembered << ").";
        return;
      }
      if (!DeleteFileW(path.c_str())) {
        const DWORD code = GetLastError();
        if (code != ERROR_FILE_NOT_FOUND && code != ERROR_PATH_NOT_FOUND) {
          error = error_text({static_cast<int>(code), std::system_category()});
        }
      }
    }
    if (error) {
      BOOST_LOG(warning) << "Steam Big Picture controls: could not delete Steam's debug-port flag file (" << *remembered << "): " << *error;
      return;
    }
    statefile::save_steam_debug_port_flag(std::nullopt);
    BOOST_LOG(info) << "Steam Big Picture controls: removed the debug-port flag file Vibeshine created (" << *remembered << ").";
  }

  std::optional<std::string> restart_steam(std::stop_token stop, bool big_picture) {
    const auto session = console_session();
    if (!session) {
      return "No user is signed in on this PC.";
    }
    std::optional<fs::path> root;
    {
      discovery::discovery_scope scope;
      if (!scope.user_hive()) {
        return "No user is signed in on this PC.";
      }
      root = steam_root(session_processes(L"steam.exe", *session), scope);
    }
    if (!root) {
      return "Steam's folder was not found.";
    }
    BOOST_LOG(info) << "Steam Big Picture controls: restarting Steam" << (big_picture ? " in Big Picture" : "") << '.';
    if (auto error = run_steam(*root, L"-shutdown")) {
      return "Could not ask Steam to close: " + *error;
    }
    // Steam saves its state before it exits. Its web helpers hold the debug port, so wait for them too.
    const auto deadline = std::chrono::steady_clock::now() + steam_close_timeout;
    if (!wait_for_exit(stop, L"steam.exe", *session, deadline) || !wait_for_exit(stop, L"steamwebhelper.exe", *session, deadline)) {
      return stop.stop_requested() ? "Vibeshine stopped while Steam was closing." : "Steam did not close within 40 seconds.";
    }
    if (auto error = run_steam(*root, big_picture ? L"-gamepadui" : L"")) {
      return "Steam closed but could not be started again: " + *error;
    }
    return std::nullopt;
  }
}  // namespace platf::steam::big_picture::debug_port
#endif

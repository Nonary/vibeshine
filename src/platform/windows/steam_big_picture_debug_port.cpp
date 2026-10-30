/**
 * @file src/platform/windows/steam_big_picture_debug_port.cpp
 * @brief Steam's debug-port flag file and Steam restarts, for the Big Picture controls' web UI.
 *
 * Steam opens its CEF debug port at start when an empty .cef-enable-remote-debugging file is in its
 * folder. Everything here acts as the console user: the file is created with that user's access
 * checks and Steam is started under that user's token, never as SYSTEM. The one exception is an
 * uninstall with nobody signed in, which deletes the file Vibeshine created as Vibeshine, and only in
 * the folder Steam's installer registered.
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
  // How long a restart waits for steam.exe alone, counted from the shutdown request, before giving up.
  constexpr auto steam_exit_timeout = 3min;
  // How much longer a restart waits for Steam to finish closing once Vibeshine is stopping. Vibeshine's
  // shutdown watchdog forces an exit 10 s after shutdown starts, so Steam must be started well before.
  constexpr auto steam_close_grace_on_stop = 5s;

  std::string windows_error_text(DWORD code) {
    return platf::steam::big_picture::debug_port::error_text({static_cast<int>(code), std::system_category()});
  }

  std::string last_error_text() {
    return windows_error_text(GetLastError());
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

  // A steam.exe below medium integrity or in an app container, as one a sandboxed program started
  // would be, could be any program. Restarting Steam starts its folder's steam.exe outside that
  // sandbox, so such a process is never taken as Steam.
  bool trusted_steam_process(HANDLE process) {
    HANDLE token = nullptr;
    if (!OpenProcessToken(process, TOKEN_QUERY, &token)) {
      return false;
    }
    auto close_token = util::fail_guard([&] {
      CloseHandle(token);
    });
    DWORD app_container = 0;
    DWORD bytes = 0;
    if (!GetTokenInformation(token, TokenIsAppContainer, &app_container, sizeof(app_container), &bytes) || app_container != 0) {
      return false;
    }
    GetTokenInformation(token, TokenIntegrityLevel, nullptr, 0, &bytes);
    if (bytes < sizeof(TOKEN_MANDATORY_LABEL) || bytes > 1024) {
      return false;
    }
    std::vector<BYTE> label(bytes);
    if (!GetTokenInformation(token, TokenIntegrityLevel, label.data(), bytes, &bytes)) {
      return false;
    }
    const PSID sid = reinterpret_cast<TOKEN_MANDATORY_LABEL *>(label.data())->Label.Sid;
    if (!IsValidSid(sid) || *GetSidSubAuthorityCount(sid) == 0) {
      return false;
    }
    return *GetSidSubAuthority(sid, *GetSidSubAuthorityCount(sid) - 1) >= SECURITY_MANDATORY_MEDIUM_RID;
  }

  // The image path of a steam.exe process that may be taken as Steam.
  std::optional<fs::path> steam_process_path(DWORD pid) {
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!process) {
      return std::nullopt;
    }
    auto close_process = util::fail_guard([&] {
      CloseHandle(process);
    });
    if (!trusted_steam_process(process)) {
      return std::nullopt;
    }
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
      if (auto path = steam_process_path(pid)) {
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

  // Whether folder is where Steam's installer registered Steam for all users. Unlike the console
  // user's own registration, only an administrator can change it.
  bool registered_steam_folder(const fs::path &folder) {
    for (const auto view : {RRF_SUBKEY_WOW6432KEY, RRF_SUBKEY_WOW6464KEY}) {
      if (const auto root = discovery::detail::registry_path(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Valve\\Steam", L"InstallPath", view); root && policy::same_windows_path(root->wstring(), folder.wstring())) {
        return true;
      }
    }
    return false;
  }

  // The volume and file ID of an open file. Deleting a file and creating it again changes the ID.
  std::optional<std::string> file_id(HANDLE file) {
    FILE_ID_INFO info {};
    if (!GetFileInformationByHandleEx(file, FileIdInfo, &info, sizeof(info))) {
      return std::nullopt;
    }
    constexpr std::string_view digits = "0123456789abcdef";
    std::string text = std::to_string(info.VolumeSerialNumber) + ':';
    for (const auto byte : info.FileId.Identifier) {
      text += digits[byte >> 4];
      text += digits[byte & 0xF];
    }
    return text;
  }

  // Opens a link itself, never its target, so only this name in Steam's folder is ever touched.
  HANDLE open_flag_file(const fs::path &path, DWORD access) {
    return CreateFileW(path.c_str(), access, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
  }

  // The file ID of the file at path, when it exists and can be read.
  std::optional<std::string> file_id(const fs::path &path) {
    HANDLE file = open_flag_file(path, FILE_READ_ATTRIBUTES);
    if (file == INVALID_HANDLE_VALUE) {
      return std::nullopt;
    }
    auto close_file = util::fail_guard([&] {
      CloseHandle(file);
    });
    return file_id(file);
  }

  enum class removal_e {
    deleted,
    missing,
    replaced,  ///< Another file is there now, and was left alone.
    failed,
  };

  // Delete the flag file at path if it is still the one with expected_id (any file, for an empty
  // ID). Runs as the calling thread's user.
  removal_e remove_flag_file(const fs::path &path, const std::string &expected_id, std::string &error) {
    HANDLE file = open_flag_file(path, DELETE | FILE_READ_ATTRIBUTES);
    if (file == INVALID_HANDLE_VALUE) {
      const DWORD code = GetLastError();
      if (code == ERROR_FILE_NOT_FOUND || code == ERROR_PATH_NOT_FOUND) {
        return removal_e::missing;
      }
      error = windows_error_text(code);
      return removal_e::failed;
    }
    auto close_file = util::fail_guard([&] {
      CloseHandle(file);
    });
    // Checked on the handle that deletes it, so the file can't be swapped in between.
    if (!expected_id.empty() && file_id(file) != expected_id) {
      return removal_e::replaced;
    }
    FILE_DISPOSITION_INFO disposition {TRUE};
    if (!SetFileInformationByHandle(file, FileDispositionInfo, &disposition, sizeof(disposition))) {
      error = last_error_text();
      return removal_e::failed;
    }
    return removal_e::deleted;
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
    // Read before acting as the console user: the state file is Vibeshine's.
    const auto remembered = statefile::load_steam_debug_port_flag();
    discovery::discovery_scope scope;
    if (!scope.user_hive()) {
      return steam;
    }
    steam.root = steam_root(running, scope);
    if (steam.root) {
      const auto path = *steam.root / policy::debug_port_flag_file;
      std::error_code error;
      steam.flag_file = fs::is_regular_file(path, error);
      steam.flag_file_created = steam.flag_file && remembered && policy::is_recorded_flag_file(platf::from_utf8(remembered->path), remembered->file_id, path.wstring(), file_id(path));
    }
    steam.big_picture = steam.running && big_picture_open(*session);
    return steam;
  }

  std::optional<std::string> set_flag_file(bool enabled) {
    const auto session = console_session();
    if (!session) {
      return "No user is signed in on this PC.";
    }
    // The state file is Vibeshine's, so it is read and written outside the console user's scope.
    const auto remembered = statefile::load_steam_debug_port_flag();
    fs::path path;
    std::optional<std::string> created;  // File ID of a file this call created; empty if unknown.
    std::optional<removal_e> removal;
    bool forget = false;  // The record names a file that is gone or was replaced.
    bool left_alone = false;  // A file Vibeshine didn't create stays.
    {
      // Only the file itself is touched as the console user.
      discovery::discovery_scope scope;
      if (!scope.user_hive()) {
        return "No user is signed in on this PC.";
      }
      const auto root = steam_root(session_processes(L"steam.exe", *session), scope);
      if (!root) {
        return "Steam's folder was not found.";
      }
      path = *root / policy::debug_port_flag_file;
      const bool remembered_path = remembered && policy::same_windows_path(platf::from_utf8(remembered->path), path.wstring());
      if (enabled) {
        HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE | FILE_READ_ATTRIBUTES, 0, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE) {
          const auto error = last_error_text();
          return "Could not create " + platf::to_utf8(path.wstring()) + ": " + error;
        }
        // OPEN_ALWAYS reports ERROR_ALREADY_EXISTS when the file was there before us.
        const bool existed = GetLastError() == ERROR_ALREADY_EXISTS;
        const auto id = file_id(file);
        CloseHandle(file);
        if (!existed) {
          created = id.value_or(std::string {});
        } else if (remembered_path && !policy::is_recorded_flag_file(platf::from_utf8(remembered->path), remembered->file_id, path.wstring(), id)) {
          forget = true;
        }
      } else if (remembered_path) {
        std::string error;
        removal = remove_flag_file(path, remembered->file_id, error);
        if (*removal == removal_e::failed) {
          return "Could not delete " + platf::to_utf8(path.wstring()) + ": " + error;
        }
        forget = true;
      } else {
        // The user or another Steam mod added it, so it stays even when the controls are turned off.
        std::error_code error;
        left_alone = fs::exists(path, error);
      }
    }
    const auto path_text = platf::to_utf8(path.wstring());
    if (created && !statefile::save_steam_debug_port_flag(statefile::steam_debug_port_flag_t {path_text, *created})) {
      // Without the record the file would look like another program's, and neither turning the
      // controls off nor uninstalling would remove it. Take it back instead.
      std::string failure;
      std::optional<removal_e> undone;
      {
        discovery::discovery_scope scope;
        if (scope.user_hive()) {
          undone = remove_flag_file(path, *created, failure);
        }
      }
      BOOST_LOG(error) << "Steam Big Picture controls: could not record Steam's debug-port flag file in Vibeshine's state file, so it was "
                       << (undone == removal_e::deleted ? "removed again" : "left in place: " + (failure.empty() ? std::string("nobody is signed in") : failure)) << " (" << path_text << ").";
      if (undone == removal_e::deleted) {
        return "Vibeshine could not save which flag file it created, so it removed the file again and Steam's debug port stays off. Vibeshine's log has the details.";
      }
      return "Vibeshine could not save which flag file it created, and could not remove it again. Delete " + path_text + " to keep Steam's debug port off.";
    }
    if (forget && !statefile::save_steam_debug_port_flag(std::nullopt)) {
      BOOST_LOG(warning) << "Steam Big Picture controls: could not forget the debug-port flag file in Vibeshine's state file.";
    }
    if (removal == removal_e::replaced) {
      BOOST_LOG(info) << "Steam Big Picture controls: another program replaced the debug-port flag file Vibeshine created, so it was left in place (" << path_text << ").";
    } else if (left_alone) {
      BOOST_LOG(info) << "Steam Big Picture controls: Vibeshine didn't create the debug-port flag file, so it was left in place (" << path_text << ").";
    } else {
      BOOST_LOG(info) << "Steam Big Picture controls: Steam's debug port turned " << (enabled ? "on" : "off") << " (" << path_text << ").";
    }
    return std::nullopt;
  }

  void remove_created_flag_file() {
    const auto remembered = statefile::load_steam_debug_port_flag();
    if (!remembered) {
      return;
    }
    const fs::path path = platf::from_utf8(remembered->path);
    // Whatever the state file says, never delete anything but the flag file.
    if (path.filename() != fs::path(policy::debug_port_flag_file)) {
      statefile::save_steam_debug_port_flag(std::nullopt);
      return;
    }
    std::string error;
    std::optional<removal_e> removal;
    {
      discovery::discovery_scope scope;
      if (scope.user_hive()) {
        removal = remove_flag_file(path, remembered->file_id, error);
      }
    }
    if (!removal) {
      // Nobody is signed in, as in a remote uninstall. Left behind, the file would open Steam's debug
      // port at every Steam start after Vibeshine is gone, and factory reset deletes this record. So
      // it is deleted as Vibeshine, but only in the folder Steam's installer registered, and only the
      // very file Vibeshine created when its ID was recorded.
      if (!registered_steam_folder(path.parent_path())) {
        BOOST_LOG(warning) << "Steam Big Picture controls: no user is signed in and the debug-port flag file isn't in Steam's registered folder, so it was left in place (" << remembered->path << ").";
        return;
      }
      removal = remove_flag_file(path, remembered->file_id, error);
    }
    switch (*removal) {
      case removal_e::failed:
        BOOST_LOG(warning) << "Steam Big Picture controls: could not delete Steam's debug-port flag file (" << remembered->path << "): " << error;
        return;
      case removal_e::replaced:
        BOOST_LOG(info) << "Steam Big Picture controls: another program replaced the debug-port flag file Vibeshine created, so it was left in place (" << remembered->path << ").";
        break;
      case removal_e::deleted:
        BOOST_LOG(info) << "Steam Big Picture controls: removed the debug-port flag file Vibeshine created (" << remembered->path << ").";
        break;
      case removal_e::missing:
        break;
    }
    statefile::save_steam_debug_port_flag(std::nullopt);
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
    const auto closed = [&](std::stop_token token, std::chrono::steady_clock::time_point deadline) {
      return wait_for_exit(token, L"steam.exe", *session, deadline) && wait_for_exit(token, L"steamwebhelper.exe", *session, deadline);
    };
    const auto requested = std::chrono::steady_clock::now();
    const auto deadline = requested + steam_close_timeout;
    if (!closed(stop, deadline)) {
      if (!stop.stop_requested()) {
        // The shutdown request can still complete after the initial deadline. Starting Steam while
        // its old process is closing can hand the launch back to that process and leave it closed.
        // Keep waiting for steam.exe, but don't let a lingering web helper prevent the restart.
        BOOST_LOG(warning) << "Steam Big Picture controls: Steam did not finish closing within 40 seconds; waiting up to 3 minutes for Steam to exit before starting it again.";
        if (!wait_for_exit(stop, L"steam.exe", *session, requested + steam_exit_timeout) && !stop.stop_requested()) {
          return "Steam did not close within 3 minutes.";
        }
      }
      if (stop.stop_requested()) {
        // Steam was already asked to close, so it must not be left closed just because Vibeshine is
        // stopping. Give it a little longer, then start it either way.
        BOOST_LOG(info) << "Steam Big Picture controls: Vibeshine is stopping while Steam closes; starting Steam again before exiting.";
        wait_for_exit({}, L"steam.exe", *session, std::chrono::steady_clock::now() + steam_close_grace_on_stop);
      }
    }
    if (auto error = run_steam(*root, big_picture ? L"-gamepadui" : L"")) {
      return "Steam closed but could not be started again: " + *error;
    }
    return std::nullopt;
  }
}  // namespace platf::steam::big_picture::debug_port
#endif

/**
 * @file src/platform/windows/steam_big_picture_controls.cpp
 * @brief Supervises the Steam UI helper and runs the actions it relays from Steam Big Picture.
 *
 * The helper (tools/sunshine_steam_ui_helper.exe) runs as the console user and talks to Steam. This
 * module launches it over a SID-restricted pipe and verifies it like the end-stream hotkey helper,
 * pushes the stream status built from non-blocking snapshots, and runs End Stream / Stop Streaming
 * on its own thread. See src/steam_big_picture_controls_protocol.h for the messages.
 */
#ifdef _WIN32
  #include "steam_big_picture_controls.h"

  #include "end_stream_hotkey_protocol.h"
  #include "misc.h"
  #include "src/logging.h"
  #include "src/nvhttp.h"
  #include "src/process.h"
  #include "src/rtsp.h"
  #include "src/steam_big_picture_controls_policy.h"
  #include "src/steam_big_picture_controls_protocol.h"
  #include "src/stream.h"
  #include "src/utility.h"
  #include "src/webrtc_stream.h"
  #include "steam_big_picture_debug_port.h"
  #include "steam_big_picture_pipe.h"

  #include <algorithm>
  #include <array>
  #include <atomic>
  #include <condition_variable>
  #include <deque>
  #include <filesystem>
  #include <map>
  #include <mutex>
  #include <objbase.h>
  #include <sddl.h>
  #include <set>
  #include <stop_token>
  #include <thread>
  #include <vector>
  #include <wtsapi32.h>

namespace {
  using namespace std::chrono_literals;
  using json = nlohmann::json;
  namespace policy = platf::steam::big_picture::policy;
  namespace protocol = platf::steam::big_picture::protocol;

  constexpr auto status_fallback_interval = 5s;  // A game that exits by itself is only seen by polling.
  constexpr auto status_check_interval = 250ms;

  struct configuration_t {
    bool enabled = false;
    std::uint64_t generation = 0;
  };

  struct helper_state_t {
    std::string state = "off";
    std::string detail;
    json features = json::object();
    std::string steam_build;
    int script_version = 0;
    int script_errors = 0;
    std::string last_error;
    bool connected = false;
  };

  struct action_request_t {
    policy::action_t action;
    policy::client_kind_e kind;
    std::uint64_t generation;
    std::string client_name;  // For the log.
  };

  struct restart_state_t {
    std::string state = "idle";  // idle, restarting, done or failed
    std::string detail;
    std::optional<std::chrono::steady_clock::time_point> big_picture_restarted;  // Last reopened in Big Picture.
  };

  struct manager_t {
    std::mutex mutex;
    std::condition_variable_any changed;
    configuration_t config;
    helper_state_t helper;
    std::vector<policy::client_t> clients;  // From the last status snapshot, to validate actions.
    policy::action_gate_t gate;
    std::deque<action_request_t> actions;
    std::uint64_t next_check_id = 1;
    std::vector<std::uint64_t> pending_checks;  // Not sent to the helper yet.
    std::set<std::uint64_t> waiting_checks;  // A check() call is still waiting for these.
    std::map<std::uint64_t, json> check_results;
    restart_state_t restart;
    bool stopping = false;  // stop() has begun: no new Steam restarts.
    std::jthread worker;
    std::jthread action_worker;
    std::jthread restart_worker;
  };

  manager_t &state() {
    static manager_t instance;
    return instance;
  }

  // Set by lifecycle hooks, which may hold lifecycle locks: only a flag, read by the supervisor.
  std::atomic_bool status_dirty {true};

  configuration_t snapshot_config() {
    auto &s = state();
    std::lock_guard lock(s.mutex);
    return s.config;
  }

  void pause(std::stop_token stop, std::uint64_t generation, std::chrono::milliseconds duration) {
    auto &s = state();
    std::unique_lock lock(s.mutex);
    s.changed.wait_for(lock, stop, duration, [&] {
      return s.config.generation != generation;
    });
  }

  bool current(std::stop_token stop, const configuration_t &config, DWORD session) {
    const auto now = snapshot_config();
    return !stop.stop_requested() && now.enabled && now.generation == config.generation && WTSGetActiveConsoleSessionId() == session;
  }

  void set_helper(std::string helper_state, std::string detail = {}, std::optional<bool> connected = std::nullopt) {
    auto &s = state();
    std::lock_guard lock(s.mutex);
    s.helper.state = std::move(helper_state);
    s.helper.detail = std::move(detail);
    if (connected) {
      s.helper.connected = *connected;
      s.changed.notify_all();  // A check() waiting on a helper that went away can stop waiting.
    }
  }

  // The helper couldn't be started or connected: show why on the Integrations page. Logged only when
  // the reason changes, since the supervisor retries every few seconds.
  void report_failure(std::string helper_state, std::string detail) {
    auto &s = state();
    std::lock_guard lock(s.mutex);
    if (s.helper.state != helper_state || s.helper.detail != detail) {
      BOOST_LOG(warning) << "Steam Big Picture controls: " << detail;
    }
    s.helper.state = std::move(helper_state);
    s.helper.detail = std::move(detail);
  }

  // "<what> failed: <Windows error text>", for the last error of the calling thread.
  std::string api_failure(std::string_view what, DWORD code = GetLastError()) {
    return std::string(what) + " failed: " + platf::steam::big_picture::debug_port::error_text({static_cast<int>(code), std::system_category()});
  }

  // ---------------------------------------------------------------------------------------------
  // Status, built from snapshots that never take part in a session's lifecycle.
  // ---------------------------------------------------------------------------------------------
  policy::snapshot_t take_snapshot(std::string &app_name) {
    policy::snapshot_t snapshot;
    snapshot.app_running = proc::proc.current_app_id() > 0;
    if (!snapshot.app_running) {
      app_name.clear();
    } else if (const auto app = proc::proc.active_app_snapshot()) {
      // Under contention the snapshot is empty; keep the last name rather than wait.
      app_name = app->name;
    }
    snapshot.app_name = app_name;

    const auto names = nvhttp::paired_client_names();
    for (const auto &session : rtsp_stream::get_sessions_snapshot()) {
      if (!session || stream::session::state(*session) != stream::session::state_e::RUNNING) {
        continue;
      }
      auto id = stream::session::uuid(*session);
      if (id.empty() || std::any_of(snapshot.clients.begin(), snapshot.clients.end(), [&](const auto &client) {
            return client.id == id;
          })) {
        continue;
      }
      const auto name = names.find(id);
      snapshot.clients.push_back({std::move(id), name != names.end() && !name->second.empty() ? name->second : "Moonlight client", policy::client_kind_e::moonlight});
    }
    for (const auto &info : stream::get_all_session_info()) {
      snapshot.streams.push_back({info.width, info.height, info.fps, std::string(policy::codec_name(info.video_format)), info.hdr});
    }
    for (const auto &browser : webrtc_stream::list_sessions()) {
      snapshot.clients.push_back({browser.id, browser.client_name.value_or("Browser"), policy::client_kind_e::browser});
      if (browser.width && browser.height) {
        snapshot.streams.push_back({*browser.width, *browser.height, browser.fps.value_or(0), browser.codec.value_or(""), browser.hdr.value_or(false)});
      }
    }
    return snapshot;
  }

  // ---------------------------------------------------------------------------------------------
  // Messages from the helper
  // ---------------------------------------------------------------------------------------------
  void queue_action(const json &message, std::uint64_t generation) {
    const auto action = policy::parse_action(message);
    if (!action) {
      BOOST_LOG(warning) << "Steam Big Picture controls: ignored a malformed action from Steam.";
      return;
    }
    auto &s = state();
    std::lock_guard lock(s.mutex);
    auto kind = policy::client_kind_e::moonlight;
    switch (s.gate.check(*action, s.clients, std::chrono::steady_clock::now(), kind)) {
      case policy::action_gate_t::verdict_e::too_soon:
        BOOST_LOG(info) << "Steam Big Picture controls: ignored an action that followed another within two seconds.";
        return;
      case policy::action_gate_t::verdict_e::not_connected:
        BOOST_LOG(warning) << "Steam Big Picture controls: ignored Stop Streaming for a client that is not connected.";
        return;
      case policy::action_gate_t::verdict_e::allowed:
        break;
    }
    const auto client = std::find_if(s.clients.begin(), s.clients.end(), [&](const auto &candidate) {
      return candidate.id == action->client;
    });
    s.actions.push_back({*action, kind, generation, client != s.clients.end() ? client->name : std::string {}});
    s.changed.notify_all();
  }

  void handle_helper_message(const json &message, std::uint64_t generation) {
    auto &s = state();
    const auto type = message.value("type", std::string {});
    if (type == protocol::message::action) {
      queue_action(message, generation);
      return;
    }
    if (type == protocol::message::log) {
      const auto level = message.value("level", std::string {});
      const auto text = message.value("message", std::string {});
      if (level == "error") {
        BOOST_LOG(error) << "Steam Big Picture controls: " << text;
      } else if (level == "warning") {
        BOOST_LOG(warning) << "Steam Big Picture controls: " << text;
      } else {
        BOOST_LOG(info) << "Steam Big Picture controls: " << text;
      }
      return;
    }
    std::lock_guard lock(s.mutex);
    if (type == protocol::message::state) {
      s.helper.state = message.value("steam", std::string {});
      s.helper.detail = message.value("detail", std::string {});
      if (const auto text = policy::steam_state_log(s.helper.state); !text.empty()) {
        BOOST_LOG(info) << "Steam Big Picture controls: " << text;
      }
    } else if (type == protocol::message::report) {
      s.helper.features = message.value("features", json::object());
      s.helper.steam_build = message.value("steam_build", std::string {});
      s.helper.script_version = message.value("script_version", 0);
      s.helper.script_errors = message.value("errors", 0);
      BOOST_LOG(info) << "Steam Big Picture controls: injected into Steam build " << s.helper.steam_build << " (" << policy::features_summary(s.helper.features) << ").";
    } else if (type == protocol::message::feature) {
      s.helper.features[message.value("name", std::string {"?"})] = message.value("value", std::string {});
      BOOST_LOG(info) << "Steam Big Picture controls: " << message.value("name", std::string {}) << ": " << message.value("value", std::string {});
    } else if (type == protocol::message::error) {
      ++s.helper.script_errors;
      s.helper.last_error = message.value("where", std::string {}) + ": " + message.value("message", std::string {});
      BOOST_LOG(warning) << "Steam Big Picture controls: error inside Steam in " << s.helper.last_error;
    } else if (type == protocol::message::disabled) {
      s.helper.state = "script_disabled";
      s.helper.detail = message.value("reason", std::string {});
      BOOST_LOG(warning) << "Steam Big Picture controls: the script inside Steam switched itself off (" << s.helper.detail << ")";
    } else if (type == protocol::message::check_result) {
      // Keep only answers someone still waits for; a late one would otherwise stay forever.
      if (const auto id = message.value("id", std::uint64_t {0}); s.waiting_checks.contains(id)) {
        s.check_results[id] = message;
        s.changed.notify_all();
      }
    }
  }

  // ---------------------------------------------------------------------------------------------
  // Helper process and pipe
  // ---------------------------------------------------------------------------------------------
  enum class helper_run_e {
    no_user,  ///< No one is signed in yet (the sign-in screen), so there was no one to run it as.
    failed,  ///< It never connected; report_failure() said why, unless it was turned off or stopped.
    connected,  ///< It connected, then stopped.
  };

  // Runs one helper until it exits, the pipe breaks, or the configuration or console session changes.
  helper_run_e run_helper(std::stop_token stop, const configuration_t &config, DWORD session) {
    // Same identity rules as the end-stream hotkey helper: the console user's limited token when
    // running as SYSTEM, otherwise our own.
    HANDLE token = nullptr;
    const bool system = platf::is_running_as_system();
    if (system) {
      token = platf::retrieve_users_token(false);
    } else if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) {
      token = nullptr;
    }
    if (!token) {
      set_helper("no_console_user");
      return helper_run_e::no_user;
    }
    auto close_token = util::fail_guard([&] {
      CloseHandle(token);
    });
    DWORD token_session = 0, needed = 0;
    if (!GetTokenInformation(token, TokenSessionId, &token_session, sizeof(token_session), &needed)) {
      report_failure("helper_failed", api_failure("Reading the signed-in user's session"));
      return helper_run_e::failed;
    }
    if (token_session != session) {
      if (system) {
        report_failure("helper_failed", "The signed-in user changed while the helper was starting.");
      } else {
        // The helper must run where Steam is on screen, and it can only run as us.
        report_failure("wrong_session", "Vibeshine is running in Windows session " + std::to_string(token_session) + ", not in the console session " + std::to_string(session) + " (for example over Remote Desktop). The Big Picture controls need Vibeshine running as its service, or signed in at the PC itself.");
      }
      return helper_run_e::failed;
    }
    GetTokenInformation(token, TokenUser, nullptr, 0, &needed);
    std::vector<std::uint8_t> token_user(needed);
    if (!GetTokenInformation(token, TokenUser, token_user.data(), needed, &needed)) {
      report_failure("helper_failed", api_failure("Reading the signed-in user"));
      return helper_run_e::failed;
    }
    LPWSTR sid_text = nullptr;
    if (!ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER *>(token_user.data())->User.Sid, &sid_text)) {
      report_failure("helper_failed", api_failure("Reading the signed-in user's SID"));
      return helper_run_e::failed;
    }
    auto free_sid = util::fail_guard([&] {
      LocalFree(sid_text);
    });
    // SYSTEM and the console user only; the helper reads status and writes messages.
    const std::wstring sddl = L"D:P(A;;GA;;;SY)(A;;GRGW;;;" + std::wstring(sid_text) + L")S:(ML;;NW;;;ME)";
    PSECURITY_DESCRIPTOR security = nullptr;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(), SDDL_REVISION_1, &security, nullptr)) {
      report_failure("helper_failed", api_failure("Building the helper pipe's permissions"));
      return helper_run_e::failed;
    }
    auto free_security = util::fail_guard([&] {
      LocalFree(security);
    });
    SECURITY_ATTRIBUTES attributes {sizeof(attributes), security, FALSE};
    GUID guid {};
    wchar_t guid_text[40] {};
    if (FAILED(CoCreateGuid(&guid)) || StringFromGUID2(guid, guid_text, 40) == 0) {
      report_failure("helper_failed", "Could not create a name for the helper's pipe.");
      return helper_run_e::failed;
    }
    const std::wstring pipe_name = L"\\\\.\\pipe\\vibeshine_steam_ui_" + std::wstring(guid_text);
    HANDLE pipe = CreateNamedPipeW(pipe_name.c_str(), PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED | FILE_FLAG_FIRST_PIPE_INSTANCE, PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS, 1, 64 * 1024, 64 * 1024, 0, &attributes);
    if (pipe == INVALID_HANDLE_VALUE) {
      report_failure("helper_failed", api_failure("Creating the helper's pipe"));
      return helper_run_e::failed;
    }
    auto close_pipe = util::fail_guard([&] {
      CancelIoEx(pipe, nullptr);
      CloseHandle(pipe);
    });
    HANDLE event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!event) {
      report_failure("helper_failed", api_failure("Creating an event for the helper's pipe"));
      return helper_run_e::failed;
    }
    auto close_event = util::fail_guard([&] {
      CloseHandle(event);
    });
    OVERLAPPED connection {};
    connection.hEvent = event;
    const bool immediate = ConnectNamedPipe(pipe, &connection) != FALSE;
    const DWORD connection_error = immediate ? ERROR_SUCCESS : GetLastError();
    if (!immediate && connection_error != ERROR_IO_PENDING && connection_error != ERROR_PIPE_CONNECTED) {
      report_failure("helper_failed", api_failure("Opening the helper's pipe", connection_error));
      return helper_run_e::failed;
    }
    auto cancel_connection = util::fail_guard([&] {
      if (connection_error == ERROR_IO_PENDING) {
        CancelIoEx(pipe, &connection);
        DWORD ignored = 0;
        GetOverlappedResult(pipe, &connection, &ignored, TRUE);
      }
    });

    std::array<wchar_t, 32768> executable {};
    const DWORD length = GetModuleFileNameW(nullptr, executable.data(), static_cast<DWORD>(executable.size()));
    if (length == 0 || length >= executable.size()) {
      report_failure("helper_failed", api_failure("Finding Vibeshine's folder"));
      return helper_run_e::failed;
    }
    const auto directory = std::filesystem::path(executable.data()).parent_path();
    const auto helper = directory / L"tools" / L"sunshine_steam_ui_helper.exe";
    const std::wstring command = L"\"" + helper.wstring() + L"\" --pipe \"" + pipe_name + L"\" --host-pid " + std::to_wstring(GetCurrentProcessId());
    std::error_code error;
    auto startup = platf::create_startup_info(nullptr, nullptr, error);
    if (error) {
      report_failure("helper_failed", "Preparing to start the helper failed: " + platf::steam::big_picture::debug_port::error_text(error));
      return helper_run_e::failed;
    }
    auto free_startup = util::fail_guard([&] {
      platf::free_proc_thread_attr_list(startup.lpAttributeList);
    });
    wchar_t desktop[] = L"winsta0\\default";
    startup.StartupInfo.lpDesktop = desktop;
    PROCESS_INFORMATION child {};
    constexpr DWORD flags = EXTENDED_STARTUPINFO_PRESENT | CREATE_UNICODE_ENVIRONMENT | CREATE_NO_WINDOW;
    const bool launched = system ?
                            platf::launch_process_with_impersonation(false, platf::to_utf8(command), directory.wstring(), flags, startup, child, error) :
                            platf::launch_process_without_impersonation(platf::to_utf8(command), directory.wstring(), flags, startup, child, error);
    if (!launched) {
      report_failure("helper_failed", "The helper could not be started: " + platf::steam::big_picture::debug_port::error_text(error));
      return helper_run_e::failed;
    }
    CloseHandle(child.hThread);
    auto retire_child = util::fail_guard([&] {
      // Closing the pipe tells the helper to remove its patches from Steam and exit; give it time to,
      // then retire only the exact process we launched.
      DisconnectNamedPipe(pipe);
      if (WaitForSingleObject(child.hProcess, 3000) == WAIT_TIMEOUT) {
        TerminateProcess(child.hProcess, 0);
        WaitForSingleObject(child.hProcess, 1500);
      }
      CloseHandle(child.hProcess);
    });
    DWORD child_session = 0;
    if (!ProcessIdToSessionId(child.dwProcessId, &child_session) || child_session != session) {
      report_failure("helper_failed", "The helper started outside the console session " + std::to_string(session) + '.');
      return helper_run_e::failed;
    }
    const auto connect_deadline = std::chrono::steady_clock::now() + 5s;
    if (connection_error == ERROR_IO_PENDING) {
      while (WaitForSingleObject(event, 100) == WAIT_TIMEOUT) {
        if (!current(stop, config, session)) {
          return helper_run_e::failed;  // Turned off, reconfigured or stopping: not a failure.
        }
        if (DWORD code = 0; WaitForSingleObject(child.hProcess, 0) != WAIT_TIMEOUT && GetExitCodeProcess(child.hProcess, &code)) {
          report_failure("helper_failed", "The helper exited before it connected (exit code " + std::to_string(code) + ").");
          return helper_run_e::failed;
        }
        if (std::chrono::steady_clock::now() >= connect_deadline) {
          report_failure("helper_failed", "The helper didn't connect within 5 seconds.");
          return helper_run_e::failed;
        }
      }
      DWORD ignored = 0;
      if (!GetOverlappedResult(pipe, &connection, &ignored, FALSE)) {
        report_failure("helper_failed", api_failure("Connecting to the helper"));
        return helper_run_e::failed;
      }
    }
    cancel_connection.disable();
    ULONG client_pid = 0, client_session = 0;
    if (!GetNamedPipeClientProcessId(pipe, &client_pid) || !GetNamedPipeClientSessionId(pipe, &client_session) ||
        !platf::hotkey::end_stream::authorized_sender(child.dwProcessId, client_pid, session, client_session, WTSGetActiveConsoleSessionId())) {
      report_failure("helper_failed", "Refused a pipe client that isn't the helper Vibeshine started.");
      return helper_run_e::failed;
    }

    platf::steam::big_picture::framed_pipe_t channel(pipe);
    std::optional<json> hello;
    while (!hello && !channel.closed() && std::chrono::steady_clock::now() < connect_deadline + 5s) {
      if (auto message = channel.read(250ms); message && message->value("type", std::string {}) == protocol::message::hello) {
        hello = std::move(message);
      }
    }
    if (!hello || hello->value("protocol", 0) != protocol::version || hello->value("pid", 0UL) != child.dwProcessId) {
      report_failure("helper_failed", "The helper didn't introduce itself correctly, so it was stopped.");
      return helper_run_e::failed;
    }
    const auto script = std::filesystem::absolute(std::filesystem::path(SUNSHINE_ASSETS_DIR) / "steam" / "big_picture_controls.js");
    if (!channel.write({
          {"type", protocol::message::config},
          {"protocol", protocol::version},
          {"script", platf::to_utf8(script.wstring())},
          {"features", {{"power_menu", true}, {"header_icon", true}}},
          {"inject_options", json::object()},
        })) {
      report_failure("helper_failed", "Could not send the helper its configuration.");
      return helper_run_e::failed;
    }
    set_helper("connecting", {}, true);
    BOOST_LOG(info) << "Steam Big Picture controls: helper started in console session " << session << '.';

    auto unmark_connected = util::fail_guard([&] {
      std::string detail;
      if (DWORD code = 0; WaitForSingleObject(child.hProcess, 0) == WAIT_OBJECT_0 && GetExitCodeProcess(child.hProcess, &code)) {
        detail = "The helper exited (exit code " + std::to_string(code) + ").";
      }
      set_helper("helper_stopped", std::move(detail), false);
    });
    std::string app_name;
    std::optional<json> last_sent;
    auto next_status = std::chrono::steady_clock::time_point {};
    while (current(stop, config, session) && WaitForSingleObject(child.hProcess, 0) == WAIT_TIMEOUT && !channel.closed()) {
      const auto now = std::chrono::steady_clock::now();
      if (status_dirty.exchange(false) || now >= next_status) {
        next_status = now + status_fallback_interval;
        try {
          auto snapshot = take_snapshot(app_name);
          const auto payload = policy::status_payload(snapshot, {}, {});
          {
            auto &s = state();
            std::lock_guard lock(s.mutex);
            s.clients = std::move(snapshot.clients);
          }
          if (payload != last_sent) {
            channel.write({{"type", protocol::message::status}, {"status", payload}});
            last_sent = payload;
          }
        } catch (const std::exception &failure) {
          BOOST_LOG(warning) << "Steam Big Picture controls: could not read the stream status: " << failure.what();
        }
      }
      std::vector<std::uint64_t> checks;
      {
        auto &s = state();
        std::lock_guard lock(s.mutex);
        checks.swap(s.pending_checks);
      }
      for (const auto id : checks) {
        channel.write({{"type", protocol::message::check}, {"id", id}});
      }
      if (auto message = channel.read(status_check_interval)) {
        try {
          handle_helper_message(*message, config.generation);
        } catch (const std::exception &failure) {
          BOOST_LOG(warning) << "Steam Big Picture controls: ignored a message from the helper: " << failure.what();
        }
      }
    }
    return helper_run_e::connected;
  }

  void supervise(std::stop_token stop) {
    auto backoff = std::chrono::milliseconds(5s);
    while (!stop.stop_requested()) {
      const auto config = snapshot_config();
      if (!config.enabled) {
        set_helper("off", {}, false);
        pause(stop, config.generation, 1h);
        continue;
      }
      const DWORD session = WTSGetActiveConsoleSessionId();
      if (session == 0 || session == 0xFFFFFFFF) {
        set_helper("no_console_user", {}, false);
        pause(stop, config.generation, 2s);
        continue;
      }
      const auto started = std::chrono::steady_clock::now();
      auto result = helper_run_e::failed;
      try {
        result = run_helper(stop, config, session);
      } catch (const std::exception &failure) {
        BOOST_LOG(error) << "Steam Big Picture controls: helper failed: " << failure.what();
        // The exception text may not be UTF-8, so the page gets a fixed message.
        set_helper("helper_failed", "The helper failed unexpectedly; the log has the details.");
      }
      if (!current(stop, config, session)) {
        continue;  // Turned off, reconfigured or stopping: act on that right away.
      }
      if (result == helper_run_e::no_user) {
        // The sign-in screen: check again soon, like for a missing console session, so the controls
        // start right after someone signs in instead of after a grown backoff.
        backoff = 5s;
        pause(stop, config.generation, 2s);
        continue;
      }
      const bool connected = result == helper_run_e::connected;
      // Restart a helper that exits, backing off while it keeps exiting quickly.
      if (std::chrono::steady_clock::now() - started > 60s) {
        backoff = 5s;
      }
      // A helper that never connected already logged why; don't repeat it on every retry.
      BOOST_LOG(connected ? warning : debug) << "Steam Big Picture controls: helper stopped; restarting in " << std::chrono::duration_cast<std::chrono::seconds>(backoff).count() << " s.";
      pause(stop, config.generation, backoff);
      backoff = std::min<std::chrono::milliseconds>(backoff * 2, 60s);
    }
  }

  // ---------------------------------------------------------------------------------------------
  // Actions run here, never on the pipe thread or inside a lifecycle hook: they block while streams
  // drain and must not hold an activity guard.
  // ---------------------------------------------------------------------------------------------
  void run_actions(std::stop_token stop) {
    auto &s = state();
    for (;;) {
      action_request_t request;
      {
        std::unique_lock lock(s.mutex);
        if (!s.changed.wait(lock, stop, [&] {
              return !s.actions.empty();
            })) {
          return;
        }
        request = s.actions.front();
        s.actions.pop_front();
      }
      try {
        if (request.action.kind == policy::action_t::kind_e::end_stream) {
          // Shares the end-stream hotkey's path, so its display cleanup logs reason=end_stream_hotkey.
          BOOST_LOG(info) << "Steam Big Picture controls: End Stream selected in Steam.";
          nvhttp::terminate_all_streams_from_host([&] {
            const auto config = snapshot_config();
            return !stop.stop_requested() && config.enabled && config.generation == request.generation;
          });
        } else {
          BOOST_LOG(info) << "Steam Big Picture controls: Stop Streaming selected for " << (request.client_name.empty() ? "a client" : request.client_name)
                          << (request.kind == policy::client_kind_e::browser ? " (browser stream " : " (") << request.action.client << ").";
          if (request.kind == policy::client_kind_e::browser) {
            webrtc_stream::close_session(request.action.client);
          } else {
            nvhttp::disconnect_client(request.action.client);
          }
        }
      } catch (const std::exception &failure) {
        BOOST_LOG(error) << "Steam Big Picture controls: action failed: " << failure.what();
      }
      platf::steam::big_picture::notify_changed();
    }
  }

  // ---------------------------------------------------------------------------------------------
  // Steam restarts for the web UI's debug-port switch, on their own thread: closing Steam takes a while.
  // ---------------------------------------------------------------------------------------------
  bool streaming_now() {
    return proc::proc.current_app_id() > 0 || rtsp_stream::session_count_no_cleanup() > 0 || webrtc_stream::active_session_count() > 0;
  }

  // Call with the module lock held.
  bool restart_running() {
    return state().restart.state == "restarting";
  }

  bool start_restart(bool big_picture) {
    auto &s = state();
    std::lock_guard lock(s.mutex);
    if (s.stopping || restart_running()) {
      return false;
    }
    if (s.restart_worker.joinable()) {
      s.restart_worker.join();  // Finished: it sets the final state as its last step.
    }
    s.restart.state = "restarting";
    s.restart.detail.clear();
    s.restart_worker = std::jthread([big_picture](std::stop_token stop) {
      std::optional<std::string> error;
      try {
        error = platf::steam::big_picture::debug_port::restart_steam(stop, big_picture);
      } catch (const std::exception &failure) {
        // Like the other workers: an exception escaping a thread would end Vibeshine.
        error = std::string("unexpected error: ") + failure.what();
      }
      if (error) {
        BOOST_LOG(warning) << "Steam Big Picture controls: Steam restart failed: " << *error;
      }
      auto &s = state();
      std::lock_guard lock(s.mutex);
      s.restart.state = error ? "failed" : "done";
      s.restart.detail = error ? std::move(*error) : std::string {};
      if (!error && big_picture) {
        s.restart.big_picture_restarted = std::chrono::steady_clock::now();
      }
    });
    return true;
  }
}  // namespace

namespace platf::steam::big_picture {
  void configure(bool enabled) {
    auto &s = state();
    {
      std::lock_guard lock(s.mutex);
      if (s.config.enabled == enabled) {
        return;
      }
      s.config = {enabled, s.config.generation + 1};
    }
    s.changed.notify_all();
  }

  void notify_changed() {
    status_dirty.store(true, std::memory_order_relaxed);
  }

  void start() {
    auto &s = state();
    {
      std::lock_guard lock(s.mutex);
      s.stopping = false;
    }
    s.worker = std::jthread(supervise);
    s.action_worker = std::jthread(run_actions);
  }

  void stop() {
    auto &s = state();
    std::jthread restart_worker;
    {
      // The web UI keeps serving while nvhttp stops, and start_restart() replaces the restart thread
      // under this lock. Take it out here so the two never touch it at once, and start no more.
      std::lock_guard lock(s.mutex);
      s.stopping = true;
      restart_worker = std::move(s.restart_worker);
    }
    s.worker.request_stop();
    s.action_worker.request_stop();
    restart_worker.request_stop();
    s.changed.notify_all();
    if (s.worker.joinable()) {
      s.worker.join();
    }
    if (s.action_worker.joinable()) {
      s.action_worker.join();
    }
    if (restart_worker.joinable()) {
      restart_worker.join();
    }
  }

  nlohmann::json status() {
    // Looked up outside the lock: it reads the registry and the process list.
    const auto steam = debug_port::inspect();
    auto &s = state();
    std::lock_guard lock(s.mutex);
    return {
      {"debug_port",
       {
         {"steam_found", steam.root.has_value()},
         {"steam_root", steam.root ? platf::to_utf8(steam.root->wstring()) : std::string {}},
         {"steam_running", steam.running},
         {"big_picture_open", steam.big_picture},
         {"flag_file", steam.flag_file},
         {"flag_file_created", steam.flag_file_created},
       }},
      {"steam_restart", {{"state", s.restart.state}, {"detail", s.restart.detail}}},
      {"status", true},
      {"supported", true},
      {"enabled", s.config.enabled},
      {"state", s.helper.state},
      {"detail", s.helper.detail},
      {"helper_connected", s.helper.connected},
      {"features", s.helper.features},
      {"steam_build", s.helper.steam_build},
      {"script_version", s.helper.script_version},
      {"script_errors", s.helper.script_errors},
      {"last_error", s.helper.last_error},
    };
  }

  check_result_t check(std::chrono::milliseconds timeout) {
    auto &s = state();
    std::unique_lock lock(s.mutex);
    if (!s.helper.connected) {
      return {check_outcome_e::not_connected, {}};
    }
    const auto id = s.next_check_id++;
    s.pending_checks.push_back(id);
    s.waiting_checks.insert(id);
    const bool woke = s.changed.wait_for(lock, timeout, [&] {
      return s.check_results.contains(id) || !s.helper.connected;
    });
    s.waiting_checks.erase(id);
    std::erase(s.pending_checks, id);
    if (const auto found = s.check_results.find(id); found != s.check_results.end()) {
      auto message = std::move(found->second);
      s.check_results.erase(found);
      return {check_outcome_e::answered, std::move(message)};
    }
    // Woken without an answer: the helper went away during the check.
    return {woke ? check_outcome_e::not_connected : check_outcome_e::timed_out, {}};
  }

  debug_port_result_t set_debug_port(bool enabled, bool restart_steam, bool allow_during_stream) {
    debug_port_result_t result;
    const auto steam = debug_port::inspect();
    std::optional<policy::restart_decision_e> decision;
    bool big_picture = steam.big_picture;
    if (restart_steam) {
      const bool streaming = streaming_now();
      auto &s = state();
      std::lock_guard lock(s.mutex);
      decision = policy::restart_decision(steam.running, streaming, allow_during_stream, restart_running());
      if (const auto restarted = s.restart.big_picture_restarted) {
        big_picture = policy::restart_into_big_picture(steam.big_picture, std::chrono::steady_clock::now() - *restarted);
      }
    }
    result.restart = decision ? std::string(policy::restart_decision_name(*decision)) : "skipped";
    if (decision == policy::restart_decision_e::stream_active || decision == policy::restart_decision_e::busy) {
      result.error_code = result.restart;
      result.error = *decision == policy::restart_decision_e::busy ? "Steam is already restarting." : "A stream or app is running. Restarting Steam closes games running through Steam.";
      return result;
    }
    if (auto error = debug_port::set_flag_file(enabled)) {
      result.error_code = "failed";
      result.error = std::move(*error);
      return result;
    }
    if (decision == policy::restart_decision_e::start && !start_restart(big_picture)) {
      result.restart = policy::restart_decision_name(policy::restart_decision_e::busy);
    }
    result.ok = true;
    return result;
  }
}  // namespace platf::steam::big_picture
#endif

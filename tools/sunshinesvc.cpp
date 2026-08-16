/**
 * @file tools/sunshinesvc.cpp
 * @brief Handles launching Sunshine.exe into user sessions as SYSTEM
 */
#define WIN32_LEAN_AND_MEAN
#include <format>
#include <cstdint>
#include <string>
#include <Windows.h>
#include <WtsApi32.h>
#include <shellapi.h>
#include <memory>
#include <atomic>
#include <mutex>
#include <unordered_map>
#include <vector>

#include "src/platform/windows/service_constants.h"
#include "src/platform/windows/terminal_session_seat_provider.h"
#include "src/terminal_session_launch_codec.h"
#include "src/terminal_session_runtime.h"
#include "src/terminal_session_service.h"
#include "src/terminal_session_worker_process.h"

// PROC_THREAD_ATTRIBUTE_JOB_LIST is currently missing from MinGW headers
#ifndef PROC_THREAD_ATTRIBUTE_JOB_LIST
  #define PROC_THREAD_ATTRIBUTE_JOB_LIST ProcThreadAttributeValue(13, FALSE, TRUE, FALSE)
#endif

SERVICE_STATUS_HANDLE service_status_handle;
SERVICE_STATUS service_status;
HANDLE stop_event;
HANDLE session_change_event;
std::unique_ptr<terminal_session::service::pipe_server_t> terminal_broker;
std::mutex terminal_state_mutex;
struct terminal_state_t {
  std::uint64_t generation {};
  std::uint32_t launch_id {};
  bool connected {};
  terminal_session::route_t route;
  std::int32_t app_id {};
  std::string app_uuid;
  std::string app_name;
};
std::unordered_map<std::string, terminal_state_t> terminal_states;
std::unique_ptr<terminal_session::runtime_t> terminal_runtime;
std::atomic<DWORD> authorized_sunshine_pid {0};
std::atomic<std::uint64_t> authorized_sunshine_creation {0};

std::uint64_t ProcessCreationTime(HANDLE process) {
  FILETIME created {}, exited {}, kernel {}, user {};
  if (!GetProcessTimes(process, &created, &exited, &kernel, &user)) return 0;
  ULARGE_INTEGER value {};
  value.LowPart = created.dwLowDateTime;
  value.HighPart = created.dwHighDateTime;
  return value.QuadPart;
}

constexpr auto SERVICE_NAME = "SunshineService";
constexpr DWORD FAST_EXIT_WINDOW_MS = 60 * 1000;
constexpr DWORD CRASH_LOOP_RESTART_DELAY_MS = 30 * 1000;
constexpr DWORD CRASH_LOOP_FAST_EXIT_THRESHOLD = 3;

terminal_session::protocol::response_t HandleTerminalBroker(const terminal_session::protocol::request_t &request) {
  terminal_session::protocol::response_t response;
  response.client_uuid = request.client_uuid;
  response.generation = request.generation;
  response.launch_id = request.launch_id;
  std::lock_guard lock {terminal_state_mutex};
  if (request.operation == terminal_session::protocol::opcode::control_query) {
    const auto found = terminal_states.find(request.client_uuid);
    response.accepted = true;
    if (found == terminal_states.end()) return response;
    response.state_exists = true;
    response.state_connected = found->second.connected;
    response.owner_generation = found->second.generation;
    response.owner_launch_id = found->second.launch_id;
    response.windows_session_id = found->second.route.windows_session_id;
    response.seat_id = found->second.route.seat_id;
    response.rtsp_port = found->second.route.rtsp_port;
    response.control_port = found->second.route.control_port;
    response.video_port = found->second.route.video_port;
    response.audio_port = found->second.route.audio_port;
    response.app_id = found->second.app_id;
    response.app_uuid = found->second.app_uuid;
    response.app_name = found->second.app_name;
    return response;
  }
  if (request.operation == terminal_session::protocol::opcode::control_release) {
    const auto found = terminal_states.find(request.client_uuid);
    if (found == terminal_states.end() || found->second.generation != request.generation || found->second.launch_id != request.launch_id) {
      response.reason = terminal_session::protocol::reject_reason::stale_generation;
      response.error = "Terminal release does not match the owned launch.";
      return response;
    }
    if (!terminal_runtime || !terminal_runtime->disconnect(request.client_uuid, "authenticated control release", request.release)) {
      response.reason = terminal_session::protocol::reject_reason::provider_unavailable;
      response.error = "The exact terminal worker/session teardown did not complete.";
      return response;
    }
    if (request.release == terminal_session::protocol::release_mode::retain) {
      found->second.connected = false;
    } else {
      terminal_states.erase(found);
    }
    response.accepted = true;
    return response;
  }
  if (request.operation != terminal_session::protocol::opcode::control_prepare) {
    response.reason = terminal_session::protocol::reject_reason::invalid_state;
    response.error = "The service accepts only authenticated control requests.";
    return response;
  }
  const auto found = terminal_states.find(request.client_uuid);
  if (found != terminal_states.end() && found->second.generation != request.generation) {
    response.reason = terminal_session::protocol::reject_reason::stale_generation;
    response.error = "Terminal launch generation is stale.";
    return response;
  }
  if (!terminal_runtime) {
    response.reason = terminal_session::protocol::reject_reason::provider_unavailable;
    response.error = "The managed terminal runtime is not initialized.";
    return response;
  }
  std::string decode_error;
  auto launch = terminal_session::launch_codec::decode(request.launch_payload, decode_error);
  if (!launch || !launch->launch_session || launch->launch_session->client_uuid != request.client_uuid ||
      launch->launch_session->role_generation != request.generation || launch->launch_session->id != request.launch_id) {
    response.reason = terminal_session::protocol::reject_reason::malformed;
    response.error = decode_error.empty() ? "The protected launch payload does not match its authenticated envelope." : decode_error;
    return response;
  }
  const auto app_id = launch->launch_session->appid;
  const auto app_uuid = launch->launch_session->app_metadata ? launch->launch_session->app_metadata->uuid : std::string {};
  const auto app_name = launch->launch_session->app_metadata ? launch->launch_session->app_metadata->name : std::string {};
  const auto route = terminal_runtime->prepare(std::move(*launch));
  if (!route.accepted || !route.ready) {
    response.reason = terminal_session::protocol::reject_reason::provider_unavailable;
    response.error = route.error.empty() ? "The managed seat did not become ready." : route.error;
    return response;
  }
  terminal_states.insert_or_assign(request.client_uuid, terminal_state_t {
    request.generation, request.launch_id, true, route, app_id, app_uuid, app_name
  });
  response.accepted = true;
  response.windows_session_id = route.windows_session_id;
  response.seat_id = route.seat_id;
  response.rtsp_port = route.rtsp_port;
  response.control_port = route.control_port;
  response.video_port = route.video_port;
  response.audio_port = route.audio_port;
  return response;
}

DWORD WINAPI HandlerEx(DWORD dwControl, DWORD dwEventType, LPVOID lpEventData, LPVOID lpContext) {
  switch (dwControl) {
    case SERVICE_CONTROL_INTERROGATE:
      return NO_ERROR;

    case SERVICE_CONTROL_SESSIONCHANGE:
      // If a new session connects to the console, restart Sunshine
      // to allow it to spawn inside the new console session.
      if (dwEventType == WTS_CONSOLE_CONNECT) {
        SetEvent(session_change_event);
      }
      return NO_ERROR;

    case SERVICE_CONTROL_PRESHUTDOWN:
      // The system is shutting down
    case SERVICE_CONTROL_STOP:
      // Let SCM know we're stopping in up to 30 seconds
      service_status.dwCurrentState = SERVICE_STOP_PENDING;
      service_status.dwControlsAccepted = 0;
      service_status.dwWaitHint = 30 * 1000;
      SetServiceStatus(service_status_handle, &service_status);

      // Trigger ServiceMain() to start cleanup
      SetEvent(stop_event);
      return NO_ERROR;

    default:
      return ERROR_CALL_NOT_IMPLEMENTED;
  }
}

HANDLE CreateJobObjectForChildProcess() {
  HANDLE job_handle = CreateJobObjectW(nullptr, nullptr);
  if (!job_handle) {
    return nullptr;
  }

  JOBOBJECT_EXTENDED_LIMIT_INFORMATION job_limit_info = {};

  // Kill Sunshine.exe when the final job object handle is closed (which will happen if we terminate unexpectedly).
  // This ensures we don't leave an orphaned Sunshine.exe running with an inherited handle to our log file.
  job_limit_info.BasicLimitInformation.LimitFlags |= JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;

  // Allow Sunshine.exe to use CREATE_BREAKAWAY_FROM_JOB when spawning processes to ensure they can to live beyond
  // the lifetime of SunshineSvc.exe. This avoids unexpected user data loss if we crash or are killed.
  job_limit_info.BasicLimitInformation.LimitFlags |= JOB_OBJECT_LIMIT_BREAKAWAY_OK;

  if (!SetInformationJobObject(job_handle, JobObjectExtendedLimitInformation, &job_limit_info, sizeof(job_limit_info))) {
    CloseHandle(job_handle);
    return nullptr;
  }

  return job_handle;
}

LPPROC_THREAD_ATTRIBUTE_LIST AllocateProcThreadAttributeList(DWORD attribute_count) {
  SIZE_T size;
  InitializeProcThreadAttributeList(nullptr, attribute_count, 0, &size);

  auto list = (LPPROC_THREAD_ATTRIBUTE_LIST) HeapAlloc(GetProcessHeap(), 0, size);
  if (list == nullptr) {
    return nullptr;
  }

  if (!InitializeProcThreadAttributeList(list, attribute_count, 0, &size)) {
    HeapFree(GetProcessHeap(), 0, list);
    return nullptr;
  }

  return list;
}

HANDLE DuplicateTokenForSession(DWORD console_session_id) {
  HANDLE current_token;
  if (!OpenProcessToken(GetCurrentProcess(), TOKEN_DUPLICATE, &current_token)) {
    return nullptr;
  }

  // Duplicate our own LocalSystem token
  HANDLE new_token;
  if (!DuplicateTokenEx(current_token, TOKEN_ALL_ACCESS, nullptr, SecurityImpersonation, TokenPrimary, &new_token)) {
    CloseHandle(current_token);
    return nullptr;
  }

  CloseHandle(current_token);

  // Change the duplicated token to the console session ID
  if (!SetTokenInformation(new_token, TokenSessionId, &console_session_id, sizeof(console_session_id))) {
    CloseHandle(new_token);
    return nullptr;
  }

  return new_token;
}

HANDLE OpenLogFileHandle() {
  WCHAR log_file_name[MAX_PATH];

  // Create sunshine.log in the Temp folder (usually %SYSTEMROOT%\Temp)
  GetTempPathW(_countof(log_file_name), log_file_name);
  wcscat_s(log_file_name, L"sunshine.log");

  // The file handle must be inheritable for our child process to use it
  SECURITY_ATTRIBUTES security_attributes = {sizeof(security_attributes), nullptr, TRUE};

  // Overwrite the old sunshine.log
  return CreateFileW(log_file_name, GENERIC_WRITE, FILE_SHARE_READ, &security_attributes, CREATE_ALWAYS, 0, nullptr);
}

bool RunTerminationHelper(HANDLE console_token, DWORD pid) {
  WCHAR module_path[MAX_PATH];
  GetModuleFileNameW(nullptr, module_path, _countof(module_path));
  std::wstring command;

  command += L'"';
  command += module_path;
  command += L'"';
  command += std::format(L" --terminate {}", pid);

  STARTUPINFOW startup_info = {};
  startup_info.cb = sizeof(startup_info);
  startup_info.lpDesktop = (LPWSTR) L"winsta0\\default";

  // Execute ourselves as a detached process in the user session with the --terminate argument.
  // This will allow us to attach to Sunshine's console and send it a Ctrl-C event.
  PROCESS_INFORMATION process_info;
  if (!CreateProcessAsUserW(console_token, module_path, (LPWSTR) command.c_str(), nullptr, nullptr, FALSE, CREATE_UNICODE_ENVIRONMENT | DETACHED_PROCESS, nullptr, nullptr, &startup_info, &process_info)) {
    return false;
  }

  // Wait for the termination helper to complete
  WaitForSingleObject(process_info.hProcess, INFINITE);

  // Check the exit status of the helper process
  DWORD exit_code;
  GetExitCodeProcess(process_info.hProcess, &exit_code);

  // Cleanup handles
  CloseHandle(process_info.hProcess);
  CloseHandle(process_info.hThread);

  // If the helper process returned 0, it succeeded
  return exit_code == 0;
}

VOID WINAPI ServiceMain(DWORD dwArgc, LPTSTR *lpszArgv) {
  service_status_handle = RegisterServiceCtrlHandlerEx(SERVICE_NAME, HandlerEx, nullptr);
  if (service_status_handle == nullptr) {
    // Nothing we can really do here but terminate ourselves
    ExitProcess(GetLastError());
    return;
  }

  // Tell SCM we're starting
  service_status.dwServiceType = SERVICE_WIN32_OWN_PROCESS;
  service_status.dwServiceSpecificExitCode = 0;
  service_status.dwWin32ExitCode = NO_ERROR;
  service_status.dwWaitHint = 0;
  service_status.dwControlsAccepted = 0;
  service_status.dwCheckPoint = 0;
  service_status.dwCurrentState = SERVICE_START_PENDING;
  SetServiceStatus(service_status_handle, &service_status);

  // Create a manual-reset stop event
  stop_event = CreateEventA(nullptr, TRUE, FALSE, nullptr);
  if (stop_event == nullptr) {
    // Tell SCM we failed to start
    service_status.dwWin32ExitCode = GetLastError();
    service_status.dwCurrentState = SERVICE_STOPPED;
    SetServiceStatus(service_status_handle, &service_status);
    return;
  }

  // Create an auto-reset session change event
  session_change_event = CreateEventA(nullptr, FALSE, FALSE, nullptr);
  if (session_change_event == nullptr) {
    // Tell SCM we failed to start
    service_status.dwWin32ExitCode = GetLastError();
    service_status.dwCurrentState = SERVICE_STOPPED;
    SetServiceStatus(service_status_handle, &service_status);
    return;
  }

  auto log_file_handle = OpenLogFileHandle();
  if (log_file_handle == INVALID_HANDLE_VALUE) {
    // Tell SCM we failed to start
    service_status.dwWin32ExitCode = GetLastError();
    service_status.dwCurrentState = SERVICE_STOPPED;
    SetServiceStatus(service_status_handle, &service_status);
    return;
  }

  // We can use a single STARTUPINFOEXW for all the processes that we launch
  STARTUPINFOEXW startup_info = {};
  startup_info.StartupInfo.cb = sizeof(startup_info);
  startup_info.StartupInfo.lpDesktop = (LPWSTR) L"winsta0\\default";
  startup_info.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
  startup_info.StartupInfo.hStdInput = nullptr;
  startup_info.StartupInfo.hStdOutput = log_file_handle;
  startup_info.StartupInfo.hStdError = log_file_handle;

  // Allocate an attribute list with space for 2 entries
  startup_info.lpAttributeList = AllocateProcThreadAttributeList(2);
  if (startup_info.lpAttributeList == nullptr) {
    // Tell SCM we failed to start
    service_status.dwWin32ExitCode = GetLastError();
    service_status.dwCurrentState = SERVICE_STOPPED;
    SetServiceStatus(service_status_handle, &service_status);
    return;
  }

  // Only allow Sunshine.exe to inherit the log file handle, not all inheritable handles
  UpdateProcThreadAttribute(startup_info.lpAttributeList, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, &log_file_handle, sizeof(log_file_handle), nullptr, nullptr);

  // Tell SCM we're running (and stoppable now)
  service_status.dwControlsAccepted = SERVICE_ACCEPT_STOP | SERVICE_ACCEPT_PRESHUTDOWN | SERVICE_ACCEPT_SESSIONCHANGE;
  service_status.dwCurrentState = SERVICE_RUNNING;
  SetServiceStatus(service_status_handle, &service_status);

  // The privileged service owns the sole production terminal broker endpoint.
  // Provider preflight remains fail-closed and never restarts TermService or
  // enables the isolated listener on its own.
  // A previous service crash may have interrupted the narrow RDP bootstrap
  // interval. Never expose the broker until every tagged account is disabled.
  if (terminal_session::windows::secure_managed_accounts(false)) {
    terminal_runtime = std::make_unique<terminal_session::runtime_t>(
      terminal_session::windows::make_seat_provider(),
      [] { return std::make_unique<terminal_session::worker::process_t>(); }
    );
    terminal_broker = std::make_unique<terminal_session::service::pipe_server_t>(HandleTerminalBroker, [](const terminal_session::protocol::peer_identity_t &peer) {
      return peer.pid == authorized_sunshine_pid.load(std::memory_order_acquire) &&
             peer.creation_time == authorized_sunshine_creation.load(std::memory_order_acquire);
    });
    if (!terminal_broker->start()) {
      terminal_broker.reset();
      terminal_runtime.reset();
    }
  }

  SetEnvironmentVariableW(platf::service_launch::launched_by_service_env_var, L"1");

  DWORD fast_exit_count = 0;
  ULONGLONG first_fast_exit_tick = 0;

  // Loop every 3 seconds until the stop event is set or Sunshine.exe is running
  while (WaitForSingleObject(stop_event, 3000) != WAIT_OBJECT_0) {
    auto console_session_id = WTSGetActiveConsoleSessionId();
    if (console_session_id == 0xFFFFFFFF) {
      // No console session yet
      continue;
    }

    auto console_token = DuplicateTokenForSession(console_session_id);
    if (console_token == nullptr) {
      continue;
    }

    // Job objects cannot span sessions, so we must create one for each process
    auto job_handle = CreateJobObjectForChildProcess();
    if (job_handle == nullptr) {
      CloseHandle(console_token);
      continue;
    }

    // Start Sunshine.exe inside our job object
    UpdateProcThreadAttribute(startup_info.lpAttributeList, 0, PROC_THREAD_ATTRIBUTE_JOB_LIST, &job_handle, sizeof(job_handle), nullptr, nullptr);

    PROCESS_INFORMATION process_info;
    if (!CreateProcessAsUserW(console_token, L"Sunshine.exe", nullptr, nullptr, nullptr, TRUE, CREATE_UNICODE_ENVIRONMENT | CREATE_NO_WINDOW | EXTENDED_STARTUPINFO_PRESENT, nullptr, nullptr, (LPSTARTUPINFOW) &startup_info, &process_info)) {
      CloseHandle(console_token);
      CloseHandle(job_handle);
      continue;
    }
    const auto creation = ProcessCreationTime(process_info.hProcess);
    if (!creation) {
      TerminateProcess(process_info.hProcess, ERROR_PROCESS_ABORTED);
      CloseHandle(process_info.hThread);
      CloseHandle(process_info.hProcess);
      CloseHandle(console_token);
      CloseHandle(job_handle);
      continue;
    }
    authorized_sunshine_creation.store(creation, std::memory_order_release);
    authorized_sunshine_pid.store(process_info.dwProcessId, std::memory_order_release);

    bool still_running = true;
    bool self_exit = false;
    const auto process_start_tick = GetTickCount64();
    do {
      // Wait for the stop event to be set, Sunshine.exe to terminate, or the console session to change
      const HANDLE wait_objects[] = {stop_event, process_info.hProcess, session_change_event};
      switch (WaitForMultipleObjects(_countof(wait_objects), wait_objects, FALSE, INFINITE)) {
        case WAIT_OBJECT_0 + 2:
          if (WTSGetActiveConsoleSessionId() == console_session_id) {
            // The active console session didn't actually change. Let Sunshine keep running.
            still_running = true;
            continue;
          }
          // Fall-through to terminate Sunshine.exe and start it again.
        case WAIT_OBJECT_0:
          // The service is shutting down, so try to gracefully terminate Sunshine.exe.
          // If it doesn't terminate in 20 seconds, we will forcefully terminate it.
          if (!RunTerminationHelper(console_token, process_info.dwProcessId) ||
              WaitForSingleObject(process_info.hProcess, 20000) != WAIT_OBJECT_0) {
            // If it won't terminate gracefully, kill it now
            TerminateProcess(process_info.hProcess, ERROR_PROCESS_ABORTED);
          }
          still_running = false;
          break;

        case WAIT_OBJECT_0 + 1:
          {
            // Sunshine terminated itself.
            self_exit = true;

            DWORD exit_code;
            if (GetExitCodeProcess(process_info.hProcess, &exit_code) && exit_code == ERROR_SHUTDOWN_IN_PROGRESS) {
              // Sunshine is asking for us to shut down, so gracefully stop ourselves.
              SetEvent(stop_event);
            }
            still_running = false;
            break;
          }
      }
    } while (still_running);

    CloseHandle(process_info.hThread);
    CloseHandle(process_info.hProcess);
    authorized_sunshine_pid.store(0, std::memory_order_release);
    authorized_sunshine_creation.store(0, std::memory_order_release);
    CloseHandle(console_token);
    CloseHandle(job_handle);

    if (!self_exit) {
      fast_exit_count = 0;
      first_fast_exit_tick = 0;
      continue;
    }

    const auto now_tick = GetTickCount64();
    if (now_tick - process_start_tick > FAST_EXIT_WINDOW_MS) {
      fast_exit_count = 0;
      first_fast_exit_tick = 0;
      continue;
    }

    if (first_fast_exit_tick == 0 || now_tick - first_fast_exit_tick > FAST_EXIT_WINDOW_MS) {
      first_fast_exit_tick = now_tick;
      fast_exit_count = 1;
    } else {
      ++fast_exit_count;
    }

    if (fast_exit_count >= CRASH_LOOP_FAST_EXIT_THRESHOLD) {
      if (WaitForSingleObject(stop_event, CRASH_LOOP_RESTART_DELAY_MS) == WAIT_OBJECT_0) {
        break;
      }
    }
  }

  if (terminal_broker) {
    terminal_broker->stop();
    terminal_broker.reset();
  }
  if (terminal_runtime) {
    terminal_runtime->shutdown();
    terminal_runtime.reset();
  }
  {
    std::lock_guard lock {terminal_state_mutex};
    terminal_states.clear();
  }

  // Let SCM know we've stopped
  service_status.dwCurrentState = SERVICE_STOPPED;
  SetServiceStatus(service_status_handle, &service_status);
}

// This will run in a child process in the user session
int DoGracefulTermination(DWORD pid) {
  // Attach to Sunshine's console
  if (!AttachConsole(pid)) {
    return GetLastError();
  }

  // Disable our own Ctrl-C handling
  SetConsoleCtrlHandler(nullptr, TRUE);

  // Send a Ctrl-C event to Sunshine
  if (!GenerateConsoleCtrlEvent(CTRL_C_EVENT, 0)) {
    return GetLastError();
  }

  return 0;
}

int main(int argc, char *argv[]) {
  static const SERVICE_TABLE_ENTRY service_table[] = {
    {(LPSTR) SERVICE_NAME, ServiceMain},
    {nullptr, nullptr}
  };

  // Check if this is a reinvocation of ourselves to send Ctrl-C to Sunshine.exe
  if (argc == 3 && strcmp(argv[1], "--terminate") == 0) {
    return DoGracefulTermination(atol(argv[2]));
  }
  int wide_argc = 0;
  LPWSTR *wide_argv = CommandLineToArgvW(GetCommandLineW(), &wide_argc);
  if (wide_argv && wide_argc == 2 && _wcsicmp(wide_argv[1], L"--cleanup-terminal-seats") == 0) {
    const int result = terminal_session::windows::secure_managed_accounts(true) ? 0 : ERROR_ACCESS_DENIED;
    LocalFree(wide_argv);
    return result;
  }
  if (wide_argv && wide_argc >= 2 && _wcsicmp(wide_argv[1], L"--prepare-seat-acl") == 0) {
    const int result = terminal_session::windows::run_seat_acl_helper(wide_argc, wide_argv);
    LocalFree(wide_argv);
    return result;
  }
  if (wide_argv) LocalFree(wide_argv);

  // By default, services have their current directory set to %SYSTEMROOT%\System32.
  // We want to use the directory where Sunshine.exe is located instead of system32.
  // This requires stripping off 2 path components: the file name and the last folder
  WCHAR module_path[MAX_PATH];
  GetModuleFileNameW(nullptr, module_path, _countof(module_path));
  for (auto i = 0; i < 2; i++) {
    auto last_sep = wcsrchr(module_path, '\\');
    if (last_sep) {
      *last_sep = 0;
    }
  }
  SetCurrentDirectoryW(module_path);

  // Trigger our ServiceMain()
  return StartServiceCtrlDispatcher(service_table);
}

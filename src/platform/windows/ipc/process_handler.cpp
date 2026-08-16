
/**
 * @file process_handler.cpp
 * @brief Implements the ProcessHandler class for managing process creation and control on Windows.
 *
 * This file provides the implementation for starting, waiting, and terminating processes,
 * including support for attribute lists and impersonation as needed for the Sunshine project.
 */

// local includes (include our header first to enforce correct include order)
#include "process_handler.h"

// platform includes
#include <UserEnv.h>
#include <windows.h>
#include <WtsApi32.h>
#include <Sddl.h>

// standard includes
#include "src/logging.h"
#include "src/platform/windows/misc.h"
#include "src/utility.h"

#include <algorithm>
#include <cstdint>
#include <cwchar>
#include <optional>
#include <system_error>
#include <vector>

namespace {

  constexpr wchar_t kDefaultDesktopW[] = L"winsta0\\default";
  constexpr wchar_t kWinlogonDesktopW[] = L"winsta0\\winlogon";

  std::optional<std::wstring> token_sid_string(HANDLE token) {
    DWORD size = 0;
    (void) GetTokenInformation(token, TokenUser, nullptr, 0, &size);
    if (!size) return std::nullopt;
    std::vector<std::uint8_t> buffer(size);
    if (!GetTokenInformation(token, TokenUser, buffer.data(), size, &size)) return std::nullopt;
    LPWSTR sid = nullptr;
    if (!ConvertSidToStringSidW(reinterpret_cast<PTOKEN_USER>(buffer.data())->User.Sid, &sid) || !sid) {
      return std::nullopt;
    }
    std::wstring result {sid};
    LocalFree(sid);
    return result;
  }

  PSECURITY_DESCRIPTOR managed_process_security_descriptor(HANDLE token, const ACCESS_MASK user_mask) {
    const auto sid = token_sid_string(token);
    if (!sid) return nullptr;
    wchar_t mask_text[9] {};
    if (swprintf_s(mask_text, _countof(mask_text), L"%08X", user_mask) <= 0) return nullptr;
    // The admitted user must be able to assign this descriptor when the
    // helper is launched from an unprivileged worker. OWNER RIGHTS replaces
    // the owner's implicit WRITE_DAC/WRITE_OWNER grants, so the same-SID
    // owner cannot reopen the object with VM or injection rights later.
    const auto sddl = L"O:" + *sid + L"G:" + *sid + L"D:P(A;;GA;;;SY)(A;;0x" +
      std::wstring {mask_text} + L";;;OW)(A;;0x" + std::wstring {mask_text} + L";;;" + *sid + L")";
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
          sddl.c_str(), SDDL_REVISION_1, &descriptor, nullptr)) {
      return nullptr;
    }
    return descriptor;
  }

  bool start_as_system_in_active_console_session(
    const std::wstring &cmd_line,
    const std::wstring &working_dir,
    DWORD creation_flags,
    STARTUPINFOEXW &si,
    PROCESS_INFORMATION &pi
  ) {
    const DWORD session_id = WTSGetActiveConsoleSessionId();
    if (session_id == 0xFFFFFFFF) {
      BOOST_LOG(debug) << "Active console session id unavailable; cannot launch SYSTEM child in active session.";
      return false;
    }

    HANDLE raw_process_token = nullptr;
    if (!OpenProcessToken(
          GetCurrentProcess(),
          TOKEN_DUPLICATE | TOKEN_ASSIGN_PRIMARY | TOKEN_QUERY | TOKEN_ADJUST_SESSIONID,
          &raw_process_token
        )) {
      BOOST_LOG(debug) << "OpenProcessToken failed while launching SYSTEM child in active session, winerr=" << GetLastError();
      return false;
    }
    auto close_process_token = util::fail_guard([&]() {
      CloseHandle(raw_process_token);
    });

    HANDLE raw_primary_token = nullptr;
    if (!DuplicateTokenEx(
          raw_process_token,
          MAXIMUM_ALLOWED,
          nullptr,
          SecurityImpersonation,
          TokenPrimary,
          &raw_primary_token
        )) {
      BOOST_LOG(debug) << "DuplicateTokenEx failed while launching SYSTEM child in active session, winerr=" << GetLastError();
      return false;
    }
    auto close_primary_token = util::fail_guard([&]() {
      CloseHandle(raw_primary_token);
    });

    if (!SetTokenInformation(raw_primary_token, TokenSessionId, (PVOID) &session_id, sizeof(session_id))) {
      BOOST_LOG(debug) << "SetTokenInformation(TokenSessionId=" << session_id
                       << ") failed while launching SYSTEM child in active session, winerr=" << GetLastError();
      return false;
    }

    void *env_block = nullptr;
    if (!CreateEnvironmentBlock(&env_block, raw_primary_token, FALSE)) {
      BOOST_LOG(debug) << "CreateEnvironmentBlock failed while launching SYSTEM child in active session, winerr=" << GetLastError();
      env_block = nullptr;
    }
    auto destroy_env = util::fail_guard([&]() {
      if (env_block) {
        DestroyEnvironmentBlock(env_block);
      }
    });

    // Ensure the child runs on the interactive window station/desktop for the active console session.
    auto *prev_desktop = si.StartupInfo.lpDesktop;
    const auto try_launch = [&](const wchar_t *desktop) {
      si.StartupInfo.lpDesktop = const_cast<LPWSTR>(desktop);
      return CreateProcessAsUserW(
        raw_primary_token,
        nullptr,
        (LPWSTR) cmd_line.c_str(),
        nullptr,
        nullptr,
        FALSE,
        creation_flags,
        env_block,
        working_dir.empty() ? nullptr : working_dir.c_str(),
        (LPSTARTUPINFOW) &si,
        &pi
      );
    };

    // Prefer the user's default desktop, but fall back to Winlogon if required (e.g. lock/login screen).
    BOOL ok = try_launch(kDefaultDesktopW);
    const wchar_t *launched_desktop = kDefaultDesktopW;
    if (!ok) {
      ok = try_launch(kWinlogonDesktopW);
      launched_desktop = kWinlogonDesktopW;
    }

    si.StartupInfo.lpDesktop = prev_desktop;
    if (!ok) {
      BOOST_LOG(debug) << "CreateProcessAsUserW failed while launching SYSTEM child in active session (session_id="
                       << session_id << "), winerr=" << GetLastError();
    } else {
      BOOST_LOG(info) << "Launched SYSTEM child in active console session (session_id=" << session_id
                      << ", pid=" << pi.dwProcessId << ", desktop=" << (launched_desktop == kDefaultDesktopW ? "default" : "winlogon")
                      << ").";
    }
    return ok;
  }

}  // namespace

ProcessHandler::ProcessHandler():
    job_(create_kill_on_close_job()),
    use_job_(true) {}

ProcessHandler::ProcessHandler(bool use_job):
    job_(use_job ? create_kill_on_close_job() : winrt::handle {}),
    use_job_(use_job) {}

bool ProcessHandler::start(
  const std::wstring &application_path,
  std::wstring_view arguments,
  bool allow_system_fallback,
  bool breakaway_from_current_job
) {
  if (running_) {
    // Check if the previously started process has already exited. If so, clear stale state.
    if (pi_.hProcess != nullptr) {
      DWORD wait_result = WaitForSingleObject(pi_.hProcess, 0);
      if (wait_result == WAIT_TIMEOUT) {
        // Still running, don't start a new one
        return false;
      }

      // Process either exited or handle is invalid, clean up and allow restart
      if (pi_.hThread) {
        CloseHandle(pi_.hThread);
      }
      if (pi_.hProcess) {
        CloseHandle(pi_.hProcess);
      }
      ZeroMemory(&pi_, sizeof(pi_));
      running_ = false;
    } else {
      // No process handle but marked running; reset state to allow restart
      running_ = false;
    }
  }

  ZeroMemory(&pi_, sizeof(pi_));

  // Build command line: "app_path" [arguments]
  std::wstring cmd_line;
  cmd_line.reserve(application_path.size() + arguments.size() + 3);
  cmd_line.push_back(L'"');
  cmd_line += application_path;
  cmd_line.push_back(L'"');
  if (!arguments.empty()) {
    cmd_line.push_back(L' ');
    cmd_line.append(arguments);
  }

  BOOST_LOG(debug) << "Launching process: " << platf::to_utf8(application_path)
                   << (arguments.empty() ? "" : " with arguments") << " (hidden, detached)";

  STARTUPINFOEXW si = {};
  si.StartupInfo.cb = sizeof(si);

  DWORD creation_flags = CREATE_UNICODE_ENVIRONMENT | CREATE_NO_WINDOW | EXTENDED_STARTUPINFO_PRESENT;
  // When not using a job (keep-alive child), prefer to break away from any existing job to avoid kill-on-close
  if (!use_job_ && breakaway_from_current_job) {
    creation_flags |= CREATE_BREAKAWAY_FROM_JOB;
  }
  // Compute a sane working directory for the child: the directory of the target executable
  std::wstring working_dir;
  {
    size_t pos = application_path.find_last_of(L"\\/");
    if (pos != std::wstring::npos) {
      working_dir = application_path.substr(0, pos);
    }
  }

  BOOL ret = FALSE;

  if (platf::is_running_as_system()) {
    HANDLE user_token = platf::retrieve_users_token(false);
    if (user_token) {
      auto close_token = util::fail_guard([&]() {
        CloseHandle(user_token);
      });

      // Build a user-specific environment block for the child process
      void *env_block = nullptr;
      if (!CreateEnvironmentBlock(&env_block, user_token, FALSE)) {
        BOOST_LOG(error) << "CreateEnvironmentBlock failed, error: " << GetLastError();
        env_block = nullptr;
      }
      auto destroy_env = util::fail_guard([&]() {
        if (env_block) {
          DestroyEnvironmentBlock(env_block);
        }
      });

      PSECURITY_DESCRIPTOR process_descriptor = nullptr;
      PSECURITY_DESCRIPTOR thread_descriptor = nullptr;
      auto free_descriptors = util::fail_guard([&]() {
        if (process_descriptor) LocalFree(process_descriptor);
        if (thread_descriptor) LocalFree(thread_descriptor);
      });
      SECURITY_ATTRIBUTES process_security {
        .nLength = sizeof(SECURITY_ATTRIBUTES), .lpSecurityDescriptor = nullptr, .bInheritHandle = FALSE};
      SECURITY_ATTRIBUTES thread_security {
        .nLength = sizeof(SECURITY_ATTRIBUTES), .lpSecurityDescriptor = nullptr, .bInheritHandle = FALSE};
      if (!use_job_) {
        process_descriptor = managed_process_security_descriptor(
          user_token, PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE);
        thread_descriptor = managed_process_security_descriptor(
          user_token, THREAD_QUERY_LIMITED_INFORMATION | SYNCHRONIZE);
        if (!process_descriptor || !thread_descriptor) {
          BOOST_LOG(error) << "Failed to protect managed helper process objects.";
          return false;
        }
        process_security.lpSecurityDescriptor = process_descriptor;
        thread_security.lpSecurityDescriptor = thread_descriptor;
      }

      // Launch in the user's context with their environment and an explicit working directory
      ret = CreateProcessAsUserW(
        user_token,
        nullptr,
        (LPWSTR) cmd_line.c_str(),
        &process_security,
        &thread_security,
        FALSE,
        creation_flags,
        env_block,
        working_dir.empty() ? nullptr : working_dir.c_str(),
        (LPSTARTUPINFOW) &si,
        &pi_
      );
    } else if (allow_system_fallback && use_job_) {
      BOOST_LOG(warning) << "No user session available; launching as SYSTEM: " << platf::to_utf8(application_path);

      // Prefer launching into the active console session so display APIs (e.g., SetDisplayConfig)
      // have a better chance of working while Sunshine runs as a service.
      if (start_as_system_in_active_console_session(cmd_line, working_dir, creation_flags, si, pi_)) {
        ret = TRUE;
      } else {
        HANDLE fallback_token = nullptr;
        if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &fallback_token)) {
          BOOST_LOG(error) << "Failed to query the SYSTEM token for fallback process security.";
          return false;
        }
        auto close_fallback_token = util::fail_guard([&]() { CloseHandle(fallback_token); });
        PSECURITY_DESCRIPTOR fallback_process_descriptor = managed_process_security_descriptor(
          fallback_token, PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE);
        PSECURITY_DESCRIPTOR fallback_thread_descriptor = managed_process_security_descriptor(
          fallback_token, THREAD_QUERY_LIMITED_INFORMATION | SYNCHRONIZE);
        auto free_fallback_descriptors = util::fail_guard([&]() {
          if (fallback_process_descriptor) LocalFree(fallback_process_descriptor);
          if (fallback_thread_descriptor) LocalFree(fallback_thread_descriptor);
        });
        if (!fallback_process_descriptor || !fallback_thread_descriptor) {
          BOOST_LOG(error) << "Failed to secure SYSTEM fallback process objects.";
          return false;
        }
        SECURITY_ATTRIBUTES fallback_process_security {
          .nLength = sizeof(SECURITY_ATTRIBUTES), .lpSecurityDescriptor = fallback_process_descriptor, .bInheritHandle = FALSE};
        SECURITY_ATTRIBUTES fallback_thread_security {
          .nLength = sizeof(SECURITY_ATTRIBUTES), .lpSecurityDescriptor = fallback_thread_descriptor, .bInheritHandle = FALSE};
        ret = CreateProcessW(
          nullptr,
          (LPWSTR) cmd_line.c_str(),
          &fallback_process_security,
          &fallback_thread_security,
          FALSE,
          creation_flags,
          nullptr,
          working_dir.empty() ? nullptr : working_dir.c_str(),
          (LPSTARTUPINFOW) &si,
          &pi_
        );
      }
    } else {
      BOOST_LOG(error) << "Failed to retrieve user token while launching: " << platf::to_utf8(application_path);
      return false;
    }
  } else {
    // Non-SYSTEM: inherit our environment but still supply a sensible working directory
    PSECURITY_DESCRIPTOR process_descriptor = nullptr;
    PSECURITY_DESCRIPTOR thread_descriptor = nullptr;
    auto free_descriptors = util::fail_guard([&]() {
      if (process_descriptor) LocalFree(process_descriptor);
      if (thread_descriptor) LocalFree(thread_descriptor);
    });
    HANDLE current_token = nullptr;
    if (!use_job_ && !OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &current_token)) {
      BOOST_LOG(error) << "Failed to query the admitted user token while securing managed helper objects.";
      return false;
    }
    if (!use_job_) {
      process_descriptor = managed_process_security_descriptor(
        current_token, PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE);
      thread_descriptor = managed_process_security_descriptor(
        current_token, THREAD_QUERY_LIMITED_INFORMATION | SYNCHRONIZE);
      CloseHandle(current_token);
      if (!process_descriptor || !thread_descriptor) {
        BOOST_LOG(error) << "Failed to build both protected managed helper object descriptors.";
        return false;
      }
    }
    SECURITY_ATTRIBUTES process_security {
      .nLength = sizeof(SECURITY_ATTRIBUTES), .lpSecurityDescriptor = process_descriptor, .bInheritHandle = FALSE};
    SECURITY_ATTRIBUTES thread_security {
      .nLength = sizeof(SECURITY_ATTRIBUTES), .lpSecurityDescriptor = thread_descriptor, .bInheritHandle = FALSE};
    ret = CreateProcessW(
      nullptr,
      (LPWSTR) cmd_line.c_str(),
      &process_security,
      &thread_security,
      FALSE,
      creation_flags,
      nullptr,
      working_dir.empty() ? nullptr : working_dir.c_str(),
      (LPSTARTUPINFOW) &si,
      &pi_
    );
  }

  if (ret && use_job_ && job_) {
    AssignProcessToJobObject(job_.get(), pi_.hProcess);
  }

  running_ = ret;
  if (!running_) {
    auto winerr = GetLastError();
    BOOST_LOG(error) << "Failed to launch process: " << platf::to_utf8(application_path) << ", error: " << winerr;
    ZeroMemory(&pi_, sizeof(pi_));
  }
  if (running_) {
    DWORD pid = pi_.dwProcessId;
    BOOST_LOG(debug) << "Process started successfully (pid=" << pid << ")";
  }
  return running_;
}

bool ProcessHandler::wait(DWORD &exit_code) {
  return wait_for(exit_code, INFINITE);
}

bool ProcessHandler::wait_for(DWORD &exit_code, DWORD timeout_ms) {
  if (!running_ || pi_.hProcess == nullptr) {
    return false;
  }
  DWORD wait_result = WaitForSingleObject(pi_.hProcess, timeout_ms);
  if (wait_result != WAIT_OBJECT_0) {
    if (wait_result == WAIT_TIMEOUT) {
      BOOST_LOG(warning) << "Process wait timed out after " << timeout_ms << "ms";
    } else {
      BOOST_LOG(warning) << "Process wait failed, result=" << wait_result << ", error=" << GetLastError();
    }
    return false;
  }
  BOOL got_code = GetExitCodeProcess(pi_.hProcess, &exit_code);

  // The process has exited; release OS handles and clear state to allow clean restarts.
  running_ = false;
  if (pi_.hThread) {
    CloseHandle(pi_.hThread);
  }
  if (pi_.hProcess) {
    CloseHandle(pi_.hProcess);
  }
  ZeroMemory(&pi_, sizeof(pi_));
  return got_code != 0;
}

void ProcessHandler::terminate() {
  if (running_ && pi_.hProcess) {
    if (!TerminateProcess(pi_.hProcess, 1)) {
      BOOST_LOG(warning) << "TerminateProcess failed, error=" << GetLastError();
    }
    // Do not clear running_/handles here: callers may need to wait() for full teardown
    // to avoid overlapping helper instances and destabilizing the driver stack.
  }
}

ProcessHandler::~ProcessHandler() {
  // For helpers that should outlive the parent (use_job_ == false),
  // do not terminate them on handler destruction. Only terminate
  // processes that we explicitly manage via a kill-on-close Job.
  if (use_job_) {
    terminate();
  }

  // Clean up handles
  if (pi_.hProcess) {
    CloseHandle(pi_.hProcess);
  }
  if (pi_.hThread) {
    CloseHandle(pi_.hThread);
  }
  // job_ is a winrt::handle and will auto-cleanup
}

HANDLE ProcessHandler::get_process_handle() const {
  return running_ ? pi_.hProcess : nullptr;
}

winrt::handle create_kill_on_close_job() {
  winrt::handle job_handle {CreateJobObjectW(nullptr, nullptr)};
  if (!job_handle) {
    return {};
  }
  JOBOBJECT_EXTENDED_LIMIT_INFORMATION job_info = {};
  job_info.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
  if (!SetInformationJobObject(job_handle.get(), JobObjectExtendedLimitInformation, &job_info, sizeof(job_info))) {
    // winrt::handle will auto-close on destruction
    return {};
  }
  return job_handle;
}

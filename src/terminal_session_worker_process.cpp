#include "terminal_session_worker_process.h"

#ifdef _WIN32
  #include "terminal_session_service.h"
  #include "platform/windows/ipc/pipes.h"
  #include <Windows.h>
  #include <objbase.h>
  #include <sddl.h>
  #include <filesystem>
  #include <sstream>
#endif

namespace terminal_session::worker {
#ifdef _WIN32
  namespace {
    std::string unique_pipe() {
      GUID guid {};
      if (CoCreateGuid(&guid) != S_OK) return {};
      return "VibeshineTerminalWorker-" + std::to_string(guid.Data1) + "-" + std::to_string(guid.Data2) + "-" + std::to_string(guid.Data3);
    }
    std::wstring module_path() {
      wchar_t path[MAX_PATH] {};
      if (!GetModuleFileNameW(nullptr, path, MAX_PATH)) return {};
      return std::filesystem::path(path).parent_path().wstring();
    }
    bool token_matches_resource(HANDLE token, const provider_resource_t &resource) {
      DWORD session = 0, size = sizeof(session);
      if (!GetTokenInformation(token, TokenSessionId, &session, size, &size) || session != resource.windows_session_id || resource.user_sid.empty()) return false;
      GetTokenInformation(token, TokenUser, nullptr, 0, &size);
      auto user = std::make_unique<std::uint8_t[]>(size);
      if (!GetTokenInformation(token, TokenUser, user.get(), size, &size)) return false;
      LPWSTR sid = nullptr;
      const bool converted = ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER *>(user.get())->User.Sid, &sid) != FALSE;
      const std::wstring sid_w = converted && sid ? std::wstring {sid} : std::wstring {};
      const std::string sid_text {sid_w.begin(), sid_w.end()};
      const bool matches = converted && sid && sid_text == resource.user_sid;
      if (sid) LocalFree(sid);
      if (!converted) return false;
      // Compare without accepting a caller-selected SID: the provider must
      // have supplied the SID from this same primary token.
      return matches;
    }
    bool process_matches_resource(HANDLE process, const provider_resource_t &resource) {
      HANDLE raw_token = nullptr;
      if (!OpenProcessToken(process, TOKEN_QUERY, &raw_token)) return false;
      const bool matches = token_matches_resource(raw_token, resource);
      CloseHandle(raw_token);
      return matches;
    }
  }
#endif

  process_t::~process_t() { (void) stop({}); }

  bool process_t::cleanup_needed() const noexcept {
#ifdef _WIN32
    return process_ != nullptr || job_ != nullptr;
#else
    return false;
#endif
  }

  std::optional<route_t> process_t::start(const worker_request_t &request, std::string &error) {
#ifndef _WIN32
    error = "Private worker process is Windows-only.";
    return std::nullopt;
#else
    if (process_ || job_) { error = "A previous private worker still owns resources; teardown must complete first."; return std::nullopt; }
    pipe_name_ = unique_pipe();
    if (pipe_name_.empty() || request.resource.launch_token == 0 || request.resource.windows_session_id == 0 || request.resource.desktop_name.empty() || request.resource.user_sid.empty()) { error = "Provider did not supply a complete token/session/user/desktop launch contract."; return std::nullopt; }
    HANDLE launch_token = reinterpret_cast<HANDLE>(request.resource.launch_token);
    if (!token_matches_resource(launch_token, request.resource)) { error = "Provider launch token does not match the admitted session/user."; return std::nullopt; }
    HANDLE job = CreateJobObjectW(nullptr, nullptr);
    if (!job) { error = "Worker job creation failed."; return std::nullopt; }
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits {};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (!SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limits, sizeof(limits))) { CloseHandle(job); error = "Worker job configuration failed."; return std::nullopt; }
    const auto exe = module_path() + L"\\sunshine_terminal_worker_probe.exe";
    WIN32_FILE_ATTRIBUTE_DATA exe_attributes {};
    if (module_path().empty() || !GetFileAttributesExW(exe.c_str(), GetFileExInfoStandard, &exe_attributes) || (exe_attributes.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
      error = "Internal worker test host is not a regular file beside the service.";
      CloseHandle(job);
      return std::nullopt;
    }
    std::wstringstream command;
    command << L'"' << exe << L"\" --pipe=" << std::wstring(pipe_name_.begin(), pipe_name_.end())
            << L" --broker-pid=" << GetCurrentProcessId()
            << L" --rtsp=" << request.resource.rtsp_port << L" --control=" << request.resource.control_port
            << L" --video=" << request.resource.video_port << L" --audio=" << request.resource.audio_port;
    std::wstring command_line = command.str();
    STARTUPINFOW startup {};
    startup.cb = sizeof(startup);
    std::wstring desktop = std::wstring {request.resource.desktop_name.begin(), request.resource.desktop_name.end()};
    startup.lpDesktop = desktop.data();
    PROCESS_INFORMATION info {};
    if (!CreateProcessAsUserW(launch_token, exe.c_str(), command_line.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW | CREATE_SUSPENDED, nullptr, module_path().c_str(), &startup, &info)) {
      CloseHandle(job); error = "Private worker process launch failed."; return std::nullopt;
    }
    process_ = info.hProcess;
    job_ = job;
    pid_ = info.dwProcessId;
    auto fail_start = [&](std::string message) -> std::optional<route_t> {
      if (info.hThread) {
        CloseHandle(info.hThread);
        info.hThread = nullptr;
      }
      if (!stop({})) message += " Cleanup remains owned and must be retried.";
      error = std::move(message);
      return std::nullopt;
    };
    if (!process_matches_resource(info.hProcess, request.resource)) {
      return fail_start("Worker token/session identity did not match provider ownership.");
    }
    if (!AssignProcessToJobObject(job, info.hProcess)) {
      return fail_start("Worker job assignment failed.");
    }
    if (ResumeThread(info.hThread) == static_cast<DWORD>(-1)) {
      return fail_start("Worker resume failed.");
    }
    CloseHandle(info.hThread);
    info.hThread = nullptr;

    platf::dxgi::FramedPipeFactory factory {std::make_unique<platf::dxgi::NamedPipeFactory>()};
    auto pipe = factory.create_client(pipe_name_);
    if (!pipe) { return fail_start("Worker readiness pipe unavailable."); }
    DWORD server_pid = 0;
    if (!pipe->get_server_process_id(server_pid) || server_pid != info.dwProcessId) { return fail_start("Worker pipe peer identity mismatch."); }
    protocol::request_t admission {protocol::opcode::prepare, request.admission.client_uuid, request.admission.generation, request.admission.launch_id, request.ticket};
    if (!request.ticket_validator || !request.ticket_validator(admission)) { return fail_start("Worker admission ticket validator is unavailable or rejected the ticket."); }
    const auto bytes = protocol::encode(admission);
    if (bytes.empty() || !pipe->send(bytes, 1500)) { return fail_start("Worker admission transfer failed."); }
    std::array<std::uint8_t, protocol::max_message_size> response_bytes {};
    std::size_t size = 0;
    if (pipe->receive(response_bytes, size, 1500) != platf::dxgi::PipeResult::Success) { return fail_start("Worker readiness timed out."); }
    auto response = protocol::decode_response(std::span<const std::uint8_t> {response_bytes.data(), size});
    if (!response || !response->accepted || response->client_uuid != request.admission.client_uuid || response->generation != request.admission.generation || response->launch_id != request.admission.launch_id || response->rtsp_port != request.resource.rtsp_port || response->control_port != request.resource.control_port || response->video_port != request.resource.video_port || response->audio_port != request.resource.audio_port) { return fail_start("Worker did not prove the reserved route."); }
    return route_t {.accepted = true, .ready = true, .rtsp_port = response->rtsp_port, .control_port = response->control_port, .video_port = response->video_port, .audio_port = response->audio_port};
#endif
  }

  bool process_t::stop(const route_t &) noexcept {
#ifdef _WIN32
    bool stopped = true;
    if (process_) {
      const auto process = static_cast<HANDLE>(process_);
      DWORD exit_code = STILL_ACTIVE;
      if (GetExitCodeProcess(process, &exit_code) && exit_code == STILL_ACTIVE) {
        if (!TerminateProcess(process, ERROR_PROCESS_ABORTED) || WaitForSingleObject(process, 5000) != WAIT_OBJECT_0) stopped = false;
      }
      if (stopped) { CloseHandle(process); process_ = nullptr; }
    }
    if (stopped && job_) { CloseHandle(static_cast<HANDLE>(job_)); job_ = nullptr; }
    if (!stopped) return false;
    pid_ = 0; pipe_name_.clear();
#endif
    return true;
  }
}

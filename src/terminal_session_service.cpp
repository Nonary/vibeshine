#include "terminal_session_service.h"

#ifdef _WIN32
  #include <array>
  #include <Windows.h>
  #include <cwctype>
#endif

namespace terminal_session::service {
  std::wstring expected_installed_image(std::wstring_view module_path, bool service_image) {
    const auto slash = module_path.find_last_of(L"\\/");
    if (slash == std::wstring_view::npos) return {};
    const std::wstring_view module_dir = module_path.substr(0, slash);
    const auto parent_slash = module_dir.find_last_of(L"\\/");
    const std::wstring_view directory_name = parent_slash == std::wstring_view::npos ? module_dir : module_dir.substr(parent_slash + 1);
    const bool module_is_in_tools = directory_name == L"tools" || directory_name == L"Tools" || directory_name == L"TOOLS";
    const std::wstring root = module_is_in_tools && parent_slash != std::wstring_view::npos
      ? std::wstring {module_dir.substr(0, parent_slash)}
      : std::wstring {module_dir};
    return root + (service_image ? L"\\tools\\sunshinesvc.exe" : L"\\sunshine.exe");
  }

  std::vector<std::uint8_t> endpoint_t::handle(std::span<const std::uint8_t> bytes, protocol::peer_identity_t peer) {
    protocol::response_t response;
    const auto request = protocol::decode_request(bytes);
    if (!request) {
      response.reason = protocol::reject_reason::malformed;
      response.error = "Malformed or oversized terminal-session request.";
      return protocol::encode(response);
    }
    auto authenticated = *request;
    authenticated.peer = std::move(peer);
    if (!authenticated.peer.authenticated || authenticated.peer.pid == 0 || authenticated.peer.sid.empty()) {
      response.reason = protocol::reject_reason::unauthenticated_peer;
      response.client_uuid = authenticated.client_uuid;
      response.generation = authenticated.generation;
      response.launch_id = authenticated.launch_id;
      response.error = "Terminal-session peer identity was not authenticated.";
      return protocol::encode(response);
    }
    if (authenticated.operation == protocol::opcode::control_challenge) {
      response.client_uuid = authenticated.client_uuid;
      response.generation = authenticated.generation;
      response.launch_id = authenticated.launch_id;
      response.ticket = admissions_.issue(authenticated.client_uuid, authenticated.generation, authenticated.launch_id, authenticated.peer,
                                          protocol::admission_authority::clock_t::now(), authenticated.ticket.operation);
      if (!response.ticket) {
        response.reason = protocol::reject_reason::worker_unavailable;
        response.error = "Terminal-session admission nonce generation failed.";
        return protocol::encode(response);
      }
      response.accepted = true;
      return protocol::encode(response);
    }
    if (const auto rejected = admissions_.consume(authenticated)) {
      response.reason = *rejected;
      response.client_uuid = authenticated.client_uuid;
      response.generation = authenticated.generation;
      response.launch_id = authenticated.launch_id;
      response.error = "Terminal-session admission rejected.";
      return protocol::encode(response);
    }
    if (!handler_) {
      response.reason = protocol::reject_reason::invalid_state;
      response.error = "Terminal-session service is not ready.";
      return protocol::encode(response);
    }
    response = handler_(authenticated);
    return protocol::encode(response);
  }

#ifdef _WIN32
  namespace {
    std::uint64_t process_creation_time(HANDLE process) {
      FILETIME created {}, exited {}, kernel {}, user {};
      if (!GetProcessTimes(process, &created, &exited, &kernel, &user)) return 0;
      ULARGE_INTEGER value {};
      value.LowPart = created.dwLowDateTime;
      value.HighPart = created.dwHighDateTime;
      return value.QuadPart;
    }
    std::wstring canonical_image(std::wstring path) {
      wchar_t full[MAX_PATH] {};
      const DWORD length = GetFullPathNameW(path.c_str(), _countof(full), full, nullptr);
      if (length != 0 && length < _countof(full)) path.assign(full, length);
      for (auto &ch : path) ch = static_cast<wchar_t>(std::towlower(ch));
      return path;
    }
    std::wstring current_module_path() {
      wchar_t module[MAX_PATH] {};
      const DWORD length = GetModuleFileNameW(nullptr, module, _countof(module));
      return length && length < _countof(module) ? std::wstring {module, length} : std::wstring {};
    }
    bool expected_main_peer(DWORD pid, std::uint64_t expected_creation) {
      winrt::handle process {OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid)};
      if (!process) return false;
      if (!expected_creation || process_creation_time(process.get()) != expected_creation) return false;
      DWORD session = 0;
      if (!ProcessIdToSessionId(pid, &session) || session != WTSGetActiveConsoleSessionId()) return false;
      wchar_t image[MAX_PATH] {};
      DWORD length = _countof(image);
      if (!QueryFullProcessImageNameW(process.get(), 0, image, &length)) return false;
      return canonical_image(std::wstring {image, length}) == canonical_image(expected_installed_image(current_module_path(), false));
    }
  }

  namespace {
    bool expected_service_peer(platf::dxgi::INamedPipe &pipe) {
      DWORD pid = 0;
      if (!pipe.get_server_process_id(pid)) return false;
      winrt::handle process {OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid)};
      if (!process) return false;
      HANDLE token_raw = nullptr;
      if (!OpenProcessToken(process.get(), TOKEN_QUERY, &token_raw)) return false;
      winrt::handle token {token_raw};
      DWORD size = 0;
      GetTokenInformation(token.get(), TokenUser, nullptr, 0, &size);
      auto user = std::make_unique<std::uint8_t[]>(size);
      if (!GetTokenInformation(token.get(), TokenUser, user.get(), size, &size)) return false;
      auto token_user = reinterpret_cast<TOKEN_USER *>(user.get());
      if (!IsWellKnownSid(token_user->User.Sid, WinLocalSystemSid)) return false;
      DWORD session = 0;
      if (!ProcessIdToSessionId(pid, &session) || session != 0) return false;
      wchar_t image[MAX_PATH] {};
      DWORD length = _countof(image);
      if (!QueryFullProcessImageNameW(process.get(), 0, image, &length)) return false;
      return canonical_image(std::wstring {image, length}) == canonical_image(expected_installed_image(current_module_path(), true));
    }
  }

  pipe_server_t::pipe_server_t(handler_t handler, peer_validator_t validator): endpoint_(std::move(handler)), validator_(std::move(validator)) {}
  pipe_server_t::~pipe_server_t() { stop(); }

  bool pipe_server_t::start() {
    if (running_.exchange(true)) return true;
    thread_ = std::jthread([this](std::stop_token) { run(); });
    return true;
  }

  void pipe_server_t::stop() {
    if (!running_.exchange(false)) return;
    if (thread_.joinable()) thread_.request_stop();
    if (thread_.joinable()) thread_.join();
  }

  void pipe_server_t::run() {
    // The service account creates the endpoint. NamedPipeFactory supplies a
    // non-inheritable DACL; peer PID/SID are checked again by endpoint_t.
    platf::dxgi::FramedPipeFactory factory {std::make_unique<platf::dxgi::NamedPipeFactory>()};
    while (running_.load()) {
      auto pipe = factory.create_server(std::string {broker_pipe_name});
      if (!pipe) { running_.store(false); return; }
      pipe->wait_for_client_connection(250);
      if (!pipe->is_connected()) continue;
      std::array<std::uint8_t, protocol::max_message_size> buffer {};
      std::size_t bytes = 0;
      const auto receive = pipe->receive(buffer, bytes, 1500);
      if (receive == platf::dxgi::PipeResult::Success && bytes <= buffer.size()) {
        DWORD pid = 0;
        std::wstring sid;
        std::uint64_t creation_time = 0;
        const bool has_pid = pipe->get_client_process_id(pid);
        const bool has_sid = pipe->get_client_user_sid_string(sid);
        const bool has_creation = pipe->get_client_process_creation_time(creation_time);
        std::string sid_utf8;
        if (has_sid) sid_utf8.assign(sid.begin(), sid.end());
        protocol::peer_identity_t identity {.pid = has_pid ? static_cast<std::uint32_t>(pid) : 0, .sid = std::move(sid_utf8), .creation_time = creation_time, .authenticated = has_pid && has_sid && has_creation && expected_main_peer(pid, creation_time)};
        if (validator_ && !validator_(identity)) identity.authenticated = false;
        const auto reply = endpoint_.handle(std::span<const std::uint8_t> {buffer.data(), bytes}, identity);
        if (!reply.empty()) (void) pipe->send(reply, 1500);
      }
      pipe->disconnect();
    }
  }

  std::optional<protocol::response_t> pipe_client_t::transact(const protocol::request_t &request, int timeout_ms) {
    platf::dxgi::FramedPipeFactory factory {std::make_unique<platf::dxgi::NamedPipeFactory>()};
    auto pipe = factory.create_client(std::string {broker_pipe_name});
    if (!pipe) return std::nullopt;
    if (!expected_service_peer(*pipe)) return std::nullopt;
    const auto encoded = protocol::encode(request);
    if (encoded.empty() || encoded.size() > protocol::max_message_size || !pipe->send(encoded, timeout_ms)) return std::nullopt;
    std::array<std::uint8_t, protocol::max_message_size> buffer {};
    std::size_t bytes = 0;
    if (pipe->receive(buffer, bytes, timeout_ms) != platf::dxgi::PipeResult::Success || bytes > buffer.size()) return std::nullopt;
    if (!expected_service_peer(*pipe)) return std::nullopt;
    auto response = protocol::decode_response(std::span<const std::uint8_t> {buffer.data(), bytes});
    if (!response || response->client_uuid != request.client_uuid || response->generation != request.generation || response->launch_id != request.launch_id) return std::nullopt;
    return response;
  }
#endif
}

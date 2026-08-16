#include "terminal_session_display_protocol.h"

#ifdef _WIN32
  #include "src/platform/windows/display_helper_session.h"
  #include "src/platform/windows/ipc/pipes.h"

  #include <atomic>
  #include <array>
  #include <chrono>
  #include <cwctype>
  #include <optional>
  #include <vector>
  #include <span>

namespace terminal_session::display {
  namespace {
    std::atomic<std::uint64_t> next_request_id {1};

    std::wstring canonical_image(std::wstring path) {
      wchar_t full[MAX_PATH] {};
      const DWORD length = GetFullPathNameW(path.c_str(), _countof(full), full, nullptr);
      if (length != 0 && length < _countof(full)) path.assign(full, length);
      for (auto &ch : path) ch = static_cast<wchar_t>(std::towlower(ch));
      return path;
    }

    std::wstring expected_broker_image() {
      wchar_t module[MAX_PATH] {};
      const DWORD length = GetModuleFileNameW(nullptr, module, _countof(module));
      if (!length || length >= _countof(module)) return {};
      std::wstring path {module, length};
      const auto slash = path.find_last_of(L"\\/");
      if (slash == std::wstring::npos) return {};
      const auto tools = path.substr(0, slash);
      const auto parent_slash = tools.find_last_of(L"\\/");
      if (parent_slash == std::wstring::npos) return {};
      return tools.substr(0, parent_slash) + L"\\tools\\sunshinesvc.exe";
    }

    bool authenticate_broker_peer(platf::dxgi::INamedPipe &pipe) {
      DWORD pid = 0;
      if (!pipe.get_server_process_id(pid) || !pid) return false;
      winrt::handle process {OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid)};
      if (!process) return false;
      HANDLE raw_token = nullptr;
      if (!OpenProcessToken(process.get(), TOKEN_QUERY, &raw_token)) return false;
      winrt::handle token {raw_token};
      DWORD size = 0;
      (void) GetTokenInformation(token.get(), TokenUser, nullptr, 0, &size);
      if (!size) return false;
      std::vector<std::uint8_t> user(size);
      if (!GetTokenInformation(token.get(), TokenUser, user.data(), size, &size)) return false;
      std::array<std::byte, SECURITY_MAX_SID_SIZE> system_storage {};
      DWORD system_size = static_cast<DWORD>(system_storage.size());
      if (!CreateWellKnownSid(WinLocalSystemSid, nullptr, system_storage.data(), &system_size) ||
          !EqualSid(reinterpret_cast<TOKEN_USER *>(user.data())->User.Sid, system_storage.data())) return false;
      DWORD session = 0;
      if (!ProcessIdToSessionId(pid, &session) || session != 0) return false;
      wchar_t image[MAX_PATH] {};
      DWORD image_length = _countof(image);
      return QueryFullProcessImageNameW(process.get(), 0, image, &image_length) &&
             canonical_image(std::wstring {image, image_length}) == canonical_image(expected_broker_image());
    }
  }

  std::optional<response_t> transact(const operation operation_code,
                                     const std::uint64_t generation,
                                     const std::uint32_t width,
                                     const std::uint32_t height,
                                     const std::uint32_t refresh_rate_millihz,
                                     const std::uint32_t hdr_enabled) {
    const auto pipe_name = display_helper_session::display_pipe_name();
    if (!pipe_name || !display_helper_session::managed_context_is_valid() ||
        generation == 0 || generation != display_helper_session::managed_generation()) {
      return std::nullopt;
    }

    request_t request {
      .operation = static_cast<std::uint8_t>(operation_code),
      .generation = generation,
      .request_id = next_request_id.fetch_add(1, std::memory_order_relaxed),
      .width = width,
      .height = height,
      .refresh_rate_millihz = refresh_rate_millihz,
      .hdr_enabled = hdr_enabled,
    };
    if (!valid_request(request)) return std::nullopt;

    platf::dxgi::FramedPipeFactory factory {std::make_unique<platf::dxgi::NamedPipeFactory>()};
    auto pipe = factory.create_client(*pipe_name);
    if (!pipe || !authenticate_broker_peer(*pipe)) return std::nullopt;
    if (!pipe->send(std::span<const std::uint8_t> {
          reinterpret_cast<const std::uint8_t *>(&request), sizeof(request)}, 5000)) {
      return std::nullopt;
    }
    std::array<std::uint8_t, max_message_size> bytes {};
    std::size_t size = 0;
    if (pipe->receive(bytes, size, 5000) != platf::dxgi::PipeResult::Success) return std::nullopt;
    response_t response {};
    if (!decode(bytes.data(), size, response) || !valid_response(response) ||
        response.operation != request.operation || response.request_id != request.request_id ||
        response.generation != request.generation) {
      return std::nullopt;
    }
    return response;
  }

  std::optional<response_t> transact_snapshot(
    const operation operation_code,
    const std::uint64_t generation,
    const std::uint32_t tier,
    const std::uint64_t sequence,
    const std::uint64_t display_id,
    const std::array<std::uint8_t, 32> &digest,
    const std::array<std::uint8_t, 32> &tag) {
    const auto pipe_name = display_helper_session::display_pipe_name();
    if (!pipe_name || !display_helper_session::managed_context_is_valid() ||
        generation == 0 || generation != display_helper_session::managed_generation() || tier > 2) {
      return std::nullopt;
    }
    request_t request {
      .operation = static_cast<std::uint8_t>(operation_code),
      .generation = generation,
      .request_id = next_request_id.fetch_add(1, std::memory_order_relaxed),
      .snapshot_tier = tier,
      .snapshot_sequence = sequence,
      .snapshot_display_id = display_id,
      .snapshot_digest = digest,
      .snapshot_tag = tag,
    };
    if (!valid_request(request)) return std::nullopt;

    platf::dxgi::FramedPipeFactory factory {std::make_unique<platf::dxgi::NamedPipeFactory>()};
    auto pipe = factory.create_client(*pipe_name);
    if (!pipe || !authenticate_broker_peer(*pipe) ||
        !pipe->send(std::span<const std::uint8_t> {
          reinterpret_cast<const std::uint8_t *>(&request), sizeof(request)}, 5000)) {
      return std::nullopt;
    }
    std::array<std::uint8_t, max_message_size> bytes {};
    std::size_t size = 0;
    if (pipe->receive(bytes, size, 5000) != platf::dxgi::PipeResult::Success) return std::nullopt;
    response_t response {};
    if (!decode(bytes.data(), size, response) || !valid_response(response) ||
        response.operation != request.operation || response.request_id != request.request_id ||
        response.generation != request.generation) {
      return std::nullopt;
    }
    return response;
  }
}
#endif

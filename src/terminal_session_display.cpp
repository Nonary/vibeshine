#include "terminal_session_display_protocol.h"

#ifdef _WIN32
  #include "terminal_session_service.h"
  #include "src/platform/windows/display_helper_session.h"
  #include "src/platform/windows/ipc/pipes.h"

  #include <atomic>
  #include <array>
  #include <chrono>
  #include <optional>
  #include <span>

namespace terminal_session::display {
  namespace {
    std::atomic<std::uint64_t> next_request_id {1};
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
    if (!pipe || !service::authenticate_service_peer(*pipe)) return std::nullopt;
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
}
#endif

#pragma once

#include "terminal_session_protocol.h"

#include <functional>
#include <utility>
#include <thread>
#include <atomic>
#include <string_view>
#include <string>

#ifdef _WIN32
  #include "platform/windows/ipc/pipes.h"
#endif

namespace terminal_session::service {
  /**
   * Transport-neutral service endpoint. The Windows adapter is expected to
   * use NamedPipeFactory/FramedPipe, whose SYSTEM/user SID ACL is the first
   * authentication gate; this endpoint applies the bounded protocol and
   * one-use admission gate before invoking seat orchestration.
   */
  class endpoint_t {
  public:
    using handler_t = std::function<protocol::response_t(const protocol::request_t &)>;

    explicit endpoint_t(handler_t handler): handler_(std::move(handler)) {}
    [[nodiscard]] std::vector<std::uint8_t> handle(std::span<const std::uint8_t> bytes, protocol::peer_identity_t peer);
    [[nodiscard]] protocol::admission_authority &admissions() { return admissions_; }

  private:
    protocol::admission_authority admissions_;
    handler_t handler_;
  };

  inline constexpr std::string_view broker_pipe_name = "VibeshineTerminalBroker";

  // Installed layout keeps Sunshine.exe at the product root and the SCM
  // service at <root>\\tools\\sunshinesvc.exe. The helper is pure so both
  // directions of the peer check can be tested without a live service.
  [[nodiscard]] std::wstring expected_installed_image(std::wstring_view module_path, bool service_image);

#ifdef _WIN32
  /** The only production transport: a framed, ACL-protected local pipe. */
  class pipe_server_t {
  public:
    using handler_t = endpoint_t::handler_t;
    using peer_validator_t = std::function<bool(const protocol::peer_identity_t &)>;
    explicit pipe_server_t(handler_t handler, peer_validator_t validator = {});
    ~pipe_server_t();
    bool start();
    void stop();
    [[nodiscard]] bool running() const { return running_.load(); }

  private:
    void run();
    endpoint_t endpoint_;
    peer_validator_t validator_;
    std::atomic<bool> running_ {false};
    std::jthread thread_;
  };

  class pipe_client_t {
  public:
    [[nodiscard]] static std::optional<protocol::response_t> transact(const protocol::request_t &request, int timeout_ms = 1500);
  };
#endif
}

#pragma once

#include "terminal_session_protocol.h"

#include <functional>
#include <utility>

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
}

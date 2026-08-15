#include "terminal_session_service.h"

namespace terminal_session::service {
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
}

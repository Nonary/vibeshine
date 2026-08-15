/**
 * @file src/terminal_session_protocol.h
 * @brief Bounded, one-use admission protocol for private terminal seats.
 *
 * The protocol is intentionally transport independent. Windows production
 * transport uses the protected local pipe implementation; tests use the same
 * codec and admission authority without requiring a Windows session.
 */
#pragma once

#include <array>
#include <chrono>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace terminal_session::protocol {
  constexpr std::uint32_t magic = 0x31534254; // "TSB1"
  constexpr std::uint16_t version = 1;
  constexpr std::size_t max_message_size = 4096;
  constexpr std::size_t max_uuid_size = 128;
  constexpr std::size_t max_sid_size = 184;
  constexpr std::size_t max_error_size = 256;
  constexpr auto ticket_lifetime = std::chrono::seconds {10};

  enum class opcode : std::uint8_t { prepare = 1, resume = 2, release = 3, reply = 4, reject = 5 };

  enum class reject_reason : std::uint8_t {
    malformed,
    unsupported_version,
    unauthenticated_peer,
    wrong_client,
    stale_generation,
    replayed_ticket,
    expired_ticket,
    invalid_state,
    provider_unavailable,
    worker_unavailable,
  };

  struct peer_identity_t {
    std::uint32_t pid {};
    std::string sid;
    bool authenticated {};
  };

  struct ticket_t {
    std::string client_uuid;
    std::uint64_t generation {};
    std::uint32_t launch_id {};
    std::array<std::uint8_t, 16> nonce {};
    std::chrono::steady_clock::time_point expires_at {};
  };

  struct request_t {
    opcode operation {opcode::prepare};
    std::string client_uuid;
    std::uint64_t generation {};
    std::uint32_t launch_id {};
    ticket_t ticket;
    peer_identity_t peer;
  };

  struct response_t {
    bool accepted {};
    reject_reason reason {reject_reason::malformed};
    std::string error;
    std::string client_uuid;
    std::uint64_t generation {};
    std::uint32_t launch_id {};
    std::uint32_t windows_session_id {};
    std::string seat_id;
    std::uint16_t rtsp_port {};
    std::uint16_t control_port {};
    std::uint16_t video_port {};
    std::uint16_t audio_port {};
  };

  [[nodiscard]] std::vector<std::uint8_t> encode(const request_t &request);
  [[nodiscard]] std::optional<request_t> decode_request(std::span<const std::uint8_t> bytes);
  [[nodiscard]] std::vector<std::uint8_t> encode(const response_t &response);
  [[nodiscard]] std::optional<response_t> decode_response(std::span<const std::uint8_t> bytes);

  class admission_authority {
  public:
    using clock_t = std::chrono::steady_clock;

    admission_authority();
    explicit admission_authority(std::uint64_t seed);

    [[nodiscard]] ticket_t issue(std::string_view client_uuid, std::uint64_t generation, std::uint32_t launch_id,
                                 clock_t::time_point now = clock_t::now());
    [[nodiscard]] std::optional<reject_reason> consume(const request_t &request,
                                                        clock_t::time_point now = clock_t::now());
    void expire(clock_t::time_point now = clock_t::now());

  private:
    struct ticket_record { ticket_t ticket; bool used {}; };
    std::array<std::uint8_t, 16> next_nonce();
    std::uint64_t counter_ {};
    std::vector<ticket_record> tickets_;
  };
} // namespace terminal_session::protocol

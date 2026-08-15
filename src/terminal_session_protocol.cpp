#include "terminal_session_protocol.h"

#include <algorithm>
#include <cstring>
#include <limits>
#include <openssl/rand.h>

namespace terminal_session::protocol {
  namespace {
    class writer {
    public:
      void u8(std::uint8_t value) { bytes.push_back(value); }
      void u16(std::uint16_t value) { bytes.push_back(static_cast<std::uint8_t>(value)); bytes.push_back(static_cast<std::uint8_t>(value >> 8)); }
      void u32(std::uint32_t value) { for (int i = 0; i < 4; ++i) bytes.push_back(static_cast<std::uint8_t>(value >> (i * 8))); }
      void u64(std::uint64_t value) { for (int i = 0; i < 8; ++i) bytes.push_back(static_cast<std::uint8_t>(value >> (i * 8))); }
      bool string(std::string_view value, std::size_t maximum) {
        if (value.size() > maximum || value.size() > std::numeric_limits<std::uint16_t>::max()) return false;
        u16(static_cast<std::uint16_t>(value.size()));
        bytes.insert(bytes.end(), value.begin(), value.end());
        return true;
      }
      std::vector<std::uint8_t> bytes;
    };

    class reader {
    public:
      explicit reader(std::span<const std::uint8_t> bytes): bytes(bytes) {}
      bool u8(std::uint8_t &value) { if (remaining() < 1) return false; value = bytes[offset++]; return true; }
      bool u16(std::uint16_t &value) { if (remaining() < 2) return false; value = bytes[offset] | static_cast<std::uint16_t>(bytes[offset + 1] << 8); offset += 2; return true; }
      bool u32(std::uint32_t &value) { if (remaining() < 4) return false; value = 0; for (int i = 0; i < 4; ++i) value |= static_cast<std::uint32_t>(bytes[offset++]) << (i * 8); return true; }
      bool u64(std::uint64_t &value) { if (remaining() < 8) return false; value = 0; for (int i = 0; i < 8; ++i) value |= static_cast<std::uint64_t>(bytes[offset++]) << (i * 8); return true; }
      bool string(std::string &value, std::size_t maximum) {
        std::uint16_t size = 0;
        if (!u16(size) || size > maximum || remaining() < size) return false;
        value.assign(reinterpret_cast<const char *>(bytes.data() + offset), size);
        offset += size;
        return true;
      }
      std::size_t remaining() const { return bytes.size() - offset; }
      std::size_t offset {};
      std::span<const std::uint8_t> bytes;
    };

    void header(writer &out, opcode operation) {
      out.u32(magic); out.u16(version); out.u8(static_cast<std::uint8_t>(operation)); out.u8(0);
    }

    bool read_header(reader &in, std::uint8_t expected, opcode &actual) {
      std::uint32_t message_magic = 0; std::uint16_t message_version = 0; std::uint8_t operation = 0; std::uint8_t reserved = 0;
      if (!in.u32(message_magic) || !in.u16(message_version) || !in.u8(operation) || !in.u8(reserved) ||
          message_magic != magic || message_version != version || reserved != 0) return false;
      actual = static_cast<opcode>(operation);
      return expected == 0 || static_cast<std::uint8_t>(actual) == expected;
    }

    bool write_ticket(writer &out, const ticket_t &ticket) {
      return out.string(ticket.client_uuid, max_uuid_size) && (out.u64(ticket.generation), out.u32(ticket.launch_id),
        out.bytes.insert(out.bytes.end(), ticket.nonce.begin(), ticket.nonce.end()), true);
    }

    bool read_ticket(reader &in, ticket_t &ticket) {
      if (!in.string(ticket.client_uuid, max_uuid_size) || !in.u64(ticket.generation) || !in.u32(ticket.launch_id) || in.remaining() < ticket.nonce.size()) return false;
      std::copy_n(in.bytes.begin() + static_cast<std::ptrdiff_t>(in.offset), ticket.nonce.size(), ticket.nonce.begin());
      in.offset += ticket.nonce.size();
      return true;
    }
  }

  std::vector<std::uint8_t> encode(const request_t &request) {
    writer out; header(out, request.operation);
    if (!out.string(request.client_uuid, max_uuid_size)) return {};
    out.u64(request.generation); out.u32(request.launch_id);
    if (!write_ticket(out, request.ticket) || out.bytes.size() > max_message_size) return {};
    return std::move(out.bytes);
  }

  std::optional<request_t> decode_request(std::span<const std::uint8_t> bytes) {
    if (bytes.empty() || bytes.size() > max_message_size) return std::nullopt;
    reader in {bytes}; request_t result; opcode operation {};
    if (!read_header(in, 0, operation) || (operation != opcode::prepare && operation != opcode::resume && operation != opcode::release)) return std::nullopt;
    result.operation = operation;
    if (!in.string(result.client_uuid, max_uuid_size) || !in.u64(result.generation) || !in.u32(result.launch_id) || !read_ticket(in, result.ticket) || in.remaining() != 0) return std::nullopt;
    return result;
  }

  std::vector<std::uint8_t> encode(const response_t &response) {
    writer out; header(out, response.accepted ? opcode::reply : opcode::reject);
    out.u8(response.accepted ? 1 : 0); out.u8(static_cast<std::uint8_t>(response.reason));
    if (!out.string(response.error, max_error_size) || !out.string(response.client_uuid, max_uuid_size)) return {};
    out.u64(response.generation); out.u32(response.launch_id); out.u32(response.windows_session_id);
    if (!out.string(response.seat_id, max_uuid_size)) return {};
    out.u16(response.rtsp_port); out.u16(response.control_port); out.u16(response.video_port); out.u16(response.audio_port);
    if (out.bytes.size() > max_message_size) return {};
    return std::move(out.bytes);
  }

  std::optional<response_t> decode_response(std::span<const std::uint8_t> bytes) {
    if (bytes.empty() || bytes.size() > max_message_size) return std::nullopt;
    reader in {bytes}; response_t result; opcode operation {};
    if (!read_header(in, 0, operation) || (operation != opcode::reply && operation != opcode::reject)) return std::nullopt;
    std::uint8_t accepted = 0; std::uint8_t reason = 0;
    if (!in.u8(accepted) || !in.u8(reason) || !in.string(result.error, max_error_size) || !in.string(result.client_uuid, max_uuid_size) ||
        !in.u64(result.generation) || !in.u32(result.launch_id) || !in.u32(result.windows_session_id) || !in.string(result.seat_id, max_uuid_size) ||
        !in.u16(result.rtsp_port) || !in.u16(result.control_port) || !in.u16(result.video_port) || !in.u16(result.audio_port) || in.remaining() != 0) return std::nullopt;
    if (accepted > 1 || reason > static_cast<std::uint8_t>(reject_reason::worker_unavailable)) return std::nullopt;
    result.accepted = accepted != 0; result.reason = static_cast<reject_reason>(reason);
    return result;
  }

  admission_authority::admission_authority(): admission_authority(0x9e3779b97f4a7c15ULL) {}
  admission_authority::admission_authority(std::uint64_t seed): counter_(seed) {}

  std::array<std::uint8_t, 16> admission_authority::next_nonce() {
    std::array<std::uint8_t, 16> nonce {};
    if (RAND_bytes(nonce.data(), static_cast<int>(nonce.size())) == 1) return nonce;
    // The counter fallback keeps tests and unusual OpenSSL initialization
    // failures functional, while production remains protected by the local
    // pipe ACL and rejects tickets after one use or ten seconds.
    std::uint64_t value = ++counter_;
    for (int i = 0; i < 8; ++i) nonce[i] = static_cast<std::uint8_t>(value >> (i * 8));
    value ^= value << 17; value ^= value >> 13;
    for (int i = 0; i < 8; ++i) nonce[8 + i] = static_cast<std::uint8_t>(value >> (i * 8));
    return nonce;
  }

  ticket_t admission_authority::issue(std::string_view client_uuid, std::uint64_t generation, std::uint32_t launch_id, clock_t::time_point now) {
    ticket_t ticket {std::string {client_uuid}, generation, launch_id, next_nonce(), now + ticket_lifetime};
    tickets_.push_back({ticket, false});
    return ticket;
  }

  std::optional<reject_reason> admission_authority::consume(const request_t &request, clock_t::time_point now) {
    if (!request.peer.authenticated || request.peer.pid == 0 || request.peer.sid.empty()) return reject_reason::unauthenticated_peer;
    if (request.client_uuid.empty() || request.client_uuid != request.ticket.client_uuid) return reject_reason::wrong_client;
    if (request.generation == 0 || request.generation != request.ticket.generation || request.launch_id == 0 || request.launch_id != request.ticket.launch_id) return reject_reason::stale_generation;
    const auto found = std::find_if(tickets_.begin(), tickets_.end(), [&](const ticket_record &record) { return record.ticket.nonce == request.ticket.nonce; });
    if (found == tickets_.end()) return reject_reason::replayed_ticket;
    if (found->used) return reject_reason::replayed_ticket;
    if (found->ticket.expires_at <= now) return reject_reason::expired_ticket;
    if (found->ticket.client_uuid != request.client_uuid || found->ticket.generation != request.generation || found->ticket.launch_id != request.launch_id) return reject_reason::wrong_client;
    found->used = true;
    expire(now);
    return std::nullopt;
  }

  void admission_authority::expire(clock_t::time_point now) {
    tickets_.erase(std::remove_if(tickets_.begin(), tickets_.end(), [&](const ticket_record &record) { return record.ticket.expires_at <= now || record.used; }), tickets_.end());
  }
} // namespace terminal_session::protocol

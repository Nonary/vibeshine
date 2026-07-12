#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace platf::display_helper_protocol {
  /**
   * @brief IPC message types shared by Sunshine and both display helpers.
   *
   * Correlated request types intentionally use new message IDs. An older helper
   * will ignore them instead of executing an operation that cannot honor the
   * request deadline or acknowledgement contract.
   */
  enum class MsgType : std::uint8_t {
    Apply = 1,
    Revert = 2,
    Reset = 3,
    ExportGolden = 4,
    LogLevel = 5,
    ApplyResult = 6,
    Disarm = 7,  // Legacy, unacknowledged request.
    SnapshotCurrent = 8,  // Legacy, unacknowledged request.
    VerificationResult = 9,
    SnapshotCurrentRequest = 10,
    SnapshotCurrentResult = 11,
    DisarmRequest = 12,
    DisarmResult = 13,
    ApplyRequest = 16,
    ApplyResultCorrelated = 17,
    VerificationResultCorrelated = 18,
    Ping = 0xFE,
    Stop = 0xFF,
  };

  enum class ResultStatus : std::uint8_t {
    Failed = 0,
    Succeeded = 1,
    Busy = 2,
    Expired = 3,
    Invalid = 4,
  };

  inline constexpr std::uint8_t kCorrelatedProtocolVersion = 1;
  inline constexpr std::size_t kCorrelatedRequestHeaderSize = 17;
  inline constexpr std::size_t kCorrelatedResultSize = 10;

  struct CorrelatedRequestView {
    std::uint64_t request_id = 0;
    std::uint64_t not_after_tick_ms = 0;
    std::span<const std::uint8_t> body;
  };

  struct CorrelatedResult {
    std::uint64_t request_id = 0;
    ResultStatus status = ResultStatus::Invalid;
  };

  inline void append_u64_le(std::vector<std::uint8_t> &out, std::uint64_t value) {
    for (unsigned int shift = 0; shift < 64; shift += 8) {
      out.push_back(static_cast<std::uint8_t>((value >> shift) & 0xFFu));
    }
  }

  inline std::optional<std::uint64_t> read_u64_le(std::span<const std::uint8_t> bytes, std::size_t offset) {
    if (offset > bytes.size() || bytes.size() - offset < sizeof(std::uint64_t)) {
      return std::nullopt;
    }

    std::uint64_t value = 0;
    for (unsigned int shift = 0; shift < 64; shift += 8) {
      value |= static_cast<std::uint64_t>(bytes[offset + (shift / 8)]) << shift;
    }
    return value;
  }

  inline std::vector<std::uint8_t> encode_correlated_request(
    std::uint64_t request_id,
    std::uint64_t not_after_tick_ms,
    std::span<const std::uint8_t> body = {}
  ) {
    std::vector<std::uint8_t> payload;
    payload.reserve(kCorrelatedRequestHeaderSize + body.size());
    payload.push_back(kCorrelatedProtocolVersion);
    append_u64_le(payload, request_id);
    append_u64_le(payload, not_after_tick_ms);
    payload.insert(payload.end(), body.begin(), body.end());
    return payload;
  }

  inline std::optional<CorrelatedRequestView> decode_correlated_request(std::span<const std::uint8_t> payload) {
    if (payload.size() < kCorrelatedRequestHeaderSize || payload.front() != kCorrelatedProtocolVersion) {
      return std::nullopt;
    }

    const auto request_id = read_u64_le(payload, 1);
    const auto not_after_tick_ms = read_u64_le(payload, 9);
    if (!request_id || *request_id == 0 || !not_after_tick_ms || *not_after_tick_ms == 0) {
      return std::nullopt;
    }

    return CorrelatedRequestView {
      *request_id,
      *not_after_tick_ms,
      payload.subspan(kCorrelatedRequestHeaderSize),
    };
  }

  inline std::vector<std::uint8_t> encode_correlated_result(std::uint64_t request_id, ResultStatus status) {
    std::vector<std::uint8_t> payload;
    payload.reserve(kCorrelatedResultSize);
    payload.push_back(kCorrelatedProtocolVersion);
    append_u64_le(payload, request_id);
    payload.push_back(static_cast<std::uint8_t>(status));
    return payload;
  }

  inline std::optional<CorrelatedResult> decode_correlated_result(std::span<const std::uint8_t> payload) {
    if (payload.size() != kCorrelatedResultSize || payload.front() != kCorrelatedProtocolVersion) {
      return std::nullopt;
    }

    const auto request_id = read_u64_le(payload, 1);
    if (!request_id || *request_id == 0) {
      return std::nullopt;
    }

    const auto raw_status = payload[9];
    if (raw_status > static_cast<std::uint8_t>(ResultStatus::Invalid)) {
      return std::nullopt;
    }
    return CorrelatedResult {*request_id, static_cast<ResultStatus>(raw_status)};
  }
}  // namespace platf::display_helper_protocol

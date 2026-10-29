// SPDX-License-Identifier: GPL-3.0-or-later
// Client-side cursor control-stream extension (docs/client-cursor-design.md).
// Versioned Vibeshine extension. All multibyte fields are little-endian; the
// client copy lives in moonlight-common-c/src/HostCursor.h.
#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace host_cursor_protocol {
  // Advertised in a=x-ss-general.featureFlags. A fork-owned high bit: upstream
  // allocates this space sequentially from 0x01.
  constexpr std::uint32_t feature_flag = 0x00010000;

  constexpr std::uint16_t shape_packet_type = 0x5610;  // host -> client
  constexpr std::uint16_t state_packet_type = 0x5611;  // host -> client
  constexpr std::uint16_t control_packet_type = 0x5612;  // client -> host

  // Reliable channel for host -> client cursor traffic, so a large shape never
  // blocks the reliable feedback on channel 0.
  constexpr std::uint8_t channel = 0x09;

  constexpr std::uint8_t version = 1;
  constexpr std::uint8_t format_bgra8 = 1;  // B8G8R8A8, straight alpha, top-down, packed
  constexpr std::uint16_t max_dimension = 256;
  constexpr std::size_t max_chunk_bytes = 16384;
  constexpr std::size_t shape_header_size = 28;
  constexpr std::size_t state_payload_size = 44;
  constexpr std::size_t control_payload_size = 8;
  constexpr std::size_t control_header_size = 4;  // control_header_v2: type, payloadLength

  namespace state_flags {
    constexpr std::uint8_t visible = 0x01;
    constexpr std::uint8_t fallback = 0x02;
    constexpr std::uint8_t has_position = 0x04;
    constexpr std::uint8_t has_clip = 0x08;
    constexpr std::uint8_t composited = 0x10;
    constexpr std::uint8_t resync = 0x20;
    constexpr std::uint8_t all = 0x3F;
  }  // namespace state_flags

  namespace control_flags {
    constexpr std::uint8_t subscribe = 0x01;
    constexpr std::uint8_t client_draws = 0x02;
    constexpr std::uint8_t all = 0x03;
  }  // namespace control_flags

  struct state_t {
    std::uint8_t flags = 0;
    std::uint32_t seq = 0;
    std::uint32_t shape_id = 0;
    std::uint16_t ref_width = 0;
    std::uint16_t ref_height = 0;
    std::uint32_t scale_q16 = 0x10000;
    std::int32_t x = 0;
    std::int32_t y = 0;
    std::int32_t clip_left = 0;
    std::int32_t clip_top = 0;
    std::int32_t clip_right = 0;
    std::int32_t clip_bottom = 0;

    bool operator==(const state_t &) const = default;
  };

  struct control_t {
    std::uint8_t flags = 0;
    std::uint32_t resend_shape_id = 0;

    bool operator==(const control_t &) const = default;
  };

  namespace detail {
    inline void put16(std::uint8_t *p, std::uint16_t v) {
      p[0] = std::uint8_t(v);
      p[1] = std::uint8_t(v >> 8);
    }

    inline void put32(std::uint8_t *p, std::uint32_t v) {
      put16(p, std::uint16_t(v));
      put16(p + 2, std::uint16_t(v >> 16));
    }

    inline std::uint32_t get32(const std::uint8_t *p) {
      return std::uint32_t(p[0]) | std::uint32_t(p[1]) << 8 | std::uint32_t(p[2]) << 16 | std::uint32_t(p[3]) << 24;
    }

    inline std::vector<std::uint8_t> with_header(std::uint16_t type, std::size_t payload_size) {
      std::vector<std::uint8_t> out(control_header_size + payload_size);
      put16(out.data(), type);
      put16(out.data() + 2, std::uint16_t(payload_size));
      return out;
    }
  }  // namespace detail

  // Returns the control_header_v2 plaintext for one CURSOR_STATE message.
  inline std::vector<std::uint8_t> encode_state(const state_t &state) {
    auto out = detail::with_header(state_packet_type, state_payload_size);
    auto *p = out.data() + control_header_size;
    p[0] = version;
    p[1] = state.flags & state_flags::all;
    detail::put32(p + 4, state.seq);
    detail::put32(p + 8, state.shape_id);
    detail::put16(p + 12, state.ref_width);
    detail::put16(p + 14, state.ref_height);
    detail::put32(p + 16, state.scale_q16);
    detail::put32(p + 20, std::uint32_t(state.x));
    detail::put32(p + 24, std::uint32_t(state.y));
    detail::put32(p + 28, std::uint32_t(state.clip_left));
    detail::put32(p + 32, std::uint32_t(state.clip_top));
    detail::put32(p + 36, std::uint32_t(state.clip_right));
    detail::put32(p + 40, std::uint32_t(state.clip_bottom));
    return out;
  }

  // Splits one straight-alpha BGRA shape into CURSOR_SHAPE plaintexts of at most
  // max_chunk_bytes pixels each. Returns nothing for a shape the client would reject.
  inline std::vector<std::vector<std::uint8_t>> encode_shape(
    std::uint32_t shape_id,
    std::uint16_t width,
    std::uint16_t height,
    std::uint16_t hotspot_x,
    std::uint16_t hotspot_y,
    std::span<const std::uint8_t> bgra
  ) {
    std::vector<std::vector<std::uint8_t>> chunks;
    const std::size_t total = std::size_t(width) * height * 4;
    if (!shape_id || !width || !height || width > max_dimension || height > max_dimension ||
        hotspot_x >= width || hotspot_y >= height || bgra.size() != total) {
      return chunks;
    }
    for (std::size_t offset = 0; offset < total; offset += max_chunk_bytes) {
      const auto length = std::min(max_chunk_bytes, total - offset);
      auto out = detail::with_header(shape_packet_type, shape_header_size + length);
      auto *p = out.data() + control_header_size;
      p[0] = version;
      p[1] = format_bgra8;
      detail::put32(p + 4, shape_id);
      detail::put16(p + 8, width);
      detail::put16(p + 10, height);
      detail::put16(p + 12, hotspot_x);
      detail::put16(p + 14, hotspot_y);
      detail::put32(p + 16, std::uint32_t(total));
      detail::put32(p + 20, std::uint32_t(offset));
      detail::put16(p + 24, std::uint16_t(length));
      std::copy_n(bgra.begin() + offset, length, p + shape_header_size);
      chunks.push_back(std::move(out));
    }
    return chunks;
  }

  // Parses a CURSOR_CONTROL payload (after the control header). Anything that is
  // not exactly a version 1 message with known flags and zero reserved bytes is
  // rejected.
  inline std::optional<control_t> parse_control(std::span<const std::uint8_t> payload) {
    if (payload.size() != control_payload_size || payload[0] != version ||
        (payload[1] & ~control_flags::all) || payload[2] || payload[3]) {
      return std::nullopt;
    }
    return control_t {payload[1], detail::get32(payload.data() + 4)};
  }

  // Test helper and the reference encoding of the client message.
  inline std::array<std::uint8_t, control_payload_size> encode_control(const control_t &control) {
    std::array<std::uint8_t, control_payload_size> out {};
    out[0] = version;
    out[1] = control.flags & control_flags::all;
    detail::put32(out.data() + 4, control.resend_shape_id);
    return out;
  }
}  // namespace host_cursor_protocol

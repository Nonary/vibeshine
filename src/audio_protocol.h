/**
 * @file src/audio_protocol.h
 * @brief Bounded audio negotiation shared by the production launch/RTSP paths.
 */
#pragma once

#include "audio_policy.h"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>

namespace audio::protocol {
  inline constexpr int discrete_channels = 12;
  inline constexpr std::uint32_t discrete_wire_mask = 0xF63F;
  inline constexpr std::uint32_t discrete_wave_mask = 0x2D63F;
  inline constexpr int discrete_sample_rate = 48000;
  inline constexpr int discrete_duration_ms = 5;
  inline constexpr int discrete_default_bitrate = 600000;

  std::optional<std::uint32_t> parse_uint32(std::string_view value);
  std::optional<policy::stream_layout_t> parse_surround_params(std::string_view value);
  bool is_discrete_layout(const policy::stream_layout_t &layout);
  bool valid_launch_audio(std::uint32_t surround_info, std::string_view custom);

  // Legacy normal offers put LFE last; the real Common parser moves it back.
  // Custom normal/HQ offers must likewise be serialized in their respective order.
  std::string format_surround_params(policy::stream_layout_t layout, bool legacy_normal_rotation);
  policy::stream_layout_t discrete_layout();

  using attribute_map_t = std::unordered_map<std::string_view, std::string_view>;
  // Views remain valid while payload does. Reject duplicate audio attributes,
  // malformed lines, embedded NULs and bounded-size violations before indexing.
  bool parse_announce_attributes(std::string_view payload, attribute_map_t &attributes, std::string_view &client);
  bool validate_714_request(int channels, std::uint32_t wire_mask, int duration_ms, bool high_quality,
                            const std::optional<policy::stream_layout_t> &custom);
  bool valid_714_bitrate(int bitrate);
  // Conservative reservation includes 4+2 FEC, actual RTP/FEC headers, CBC's
  // mandatory padding block, IPv6/UDP and Ethernet framing/preamble/intergap.
  int reservation_714_kbps(int bitrate, bool encrypted);
}  // namespace audio::protocol

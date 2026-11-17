#include "audio_protocol.h"

#include <algorithm>
#include <array>
#include <limits>

namespace audio::protocol {
  std::optional<std::uint32_t> parse_uint32(std::string_view value) {
    if (value.empty() || value.size() > 10) return std::nullopt;
    std::uint32_t result = 0;
    for (const auto character : value) {
      if (character < '0' || character > '9') return std::nullopt;
      const auto digit = static_cast<std::uint32_t>(character - '0');
      if (result > (std::numeric_limits<std::uint32_t>::max() - digit) / 10) return std::nullopt;
      result = result * 10 + digit;
    }
    return result;
  }

  policy::stream_layout_t discrete_layout() {
    return {12, 8, 4, {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11}};
  }

  bool is_discrete_layout(const policy::stream_layout_t &layout) {
    const auto expected = discrete_layout();
    return layout.channels == expected.channels && layout.streams == expected.streams &&
           layout.coupled_streams == expected.coupled_streams && layout.mapping == expected.mapping;
  }

  std::optional<policy::stream_layout_t> parse_surround_params(std::string_view value) {
    if (value.empty() || value.size() > 128) return std::nullopt;
    std::array<std::uint32_t, 15> fields {};
    std::size_t count = 0;
    const bool comma_format = value.find(',') != std::string_view::npos;
    if (comma_format) {
      std::size_t begin = 0;
      do {
        if (count == fields.size()) return std::nullopt;
        const auto end = value.find(',', begin);
        const auto token = value.substr(begin, end == std::string_view::npos ? end : end - begin);
        const auto number = parse_uint32(token);
        if (!number || *number > 255) return std::nullopt;
        fields[count++] = *number;
        if (end == std::string_view::npos) break;
        begin = end + 1;
      } while (begin <= value.size());
    } else {
      if (value.size() > fields.size()) return std::nullopt;
      for (const auto character : value) {
        if (character < '0' || character > '9') return std::nullopt;
        fields[count++] = static_cast<std::uint32_t>(character - '0');
      }
    }
    if (count < 3) return std::nullopt;
    const auto channels = fields[0];
    if (comma_format ? channels != 12 : (channels != 2 && channels != 6 && channels != 8)) return std::nullopt;
    if (count != 3 + channels || fields[1] == 0 || fields[2] > fields[1] || fields[1] + fields[2] != channels) return std::nullopt;
    policy::stream_layout_t layout {static_cast<int>(channels), static_cast<int>(fields[1]), static_cast<int>(fields[2]), {}};
    std::array<bool, 12> mapped {};
    for (std::size_t index = 0; index < channels; ++index) {
      const auto coded_channel = fields[3 + index];
      // This is an encoder mapping. Each coded channel needs an input lane.
      if (coded_channel >= channels || mapped[coded_channel]) return std::nullopt;
      mapped[coded_channel] = true;
      layout.mapping[index] = static_cast<unsigned char>(coded_channel);
    }
    // Common's stereo decoder uses the fixed 1/1 identity topology regardless
    // of surround offers. Do not accept a custom stereo encoder it cannot select.
    if (channels == 2 && (layout.streams != 1 || layout.coupled_streams != 1 || layout.mapping[0] != 0 || layout.mapping[1] != 1)) return std::nullopt;
    if (channels == 12 && !is_discrete_layout(layout)) return std::nullopt;
    return layout;
  }

  bool valid_launch_audio(std::uint32_t surround_info, std::string_view custom) {
    const auto channels = static_cast<int>(surround_info & 0xFFFFU);
    if (policy::stream_index(channels, false) < 0) return false;
    if (channels == 12 && (surround_info >> 16) != discrete_wire_mask) return false;
    if (!custom.empty()) {
      const auto layout = parse_surround_params(custom);
      if (!layout || layout->channels != channels) return false;
    }
    return true;
  }

  std::string format_surround_params(policy::stream_layout_t layout, bool legacy_normal_rotation) {
    if (layout.channels != 2 && layout.channels != 6 && layout.channels != 8 && layout.channels != 12) return {};
    if (legacy_normal_rotation && (layout.channels == 6 || layout.channels == 8)) {
      std::rotate(layout.mapping.begin() + 3, layout.mapping.begin() + 4, layout.mapping.begin() + layout.channels);
    }
    std::string result;
    const bool comma_format = layout.channels == 12;
    const auto append = [&](int value) {
      if (comma_format && !result.empty()) result.push_back(',');
      result += std::to_string(value);
    };
    append(layout.channels);
    append(layout.streams);
    append(layout.coupled_streams);
    for (int index = 0; index < layout.channels; ++index) append(layout.mapping[index]);
    return result;
  }

  bool parse_announce_attributes(std::string_view payload, attribute_map_t &attributes, std::string_view &client) {
    attributes.clear();
    client = {};
    if (payload.size() > 65536 || payload.find('\0') != std::string_view::npos) return false;
    std::size_t begin = 0;
    std::size_t lines = 0;
    while (begin < payload.size()) {
      if (++lines > 256) return false;
      const auto end = payload.find('\n', begin);
      auto line = payload.substr(begin, end == std::string_view::npos ? end : end - begin);
      if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
      if (line.size() > 2048 || line.find('\r') != std::string_view::npos) return false;
      if (line.starts_with("s=")) client = line.substr(2);
      else if (line.starts_with("a=")) {
        const auto delimiter = line.find(':', 2);
        if (delimiter == std::string_view::npos || delimiter == 2) return false;
        const auto name = line.substr(2, delimiter - 2);
        auto value = line.substr(delimiter + 1);
        // GFE-style ANNOUNCE values may carry a single trailing space.
        if (!value.empty() && value.back() == ' ') value.remove_suffix(1);
        if (value.empty()) return false;
        const auto [entry, inserted] = attributes.emplace(name, value);
        if (!inserted && (name.starts_with("x-nv-audio.") || name.starts_with("x-nv-aqos.") || name == "x-ss-audio-codec-accept")) return false;
      }
      if (end == std::string_view::npos) break;
      begin = end + 1;
    }
    return true;
  }

  bool validate_714_request(int channels, std::uint32_t wire_mask, int duration_ms, bool high_quality,
                            const std::optional<policy::stream_layout_t> &custom) {
    return channels == 12 && wire_mask == discrete_wire_mask && duration_ms == 5 && !high_quality &&
           (!custom || is_discrete_layout(*custom));
  }

  bool valid_714_bitrate(int bitrate) {
    return bitrate == 600000 || bitrate == 1536000 || bitrate == 2048000;
  }

  int reservation_714_kbps(int bitrate, bool encrypted) {
    if (!valid_714_bitrate(bitrate)) return -1;
    const auto payload = bitrate / 8 / 200;
    const auto shard = encrypted ? (payload / 16 + 1) * 16 : payload;
    // 12-byte RTP, another 12-byte FEC header; 48-byte IPv6/UDP and 38-byte
    // Ethernet wire framing. No changes to legacy-profile budget accounting.
    const auto four_data_two_fec = 4 * (12 + shard + 86) + 2 * (24 + shard + 86);
    return (four_data_two_fec * 50 * 8 + 999) / 1000;
  }
}  // namespace audio::protocol

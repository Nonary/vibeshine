#pragma once
#include <array>
#include <cstdint>
#include <span>

namespace audio {
  struct atmos_format_t {
    std::uint32_t channels = 12, channel_mask = 0x2d63f;
    std::uint16_t container_bits = 32, valid_bits = 24;
    bool operator==(const atmos_format_t &) const = default;
    bool valid() const {
      const bool layout = (channels==2 && channel_mask==3) ||
        (channels==6 && (channel_mask==0x3f || channel_mask==0x60f)) ||
        (channels==8 && channel_mask==0x63f) || (channels==12 && channel_mask==0x2d63f);
      return layout && ((container_bits==32 && valid_bits==24) || (container_bits==16 && valid_bits==16));
    }
    std::size_t samples() const { return channels*240; }
    std::uint16_t block_align() const { return static_cast<std::uint16_t>(channels*container_bits/8); }
  };
  inline std::uint32_t atmos_default_mask(unsigned channels) {
    switch(channels) { case 2:return 3; case 6:return 0x3f; case 8:return 0x63f; case 12:return 0x2d63f; default:return 0; }
  }
}

/**
 * @file src/video_rtp_timing.h
 * @brief Preserve image-associated source timing when packetizing video.
 */
#pragma once

#include <chrono>
#include <cstdint>
#include <optional>

namespace video::rtp_timing {
  using clock_t = std::chrono::steady_clock;

  struct frame_time_t {
    clock_t::time_point source_timestamp;
    std::uint32_t rtp_timestamp;
    bool synthetic;
  };

  // A source timestamp belongs to the encoded image. Capture delivery, encoder
  // delay, packet scheduling and unrelated CPU Present calls must not rewrite
  // it. Only frames without source metadata use the transport's duplicate slot.
  inline frame_time_t stamp(
    std::optional<clock_t::time_point> source,
    clock_t::time_point duplicate_slot,
    clock_t::time_point epoch
  ) {
    const auto selected = source.value_or(duplicate_slot);
    using rtp_tick = std::chrono::duration<std::int64_t, std::ratio<1, 90000>>;
    const auto ticks = std::chrono::round<rtp_tick>(selected - epoch).count();
    return {selected, static_cast<std::uint32_t>(ticks), !source.has_value()};
  }
}  // namespace video::rtp_timing

/**
 * @file src/platform/linux/kmsgrab_framebuffer.h
 * @brief Release GEM handles owned by a captured KMS framebuffer, and track
 * gaps in which the captured plane has no framebuffer.
 */
#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <utility>

namespace platf::kms {
  template<typename CloseHandle>
  void close_framebuffer_handles(std::span<std::uint32_t> handles, CloseHandle &&close_handle) {
    // GETFB2 can return the same GEM handle for several planes. Handles are
    // not reference-counted: clear every alias before closing the handle once.
    // https://docs.kernel.org/gpu/drm-uapi.html#drm-ioctl-mode-getfb2
    for (std::size_t plane = 0; plane < handles.size(); ++plane) {
      const auto handle = handles[plane];
      if (!handle) {
        continue;
      }
      for (std::size_t alias = plane; alias < handles.size(); ++alias) {
        if (handles[alias] == handle) {
          handles[alias] = 0;
        }
      }
      close_handle(handle);
    }
  }

  /**
   * @brief Tracks how long the captured plane has had no framebuffer.
   *
   * A modeset, or a compositor reconfiguring its outputs, can clear the plane's
   * framebuffer for a moment. Capture holds the last frame through such a gap.
   * A gap that lasts longer than `limit` is not transient (the output is off, or
   * it moved to another plane), so capture reinitializes instead of freezing.
   */
  class framebuffer_gap_t {
  public:
    using clock = std::chrono::steady_clock;

    /**
     * @brief What capture does for a frame without a framebuffer.
     */
    enum class step_e {
      started,  ///< The gap began. Hold the last frame.
      holding,  ///< The gap continues. Hold the last frame.
      expired,  ///< The gap lasted `limit`. Reinitialize capture.
    };

    static constexpr auto limit = std::chrono::seconds(5);

    /**
     * @brief Records a frame for which the plane has no framebuffer.
     * @param now The time of the frame.
     * @return What capture does for this frame. After `expired` the next
     * missing frame starts a new gap.
     */
    step_e missing(clock::time_point now) {
      if (!start) {
        start = now;
        return step_e::started;
      }
      if (now - *start >= limit) {
        start.reset();
        return step_e::expired;
      }
      return step_e::holding;
    }

    /**
     * @brief Records a frame for which the plane has a framebuffer again.
     * @return `true` if this frame ends a gap.
     */
    bool present() {
      return std::exchange(start, std::nullopt).has_value();
    }

  private:
    std::optional<clock::time_point> start;
  };
}  // namespace platf::kms

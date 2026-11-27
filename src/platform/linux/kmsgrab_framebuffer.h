/**
 * @file src/platform/linux/kmsgrab_framebuffer.h
 * @brief Release GEM handles owned by a captured KMS framebuffer, and track
 * gaps in which the captured output has no framebuffer.
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
   * @brief Tracks gaps in which the captured output has no framebuffer.
   *
   * A modeset, or a compositor reconfiguring its outputs, can clear the
   * framebuffer of the captured plane or CRTC for a moment. Capture holds the
   * last frame through such a gap in the running capture generation, so it
   * does not reinitialize while the output is still reconfiguring.
   *
   * The gap ends the capture generation in either of two ways. When the gap
   * lasts `limit`, the output is not coming back soon (for example, it is
   * off). When a framebuffer returns, the modeset can have changed what
   * `init()` cached: the plane's CRTC, the connector on that CRTC, the CRTC
   * origin, or the compositor's output placement and scale. A framebuffer of
   * the same size does not show that those are unchanged, so capture
   * reinitializes and rebuilds them from the live state.
   */
  class framebuffer_gap_t {
  public:
    using clock = std::chrono::steady_clock;

    /**
     * @brief What capture found for the captured output.
     */
    enum class frame_e {
      acquired,  ///< A framebuffer was read.
      missing,  ///< The output has no framebuffer.
      unreadable,  ///< The output has a framebuffer ID that could not be read.
    };

    /**
     * @brief What capture does for a frame.
     */
    enum class step_e {
      capture,  ///< No gap. Capture the framebuffer.
      skip,  ///< No gap, but the framebuffer could not be read. Skip this frame.
      started,  ///< A gap began. Hold the last frame.
      holding,  ///< The gap continues. Hold the last frame.
      expired,  ///< The gap lasted `limit`. Reinitialize capture.
      recovered,  ///< A framebuffer returned after a gap. Reinitialize capture.
    };

    static constexpr auto limit = std::chrono::seconds(5);

    /**
     * @brief Records what capture found for a frame.
     * @param frame What capture found.
     * @param now The time of the frame.
     * @return What capture does for this frame.
     *
     * Outside a gap, a framebuffer that cannot be read is a transient lookup
     * race and does not start a gap. Inside a gap, it continues the gap, so
     * only a framebuffer that capture read ends the gap as `recovered`.
     */
    step_e observe(frame_e frame, clock::time_point now) {
      if (frame == frame_e::acquired) {
        return std::exchange(start, std::nullopt) ? step_e::recovered : step_e::capture;
      }
      if (!start) {
        if (frame == frame_e::unreadable) {
          return step_e::skip;
        }
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
     * @brief Whether the captured output is in a gap.
     */
    [[nodiscard]] bool active() const {
      return start.has_value();
    }

  private:
    std::optional<clock::time_point> start;
  };
}  // namespace platf::kms

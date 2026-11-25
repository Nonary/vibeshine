/**
 * @file src/platform/linux/kmsgrab_framebuffer.h
 * @brief Release GEM handles owned by a captured KMS framebuffer, track gaps
 * in which the captured output has no framebuffer, and check the output that a
 * framebuffer belongs to when it returns.
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
   * @brief Tracks how long the captured output has had no framebuffer.
   *
   * A modeset, or a compositor reconfiguring its outputs, can clear the
   * framebuffer of the captured plane or CRTC for a moment. Capture holds the
   * last frame through such a gap in the running capture generation. A gap
   * that lasts `limit` is not transient (for example, the output is off), so
   * capture reinitializes instead of freezing.
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
     * @brief Records a frame for which the output has no framebuffer.
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
     * @brief Records a frame for which the output has a framebuffer again.
     * @return `true` if this frame ends a gap. Capture then checks the
     * framebuffer with `recovered_framebuffer_matches()` before it accepts it.
     */
    bool present() {
      return std::exchange(start, std::nullopt).has_value();
    }

  private:
    std::optional<clock::time_point> start;
  };

  /**
   * @brief Checks that a framebuffer that returns after a gap belongs to the
   * captured output.
   *
   * During a gap, the compositor can move the captured plane to another CRTC,
   * or bind another connector to the captured CRTC. A framebuffer of the same
   * size would pass the dimensions check, so capture compares the live bindings
   * with the CRTC it selected, and reinitializes when either one changed.
   *
   * @param captured_crtc_id The CRTC that capture selected.
   * @param connector_crtc_id The CRTC that drives the selected connector now,
   * or `std::nullopt` when the connector cannot be read, is disconnected, or
   * has no encoder.
   * @param plane_crtc_id The CRTC that the captured plane scans out to now, or
   * `std::nullopt` when capture exports the CRTC's framebuffer and reads no
   * plane.
   * @return `true` if capture can accept the framebuffer.
   */
  constexpr bool recovered_framebuffer_matches(
    std::uint32_t captured_crtc_id,
    std::optional<std::uint32_t> connector_crtc_id,
    std::optional<std::uint32_t> plane_crtc_id
  ) {
    return connector_crtc_id == captured_crtc_id &&
           (!plane_crtc_id || *plane_crtc_id == captured_crtc_id);
  }
}  // namespace platf::kms

#pragma once

#include <chrono>
#include <cstdint>
#include <limits>
#include <optional>

namespace platf::dxgi::wgc_policy {
  // Correlate once per capture session. Any sampling offset is then constant
  // and cancels from frame intervals, even when later capture reads are delayed.
  class source_clock_t {
  public:
    source_clock_t(std::int64_t qpc_anchor, std::chrono::steady_clock::time_point steady_anchor):
        qpc_anchor_(qpc_anchor),
        steady_anchor_(steady_anchor) {}

    template<class Difference>
    std::optional<std::chrono::steady_clock::time_point> timestamp(
      std::uint64_t frame_qpc, Difference difference
    ) const {
      if (qpc_anchor_ <= 0 || frame_qpc == 0 ||
          frame_qpc > static_cast<std::uint64_t>((std::numeric_limits<std::int64_t>::max)())) {
        return std::nullopt;
      }
      return steady_anchor_ + difference(static_cast<std::int64_t>(frame_qpc), qpc_anchor_);
    }

  private:
    const std::int64_t qpc_anchor_;
    const std::chrono::steady_clock::time_point steady_anchor_;
  };
}  // namespace platf::dxgi::wgc_policy

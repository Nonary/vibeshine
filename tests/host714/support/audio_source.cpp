#include "audio_source.h"
#include <cmath>
#include <numbers>
#include <stdexcept>
#include <algorithm>

namespace audio {
  synthetic_source_t::synthetic_source_t(unsigned seconds) {
    if (seconds < 1 || seconds > 120) throw std::invalid_argument("synthetic test duration must be 1..120 seconds");
    total_frames_ = static_cast<std::uint64_t>(seconds) * 48000;
  }

  bool synthetic_source_t::read(std::span<float> pcm) {
    if (pcm.size() != 2880) throw std::invalid_argument("synthetic source requires 240 interleaved 12ch frames");
    if (exhausted()) return false;
    // FL FR FC LFE BL BR SL SR TFL TFR TBL TBR; independent nonharmonic
    // tones plus a channel-specific second tone. Peak sum is below -30 dBFS.
    constexpr double frequencies[12] {223, 293, 359, 61, 443, 521, 607, 691, 773, 859, 947, 1031};
    for (unsigned sample = 0; sample < 240; ++sample) {
      const auto position = frame_ + sample;
      const double time = static_cast<double>(position) / 48000.0;
      const double ramp = std::clamp(std::min(static_cast<double>(position), static_cast<double>(total_frames_ - position - 1)) / 2400.0, 0.0, 1.0);
      const double gain = 0.5 - 0.5 * std::cos(std::numbers::pi * ramp);
      for (unsigned channel = 0; channel < 12; ++channel) {
        const double first = std::sin(2.0 * std::numbers::pi * frequencies[channel] * time);
        const double second = std::sin(2.0 * std::numbers::pi * (frequencies[channel] * 1.37 + channel * 13) * time);
        pcm[sample * 12 + channel] = static_cast<float>(gain * (0.018 * first + 0.009 * second));
      }
    }
    frame_ += 240;
    return true;
  }
}  // namespace audio

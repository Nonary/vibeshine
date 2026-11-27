/** Explicit synthetic test input. No endpoint, routing, file, or device access. */
#pragma once
#include <cstdint>
#include <span>

namespace audio {
  class synthetic_source_t {
  public:
    explicit synthetic_source_t(unsigned seconds);
    // Exactly 240 frames / 2880 Float32 values. EOF never loops implicitly.
    // Offline tests call read without pacing; capture uses a monotonic 5ms clock.
    bool read(std::span<float> pcm);
    bool exhausted() const { return frame_ >= total_frames_; }
    std::uint64_t frames_delivered() const { return frame_; }
  private:
    std::uint64_t frame_ = 0;
    std::uint64_t total_frames_ = 0;
  };
}  // namespace audio

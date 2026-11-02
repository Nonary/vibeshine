/**
 * @file src/platform/windows/audio_continuous_buffer.h
 * @brief Bounded, timestamp-aligned loopback audio and silence.
 */
#pragma once

#include <algorithm>
#include <cstdint>
#include <span>
#include <vector>

namespace platf::audio_detail {
  // WASAPI packet QPC timestamps are already expressed in 100-nanosecond units.
  // Keep the output cursor independent of event delivery: an empty/spurious
  // event cannot create silence, and late packets cannot replay emitted time.
  class continuous_buffer_t {
  public:
    static constexpr std::int64_t ticks_per_second = 10000000;

    continuous_buffer_t(std::uint32_t rate, std::uint32_t channels, std::uint32_t packet_frames, std::uint32_t device_frames, std::int64_t origin):
        rate_ {rate},
        channels_ {channels},
        packet_frames_ {packet_frames},
        margin_frames_ {2 * device_frames + packet_frames},
        max_backlog_ {std::max(2 * device_frames, 4 * packet_frames)},
        capacity_ {std::max(rate, 4 * (device_frames + packet_frames))},
        origin_ {origin},
        samples_(std::size_t(capacity_) * channels, 0.0f) {}

    // Drop old time after a scheduler stall rather than emit seconds of stale
    // packets in a burst. Retiring frames always clears their circular slots.
    void advance(std::int64_t now) {
      auto frontier = frame_at(std::max(now, origin_) - origin_) - margin_frames_;
      if (frontier - cursor_ > max_backlog_) {
        retire(std::max(cursor_, frontier - packet_frames_));
      }
    }

    bool put(std::int64_t timestamp, std::uint32_t frames, const float *data, bool silent) {
      // Reject untrustworthy timestamps before arithmetic or allocation. The
      // caller discards unlocated audio while retaining the output clock.
      if (timestamp < 0 || timestamp < origin_ - ticks_per_second) {
        return false;
      }
      auto first = frame_at(timestamp - origin_);
      auto end = first + frames;
      if (end <= cursor_) {
        return true;
      }
      if (first > cursor_ + capacity_ || end > cursor_ + capacity_) {
        return false;
      }
      auto skip = std::max<std::int64_t>(0, cursor_ - first);
      for (auto frame = first + skip; frame < end; ++frame) {
        auto destination = std::size_t(frame % capacity_) * channels_;
        auto source = std::size_t(frame - first) * channels_;
        for (std::uint32_t channel = 0; channel < channels_; ++channel) {
          samples_[destination + channel] = silent ? 0.0f : data[source + channel];
        }
      }
      return true;
    }

    bool ready(std::int64_t now) const {
      return frame_at(std::max(now, origin_) - origin_) >= cursor_ + packet_frames_ + margin_frames_;
    }

    std::uint32_t wait_ms(std::int64_t now) const {
      auto remaining = cursor_ + packet_frames_ + margin_frames_ - frame_at(std::max(now, origin_) - origin_);
      return remaining <= 0 ? 0 : std::uint32_t((remaining * 1000 + rate_ - 1) / rate_);
    }

    bool take(std::int64_t now, std::span<float> output) {
      if (output.size() != std::size_t(packet_frames_) * channels_ || !ready(now)) {
        return false;
      }
      for (std::uint32_t frame = 0; frame < packet_frames_; ++frame) {
        auto source = std::size_t((cursor_ + frame) % capacity_) * channels_;
        std::copy_n(samples_.begin() + source, channels_, output.begin() + std::size_t(frame) * channels_);
      }
      retire(cursor_ + packet_frames_);
      return true;
    }

  private:
    std::int64_t frame_at(std::int64_t ticks) const {
      // Split the integer conversion so uptime cannot overflow ticks * rate.
      // Round to the nearest frame, including negative pre-start timestamps.
      auto seconds = ticks / ticks_per_second;
      auto remainder = ticks % ticks_per_second;
      auto scaled = remainder * rate_;
      return seconds * rate_ + (scaled + (scaled < 0 ? -ticks_per_second / 2 : ticks_per_second / 2)) / ticks_per_second;
    }

    void retire(std::int64_t next) {
      if (next - cursor_ >= capacity_) {
        std::fill(samples_.begin(), samples_.end(), 0.0f);
      } else {
        for (auto frame = cursor_; frame < next; ++frame) {
          auto start = std::size_t(frame % capacity_) * channels_;
          std::fill_n(samples_.begin() + start, channels_, 0.0f);
        }
      }
      cursor_ = next;
    }

    std::int64_t rate_, channels_, packet_frames_, margin_frames_, max_backlog_, capacity_;
    std::int64_t origin_, cursor_ = 0;
    std::vector<float> samples_;
  };
}  // namespace platf::audio_detail

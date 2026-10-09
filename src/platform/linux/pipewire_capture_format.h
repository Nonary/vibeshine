#pragma once

#include <cstdint>
#include <mutex>

namespace pipewire {
  struct capture_format_t {
    int width {};
    int height {};
    int pixel_format {};
    int color_primaries {};
    int transfer_function {};
    int color_range {};
    int color_matrix {};

    bool operator==(const capture_format_t &) const = default;
  };

  struct capture_format_snapshot_t {
    capture_format_t format;
    std::uint64_t generation {};
  };

  // Format callbacks and capture run on different threads. Publish all color
  // fields together and retain transitions even if a format changes back before
  // the next frame: queued buffers can still belong to the intermediate format.
  class capture_format_state_t {
  public:
    void publish(const capture_format_t &format) {
      std::lock_guard lock {mutex_};
      if (current_.format != format) {
        current_.format = format;
        ++current_.generation;
      }
    }

    capture_format_snapshot_t snapshot() const {
      std::lock_guard lock {mutex_};
      return current_;
    }

  private:
    mutable std::mutex mutex_;
    capture_format_snapshot_t current_;
  };
}  // namespace pipewire

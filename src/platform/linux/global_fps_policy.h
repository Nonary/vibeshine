#pragma once

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <ctime>
#include <string>
#include <string_view>

namespace platf::global_fps {
  inline constexpr std::uint32_t maximum_limit_millihz = 1000000;
  inline constexpr std::int64_t lease_duration_ns = 2000000000;

  inline std::int64_t monotonic_ns() {
    timespec value {};
    if (clock_gettime(CLOCK_MONOTONIC, &value) != 0) return 0;
    return static_cast<std::int64_t>(value.tv_sec) * 1000000000 + value.tv_nsec;
  }

  inline bool selected(std::string_view provider, bool managed_steam) {
    return provider == "global" || (!managed_steam &&
      (provider.empty() || provider == "auto" || provider == "proton" || provider == "mangohud-proton"));
  }

  inline std::string lease_path(std::string_view home_directory) {
    if (home_directory.empty() || home_directory.front() != '/') return {};
    return std::string(home_directory) + "/.cache/vibeshine/frame-limiter.state";
  }

  struct lease_t {
    std::uint32_t version = 1;
    std::uint32_t limit_millihz = 0;
    std::int64_t expires_ns = 0;
  };

  inline bool valid(const lease_t &lease, std::int64_t now) {
    return lease.version == 1 && lease.limit_millihz > 0 &&
           lease.limit_millihz <= maximum_limit_millihz && lease.expires_ns > now &&
           lease.expires_ns - now <= lease_duration_ns;
  }

  class pacer_t {
  public:
    std::int64_t deadline(std::int64_t now, std::uint32_t limit) {
      if (limit == 0 || limit > maximum_limit_millihz) {
        last_ = 0;
        limit_ = 0;
        return now;
      }
      const std::int64_t interval = 1000000000000LL / limit;
      if (limit_ != limit || last_ == 0 || now - last_ > interval * 2) {
        limit_ = limit;
        last_ = now;
      } else {
        last_ = std::max(now, last_ + interval);
      }
      return last_;
    }

  private:
    std::int64_t last_ = 0;
    std::uint32_t limit_ = 0;
  };
}  // namespace platf::global_fps

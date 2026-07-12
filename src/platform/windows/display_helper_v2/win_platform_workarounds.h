#pragma once

#include "src/platform/windows/display_helper_v2/interfaces.h"

#include <mutex>
#include <thread>

namespace display_helper::v2 {
  class WinPlatformWorkarounds final: public IPlatformWorkarounds {
  public:
    ~WinPlatformWorkarounds() override;

    void blank_hdr_states(std::chrono::milliseconds delay) override;
    void cancel_pending_display_mutations() override;
    void refresh_shell() override;

  private:
    std::mutex hdr_mutex_;
    std::jthread hdr_thread_;
  };
}  // namespace display_helper::v2

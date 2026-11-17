#pragma once
#include "audio_delivery.h"
#include "utility.h"
#include <array>
#include <chrono>
#include <span>
#include <stdexcept>

namespace audio {
  using buffer_t = util::buffer_t<std::uint8_t>;
  struct packet_t {
    void *channel_data;
    buffer_t data;
    std::shared_ptr<delivery_state_t> delivery;
    // Only the opt-in Atmos path uses this storage. Legacy ownership and bytes
    // remain unchanged. Moving a packet never allocates an inline payload.
    std::array<std::uint8_t, 1400> inline_data {};
    std::size_t inline_bytes = 0;
    unsigned usb_lost_blocks = 0;
    std::chrono::steady_clock::time_point queued_at {};

    packet_t(void *channel, buffer_t payload, std::shared_ptr<delivery_state_t> progress = {}):
        channel_data {channel}, data {std::move(payload)}, delivery {std::move(progress)} {}
    packet_t(void *channel, std::span<const std::uint8_t> payload, std::shared_ptr<delivery_state_t> progress, unsigned lost_blocks=0):
        channel_data {channel}, delivery {std::move(progress)}, inline_bytes {payload.size()},
        usb_lost_blocks {lost_blocks}, queued_at {std::chrono::steady_clock::now()} {
      if (payload.empty() || payload.size() > inline_data.size()) throw std::invalid_argument("Invalid inline audio packet");
      std::copy(payload.begin(), payload.end(), inline_data.begin());
    }
    bool is_inline() const { return inline_bytes != 0; }
    std::span<const std::uint8_t> payload() const {
      return is_inline() ? std::span<const std::uint8_t>(inline_data.data(), inline_bytes) :
                           std::span<const std::uint8_t>(data.begin(), data.size());
    }
  };
}

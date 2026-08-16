/**
 * @file src/terminal_session_launch_codec.h
 * @brief Bounded private-seat serialization for authenticated launch material.
 */
#pragma once

#include "terminal_session_broker.h"

#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace terminal_session::launch_codec {
  [[nodiscard]] std::vector<std::uint8_t> encode(const request_t &request, std::string &error);
  [[nodiscard]] std::optional<request_t> decode(std::span<const std::uint8_t> payload, std::string &error);
}

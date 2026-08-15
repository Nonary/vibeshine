#pragma once

#include "terminal_session_protocol.h"

#include <cstdint>
#include <string>
#include <vector>

namespace terminal_session::worker {
  struct contract_t {
    std::string config_root;
    std::string state_root;
    std::string log_root;
    std::uint16_t rtsp_port {};
    std::uint16_t control_port {};
    std::uint16_t video_port {};
    std::uint16_t audio_port {};
  };

  // Produces a private worker command contract. The ticket is passed as an
  // inherited local-pipe admission value, never in logs or persistent config.
  [[nodiscard]] contract_t make_contract(std::string_view seat_id,
                                         std::uint16_t rtsp_port,
                                         std::uint16_t control_port,
                                         std::uint16_t video_port,
                                         std::uint16_t audio_port);
  [[nodiscard]] std::vector<std::string> command_line(const contract_t &contract);
}

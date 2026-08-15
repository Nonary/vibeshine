#include "terminal_session_worker.h"

#include <functional>

namespace terminal_session::worker {
  contract_t make_contract(std::string_view seat_id, std::uint16_t rtsp_port, std::uint16_t control_port, std::uint16_t video_port, std::uint16_t audio_port) {
    const auto stable = std::to_string(std::hash<std::string_view> {}(seat_id));
    const std::string root = "%ProgramData%\\Vibeshine\\seats\\" + stable;
    return {root + "\\config", root + "\\state", root + "\\logs", rtsp_port, control_port, video_port, audio_port};
  }

  std::vector<std::string> command_line(const contract_t &contract) {
    return {
      "--terminal-worker",
      "--config-root=" + contract.config_root,
      "--state-root=" + contract.state_root,
      "--log-root=" + contract.log_root,
      "--disable-web-ui",
      "--disable-pairing",
      "--disable-mdns",
      "--disable-updater",
      "--disable-global-display-mutations",
      "--rtsp-port=" + std::to_string(contract.rtsp_port),
      "--control-port=" + std::to_string(contract.control_port),
      "--video-port=" + std::to_string(contract.video_port),
      "--audio-port=" + std::to_string(contract.audio_port),
      "--admit-ticket-from-protected-pipe",
    };
  }
}

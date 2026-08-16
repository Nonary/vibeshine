#include "terminal_session_worker.h"

#include <array>
#include <cstdlib>
#include <functional>

#ifdef _WIN32
  #include <openssl/crypto.h>
  #include <openssl/rand.h>
#endif

namespace terminal_session::worker {
  namespace {
    const std::string &storage_namespace() {
#ifdef _WIN32
      static const std::string value = [] {
        std::array<unsigned char, 16> bytes {};
        if (RAND_bytes(bytes.data(), static_cast<int>(bytes.size())) != 1) return std::string {};
        constexpr char hex[] = "0123456789abcdef";
        std::string result = "VibeshineTerminalSeats-";
        result.reserve(result.size() + bytes.size() * 2);
        for (const auto byte : bytes) {
          result.push_back(hex[byte >> 4]);
          result.push_back(hex[byte & 0xf]);
        }
        OPENSSL_cleanse(bytes.data(), bytes.size());
        return result;
      }();
      return value;
#else
      static const std::string value = "VibeshineTerminalSeats-test";
      return value;
#endif
    }
  }

  contract_t make_contract(std::string_view seat_id, std::uint16_t rtsp_port, std::uint16_t control_port, std::uint16_t video_port, std::uint16_t audio_port) {
    std::uint64_t stable = 1469598103934665603ULL;
    for (const auto ch : seat_id) { stable ^= static_cast<unsigned char>(ch); stable *= 1099511628211ULL; }
    const char *program_data = std::getenv("ProgramData");
    const auto &storage = storage_namespace();
    const std::string root = storage.empty() ? std::string {} :
      std::string {program_data && *program_data ? program_data : "C:\\ProgramData"} + "\\" + storage + "\\" + std::to_string(stable);
    const auto base_port = rtsp_port >= 21 ? static_cast<std::uint16_t>(rtsp_port - 21) : 0;
    return {root + "\\config", root + "\\state", root + "\\logs", rtsp_port, control_port, video_port, audio_port, base_port};
  }

  std::vector<std::string> command_line(const contract_t &contract) {
    return {
      "port=" + std::to_string(contract.base_port),
      "system_tray=false",
      "upnp=false",
      "session_history_enabled=false",
      "output_name=",
      "audio_sink=",
      "virtual_sink=",
      "log_path=" + contract.log_root + "\\sunshine.log",
      "file_state=" + contract.state_root + "\\sunshine_state.json",
      "vibeshine_file_state=" + contract.state_root + "\\vibeshine_state.json",
      "credentials_file=" + contract.state_root + "\\credentials.json",
    };
  }
}

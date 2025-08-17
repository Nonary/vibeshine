/**
 * @file src/platform/windows/playnite_protocol.h
 * @brief Message schema and parsing for Playnite <-> Sunshine IPC.
 */
#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace platf::playnite_protocol {

  enum class MessageType {
    Unknown,     ///< Unknown message type.
    Categories,  ///< Message containing category list.
    Games,       ///< Message containing game list.
    Status       ///< Message containing status update.
  };

  struct Category {
    std::string id;
    std::string name;
  };

  struct Game {
    std::string id;
    std::string name;
    std::string exe;
    std::string args;
    std::string workingDir;
    std::vector<std::string> categories;
    uint64_t playtimeMinutes = 0;
    std::string lastPlayed;     // ISO8601 recommended
    std::string boxArtPath;     // file path or URL
    std::string description;
    std::vector<std::string> tags;
  };

  struct Message {
    MessageType type = MessageType::Unknown;
    std::vector<Category> categories;
    std::vector<Game> games;
    // Status
    std::string statusName;
    std::string statusGameId;
    std::string statusInstallDir;
    std::string statusExe;
  };

  // Parse a JSON message into a typed structure.
  Message parse(std::span<const uint8_t> bytes);
}

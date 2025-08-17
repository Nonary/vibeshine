/**
 * @file src/platform/windows/playnite_protocol.cpp
 */

#include "playnite_protocol.h"
#include "src/logging.h"

#include <string>

#include <nlohmann/json.hpp>

using nlohmann::json;

namespace platf::playnite_protocol {

  static std::vector<std::string> to_string_list(const json &j) {
    std::vector<std::string> out;
    if (!j.is_array()) return out;
    out.reserve(j.size());
    for (auto &v : j) {
      if (v.is_string()) out.emplace_back(v.get<std::string>());
    }
    return out;
  }

  Message parse(std::span<const uint8_t> bytes) {
    Message m;
    if (bytes.empty()) return m;
    try {
      json j = json::parse(bytes.begin(), bytes.end());
      const std::string type = j.value("type", "");
      BOOST_LOG(info) << "Playnite protocol: parsing message type='" << type << "'";
      if (type == "categories") {
        m.type = MessageType::Categories;
        auto arr = j.value("payload", json::array());
        BOOST_LOG(info) << "Playnite protocol: categories count=" << arr.size();
        for (auto &c : arr) {
          Category cat;
          cat.id = c.value("id", "");
          cat.name = c.value("name", "");
          if (!cat.id.empty() || !cat.name.empty()) m.categories.emplace_back(std::move(cat));
        }
      } else if (type == "games") {
        m.type = MessageType::Games;
        auto arr = j.value("payload", json::array());
        BOOST_LOG(info) << "Playnite protocol: games count=" << arr.size();
        for (auto &g : arr) {
          Game game;
          game.id = g.value("id", "");
          game.name = g.value("name", "");
          game.exe = g.value("exe", "");
          game.args = g.value("args", "");
          game.workingDir = g.value("workingDir", "");
          game.categories = to_string_list(g.value("categories", json::array()));
          game.playtimeMinutes = g.value("playtimeMinutes", (uint64_t) 0);
          game.lastPlayed = g.value("lastPlayed", "");
          game.boxArtPath = g.value("boxArtPath", "");
          game.description = g.value("description", "");
          game.tags = to_string_list(g.value("tags", json::array()));
          if (!game.id.empty()) m.games.emplace_back(std::move(game));
        }
      } else if (type == "status") {
        m.type = MessageType::Status;
        const auto &st = j.value("status", json::object());
        m.statusName = st.value("name", "");
        m.statusGameId = st.value("id", "");
        m.statusInstallDir = st.value("installDir", "");
        m.statusExe = st.value("exe", "");
        BOOST_LOG(info) << "Playnite protocol: status name='" << m.statusName << "' id='" << m.statusGameId << "'";
      }
    } catch (...) {
      BOOST_LOG(warning) << "Playnite protocol: failed to parse message";
      // fallthrough unknown
    }
    return m;
  }

}  // namespace platf::playnite_protocol

/** @file src/steam_big_picture_controls_policy.cpp */
#include "steam_big_picture_controls_policy.h"

#include "steam_big_picture_controls_protocol.h"

#include <algorithm>

namespace platf::steam::big_picture::policy {
  namespace {
    const char *kind_name(client_kind_e kind) {
      return kind == client_kind_e::browser ? "browser" : "moonlight";
    }

    // Client ids come back from Steam's JS context, so only accept the characters our ids use.
    bool valid_client_id(const std::string &id) {
      return !id.empty() && id.size() <= protocol::max_client_id_bytes && std::all_of(id.begin(), id.end(), [](unsigned char c) {
               return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '-' || c == '_' || c == '.';
             });
    }
  }  // namespace

  nlohmann::json status_payload(const snapshot_t &snapshot, const capabilities_t &can, const strings_t &strings) {
    nlohmann::json clients = nlohmann::json::array();
    for (const auto &client : snapshot.clients) {
      clients.push_back({{"id", client.id}, {"name", client.name}, {"kind", kind_name(client.kind)}});
    }
    nlohmann::json streams = nlohmann::json::array();
    for (const auto &stream : snapshot.streams) {
      streams.push_back({{"width", stream.width}, {"height", stream.height}, {"fps", stream.fps}, {"codec", stream.codec}, {"hdr", stream.hdr}});
    }
    return {
      {"version", protocol::version},
      {"appRunning", snapshot.app_running},
      {"appName", snapshot.app_name},
      {"clients", std::move(clients)},
      {"streams", std::move(streams)},
      {"can", {{"endStream", can.end_stream}, {"disconnect", can.disconnect}}},
      {"strings",
       {
         {"endStream", strings.end_stream},
         {"stopStreamingTo", strings.stop_streaming_to},
         {"menuTitleStreaming", strings.menu_title_streaming},
         {"menuTitlePaused", strings.menu_title_paused},
         {"iconStreaming", strings.icon_streaming},
         {"iconPaused", strings.icon_paused},
         {"idle", strings.idle},
       }},
    };
  }

  std::optional<action_t> parse_action(const nlohmann::json &message) {
    if (!message.is_object()) {
      return std::nullopt;
    }
    const auto action = message.find("action");
    if (action == message.end() || !action->is_string()) {
      return std::nullopt;
    }
    const auto &name = action->get_ref<const std::string &>();
    if (name == protocol::action::end_stream) {
      return action_t {action_t::kind_e::end_stream, {}};
    }
    if (name == protocol::action::disconnect) {
      const auto client = message.find("client");
      if (client == message.end() || !client->is_string() || !valid_client_id(client->get_ref<const std::string &>())) {
        return std::nullopt;
      }
      return action_t {action_t::kind_e::disconnect, client->get<std::string>()};
    }
    return std::nullopt;
  }
}  // namespace platf::steam::big_picture::policy

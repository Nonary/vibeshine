/** @file src/steam_big_picture_controls_policy.h
 * @brief Pure decisions for Vibeshine's stream controls inside Steam Big Picture.
 */
#pragma once

#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <vector>

namespace platf::steam::big_picture::policy {
  enum class client_kind_e {
    moonlight,  ///< Paired Moonlight client; id is its paired-client uuid.
    browser,  ///< WebRTC browser stream; id is its session id.
  };

  struct client_t {
    std::string id;
    std::string name;
    client_kind_e kind = client_kind_e::moonlight;
  };

  struct stream_t {
    int width = 0;
    int height = 0;
    int fps = 0;
    std::string codec;
    bool hdr = false;
  };

  /** What the service knows about streaming right now, gathered from non-blocking snapshots. */
  struct snapshot_t {
    bool app_running = false;
    std::string app_name;
    std::vector<client_t> clients;
    std::vector<stream_t> streams;
  };

  /** Labels shown inside Steam. "%1$s" is replaced by the client or app name. */
  struct strings_t {
    std::string end_stream = "End Stream (Vibeshine)";
    std::string stop_streaming_to = "Stop Streaming To %1$s (Vibeshine)";
    std::string menu_title_streaming = "Vibeshine · streaming to %1$s";
    std::string menu_title_paused = "Vibeshine · no client connected";
    std::string icon_streaming = "Streaming to %1$s with Vibeshine";
    std::string icon_paused = "%1$s is running on Vibeshine with no client connected";
    std::string idle = "Idle";
  };

  struct capabilities_t {
    bool end_stream = true;
    bool disconnect = true;
  };

  /** Build the payload passed to window.VibeshineBPM.setStatus() inside Steam. */
  nlohmann::json status_payload(const snapshot_t &snapshot, const capabilities_t &can, const strings_t &strings);

  struct action_t {
    enum class kind_e {
      end_stream,
      disconnect,
    } kind;

    std::string client;  ///< Set for disconnect only.
  };

  /** Parse an action message from the helper. Rejects unknown actions and malformed client ids. */
  std::optional<action_t> parse_action(const nlohmann::json &message);
}  // namespace platf::steam::big_picture::policy

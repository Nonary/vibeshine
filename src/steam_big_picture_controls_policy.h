/** @file src/steam_big_picture_controls_policy.h
 * @brief Pure decisions for Vibeshine's stream controls inside Steam Big Picture.
 */
#pragma once

#include <chrono>
#include <cstdint>
#include <deque>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <string_view>
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

  /** Decides whether an action from Steam may run: one per two seconds, and Stop Streaming only for a
   * client that is connected right now. */
  class action_gate_t {
  public:
    using clock = std::chrono::steady_clock;
    static constexpr auto min_interval = std::chrono::seconds(2);

    enum class verdict_e {
      allowed,
      too_soon,
      not_connected,
    };

    /** Check an action against the connected clients; allowed actions start the interval. Sets kind
     * to the client's kind for an allowed disconnect. */
    verdict_e check(const action_t &action, const std::vector<client_t> &clients, clock::time_point now, client_kind_e &kind);

  private:
    std::optional<clock::time_point> last_;
  };

  /** Display name of Vibeshine's video_format (0 H.264, 1 HEVC, 2 AV1). */
  std::string_view codec_name(int video_format);

  /** Log line for a Steam state from the helper (protocol::steam_state). Empty for "connecting" and
   * "injected", which the helper's own messages already cover. */
  std::string steam_state_log(std::string_view state);

  /** One-line summary of a feature report for the log: "name: result; name: result". */
  std::string features_summary(const nlohmann::json &features);

  // ---------------------------------------------------------------------------------------------
  // Web UI: Steam's debug port and Steam restarts.
  // ---------------------------------------------------------------------------------------------

  /** Name of the empty file in the Steam folder that turns on Steam's debug port at its next start. */
  constexpr std::string_view debug_port_flag_file = ".cef-enable-remote-debugging";

  /** Body of POST /api/steam/big_picture/debug_port. */
  struct debug_port_request_t {
    bool enabled = false;
    bool restart_steam = false;
    bool allow_during_stream = false;  ///< The user confirmed restarting Steam while a stream or app runs.
  };

  /** Parse a debug_port request. "enabled" is required; every field must be a boolean when present. */
  std::optional<debug_port_request_t> parse_debug_port_request(const nlohmann::json &body);

  enum class restart_decision_e {
    start,
    not_running,  ///< Nothing to restart: Steam picks the change up when it starts.
    stream_active,  ///< Restarting would close games running through Steam; needs the user's confirmation.
    busy,  ///< A restart is already running.
  };

  /** Whether a Steam restart may start now. */
  restart_decision_e restart_decision(bool steam_running, bool streaming, bool allow_during_stream, bool restart_in_progress);
  std::string_view restart_decision_name(restart_decision_e decision);

  /** True for the title of Steam's Big Picture window, so a restart can reopen Big Picture. Only a
   * hint: a miss reopens Steam in its desktop mode. */
  bool is_big_picture_window_title(std::wstring_view title);

  /** How long after Vibeshine reopened Steam in Big Picture a new restart still reopens it there:
   * Steam takes a while to start, and its Big Picture window only appears at the end. */
  constexpr auto big_picture_restart_memory = std::chrono::minutes(2);

  /** Whether a Steam restart reopens Big Picture: when its window is open now, or when the previous
   * restart reopened Big Picture recently enough that Steam may not show it yet. */
  bool restart_into_big_picture(bool big_picture_open, std::optional<std::chrono::steady_clock::duration> since_big_picture_restart);

  // ---------------------------------------------------------------------------------------------
  // Helper side: finding and trusting Steam's debugger, injecting, and keeping the script injected.
  // ---------------------------------------------------------------------------------------------

  /** Steam's CEF debug port, opened by an empty .cef-enable-remote-debugging file in the Steam folder. */
  constexpr int steam_debug_port = 8080;

  /** Name of the CDP binding the script reports through (window.__vibeshineBridge). */
  constexpr std::string_view bridge_binding = "__vibeshineBridge";

  /** Evaluated in each candidate page: true only in Steam's shared JS context, the one page that owns
   * the webpack chunk registry and was not opened by another window. */
  constexpr std::string_view is_shared_context_expression =
    "!window.opener && typeof SteamClient === 'object' && Object.keys(window).some((k) => k.startsWith('webpackChunk'))";

  /** Titles Steam has used for its shared JS context, tried before probing every Steam page. */
  constexpr std::string_view shared_context_titles[] = {"SharedJSContext", "Steam Shared Context presented by Valve™"};

  struct debug_target_t {
    std::string title;
    std::string url;
    std::string ws_url;
  };

  /** True for ws://127.0.0.1:<port>/devtools/... (or localhost / [::1]); anything else is refused. */
  bool is_local_debugger_url(std::string_view url, int port = steam_debug_port);

  /** Pages from Steam's /json list worth probing for the shared context, best first. Skips non-page
   * targets, pages outside steamloopback.host, and debugger URLs that are not local. */
  std::vector<debug_target_t> shared_context_candidates(const nlohmann::json &targets);

  enum class endpoint_e {
    ok,  ///< Steam's own steamwebhelper.exe, in the helper's session, is listening.
    not_running,  ///< Nothing listens and Steam is not running.
    port_closed,  ///< Steam runs, but its debug port is off (no flag file, or not restarted yet).
    port_foreign,  ///< Something else owns the port, or Steam's in another session; never connect.
  };

  /** What the platform code found out about the debug port. */
  struct endpoint_facts_t {
    bool steam_running = false;
    bool listening = false;
    bool owner_is_steam_webhelper = false;  ///< Owner image is <Steam root>\bin\cef\cef.win64\steamwebhelper.exe
    bool owner_in_helper_session = false;
    std::uint64_t steam_instance = 0;  ///< Tells one run of steam.exe from the next (its start time); 0 if unknown.
  };

  endpoint_e endpoint_verdict(const endpoint_facts_t &facts);
  std::string_view endpoint_state_name(endpoint_e endpoint);

  /** Expression that sets the script's options and evaluates it; its value is the script's report. */
  std::string inject_expression(const nlohmann::json &options, std::string_view script);

  /** Expression that pushes a status payload to the injected script. */
  std::string set_status_expression(const nlohmann::json &status);

  /** Expression that removes the injected script's patches, used when the helper stops. */
  constexpr std::string_view unpatch_expression = "window.VibeshineBPM?.unpatch?.(), true";

  /** Validate the script's injection report and turn it into a "report" message for the service.
   * Returns nullopt when it isn't a report or speaks another protocol version. */
  std::optional<nlohmann::json> report_message(const nlohmann::json &report, std::string_view steam_build);

  /** Validate a message the script sent through the binding and normalise it for the service.
   * Returns nullopt for anything malformed, oversized or not in the allowlist. */
  std::optional<nlohmann::json> script_message(std::string_view payload);

  /** Retry pacing and the circuit breaker for injecting into Steam.
   *
   * Failed injections back off 2 s, 4 s, 8 s ... up to 60 s. A Steam UI reload within 30 s of injecting
   * counts as a quick reload; three in five minutes trip the breaker, which stays open until reset()
   * (a config change or a helper restart), so a script that destabilises Steam stops being re-injected.
   * The controller reports a lost connection as a reload only when the same steam.exe is still running,
   * so restarting Steam never counts. */
  class injection_tracker_t {
  public:
    using clock = std::chrono::steady_clock;
    static constexpr auto initial_backoff = std::chrono::seconds(2);
    static constexpr auto max_backoff = std::chrono::seconds(60);
    static constexpr auto quick_reload = std::chrono::seconds(30);
    static constexpr auto breaker_window = std::chrono::minutes(5);
    static constexpr int breaker_reloads = 3;

    bool may_attempt(clock::time_point now) const;
    void injected(clock::time_point now);
    /** Record a failed injection; returns how long until the next attempt. */
    clock::duration failed(clock::time_point now);
    /** Record that Steam's UI reloaded; returns true if this trips the breaker. */
    bool reloaded(clock::time_point now);
    bool tripped() const {
      return tripped_;
    }
    void reset();

  private:
    int failures_ = 0;
    bool tripped_ = false;
    std::optional<clock::time_point> next_attempt_;
    std::optional<clock::time_point> injected_at_;
    std::deque<clock::time_point> quick_reloads_;
  };
}  // namespace platf::steam::big_picture::policy

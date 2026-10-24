/** @file src/steam_big_picture_controls_policy.cpp */
#include "steam_big_picture_controls_policy.h"

#include "steam_big_picture_controls_protocol.h"

#include <algorithm>
#include <iterator>

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

    // Cut at a UTF-8 code point boundary so the result stays valid for nlohmann::json::dump().
    std::string truncate_utf8(std::string text, std::size_t max_bytes) {
      if (text.size() <= max_bytes) {
        return text;
      }
      std::size_t cut = max_bytes;
      while (cut > 0 && (static_cast<unsigned char>(text[cut]) & 0xC0U) == 0x80U) {
        --cut;
      }
      text.resize(cut);
      return text;
    }

    // Read a string field, or nullopt when it's missing or another type.
    std::optional<std::string> string_field(const nlohmann::json &object, const char *key, std::size_t max_bytes) {
      const auto it = object.find(key);
      if (it == object.end() || !it->is_string()) {
        return std::nullopt;
      }
      return truncate_utf8(it->get<std::string>(), max_bytes);
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

  action_gate_t::verdict_e action_gate_t::check(const action_t &action, const std::vector<client_t> &clients, clock::time_point now, client_kind_e &kind) {
    if (last_ && now - *last_ < min_interval) {
      return verdict_e::too_soon;
    }
    if (action.kind == action_t::kind_e::disconnect) {
      const auto client = std::find_if(clients.begin(), clients.end(), [&](const client_t &candidate) {
        return candidate.id == action.client;
      });
      if (client == clients.end()) {
        return verdict_e::not_connected;
      }
      kind = client->kind;
    }
    last_ = now;
    return verdict_e::allowed;
  }

  std::string_view codec_name(int video_format) {
    switch (video_format) {
      case 0:
        return "H.264";
      case 1:
        return "HEVC";
      case 2:
        return "AV1";
      default:
        return "";
    }
  }

  std::optional<debug_port_request_t> parse_debug_port_request(const nlohmann::json &body) {
    if (!body.is_object()) {
      return std::nullopt;
    }
    debug_port_request_t request;
    const auto read = [&](const char *key, bool &out, bool required) {
      const auto found = body.find(key);
      if (found == body.end()) {
        return !required;
      }
      if (!found->is_boolean()) {
        return false;
      }
      out = found->get<bool>();
      return true;
    };
    if (!read("enabled", request.enabled, true) || !read("restart_steam", request.restart_steam, false) || !read("allow_during_stream", request.allow_during_stream, false)) {
      return std::nullopt;
    }
    return request;
  }

  restart_decision_e restart_decision(bool steam_running, bool streaming, bool allow_during_stream, bool restart_in_progress) {
    if (restart_in_progress) {
      return restart_decision_e::busy;
    }
    if (!steam_running) {
      return restart_decision_e::not_running;
    }
    if (streaming && !allow_during_stream) {
      return restart_decision_e::stream_active;
    }
    return restart_decision_e::start;
  }

  std::string_view restart_decision_name(restart_decision_e decision) {
    switch (decision) {
      case restart_decision_e::start:
        return "started";
      case restart_decision_e::not_running:
        return "not_running";
      case restart_decision_e::stream_active:
        return "stream_active";
      case restart_decision_e::busy:
        return "busy";
    }
    return "";
  }

  bool is_big_picture_window_title(std::wstring_view title) {
    // "Steam Big Picture Mode" in English; Steam keeps the "Big Picture" name in most languages.
    return title.find(L"Big Picture") != std::wstring_view::npos;
  }

  bool restart_into_big_picture(bool big_picture_open, std::optional<std::chrono::steady_clock::duration> since_big_picture_restart) {
    return big_picture_open || (since_big_picture_restart && *since_big_picture_restart < big_picture_restart_memory);
  }

  bool is_local_debugger_url(std::string_view url, int port) {
    for (const std::string_view host : {"127.0.0.1", "localhost", "[::1]"}) {
      const auto prefix = "ws://" + std::string(host) + ":" + std::to_string(port) + "/devtools/";
      if (url.starts_with(prefix) && url.size() > prefix.size() && std::all_of(url.begin() + prefix.size(), url.end(), [](unsigned char c) {
            return c > 0x20 && c < 0x7F;
          })) {
        return true;
      }
    }
    return false;
  }

  std::vector<debug_target_t> shared_context_candidates(const nlohmann::json &targets) {
    std::vector<debug_target_t> known;
    std::vector<debug_target_t> others;
    if (!targets.is_array()) {
      return {};
    }
    for (const auto &target : targets) {
      if (!target.is_object() || target.value("type", nlohmann::json {}) != "page") {
        continue;
      }
      const auto title = string_field(target, "title", 256);
      const auto url = string_field(target, "url", 2048);
      const auto ws_url = string_field(target, "webSocketDebuggerUrl", 512);
      if (!title || !url || !ws_url || !is_local_debugger_url(*ws_url)) {
        continue;
      }
      debug_target_t candidate {*title, *url, *ws_url};
      if (std::find(std::begin(shared_context_titles), std::end(shared_context_titles), candidate.title) != std::end(shared_context_titles)) {
        known.push_back(std::move(candidate));
      } else if (candidate.url.starts_with("https://steamloopback.host")) {
        others.push_back(std::move(candidate));
      }
    }
    known.insert(known.end(), std::make_move_iterator(others.begin()), std::make_move_iterator(others.end()));
    return known;
  }

  endpoint_e endpoint_verdict(const endpoint_facts_t &facts) {
    if (!facts.listening) {
      return facts.steam_running ? endpoint_e::port_closed : endpoint_e::not_running;
    }
    if (!facts.owner_is_steam_webhelper || !facts.owner_in_helper_session) {
      return endpoint_e::port_foreign;
    }
    return endpoint_e::ok;
  }

  std::string_view endpoint_state_name(endpoint_e endpoint) {
    switch (endpoint) {
      case endpoint_e::ok:
        return protocol::steam_state::connecting;
      case endpoint_e::not_running:
        return protocol::steam_state::not_running;
      case endpoint_e::port_closed:
        return protocol::steam_state::port_closed;
      case endpoint_e::port_foreign:
        return protocol::steam_state::port_foreign;
    }
    return protocol::steam_state::port_foreign;
  }

  std::string inject_expression(const nlohmann::json &options, std::string_view script) {
    return "window.__vibeshineOptions = " + options.dump() + ";\n" + std::string(script);
  }

  std::string set_status_expression(const nlohmann::json &status) {
    return "window.VibeshineBPM?.setStatus(" + status.dump() + "), true";
  }

  std::optional<nlohmann::json> report_message(const nlohmann::json &report, std::string_view steam_build) {
    if (!report.is_object()) {
      return std::nullopt;
    }
    const auto protocol_field = report.find("protocol");
    if (protocol_field == report.end() || !protocol_field->is_number_integer() || protocol_field->get<int>() != protocol::version) {
      return std::nullopt;
    }
    nlohmann::json features = nlohmann::json::object();
    if (const auto found = report.find("features"); found != report.end() && found->is_object()) {
      for (const auto &[name, value] : found->items()) {
        if (value.is_string() && features.size() < 32) {
          features[truncate_utf8(name, 64)] = truncate_utf8(value.get<std::string>(), 200);
        }
      }
    }
    const auto number = [&](const char *key) {
      const auto it = report.find(key);
      return it != report.end() && it->is_number_integer() ? it->get<int>() : 0;
    };
    const auto enabled = report.find("enabled");
    return nlohmann::json {
      {"type", protocol::message::report},
      {"script_version", number("version")},
      {"react", string_field(report, "react", 32).value_or("")},
      {"steam_build", truncate_utf8(std::string(steam_build), 64)},
      {"enabled", enabled != report.end() && enabled->is_boolean() && enabled->get<bool>()},
      {"errors", number("errors")},
      {"features", std::move(features)},
    };
  }

  std::optional<nlohmann::json> script_message(std::string_view payload) {
    if (payload.empty() || payload.size() > protocol::max_frame_bytes) {
      return std::nullopt;
    }
    const auto message = nlohmann::json::parse(payload, nullptr, false);
    if (message.is_discarded() || !message.is_object()) {
      return std::nullopt;
    }
    const auto type = string_field(message, "type", 32);
    if (!type) {
      return std::nullopt;
    }
    if (*type == protocol::message::action) {
      const auto action = parse_action(message);
      if (!action) {
        return std::nullopt;
      }
      if (action->kind == action_t::kind_e::end_stream) {
        return nlohmann::json {{"type", protocol::message::action}, {"action", protocol::action::end_stream}};
      }
      return nlohmann::json {{"type", protocol::message::action}, {"action", protocol::action::disconnect}, {"client", action->client}};
    }
    if (*type == protocol::message::feature) {
      const auto name = string_field(message, "name", 64);
      const auto value = string_field(message, "value", 200);
      if (!name || !value) {
        return std::nullopt;
      }
      return nlohmann::json {{"type", protocol::message::feature}, {"name", *name}, {"value", *value}};
    }
    if (*type == protocol::message::error) {
      const auto where = string_field(message, "where", 64);
      const auto text = string_field(message, "message", 300);
      if (!where || !text) {
        return std::nullopt;
      }
      return nlohmann::json {{"type", protocol::message::error}, {"where", *where}, {"message", *text}};
    }
    if (*type == protocol::message::disabled) {
      const auto reason = string_field(message, "reason", 200);
      if (!reason) {
        return std::nullopt;
      }
      return nlohmann::json {{"type", protocol::message::disabled}, {"reason", *reason}};
    }
    return std::nullopt;
  }

  bool injection_tracker_t::may_attempt(clock::time_point now) const {
    return !tripped_ && (!next_attempt_ || now >= *next_attempt_);
  }

  void injection_tracker_t::injected(clock::time_point now) {
    failures_ = 0;
    next_attempt_.reset();
    injected_at_ = now;
  }

  injection_tracker_t::clock::duration injection_tracker_t::failed(clock::time_point now) {
    ++failures_;
    const auto doublings = std::min(failures_ - 1, 5);
    const clock::duration delay = std::min<clock::duration>(initial_backoff * (1 << doublings), max_backoff);
    next_attempt_ = now + delay;
    return delay;
  }

  bool injection_tracker_t::reloaded(clock::time_point now) {
    const bool quick = injected_at_ && now - *injected_at_ < quick_reload;
    injected_at_.reset();
    if (tripped_ || !quick) {
      return false;
    }
    quick_reloads_.push_back(now);
    while (!quick_reloads_.empty() && now - quick_reloads_.front() > breaker_window) {
      quick_reloads_.pop_front();
    }
    if (static_cast<int>(quick_reloads_.size()) >= breaker_reloads) {
      tripped_ = true;
      return true;
    }
    return false;
  }

  void injection_tracker_t::reset() {
    *this = injection_tracker_t {};
  }
}  // namespace platf::steam::big_picture::policy

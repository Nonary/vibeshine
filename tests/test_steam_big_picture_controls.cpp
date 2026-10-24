#include "src/steam_big_picture_controls_policy.h"
#include "src/steam_big_picture_controls_protocol.h"

#include <algorithm>
#include <gtest/gtest.h>

namespace bpm = platf::steam::big_picture;
using namespace std::chrono_literals;

TEST(SteamBigPictureProtocol, FramesRoundTripLittleEndian) {
  const auto frame = bpm::protocol::encode_frame(R"({"type":"hello"})");
  ASSERT_TRUE(frame.has_value());
  ASSERT_EQ(frame->size(), bpm::protocol::frame_header_bytes + 16);
  EXPECT_EQ(static_cast<unsigned char>((*frame)[0]), 16);
  EXPECT_EQ(static_cast<unsigned char>((*frame)[1]), 0);

  std::array<std::uint8_t, bpm::protocol::frame_header_bytes> header {};
  std::copy_n(frame->begin(), header.size(), header.begin());
  EXPECT_EQ(bpm::protocol::decode_frame_length(header), 16U);
}

TEST(SteamBigPictureProtocol, RejectsEmptyAndOversizedFrames) {
  EXPECT_FALSE(bpm::protocol::encode_frame("").has_value());
  EXPECT_FALSE(bpm::protocol::encode_frame(std::string(bpm::protocol::max_frame_bytes + 1, 'x')).has_value());
  EXPECT_TRUE(bpm::protocol::encode_frame(std::string(bpm::protocol::max_frame_bytes, 'x')).has_value());

  EXPECT_FALSE(bpm::protocol::decode_frame_length({0, 0, 0, 0}).has_value());
  EXPECT_FALSE(bpm::protocol::decode_frame_length({0x01, 0x00, 0x01, 0x00}).has_value());  // 65537 bytes
}

TEST(SteamBigPicturePolicy, IdleStatusPayload) {
  const auto payload = bpm::policy::status_payload({}, {}, {});
  EXPECT_EQ(payload["version"], bpm::protocol::version);
  EXPECT_FALSE(payload["appRunning"].get<bool>());
  EXPECT_TRUE(payload["clients"].empty());
  EXPECT_TRUE(payload["streams"].empty());
  EXPECT_TRUE(payload["can"]["endStream"].get<bool>());
  EXPECT_EQ(payload["strings"]["endStream"], "End Stream (Vibeshine)");
}

TEST(SteamBigPicturePolicy, StreamingStatusPayload) {
  bpm::policy::snapshot_t snapshot;
  snapshot.app_running = true;
  snapshot.app_name = "Steam Big Picture";
  snapshot.clients = {{"A04551F1-C420-292A-7C8B-D3DBB4047207", "Steam Deck", bpm::policy::client_kind_e::moonlight}, {"webrtc-1", "Browser", bpm::policy::client_kind_e::browser}};
  snapshot.streams = {{2560, 1600, 90, "HEVC", false}};

  const auto payload = bpm::policy::status_payload(snapshot, {.end_stream = true, .disconnect = false}, {});
  EXPECT_TRUE(payload["appRunning"].get<bool>());
  EXPECT_EQ(payload["appName"], "Steam Big Picture");
  ASSERT_EQ(payload["clients"].size(), 2U);
  EXPECT_EQ(payload["clients"][0]["id"], "A04551F1-C420-292A-7C8B-D3DBB4047207");
  EXPECT_EQ(payload["clients"][0]["kind"], "moonlight");
  EXPECT_EQ(payload["clients"][1]["kind"], "browser");
  EXPECT_EQ(payload["streams"][0]["fps"], 90);
  EXPECT_FALSE(payload["can"]["disconnect"].get<bool>());
}

TEST(SteamBigPicturePolicy, ParsesKnownActions) {
  const auto end = bpm::policy::parse_action({{"type", "action"}, {"action", "end_stream"}});
  ASSERT_TRUE(end.has_value());
  EXPECT_EQ(end->kind, bpm::policy::action_t::kind_e::end_stream);

  const auto disconnect = bpm::policy::parse_action({{"type", "action"}, {"action", "disconnect"}, {"client", "A04551F1-C420-292A-7C8B-D3DBB4047207"}});
  ASSERT_TRUE(disconnect.has_value());
  EXPECT_EQ(disconnect->kind, bpm::policy::action_t::kind_e::disconnect);
  EXPECT_EQ(disconnect->client, "A04551F1-C420-292A-7C8B-D3DBB4047207");
}

TEST(SteamBigPicturePolicy, RejectsMalformedActions) {
  EXPECT_FALSE(bpm::policy::parse_action(nlohmann::json::array()).has_value());
  EXPECT_FALSE(bpm::policy::parse_action({{"action", "quit_steam"}}).has_value());
  EXPECT_FALSE(bpm::policy::parse_action({{"action", 3}}).has_value());
  EXPECT_FALSE(bpm::policy::parse_action({{"action", "disconnect"}}).has_value());
  EXPECT_FALSE(bpm::policy::parse_action({{"action", "disconnect"}, {"client", ""}}).has_value());
  EXPECT_FALSE(bpm::policy::parse_action({{"action", "disconnect"}, {"client", "../../etc"}}).has_value());
  EXPECT_FALSE(bpm::policy::parse_action({{"action", "disconnect"}, {"client", std::string(bpm::protocol::max_client_id_bytes + 1, 'a')}}).has_value());
}

TEST(SteamBigPicturePolicy, GatesActionsByTimeAndConnectedClients) {
  bpm::policy::action_gate_t gate;
  using verdict = bpm::policy::action_gate_t::verdict_e;
  const auto now = bpm::policy::action_gate_t::clock::time_point {} + 1h;
  const std::vector<bpm::policy::client_t> clients {{"deck", "Steam Deck", bpm::policy::client_kind_e::moonlight}, {"web-1", "Browser", bpm::policy::client_kind_e::browser}};
  const bpm::policy::action_t disconnect_browser {bpm::policy::action_t::kind_e::disconnect, "web-1"};
  const bpm::policy::action_t disconnect_gone {bpm::policy::action_t::kind_e::disconnect, "left-already"};
  const bpm::policy::action_t end_stream {bpm::policy::action_t::kind_e::end_stream, {}};

  auto kind = bpm::policy::client_kind_e::moonlight;
  EXPECT_EQ(gate.check(disconnect_gone, clients, now, kind), verdict::not_connected);
  EXPECT_EQ(gate.check(disconnect_browser, clients, now, kind), verdict::allowed);
  EXPECT_EQ(kind, bpm::policy::client_kind_e::browser);
  EXPECT_EQ(gate.check(end_stream, clients, now + 1s, kind), verdict::too_soon);
  EXPECT_EQ(gate.check(end_stream, {}, now + 2s, kind), verdict::allowed) << "End Stream needs no connected client";
}

TEST(SteamBigPicturePolicy, NamesCodecs) {
  EXPECT_EQ(bpm::policy::codec_name(0), "H.264");
  EXPECT_EQ(bpm::policy::codec_name(1), "HEVC");
  EXPECT_EQ(bpm::policy::codec_name(2), "AV1");
  EXPECT_EQ(bpm::policy::codec_name(9), "");
}

TEST(SteamBigPictureHelperPolicy, AcceptsOnlyLocalDebuggerUrls) {
  EXPECT_TRUE(bpm::policy::is_local_debugger_url("ws://127.0.0.1:8080/devtools/page/5253526EF14E486B05E085CA24E0375E"));
  EXPECT_TRUE(bpm::policy::is_local_debugger_url("ws://localhost:8080/devtools/page/A"));
  EXPECT_TRUE(bpm::policy::is_local_debugger_url("ws://[::1]:8080/devtools/page/A"));

  EXPECT_FALSE(bpm::policy::is_local_debugger_url("ws://192.168.1.18:8080/devtools/page/A"));
  EXPECT_FALSE(bpm::policy::is_local_debugger_url("ws://127.0.0.1.example.com:8080/devtools/page/A"));
  EXPECT_FALSE(bpm::policy::is_local_debugger_url("ws://127.0.0.1:9222/devtools/page/A"));
  EXPECT_FALSE(bpm::policy::is_local_debugger_url("wss://127.0.0.1:8080/devtools/page/A"));
  EXPECT_FALSE(bpm::policy::is_local_debugger_url("ws://127.0.0.1:8080/devtools/"));
  EXPECT_FALSE(bpm::policy::is_local_debugger_url("ws://127.0.0.1:8080/devtools/page/A B"));
}

TEST(SteamBigPictureHelperPolicy, PutsKnownSharedContextTitlesFirst) {
  const auto targets = nlohmann::json::parse(R"([
    {"type":"service_worker","title":"Service Worker","url":"https://steamloopback.host/steamui_sw.js","webSocketDebuggerUrl":"ws://127.0.0.1:8080/devtools/page/SW"},
    {"type":"page","title":"Steam","url":"https://steamloopback.host/routes/library/home","webSocketDebuggerUrl":"ws://127.0.0.1:8080/devtools/page/MAIN"},
    {"type":"page","title":"data:text/html,<body></body>","url":"data:text/html,%3Cbody%3E","webSocketDebuggerUrl":"ws://127.0.0.1:8080/devtools/page/DATA"},
    {"type":"page","title":"Remote","url":"https://steamloopback.host/","webSocketDebuggerUrl":"ws://10.0.0.5:8080/devtools/page/REMOTE"},
    {"type":"page","title":"SharedJSContext","url":"https://steamloopback.host/routes/settings","webSocketDebuggerUrl":"ws://127.0.0.1:8080/devtools/page/SHARED"},
    {"type":"page","title":7}
  ])");
  const auto candidates = bpm::policy::shared_context_candidates(targets);
  ASSERT_EQ(candidates.size(), 2U);
  EXPECT_EQ(candidates[0].title, "SharedJSContext");
  EXPECT_EQ(candidates[0].ws_url, "ws://127.0.0.1:8080/devtools/page/SHARED");
  EXPECT_EQ(candidates[1].title, "Steam");
  EXPECT_TRUE(bpm::policy::shared_context_candidates(nlohmann::json::object()).empty());
}

TEST(SteamBigPictureHelperPolicy, TrustsOnlySteamsOwnWebHelper) {
  using bpm::policy::endpoint_e;
  EXPECT_EQ(bpm::policy::endpoint_verdict({}), endpoint_e::not_running);
  EXPECT_EQ(bpm::policy::endpoint_verdict({.steam_running = true}), endpoint_e::port_closed);
  EXPECT_EQ(bpm::policy::endpoint_verdict({.steam_running = true, .listening = true, .owner_is_steam_webhelper = false, .owner_in_helper_session = true}), endpoint_e::port_foreign);
  EXPECT_EQ(bpm::policy::endpoint_verdict({.steam_running = true, .listening = true, .owner_is_steam_webhelper = true, .owner_in_helper_session = false}), endpoint_e::port_foreign);
  EXPECT_EQ(bpm::policy::endpoint_verdict({.steam_running = true, .listening = true, .owner_is_steam_webhelper = true, .owner_in_helper_session = true}), endpoint_e::ok);
  EXPECT_EQ(bpm::policy::endpoint_state_name(endpoint_e::ok), "connecting");
  EXPECT_EQ(bpm::policy::endpoint_state_name(endpoint_e::port_closed), "port_closed");
}

TEST(SteamBigPictureHelperPolicy, BuildsInjectionAndStatusExpressions) {
  EXPECT_EQ(bpm::policy::inject_expression({{"checkOnly", true}}, "(() => 1)();"), "window.__vibeshineOptions = {\"checkOnly\":true};\n(() => 1)();");
  EXPECT_EQ(bpm::policy::set_status_expression({{"appRunning", false}}), "window.VibeshineBPM?.setStatus({\"appRunning\":false}), true");
}

TEST(SteamBigPictureHelperPolicy, AcceptsOnlyReportsOfThisProtocol) {
  const auto report = bpm::policy::report_message(
    nlohmann::json::parse(R"json({"protocol":1,"version":3,"react":"19.1.1","enabled":true,"errors":2,
      "features":{"power_menu":"ok (menu parts: learned)","header_icon":7}})json"),
    "Oct 22 2026 14:39:25"
  );
  ASSERT_TRUE(report.has_value());
  EXPECT_EQ((*report)["type"], "report");
  EXPECT_EQ((*report)["script_version"], 3);
  EXPECT_EQ((*report)["steam_build"], "Oct 22 2026 14:39:25");
  EXPECT_EQ((*report)["errors"], 2);
  EXPECT_EQ((*report)["features"], nlohmann::json({{"power_menu", "ok (menu parts: learned)"}}));

  EXPECT_FALSE(bpm::policy::report_message({{"protocol", 2}}, "").has_value());
  EXPECT_FALSE(bpm::policy::report_message({{"protocol", "1"}}, "").has_value());
  EXPECT_FALSE(bpm::policy::report_message(nlohmann::json::array(), "").has_value());
}

TEST(SteamBigPictureHelperPolicy, NormalisesScriptMessages) {
  EXPECT_EQ(bpm::policy::script_message(R"({"type":"action","action":"end_stream","extra":1})"), nlohmann::json({{"type", "action"}, {"action", "end_stream"}}));
  EXPECT_EQ(bpm::policy::script_message(R"({"type":"action","action":"disconnect","client":"deck-1"})"), nlohmann::json({{"type", "action"}, {"action", "disconnect"}, {"client", "deck-1"}}));
  EXPECT_EQ(bpm::policy::script_message(R"({"type":"disabled","reason":"8 errors"})"), nlohmann::json({{"type", "disabled"}, {"reason", "8 errors"}}));

  EXPECT_FALSE(bpm::policy::script_message(R"({"type":"action","action":"disconnect","client":"a b"})").has_value());
  EXPECT_FALSE(bpm::policy::script_message(R"({"type":"action","action":"quit_steam"})").has_value());
  EXPECT_FALSE(bpm::policy::script_message(R"({"type":"eval","code":"x"})").has_value());
  EXPECT_FALSE(bpm::policy::script_message(R"({"type":"feature","name":"power_menu"})").has_value());
  EXPECT_FALSE(bpm::policy::script_message("not json").has_value());
  EXPECT_FALSE(bpm::policy::script_message("").has_value());
  EXPECT_FALSE(bpm::policy::script_message(std::string(bpm::protocol::max_frame_bytes + 1, ' ')).has_value());
}

TEST(SteamBigPictureHelperPolicy, TruncatesLongTextOnCharacterBoundaries) {
  // 150 two-byte characters: a byte cut at 200 would otherwise be fine, but 199 would split one.
  std::string value;
  for (int i = 0; i < 150; ++i) {
    value += "\xC3\xA9";
  }
  const auto message = bpm::policy::script_message(nlohmann::json({{"type", "error"}, {"where", "header"}, {"message", "x" + value}}).dump());
  ASSERT_TRUE(message.has_value());
  EXPECT_EQ((*message)["message"].get<std::string>().size(), 299U);
  EXPECT_NO_THROW(message->dump());
}

TEST(SteamBigPictureInjectionTracker, BacksOffAfterFailures) {
  bpm::policy::injection_tracker_t tracker;
  const auto start = bpm::policy::injection_tracker_t::clock::time_point {} + 1h;
  EXPECT_TRUE(tracker.may_attempt(start));
  std::vector<std::chrono::seconds> delays;
  for (int i = 0; i < 7; ++i) {
    delays.push_back(std::chrono::duration_cast<std::chrono::seconds>(tracker.failed(start)));
  }
  EXPECT_EQ(delays, (std::vector<std::chrono::seconds> {2s, 4s, 8s, 16s, 32s, 60s, 60s}));
  EXPECT_FALSE(tracker.may_attempt(start + 59s));
  EXPECT_TRUE(tracker.may_attempt(start + 60s));

  tracker.injected(start + 60s);
  EXPECT_EQ(tracker.failed(start + 61s), 2s);
}

TEST(SteamBigPictureInjectionTracker, TripsAfterThreeQuickReloads) {
  bpm::policy::injection_tracker_t tracker;
  auto now = bpm::policy::injection_tracker_t::clock::time_point {} + 1h;
  for (int i = 0; i < 2; ++i) {
    tracker.injected(now);
    now += 5s;
    EXPECT_FALSE(tracker.reloaded(now));
  }
  tracker.injected(now);
  now += 5s;
  EXPECT_TRUE(tracker.reloaded(now));
  EXPECT_TRUE(tracker.tripped());
  EXPECT_FALSE(tracker.may_attempt(now + 1h));

  tracker.reset();
  EXPECT_FALSE(tracker.tripped());
  EXPECT_TRUE(tracker.may_attempt(now));
}

TEST(SteamBigPictureInjectionTracker, IgnoresSlowAndSpreadOutReloads) {
  bpm::policy::injection_tracker_t tracker;
  auto now = bpm::policy::injection_tracker_t::clock::time_point {} + 1h;
  for (int i = 0; i < 5; ++i) {
    tracker.injected(now);
    now += 45s;  // not within 30 s of injecting
    EXPECT_FALSE(tracker.reloaded(now));
  }
  for (int i = 0; i < 5; ++i) {
    tracker.injected(now);
    now += 5s;
    EXPECT_FALSE(tracker.reloaded(now));
    now += 3min;  // quick, but spread so no three fall within five minutes
  }
  EXPECT_FALSE(tracker.tripped());
}

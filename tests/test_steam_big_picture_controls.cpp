#include "src/steam_big_picture_controls_policy.h"
#include "src/steam_big_picture_controls_protocol.h"

#include <algorithm>
#include <gtest/gtest.h>

namespace bpm = platf::steam::big_picture;

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

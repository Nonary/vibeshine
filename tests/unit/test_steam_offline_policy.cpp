#include "src/steam_offline_policy.h"
#include "src/steam_offline_filter_ioctl.h"
#include "src/terminal_session_launch_codec.h"
#include "src/terminal_session_protocol.h"
#include "src/rtsp.h"

#include <cstddef>
#include <memory>

#include <gtest/gtest.h>

TEST(SteamOfflinePolicy, RecognizesOnlySteamClientImages) {
  EXPECT_TRUE(steam_offline::is_recognized_client_image("C:\\Steam\\steamwebhelper.exe"));
  EXPECT_TRUE(steam_offline::is_recognized_client_image("GAMEOVERLAYUI.EXE"));
  EXPECT_FALSE(steam_offline::is_recognized_client_image("CivilizationVI.exe"));
  EXPECT_FALSE(steam_offline::is_recognized_client_image("steamservice.exe"));
  EXPECT_FALSE(steam_offline::is_configured_steam_client("steamservice.exe -silent"));
  EXPECT_TRUE(steam_offline::is_configured_steam_client("  \"C:\\Steam Library\\steam.exe\" -silent"));
  EXPECT_FALSE(steam_offline::is_configured_steam_client("cmd.exe /c steam.exe -silent"));
}

TEST(SteamOfflinePolicy, AugmentsOnlyConfiguredSteamAndIsIdempotent) {
  const auto command = R"("C:\Program Files (x86)\Steam\steam.exe" -silent)";
  const auto augmented = steam_offline::append_ipc_override(command, "seat-7");
  EXPECT_NE(augmented, command);
  EXPECT_NE(augmented.find("-master_ipc_name_override vibeshine-seat-seat-7"), std::string::npos);
  EXPECT_EQ(steam_offline::append_ipc_override(augmented, "seat-7"), augmented);
  EXPECT_EQ(steam_offline::append_ipc_override("CivilizationVI.exe", "seat-7"), "CivilizationVI.exe");
  EXPECT_EQ(steam_offline::append_ipc_override("\"C:\\Steam\\steam.exe\" -x -master_ipc_name_override_fake", "seat-7"),
            "\"C:\\Steam\\steam.exe\" -x -master_ipc_name_override_fake -master_ipc_name_override vibeshine-seat-seat-7");
  EXPECT_EQ(steam_offline::append_ipc_override("\"C:\\Steam\\steam.exe\" -master_ipc_name_override=existing", "seat-7"),
            "\"C:\\Steam\\steam.exe\" -master_ipc_name_override=existing");
  EXPECT_LT(steam_offline::ipc_name_for_seat(std::string(4096, 'x')).size(), steam_offline::max_ipc_name_size + 1);
  EXPECT_TRUE(steam_offline::append_ipc_override("\"C:\\Steam\\steam.exe\" " +
                                                std::string(steam_offline::max_command_line_size, 'x'), "seat-7").empty());
}

TEST(SteamOfflinePolicy, LineageRejectsPidReuseAndPreservesGeneration) {
  steam_offline::lineage_registry_t registry {4};
  ASSERT_TRUE(registry.register_root({10, 100}, 7, "seat-7"));
  EXPECT_FALSE(registry.register_root({12, 120}, 8, "seat-8"));
  ASSERT_TRUE(registry.observe_child({11, 110}, {10, 100}, "steam.exe"));
  EXPECT_EQ(registry.state({11, 110}), steam_offline::lineage_state_e::blocked_client);
  EXPECT_EQ(registry.state({11, 111}), steam_offline::lineage_state_e::empty);
  EXPECT_TRUE(registry.generation_matches(7));
  EXPECT_TRUE(registry.registration_matches({10, 100}, 7, "seat-7"));
  EXPECT_FALSE(registry.registration_matches({10, 101}, 7, "seat-7"));
  EXPECT_FALSE(registry.registration_matches({10, 100}, 8, "seat-7"));
  EXPECT_FALSE(registry.registration_matches({10, 100}, 7, "seat-8"));
  EXPECT_FALSE(registry.remove({11, 110}));
  EXPECT_TRUE(registry.remove({10, 100}));
  EXPECT_EQ(registry.state({11, 110}), steam_offline::lineage_state_e::empty);
  EXPECT_FALSE(registry.generation_matches(7));
}

TEST(SteamOfflinePolicy, DriverRegistrationContractCarriesExactIdentity) {
  static_assert(sizeof(steam_offline::driver::register_root_t) == 88);
  static_assert(offsetof(steam_offline::driver::register_root_t, seat_id) == 24);
  static_assert(sizeof(steam_offline::driver::unregister_root_t) == 88);
  static_assert(offsetof(steam_offline::driver::unregister_root_t, seat_id) == 24);
  static_assert(sizeof(steam_offline::driver::registration_t) == 24);
  static_assert(sizeof(steam_offline::driver::status_t) == 24);
  EXPECT_EQ(steam_offline::driver::protocol_version, 1u);
  EXPECT_NE(steam_offline::driver::register_root_ioctl, steam_offline::driver::unregister_root_ioctl);
  EXPECT_NE(steam_offline::driver::status_ioctl, steam_offline::driver::register_root_ioctl);
}

TEST(SteamOfflinePolicy, LaunchCodecDefaultsIsolationOffAndRoundTripsOptIn) {
  EXPECT_FALSE(steam_offline::enabled_for_terminal(false, true));
  EXPECT_FALSE(steam_offline::enabled_for_terminal(true, false));
  EXPECT_TRUE(steam_offline::enabled_for_terminal(true, true));
  auto session = std::make_shared<rtsp_stream::launch_session_t>();
  session->id = 7;
  session->role_generation = 9;
  session->role = remote_session::role_e::game;
  session->terminal_session_requested = true;
  session->client_uuid = "paired-client";
  session->client_cert = "cert";
  session->gcm_key.assign(16, 1);
  session->iv.assign(16, 2);
  session->rtsp_cipher.emplace(session->gcm_key, false);
  session->rtsp_url_scheme = "rtspenc://";
  session->appid = 1;
  session->width = 1920;
  session->height = 1080;
  session->fps = 60;
  session->steam_offline_isolation = true;
  terminal_session::protocol::request_t request;
  request.operation = terminal_session::operation_e::launch;
  request.launch_session = session;
  std::string error;
  const auto encoded = terminal_session::launch_codec::encode(request, error);
  ASSERT_FALSE(encoded.empty()) << error;
  const auto decoded = terminal_session::launch_codec::decode(encoded, error);
  ASSERT_TRUE(decoded.has_value()) << error;
  ASSERT_TRUE(decoded->launch_session);
  EXPECT_TRUE(decoded->launch_session->steam_offline_isolation);

  session->terminal_session_requested = false;
  error.clear();
  const auto non_terminal_encoded = terminal_session::launch_codec::encode(request, error);
  ASSERT_FALSE(non_terminal_encoded.empty()) << error;
  const auto non_terminal_decoded = terminal_session::launch_codec::decode(non_terminal_encoded, error);
  ASSERT_TRUE(non_terminal_decoded.has_value()) << error;
  EXPECT_FALSE(non_terminal_decoded->launch_session->steam_offline_isolation);

  session->terminal_session_requested = true;
  session->steam_offline_isolation = false;
  error.clear();
  const auto default_encoded = terminal_session::launch_codec::encode(request, error);
  ASSERT_FALSE(default_encoded.empty()) << error;
  const auto default_decoded = terminal_session::launch_codec::decode(default_encoded, error);
  ASSERT_TRUE(default_decoded.has_value()) << error;
  EXPECT_FALSE(default_decoded->launch_session->steam_offline_isolation);
}

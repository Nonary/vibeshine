#include "src/steam_offline_policy.h"
#include "src/steam_offline_policy.h"
#include "src/terminal_session_launch_codec.h"
#include "src/terminal_session_protocol.h"
#include "src/rtsp.h"

#include <cstddef>
#include <memory>

#include <gtest/gtest.h>

TEST(SteamOfflinePolicy, KeepsGameLibrariesOutsideTheClientMirror) {
  EXPECT_TRUE(steam_offline::complete_job_process_list(4, 4));
  EXPECT_FALSE(steam_offline::complete_job_process_list(4, 3));
  EXPECT_TRUE(steam_offline::game_library_outside_mirror(
    "C:/SteamLibrary/steamapps/common/Game/game.exe", "C:/ProgramData/VibeshineSteamSeats/seat/1/client"));
  EXPECT_FALSE(steam_offline::game_library_outside_mirror(
    "C:/ProgramData/VibeshineSteamSeats/seat/1/client/steam.exe",
    "C:/ProgramData/VibeshineSteamSeats/seat/1/client"));
  EXPECT_TRUE(steam_offline::path_is_same_or_descendant("C:/foo/bar", "C:/foo"));
  EXPECT_FALSE(steam_offline::path_is_same_or_descendant("C:/foobar/bar", "C:/foo"));
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

TEST(SteamOfflinePolicy, RewritesOnlySteamClientAndUsesSeatPrivateProfile) {
  const auto command = steam_offline::rewrite_client_command(
    R"("C:\Program Files (x86)\Steam\steam.exe" -silent)",
    R"(C:\ProgramData\VibeshineSteamSeats\seat-7\9\client)",
    R"(C:\ProgramData\VibeshineSteamSeats\seat-7\9\cache)", "seat-7");
  EXPECT_NE(command.find(R"(client\steam.exe)"), std::string::npos);
  EXPECT_NE(command.find(R"(-cachedir "C:\ProgramData\VibeshineSteamSeats\seat-7\9\cache\htmlcache")"), std::string::npos);
  EXPECT_NE(command.find(R"(-userdatadir "C:\ProgramData\VibeshineSteamSeats\seat-7\9\cache\userdata")"), std::string::npos);
  EXPECT_EQ(steam_offline::rewrite_client_command("CivilizationVI.exe", "mirror", "cache", "seat-7"), "");
}

TEST(SteamOfflinePolicy, FilterOwnershipKeyBindsSeatGenerationPathAndFamily) {
  const auto v4 = steam_offline::deterministic_filter_key("seat-7", 9, "C:/mirror/steam.exe", false);
  EXPECT_EQ(v4, steam_offline::deterministic_filter_key("seat-7", 9, "C:/mirror/steam.exe", false));
  EXPECT_NE(v4, steam_offline::deterministic_filter_key("seat-8", 9, "C:/mirror/steam.exe", false));
  EXPECT_NE(v4, steam_offline::deterministic_filter_key("seat-7", 10, "C:/mirror/steam.exe", false));
  EXPECT_NE(v4, steam_offline::deterministic_filter_key("seat-7", 9, "C:/mirror/steam.exe", true));
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

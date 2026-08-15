#include <gtest/gtest.h>

#include <stdexcept>

#include "src/rtsp.h"
#include "src/terminal_session_broker.h"

namespace {
  std::shared_ptr<rtsp_stream::launch_session_t> launch_material(
    const std::uint32_t id = 7,
    std::string client_uuid = "paired-client"
  ) {
    auto session = std::make_shared<rtsp_stream::launch_session_t>();
    session->id = id;
    session->client_uuid = std::move(client_uuid);
    session->role = remote_session::role_e::game;
    session->terminal_session_requested = true;
    return session;
  }
}

TEST(TerminalSessionBroker, RejectsUnauthenticatedOrIncompleteRequestsBeforeCallingRuntime) {
  int calls = 0;
  terminal_session::register_runtime_hooks({
    .prepare = [&calls](terminal_session::request_t) {
      ++calls;
      return terminal_session::route_t {};
    },
  });

  EXPECT_FALSE(terminal_session::prepare({.launch_session = {}}).accepted);
  EXPECT_FALSE(terminal_session::prepare({.launch_session = launch_material(0)}).accepted);
  EXPECT_FALSE(terminal_session::prepare({.launch_session = launch_material(7, "")}).accepted);
  auto wrong_role = launch_material();
  wrong_role->role = remote_session::role_e::monitor;
  EXPECT_FALSE(terminal_session::prepare({.launch_session = std::move(wrong_role)}).accepted);
  auto not_requested = launch_material();
  not_requested->terminal_session_requested = false;
  EXPECT_FALSE(terminal_session::prepare({.launch_session = std::move(not_requested)}).accepted);
  EXPECT_EQ(calls, 0);

  terminal_session::register_runtime_hooks({});
}

TEST(TerminalSessionBroker, MissingRuntimeFailsClosedAndRemainsRetryable) {
  terminal_session::register_runtime_hooks({});
  const auto route = terminal_session::prepare({.launch_session = launch_material()});
  EXPECT_FALSE(route.accepted);
  EXPECT_FALSE(route.ready);
  EXPECT_TRUE(route.retryable);
  EXPECT_FALSE(route.error.empty());
}

TEST(TerminalSessionBroker, RuntimeReceivesExactAuthenticatedLaunchMaterial) {
  terminal_session::operation_e operation {};
  std::string client_uuid;
  std::unordered_map<std::string, std::string> overrides;
  terminal_session::register_runtime_hooks({
    .prepare = [&](terminal_session::request_t request) {
      operation = request.operation;
      client_uuid = request.launch_session->client_uuid;
      overrides = std::move(request.runtime_config_overrides);
      return terminal_session::route_t {
        .accepted = true,
        .ready = true,
        .rtsp_port = 58021,
        .control_port = 58022,
        .video_port = 58023,
        .audio_port = 58024,
        .windows_session_id = 12,
        .seat_id = "seat-paired-client",
      };
    },
  });

  const auto route = terminal_session::prepare({
    .operation = terminal_session::operation_e::resume,
    .launch_session = launch_material(),
    .runtime_config_overrides = {{"capture", "wgc"}},
  });
  EXPECT_EQ(operation, terminal_session::operation_e::resume);
  EXPECT_EQ(client_uuid, "paired-client");
  EXPECT_EQ(overrides.at("capture"), "wgc");
  EXPECT_TRUE(route.accepted);
  EXPECT_TRUE(route.ready);
  EXPECT_EQ(route.rtsp_port, 58021);

  terminal_session::register_runtime_hooks({});
}

TEST(TerminalSessionBroker, ReadyRouteRequiresAcceptanceAndAListenerPort) {
  terminal_session::register_runtime_hooks({
    .prepare = [](terminal_session::request_t) {
      return terminal_session::route_t {.accepted = true, .ready = true};
    },
  });
  const auto route = terminal_session::prepare({.launch_session = launch_material()});
  EXPECT_FALSE(route.accepted);
  EXPECT_FALSE(route.ready);
  EXPECT_FALSE(route.retryable);
  EXPECT_FALSE(route.error.empty());

  terminal_session::register_runtime_hooks({});
}

TEST(TerminalSessionBroker, RuntimeFailureCannotEscapeTheControlPlane) {
  terminal_session::register_runtime_hooks({
    .prepare = [](terminal_session::request_t) -> terminal_session::route_t {
      throw std::runtime_error("private IPC failed");
    },
  });
  const auto route = terminal_session::prepare({.launch_session = launch_material()});
  EXPECT_FALSE(route.accepted);
  EXPECT_FALSE(route.ready);
  EXPECT_TRUE(route.retryable);
  EXPECT_NE(route.error.find("private IPC failed"), std::string::npos);

  terminal_session::register_runtime_hooks({});
}

TEST(TerminalSessionBroker, LifecycleHooksAreScopedToTheAuthenticatedClient) {
  std::string snapshot_uuid;
  std::string disconnect_uuid;
  std::string disconnect_reason;
  std::string unpaired_uuid;
  bool shutdown = false;
  terminal_session::register_runtime_hooks({
    .prepare = [](terminal_session::request_t) { return terminal_session::route_t {}; },
    .snapshot = [&](std::string_view uuid) {
      snapshot_uuid = uuid;
      return terminal_session::state_t {.exists = true, .connected = true, .app_id = 42};
    },
    .disconnect = [&](std::string_view uuid, std::string_view reason) {
      disconnect_uuid = uuid;
      disconnect_reason = reason;
      return true;
    },
    .unpair = [&](std::string_view uuid) { unpaired_uuid = uuid; },
    .shutdown = [&] { shutdown = true; },
  });

  EXPECT_TRUE(terminal_session::runtime_available());
  EXPECT_EQ(terminal_session::snapshot("paired-client").app_id, 42);
  EXPECT_TRUE(terminal_session::disconnect("paired-client", "Web UI disconnect"));
  terminal_session::notify_unpair("paired-client");
  terminal_session::notify_shutdown();
  EXPECT_EQ(snapshot_uuid, "paired-client");
  EXPECT_EQ(disconnect_uuid, "paired-client");
  EXPECT_EQ(disconnect_reason, "Web UI disconnect");
  EXPECT_EQ(unpaired_uuid, "paired-client");
  EXPECT_TRUE(shutdown);

  terminal_session::register_runtime_hooks({});
}

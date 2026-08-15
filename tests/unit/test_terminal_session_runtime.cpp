#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>

#include "src/rtsp.h"
#include "src/terminal_session_protocol.h"
#include "src/terminal_session_runtime.h"
#include "src/terminal_session_service.h"
#include "src/terminal_session_worker.h"

namespace {
  std::shared_ptr<rtsp_stream::launch_session_t> material(std::string uuid = "client", std::uint32_t id = 7, std::uint64_t generation = 9) {
    auto result = std::make_shared<rtsp_stream::launch_session_t>();
    result->id = id;
    result->role_generation = generation;
    result->client_uuid = std::move(uuid);
    result->terminal_session_requested = true;
    result->role = remote_session::role_e::game;
    return result;
  }

  class fake_provider final: public terminal_session::seat_provider_t {
  public:
    bool fail_allocate {};
    int allocations {};
    int releases {};
    terminal_session::provider_capability_t preflight() override { return {.supported = true, .concurrent_sessions = true, .remote_display = true, .audio_endpoint = true, .token_launch = true}; }
    std::optional<terminal_session::provider_resource_t> allocate(const terminal_session::provider_request_t &, std::string &error) override {
      ++allocations;
      if (fail_allocate) { error = "fake provider rejected allocation"; return std::nullopt; }
      return terminal_session::provider_resource_t {.windows_session_id = 42, .seat_id = "seat-client", .opaque_id = 1, .rtsp_port = 58021, .control_port = 58022, .video_port = 58023, .audio_port = 58024};
    }
    void release(const terminal_session::provider_resource_t &) noexcept override { ++releases; }
  };

  class fake_worker final: public terminal_session::seat_worker_t {
  public:
    bool fail_start {};
    int starts {};
    int stops {};
    std::optional<terminal_session::route_t> start(const terminal_session::worker_request_t &, std::string &error) override {
      ++starts;
      if (fail_start) { error = "fake worker rejected start"; return std::nullopt; }
      return terminal_session::route_t {.accepted = true, .ready = true, .rtsp_port = 58021, .control_port = 58022, .video_port = 58023, .audio_port = 58024};
    }
    bool stop(const terminal_session::route_t &) noexcept override { ++stops; return true; }
  };
}

TEST(TerminalSessionProtocol, CodecRejectsBoundsAndRoundTripsCompleteRoute) {
  terminal_session::protocol::request_t request;
  request.operation = terminal_session::protocol::opcode::prepare;
  request.client_uuid = "paired-client";
  request.generation = 9;
  request.launch_id = 7;
  terminal_session::protocol::admission_authority authority {11};
  request.ticket = authority.issue(request.client_uuid, request.generation, request.launch_id);
  const auto encoded = terminal_session::protocol::encode(request);
  ASSERT_FALSE(encoded.empty());
  EXPECT_TRUE(terminal_session::protocol::decode_request(encoded).has_value());
  EXPECT_FALSE(terminal_session::protocol::decode_request(std::vector<std::uint8_t>(terminal_session::protocol::max_message_size + 1)).has_value());

  terminal_session::protocol::response_t response {.accepted = true, .client_uuid = request.client_uuid, .generation = 9, .launch_id = 7,
    .windows_session_id = 42, .seat_id = "seat-client", .rtsp_port = 58021, .control_port = 58022, .video_port = 58023, .audio_port = 58024};
  const auto response_bytes = terminal_session::protocol::encode(response);
  const auto decoded = terminal_session::protocol::decode_response(response_bytes);
  ASSERT_TRUE(decoded.has_value());
  EXPECT_EQ(decoded->audio_port, 58024);
}

TEST(TerminalSessionProtocol, AdmissionIsAuthenticatedOneUseAndBoundToIdentity) {
  terminal_session::protocol::admission_authority authority {22};
  const auto ticket = authority.issue("client", 3, 8);
  terminal_session::protocol::request_t request {.operation = terminal_session::protocol::opcode::prepare, .client_uuid = "client", .generation = 3, .launch_id = 8, .ticket = ticket,
    .peer = {.pid = 100, .sid = "S-1-5-18", .authenticated = true}};
  EXPECT_FALSE(authority.consume(request).has_value());
  EXPECT_EQ(authority.consume(request), terminal_session::protocol::reject_reason::replayed_ticket);
  auto wrong = request;
  wrong.client_uuid = "other";
  EXPECT_EQ(authority.consume(wrong), terminal_session::protocol::reject_reason::wrong_client);
  auto unauthenticated = request;
  unauthenticated.ticket = authority.issue("client", 3, 9);
  unauthenticated.launch_id = 9;
  unauthenticated.peer.authenticated = false;
  EXPECT_EQ(authority.consume(unauthenticated), terminal_session::protocol::reject_reason::unauthenticated_peer);
  const auto expired = authority.issue("expired", 1, 1, terminal_session::protocol::admission_authority::clock_t::now() - terminal_session::protocol::ticket_lifetime - std::chrono::seconds {1});
  terminal_session::protocol::request_t expired_request {.operation = terminal_session::protocol::opcode::prepare, .client_uuid = "expired", .generation = 1, .launch_id = 1, .ticket = expired,
    .peer = {.pid = 100, .sid = "S-1-5-18", .authenticated = true}};
  EXPECT_EQ(authority.consume(expired_request), terminal_session::protocol::reject_reason::expired_ticket);
}

TEST(TerminalSessionProtocol, ServiceEndpointAuthenticatesBeforeHandler) {
  int calls = 0;
  terminal_session::service::endpoint_t endpoint {[&calls](const terminal_session::protocol::request_t &request) {
    ++calls;
    return terminal_session::protocol::response_t {.accepted = true, .client_uuid = request.client_uuid, .generation = request.generation, .launch_id = request.launch_id};
  }};
  auto request = terminal_session::protocol::request_t {.operation = terminal_session::protocol::opcode::prepare, .client_uuid = "client", .generation = 3, .launch_id = 8};
  request.ticket = endpoint.admissions().issue("client", 3, 8);
  const auto bytes = terminal_session::protocol::encode(request);
  const auto response = terminal_session::protocol::decode_response(endpoint.handle(bytes, {.pid = 101, .sid = "S-1-5-18", .authenticated = true}));
  ASSERT_TRUE(response.has_value());
  EXPECT_TRUE(response->accepted);
  EXPECT_EQ(calls, 1);
  const auto replay = terminal_session::protocol::decode_response(endpoint.handle(bytes, {.pid = 101, .sid = "S-1-5-18", .authenticated = true}));
  ASSERT_TRUE(replay.has_value());
  EXPECT_FALSE(replay->accepted);
  EXPECT_EQ(calls, 1);
}

TEST(TerminalSessionRuntime, RollbackAndIdempotentCleanupAreExact) {
  auto provider = std::make_unique<fake_provider>();
  auto *provider_raw = provider.get();
  auto worker = std::make_unique<fake_worker>();
  auto *worker_raw = worker.get();
  terminal_session::runtime_t runtime {std::move(provider), std::move(worker)};
  const auto route = runtime.prepare({.operation = terminal_session::operation_e::launch, .launch_session = material()});
  ASSERT_TRUE(route.ready);
  EXPECT_EQ(route.control_port, 58022);
  EXPECT_TRUE(runtime.disconnect("client", "cancel"));
  EXPECT_TRUE(runtime.disconnect("client", "duplicate cancel"));
  EXPECT_EQ(provider_raw->allocations, 1);
  EXPECT_EQ(provider_raw->releases, 1);
  EXPECT_EQ(worker_raw->starts, 1);
  EXPECT_EQ(worker_raw->stops, 1);
}

TEST(TerminalSessionRuntime, ProviderFailureNeverLeavesASeat) {
  auto provider = std::make_unique<fake_provider>();
  provider->fail_allocate = true;
  auto worker = std::make_unique<fake_worker>();
  terminal_session::runtime_t runtime {std::move(provider), std::move(worker)};
  const auto route = runtime.prepare({.operation = terminal_session::operation_e::launch, .launch_session = material()});
  EXPECT_FALSE(route.accepted);
  EXPECT_FALSE(runtime.snapshot("client").exists);
}

TEST(TerminalSessionRuntime, WorkerContractDisablesConsoleWideOwnership) {
  const auto contract = terminal_session::worker::make_contract("seat-client", 58021, 58022, 58023, 58024);
  const auto args = terminal_session::worker::command_line(contract);
  EXPECT_NE(std::find(args.begin(), args.end(), "--disable-web-ui"), args.end());
  EXPECT_NE(std::find(args.begin(), args.end(), "--disable-pairing"), args.end());
  EXPECT_NE(std::find(args.begin(), args.end(), "--disable-mdns"), args.end());
  EXPECT_NE(std::find(args.begin(), args.end(), "--disable-global-display-mutations"), args.end());
  EXPECT_NE(std::find(args.begin(), args.end(), "--admit-ticket-from-protected-pipe"), args.end());
}

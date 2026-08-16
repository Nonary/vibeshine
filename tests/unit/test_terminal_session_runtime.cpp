#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstring>

#include "src/rtsp.h"
#include "src/terminal_session_protocol.h"
#include "src/terminal_session_hdr_policy.h"
#include "src/terminal_session_launch_codec.h"
#include "src/terminal_session_runtime.h"
#include "src/terminal_session_service.h"
#include "src/terminal_session_worker.h"
#include "src/terminal_session_worker_process.h"
#ifdef _WIN32
  #include "src/platform/windows/ipc/pipes.h"
#endif

namespace {
  terminal_session::protocol::ticket_t issue_ticket(terminal_session::protocol::admission_authority &authority, std::string_view uuid, std::uint64_t generation, std::uint32_t launch_id,
                                                    terminal_session::protocol::admission_authority::clock_t::time_point now = terminal_session::protocol::admission_authority::clock_t::now()) {
    auto ticket = authority.issue(uuid, generation, launch_id, now);
    EXPECT_TRUE(ticket.has_value());
    return *ticket;
  }
  std::shared_ptr<rtsp_stream::launch_session_t> material(std::string uuid = "client", std::uint32_t id = 7, std::uint64_t generation = 9) {
    auto result = std::make_shared<rtsp_stream::launch_session_t>();
    result->id = id;
    result->role_generation = generation;
    result->client_uuid = std::move(uuid);
    result->terminal_session_requested = true;
    result->role = remote_session::role_e::game;
    result->gcm_key.assign(16, 1);
    result->iv.assign(16, 2);
    result->rtsp_url_scheme = "rtspenc://";
    result->rtsp_cipher.emplace(result->gcm_key, false);
    result->rtsp_source_address = "127.0.0.1";
    result->unique_id = "test-device";
    result->appid = 1;
    result->width = 1920;
    result->height = 1080;
    result->fps = 60;
    return result;
  }

  class fake_provider final: public terminal_session::seat_provider_t {
  public:
    bool fail_allocate {};
    bool fail_release {};
    int allocations {};
    int releases {};
    terminal_session::provider_capability_t preflight() override { return {.supported = true, .concurrent_sessions = true, .remote_display = true, .audio_endpoint = true, .token_launch = true}; }
    std::optional<terminal_session::provider_resource_t> allocate(const terminal_session::provider_request_t &, std::string &error) override {
      ++allocations;
      if (fail_allocate) { error = "fake provider rejected allocation"; return std::nullopt; }
      return terminal_session::provider_resource_t {.windows_session_id = 42, .seat_id = "seat-client", .opaque_id = 1, .rtsp_port = 58021, .control_port = 58022, .video_port = 58023, .audio_port = 58024};
    }
    void release(const terminal_session::provider_resource_t &) noexcept override { ++releases; }
    bool release_checked(const terminal_session::provider_resource_t &resource) noexcept override {
      release(resource);
      return !fail_release;
    }
  };

  class fake_worker final: public terminal_session::seat_worker_t {
  public:
    bool fail_start {};
    bool fail_stop {};
    bool cleanup_pending {};
    int starts {};
    int resumes {};
    int parks {};
    int stops {};
    std::optional<terminal_session::route_t> start(const terminal_session::worker_request_t &, std::string &error) override {
      ++starts;
      if (fail_start) { cleanup_pending = true; error = "fake worker rejected start"; return std::nullopt; }
      cleanup_pending = true;
      return terminal_session::route_t {.accepted = true, .ready = true, .rtsp_port = 58021, .control_port = 58022, .video_port = 58023, .audio_port = 58024};
    }
    std::optional<terminal_session::route_t> resume(const terminal_session::worker_request_t &, std::string &) override {
      ++resumes;
      return terminal_session::route_t {.accepted = true, .ready = true, .rtsp_port = 58021, .control_port = 58022, .video_port = 58023, .audio_port = 58024};
    }
    bool park(const terminal_session::route_t &) noexcept override { ++parks; return true; }
    bool stop(const terminal_session::route_t &) noexcept override { ++stops; if (!fail_stop) cleanup_pending = false; return !fail_stop; }
    bool cleanup_needed() const noexcept override { return cleanup_pending; }
  };
}

TEST(TerminalSessionHdrPolicy, ActivatesOnlyARequestedEligibleInactiveDisplay) {
  using terminal_session::hdr::action_e;
  using terminal_session::hdr::decide;
  using terminal_session::hdr::display_state_t;

  EXPECT_EQ(decide(false, {}), action_e::skip);
  EXPECT_EQ(decide(true, {.found = true, .supported = true, .user_enabled = true, .bits_per_color_channel = 10}), action_e::activate);
  EXPECT_EQ(decide(true, {.found = true, .supported = true, .user_enabled = true, .active = true, .hdr_color_mode = true, .bits_per_color_channel = 10}), action_e::accept_active);
  EXPECT_EQ(decide(true, {.found = true, .supported = false, .user_enabled = true, .bits_per_color_channel = 10}), action_e::reject);
  EXPECT_EQ(decide(true, {.found = true, .supported = true, .user_enabled = false, .bits_per_color_channel = 10}), action_e::reject);
  EXPECT_EQ(decide(true, {.found = true, .supported = true, .user_enabled = true, .bits_per_color_channel = 8}), action_e::reject);
}

TEST(TerminalSessionProtocol, CodecRejectsBoundsAndRoundTripsCompleteRoute) {
  terminal_session::protocol::request_t request;
  request.operation = terminal_session::protocol::opcode::prepare;
  request.client_uuid = "paired-client";
  request.generation = 9;
  request.launch_id = 7;
  terminal_session::protocol::admission_authority authority {11};
  request.ticket = issue_ticket(authority, request.client_uuid, request.generation, request.launch_id);
  const auto encoded = terminal_session::protocol::encode(request);
  ASSERT_FALSE(encoded.empty());
  EXPECT_TRUE(terminal_session::protocol::decode_request(encoded).has_value());
  EXPECT_FALSE(terminal_session::protocol::decode_request(std::vector<std::uint8_t>(terminal_session::protocol::max_message_size + 1)).has_value());

  terminal_session::protocol::response_t response {.accepted = true, .client_uuid = request.client_uuid, .generation = 9, .launch_id = 7,
    .windows_session_id = 42, .seat_id = "seat-client", .rtsp_port = 58021, .control_port = 58022, .video_port = 58023, .audio_port = 58024,
    .state_exists = true, .state_connected = false, .owner_generation = 9, .owner_launch_id = 7,
    .app_id = 1, .app_uuid = "app-uuid", .app_name = "Desktop"};
  const auto response_bytes = terminal_session::protocol::encode(response);
  const auto decoded = terminal_session::protocol::decode_response(response_bytes);
  ASSERT_TRUE(decoded.has_value());
  EXPECT_EQ(decoded->audio_port, 58024);
  EXPECT_TRUE(decoded->state_exists);
  EXPECT_FALSE(decoded->state_connected);
  EXPECT_EQ(decoded->owner_generation, 9u);
  EXPECT_EQ(decoded->app_uuid, "app-uuid");
}

TEST(TerminalSessionProtocol, WorkerHdrActivationRoundTripsAndRejectsWorkerTicket) {
  terminal_session::protocol::admission_authority authority {31};
  terminal_session::protocol::request_t request;
  request.operation = terminal_session::protocol::opcode::worker_hdr_activate;
  request.client_uuid = "paired-client";
  request.generation = 12;
  request.launch_id = 34;
  request.ticket = issue_ticket(authority, request.client_uuid, request.generation, request.launch_id);
  request.launch_payload.resize(sizeof(terminal_session::hdr::activation_capability_t));
  const auto capability = terminal_session::hdr::activation_capability_t {
    .session_id = 42,
    .source_adapter_low = 0x11223344,
    .source_adapter_high = 0x55667788,
    .source_id = 3,
    .target_adapter_low = 0x99aabbcc,
    .target_adapter_high = 0xddeeff00,
    .target_id = 4,
  };
  std::memcpy(request.launch_payload.data(), &capability, sizeof(capability));

  const auto encoded = terminal_session::protocol::encode(request);
  ASSERT_FALSE(encoded.empty());
  const auto decoded = terminal_session::protocol::decode_request(encoded);
  ASSERT_TRUE(decoded.has_value());
  EXPECT_EQ(decoded->operation, terminal_session::protocol::opcode::worker_hdr_activate);
  EXPECT_EQ(decoded->ticket.operation, terminal_session::protocol::opcode::prepare);
  EXPECT_EQ(decoded->launch_payload, request.launch_payload);

  request.ticket.operation = terminal_session::protocol::opcode::worker_hdr_activate;
  EXPECT_FALSE(terminal_session::protocol::decode_request(terminal_session::protocol::encode(request)).has_value());
}

TEST(TerminalSessionHdrPolicy, CapabilityLayoutIsFixedAndBrokerBound) {
  using terminal_session::hdr::activation_capability_t;
  static_assert(sizeof(activation_capability_t) == 48);
  static_assert(offsetof(activation_capability_t, parent_pid) == 12);
  static_assert(offsetof(activation_capability_t, session_id) == 16);
  static_assert(offsetof(activation_capability_t, source_adapter_low) == 20);
  static_assert(offsetof(activation_capability_t, source_adapter_high) == 24);
  static_assert(offsetof(activation_capability_t, source_id) == 28);
  static_assert(offsetof(activation_capability_t, target_adapter_low) == 32);
  static_assert(offsetof(activation_capability_t, target_adapter_high) == 36);
  static_assert(offsetof(activation_capability_t, target_id) == 40);
  static_assert(offsetof(activation_capability_t, reserved) == 44);

  const activation_capability_t capability {
    .parent_pid = 900,
    .session_id = 42,
    .source_adapter_low = 1,
    .source_adapter_high = 2,
    .source_id = 3,
    .target_adapter_low = 5,
    .target_adapter_high = 6,
    .target_id = 4,
  };
  EXPECT_EQ(capability.magic, terminal_session::hdr::activation_capability_magic);
  EXPECT_EQ(capability.size, sizeof(activation_capability_t));
  EXPECT_EQ(capability.version, terminal_session::hdr::activation_capability_version);
  EXPECT_EQ(capability.parent_pid, 900u);
  EXPECT_EQ(capability.reserved, 0u);
  const auto binding = terminal_session::hdr::binding_from_capability(capability);
  EXPECT_EQ(binding.session_id, 42u);
  EXPECT_EQ(binding.source_adapter_low, 1u);
  EXPECT_EQ(binding.source_adapter_high, 2u);
  EXPECT_EQ(binding.target_adapter_low, 5u);
  EXPECT_EQ(binding.target_adapter_high, 6u);
  EXPECT_EQ(binding.target_id, 4u);
}

TEST(TerminalSessionWorkerPolicy, InitialWorkerObjectsAreLocalSystemOnly)
{
  EXPECT_EQ(terminal_session::worker::worker_process_security_sddl, L"O:SYG:SYD:P(A;;GA;;;SY)");
}

TEST(TerminalSessionProtocol, PublicServiceEndpointRejectsWorkerHdrActivation) {
  terminal_session::service::endpoint_t endpoint {[](const terminal_session::protocol::request_t &) {
    return terminal_session::protocol::response_t {};
  }};
  terminal_session::protocol::request_t request;
  request.operation = terminal_session::protocol::opcode::worker_hdr_activate;
  request.client_uuid = "client";
  request.generation = 3;
  request.launch_id = 8;
  request.ticket = issue_ticket(endpoint.admissions(), request.client_uuid, request.generation, request.launch_id);
  request.launch_payload.resize(sizeof(terminal_session::hdr::activation_capability_t));
  const auto response = terminal_session::protocol::decode_response(endpoint.handle(
    terminal_session::protocol::encode(request), {.pid = 101, .sid = "S-1-5-18", .authenticated = true}));
  ASSERT_TRUE(response.has_value());
  EXPECT_FALSE(response->accepted);
  EXPECT_EQ(response->reason, terminal_session::protocol::reject_reason::invalid_state);
}

TEST(TerminalSessionProtocol, LaunchMaterialRoundTripsOnlyInsideItsBound) {
  auto session = material("paired-client", 17, 19);
  session->client_name = "Living Room";
  session->device_name = "Moonlight";
  session->app_metadata = rtsp_stream::launch_session_t::app_metadata_t {
    "1", "app-uuid", "Desktop", false, true, false, false
  };
  terminal_session::request_t request {
    .operation = terminal_session::operation_e::launch,
    .launch_session = session,
    .runtime_config_overrides = {{"fps", "120"}, {"audio_sink", "console-endpoint"}, {"dd_activate_virtual_display", "true"}},
  };
  std::string error;
  const auto encoded = terminal_session::launch_codec::encode(request, error);
  ASSERT_FALSE(encoded.empty()) << error;
  const auto decoded = terminal_session::launch_codec::decode(encoded, error);
  ASSERT_TRUE(decoded.has_value()) << error;
  ASSERT_TRUE(decoded->launch_session->app_metadata.has_value());
  EXPECT_EQ(decoded->launch_session->client_uuid, "paired-client");
  EXPECT_EQ(decoded->launch_session->app_metadata->uuid, "app-uuid");
  EXPECT_EQ(decoded->runtime_config_overrides.at("fps"), "120");
  EXPECT_FALSE(decoded->runtime_config_overrides.contains("audio_sink"));
  EXPECT_FALSE(decoded->runtime_config_overrides.contains("dd_activate_virtual_display"));

  std::vector<std::uint8_t> oversized(terminal_session::protocol::max_launch_payload_size + 1);
  EXPECT_FALSE(terminal_session::launch_codec::decode(oversized, error).has_value());
}

TEST(TerminalSessionProtocol, AdmissionIsAuthenticatedOneUseAndBoundToIdentity) {
  terminal_session::protocol::admission_authority authority {22};
  const auto ticket = issue_ticket(authority, "client", 3, 8);
  terminal_session::protocol::request_t request {.operation = terminal_session::protocol::opcode::prepare, .client_uuid = "client", .generation = 3, .launch_id = 8, .ticket = ticket,
    .peer = {.pid = 100, .sid = "S-1-5-18", .authenticated = true}};
  EXPECT_FALSE(authority.consume(request).has_value());
  EXPECT_EQ(authority.consume(request), terminal_session::protocol::reject_reason::replayed_ticket);
  auto wrong = request;
  wrong.client_uuid = "other";
  EXPECT_EQ(authority.consume(wrong), terminal_session::protocol::reject_reason::wrong_client);
  auto unauthenticated = request;
  unauthenticated.ticket = issue_ticket(authority, "client", 3, 9);
  unauthenticated.launch_id = 9;
  unauthenticated.peer.authenticated = false;
  EXPECT_EQ(authority.consume(unauthenticated), terminal_session::protocol::reject_reason::unauthenticated_peer);
  const auto expired = issue_ticket(authority, "expired", 1, 1, terminal_session::protocol::admission_authority::clock_t::now() - terminal_session::protocol::ticket_lifetime - std::chrono::seconds {1});
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
  request.ticket = issue_ticket(endpoint.admissions(), "client", 3, 8);
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
  EXPECT_TRUE(runtime.snapshot("client").connected);
  EXPECT_TRUE(runtime.disconnect("client", "cancel"));
  EXPECT_TRUE(runtime.disconnect("client", "duplicate cancel"));
  EXPECT_EQ(provider_raw->allocations, 1);
  EXPECT_EQ(provider_raw->releases, 1);
  EXPECT_EQ(worker_raw->starts, 1);
  EXPECT_EQ(worker_raw->parks, 1);
  EXPECT_EQ(worker_raw->stops, 0);
  EXPECT_TRUE(runtime.snapshot("client").exists);
  runtime.unpair("client");
  EXPECT_EQ(worker_raw->stops, 1);
  EXPECT_FALSE(runtime.snapshot("client").exists);
}

TEST(TerminalSessionRuntime, ResumeReconnectsTheRetainedSeatAndWorker) {
  auto provider = std::make_unique<fake_provider>();
  auto *provider_raw = provider.get();
  auto worker = std::make_unique<fake_worker>();
  auto *worker_raw = worker.get();
  terminal_session::runtime_t runtime {std::move(provider), std::move(worker)};
  ASSERT_TRUE(runtime.prepare({.operation = terminal_session::operation_e::launch, .launch_session = material()}).ready);
  ASSERT_TRUE(runtime.disconnect("client", "Moonlight cancel"));
  const auto retained = runtime.snapshot("client");
  EXPECT_TRUE(retained.exists);
  EXPECT_TRUE(retained.ready);
  EXPECT_FALSE(retained.connected);
  const auto resumed = runtime.prepare({.operation = terminal_session::operation_e::resume, .launch_session = material("client", 8, 9)});
  EXPECT_TRUE(resumed.ready);
  EXPECT_TRUE(runtime.snapshot("client").connected);
  EXPECT_EQ(provider_raw->allocations, 2);
  EXPECT_EQ(worker_raw->resumes, 1);
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

TEST(TerminalSessionRuntime, FailedTeardownBlocksReplacementAndPreservesOwnership) {
  auto provider = std::make_unique<fake_provider>();
  auto worker = std::make_unique<fake_worker>();
  worker->fail_stop = true;
  terminal_session::runtime_t runtime {std::move(provider), std::move(worker)};
  ASSERT_TRUE(runtime.prepare({.operation = terminal_session::operation_e::launch, .launch_session = material()}).ready);
  const auto replacement = runtime.prepare({.operation = terminal_session::operation_e::launch, .launch_session = material("client", 8, 9)});
  EXPECT_FALSE(replacement.accepted);
  EXPECT_TRUE(runtime.snapshot("client").exists);
}

TEST(TerminalSessionRuntime, WorkerStartCleanupFailureRetainsOwnedSeatForRetry) {
  auto provider = std::make_unique<fake_provider>();
  auto *provider_raw = provider.get();
  auto worker = std::make_unique<fake_worker>();
  auto *worker_raw = worker.get();
  worker->fail_start = true;
  worker->fail_stop = true;
  terminal_session::runtime_t runtime {std::move(provider), std::move(worker)};
  const auto failed = runtime.prepare({.operation = terminal_session::operation_e::launch, .launch_session = material()});
  EXPECT_FALSE(failed.accepted);
  EXPECT_TRUE(runtime.snapshot("client").exists);
  EXPECT_EQ(provider_raw->releases, 0);
  worker_raw->fail_stop = false;
  EXPECT_TRUE(runtime.disconnect("client", "retry cleanup"));
  EXPECT_FALSE(runtime.snapshot("client").exists);
  EXPECT_EQ(provider_raw->releases, 1);
}

TEST(TerminalSessionRuntime, WorkerContractDisablesConsoleWideOwnership) {
  const auto contract = terminal_session::worker::make_contract("seat-client", 58021, 58022, 58023, 58024);
  const auto args = terminal_session::worker::command_line(contract);
  EXPECT_NE(std::find(args.begin(), args.end(), "port=58000"), args.end());
  EXPECT_NE(std::find(args.begin(), args.end(), "system_tray=false"), args.end());
  EXPECT_NE(std::find(args.begin(), args.end(), "upnp=false"), args.end());
  EXPECT_NE(std::find(args.begin(), args.end(), "session_history_enabled=false"), args.end());
  EXPECT_NE(std::find(args.begin(), args.end(), "output_name="), args.end());
  EXPECT_NE(std::find(args.begin(), args.end(), "audio_sink="), args.end());
  EXPECT_NE(std::find(args.begin(), args.end(), "file_state=" + contract.state_root + "\\sunshine_state.json"), args.end());
}

TEST(TerminalSessionService, ControlChallengeIsPeerBoundAndOneUse) {
  terminal_session::service::endpoint_t endpoint {[](const auto &request) {
    return terminal_session::protocol::response_t {.accepted = request.operation == terminal_session::protocol::opcode::control_prepare,
      .client_uuid = request.client_uuid, .generation = request.generation, .launch_id = request.launch_id};
  }};
  const terminal_session::protocol::peer_identity_t peer {.pid = 42, .sid = "S-1-5-21-test", .creation_time = 99, .authenticated = true};
  terminal_session::protocol::request_t challenge {.operation = terminal_session::protocol::opcode::control_challenge, .client_uuid = "client", .generation = 4, .launch_id = 8};
  challenge.ticket.operation = terminal_session::protocol::opcode::control_prepare;
  auto issued = endpoint.handle(terminal_session::protocol::encode(challenge), peer);
  auto challenge_response = terminal_session::protocol::decode_response(issued);
  ASSERT_TRUE(challenge_response.has_value());
  ASSERT_TRUE(challenge_response->ticket.has_value());
  auto prepare = challenge;
  prepare.operation = terminal_session::protocol::opcode::control_prepare;
  prepare.ticket = *challenge_response->ticket;
  auto accepted = terminal_session::protocol::decode_response(endpoint.handle(terminal_session::protocol::encode(prepare), peer));
  ASSERT_TRUE(accepted.has_value());
  EXPECT_TRUE(accepted->accepted);
  auto replay = terminal_session::protocol::decode_response(endpoint.handle(terminal_session::protocol::encode(prepare), peer));
  ASSERT_TRUE(replay.has_value());
  EXPECT_FALSE(replay->accepted);
  EXPECT_EQ(replay->reason, terminal_session::protocol::reject_reason::replayed_ticket);
  auto wrong_opcode = prepare;
  wrong_opcode.operation = terminal_session::protocol::opcode::control_release;
  auto wrong_opcode_response = terminal_session::protocol::decode_response(endpoint.handle(terminal_session::protocol::encode(wrong_opcode), peer));
  ASSERT_TRUE(wrong_opcode_response.has_value());
  EXPECT_EQ(wrong_opcode_response->reason, terminal_session::protocol::reject_reason::invalid_state);
  auto wrong_peer = peer;
  wrong_peer.pid = 43;
  auto challenge2 = terminal_session::protocol::decode_response(endpoint.handle(terminal_session::protocol::encode(challenge), peer));
  ASSERT_TRUE(challenge2.has_value() && challenge2->ticket.has_value());
  auto second = challenge;
  second.operation = terminal_session::protocol::opcode::control_prepare;
  second.ticket = *challenge2->ticket;
  auto rejected_peer = terminal_session::protocol::decode_response(endpoint.handle(terminal_session::protocol::encode(second), wrong_peer));
  ASSERT_TRUE(rejected_peer.has_value());
  EXPECT_EQ(rejected_peer->reason, terminal_session::protocol::reject_reason::unauthenticated_peer);
}

TEST(TerminalSessionService, ReleaseDispositionIsBoundIntoTheOneUseChallenge) {
  terminal_session::service::endpoint_t endpoint {[](const auto &request) {
    return terminal_session::protocol::response_t {
      .accepted = request.operation == terminal_session::protocol::opcode::control_release,
      .client_uuid = request.client_uuid,
      .generation = request.generation,
      .launch_id = request.launch_id,
    };
  }};
  const terminal_session::protocol::peer_identity_t peer {
    .pid = 42, .sid = "S-1-5-21-test", .creation_time = 99, .authenticated = true
  };
  terminal_session::protocol::request_t challenge {
    .operation = terminal_session::protocol::opcode::control_challenge,
    .release = terminal_session::protocol::release_mode::abandon,
    .client_uuid = "client",
    .generation = 4,
    .launch_id = 8,
  };
  challenge.ticket.operation = terminal_session::protocol::opcode::control_release;
  challenge.ticket.release = terminal_session::protocol::release_mode::abandon;
  const auto issued = terminal_session::protocol::decode_response(
    endpoint.handle(terminal_session::protocol::encode(challenge), peer)
  );
  ASSERT_TRUE(issued.has_value() && issued->ticket.has_value());
  terminal_session::protocol::request_t release {
    .operation = terminal_session::protocol::opcode::control_release,
    .release = terminal_session::protocol::release_mode::retain,
    .client_uuid = "client",
    .generation = 4,
    .launch_id = 8,
    .ticket = *issued->ticket,
  };
  const auto rejected = terminal_session::protocol::decode_response(
    endpoint.handle(terminal_session::protocol::encode(release), peer)
  );
  ASSERT_TRUE(rejected.has_value());
  EXPECT_FALSE(rejected->accepted);
  EXPECT_EQ(rejected->reason, terminal_session::protocol::reject_reason::invalid_state);
}

#ifdef _WIN32
TEST(TerminalSessionService, InstalledPeerLayoutUsesRootAndTools) {
  EXPECT_EQ(terminal_session::service::expected_installed_image(L"C:\\Program Files\\Vibeshine\\sunshine.exe", true), L"C:\\Program Files\\Vibeshine\\tools\\sunshinesvc.exe");
  EXPECT_EQ(terminal_session::service::expected_installed_image(L"C:\\Program Files\\Vibeshine\\tools\\sunshinesvc.exe", false), L"C:\\Program Files\\Vibeshine\\sunshine.exe");
}

TEST(TerminalSessionService, ProductionPipeModeRejectsRemoteClients) {
  EXPECT_NE(platf::dxgi::terminal_named_pipe_mode & PIPE_REJECT_REMOTE_CLIENTS, 0u);
  EXPECT_EQ(platf::dxgi::terminal_named_pipe_timeout, 0u);
  EXPECT_NE(platf::dxgi::terminal_named_pipe_access & FILE_FLAG_FIRST_PIPE_INSTANCE, 0u);
}
#endif

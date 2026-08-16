#include <gtest/gtest.h>

#include "src/terminal_session_seat_pool.h"

#include <memory>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace {
  using namespace terminal_session::seat_pool;

  class fake_backend final: public backend_t {
  public:
    std::vector<seat_t> discovered;
    std::unordered_map<std::string, bool> retained_apps;
    int creates {};
    int connects {};
    int disconnects {};
    bool fail_connect {};
    std::uint16_t last_width {};
    std::uint16_t last_height {};

    capability_t preflight() override {
      return {.supported = true, .termwrap_ready = true, .session_controller = true, .remote_display = true, .audio_endpoint = true, .token_launch = true};
    }

    std::vector<seat_t> discover(std::string &) override { return discovered; }

    std::optional<seat_t> create(std::string &) override {
      ++creates;
      const auto suffix = std::to_string(discovered.size() + 1);
      seat_t created {.seat_id = "seat-" + suffix, .account_name = "VibeshineSeat" + suffix,
                      .account_sid = "S-1-5-21-seat-" + suffix, .connection = connection_state_e::disconnected, .managed = true};
      discovered.push_back(created);
      return created;
    }

    bool connect(seat_t &seat, const request_t &request, std::string &error) override {
      ++connects;
      last_width = request.width;
      last_height = request.height;
      if (fail_connect) {
        error = "connect failed";
        return false;
      }
      seat.connection = connection_state_e::connected;
      if (seat.windows_session_id == 0) seat.windows_session_id = static_cast<std::uint32_t>(40 + connects);
      return true;
    }

    bool disconnect(seat_t &seat, std::string &) override {
      ++disconnects;
      seat.connection = connection_state_e::disconnected;
      return true;
    }

    bool has_retained_applications(const seat_t &seat) noexcept override {
      return retained_apps[seat.seat_id];
    }
  };

  request_t request(std::string client, const std::uint64_t generation, const std::uint32_t launch) {
    return {.client_uuid = std::move(client), .generation = generation, .launch_id = launch};
  }
}

TEST(TerminalSessionSeatPool, RoundRobinsOnlyUnownedDisconnectedSeats) {
  auto backend = std::make_unique<fake_backend>();
  backend->discovered = {
    {.seat_id = "seat-1", .account_name = "VibeshineSeat1", .account_sid = "sid-1", .connection = connection_state_e::disconnected, .managed = true},
    {.seat_id = "seat-2", .account_name = "VibeshineSeat2", .account_sid = "sid-2", .connection = connection_state_e::disconnected, .managed = true},
  };
  pool_t pool {std::move(backend)};
  std::string error;
  const auto first = pool.acquire(request("client-a", 1, 10), error);
  const auto second = pool.acquire(request("client-b", 1, 11), error);
  ASSERT_TRUE(first.has_value());
  ASSERT_TRUE(second.has_value());
  EXPECT_EQ(first->seat.seat_id, "seat-1");
  EXPECT_EQ(second->seat.seat_id, "seat-2");
  EXPECT_NE(first->seat.seat_id, second->seat.seat_id);
}

TEST(TerminalSessionSeatPool, DisconnectRetainsSeatAndResumeReusesIt) {
  auto backend = std::make_unique<fake_backend>();
  auto *backend_raw = backend.get();
  backend->discovered = {
    {.seat_id = "seat-1", .account_name = "VibeshineSeat1", .account_sid = "sid-1", .connection = connection_state_e::disconnected, .managed = true},
  };
  pool_t pool {std::move(backend)};
  std::string error;
  ASSERT_TRUE(pool.acquire(request("client-a", 7, 10), error));
  EXPECT_TRUE(pool.release(request("client-a", 7, 10), release_disposition_e::retain, error));
  auto resume_request = request("client-a", 7, 12);
  resume_request.width = 2560;
  resume_request.height = 1440;
  const auto resumed = pool.acquire(resume_request, error);
  ASSERT_TRUE(resumed.has_value());
  EXPECT_TRUE(resumed->resumed);
  EXPECT_EQ(resumed->seat.seat_id, "seat-1");
  EXPECT_EQ(backend_raw->disconnects, 1);
  EXPECT_EQ(backend_raw->connects, 2);
  EXPECT_EQ(backend_raw->last_width, 2560);
  EXPECT_EQ(backend_raw->last_height, 1440);
}

TEST(TerminalSessionSeatPool, RetainedSeatIsNeverAssignedToAnotherClient) {
  auto backend = std::make_unique<fake_backend>();
  auto *backend_raw = backend.get();
  backend->discovered = {
    {.seat_id = "seat-1", .account_name = "VibeshineSeat1", .account_sid = "sid-1", .connection = connection_state_e::disconnected, .managed = true},
  };
  pool_t pool {std::move(backend), 2};
  std::string error;
  ASSERT_TRUE(pool.acquire(request("client-a", 7, 10), error));
  ASSERT_TRUE(pool.release(request("client-a", 7, 10), release_disposition_e::retain, error));
  const auto other = pool.acquire(request("client-b", 8, 20), error);
  ASSERT_TRUE(other.has_value());
  EXPECT_TRUE(other->created);
  EXPECT_EQ(other->seat.seat_id, "seat-2");
  EXPECT_EQ(backend_raw->creates, 1);
}

TEST(TerminalSessionSeatPool, CreatesAccountOnlyWhenNoReusableSeatExists) {
  auto backend = std::make_unique<fake_backend>();
  auto *backend_raw = backend.get();
  backend->discovered = {
    {.seat_id = "seat-1", .account_name = "VibeshineSeat1", .account_sid = "sid-1", .connection = connection_state_e::disconnected, .managed = true},
  };
  pool_t pool {std::move(backend), 3};
  std::string error;
  const auto first = pool.acquire(request("client-a", 1, 10), error);
  ASSERT_TRUE(first.has_value());
  EXPECT_FALSE(first->created);
  EXPECT_EQ(backend_raw->creates, 0);
  const auto second = pool.acquire(request("client-b", 1, 11), error);
  ASSERT_TRUE(second.has_value());
  EXPECT_TRUE(second->created);
  EXPECT_EQ(backend_raw->creates, 1);
}

TEST(TerminalSessionSeatPool, AbandonedSeatWaitsForRetainedApplicationsBeforeReuse) {
  auto backend = std::make_unique<fake_backend>();
  auto *backend_raw = backend.get();
  backend->discovered = {
    {.seat_id = "seat-1", .account_name = "VibeshineSeat1", .account_sid = "sid-1", .connection = connection_state_e::disconnected, .managed = true},
  };
  backend->retained_apps["seat-1"] = true;
  pool_t pool {std::move(backend), 2};
  std::string error;
  ASSERT_TRUE(pool.acquire(request("client-a", 1, 10), error));
  ASSERT_TRUE(pool.release(request("client-a", 1, 10), release_disposition_e::abandon, error));
  const auto while_retained = pool.acquire(request("client-b", 1, 11), error);
  ASSERT_TRUE(while_retained.has_value());
  EXPECT_EQ(while_retained->seat.seat_id, "seat-2");

  backend_raw->retained_apps["seat-1"] = false;
  ASSERT_TRUE(pool.release(request("client-b", 1, 11), release_disposition_e::abandon, error));
  const auto recycled = pool.acquire(request("client-c", 1, 12), error);
  ASSERT_TRUE(recycled.has_value());
  EXPECT_EQ(recycled->seat.seat_id, "seat-1");
}

TEST(TerminalSessionSeatPool, FailedConnectionDoesNotConsumeSeatOrCreateAnotherAccount) {
  auto backend = std::make_unique<fake_backend>();
  auto *backend_raw = backend.get();
  backend->discovered = {
    {.seat_id = "seat-1", .account_name = "VibeshineSeat1", .account_sid = "sid-1", .connection = connection_state_e::disconnected, .managed = true},
  };
  backend->fail_connect = true;
  pool_t pool {std::move(backend), 2};
  std::string error;
  EXPECT_FALSE(pool.acquire(request("client-a", 1, 10), error));
  EXPECT_EQ(backend_raw->creates, 0);
  backend_raw->fail_connect = false;
  const auto retried = pool.acquire(request("client-a", 1, 10), error);
  ASSERT_TRUE(retried.has_value());
  EXPECT_EQ(retried->seat.seat_id, "seat-1");
}

TEST(TerminalSessionSeatPool, OwnerlessRetainedProcessSeatIsQuarantinedAfterRestart) {
  auto backend = std::make_unique<fake_backend>();
  backend->discovered = {
    {.seat_id = "seat-1", .account_name = "VibeshineSeat1", .account_sid = "sid-1",
     .connection = connection_state_e::disconnected, .managed = true, .reusable = false},
  };
  pool_t pool {std::move(backend), 2};
  std::string error;
  const auto lease = pool.acquire(request("client-b", 1, 20), error);
  ASSERT_TRUE(lease.has_value());
  EXPECT_TRUE(lease->created);
  EXPECT_EQ(lease->seat.seat_id, "seat-2");
}

TEST(TerminalSessionSeatPool, ShutdownRecyclesTheDisconnectedAccountAfterAppsExit) {
  auto backend = std::make_unique<fake_backend>();
  backend->discovered = {
    {.seat_id = "seat-1", .account_name = "VibeshineSeat1", .account_sid = "sid-1",
     .connection = connection_state_e::disconnected, .managed = true},
  };
  pool_t pool {std::move(backend), 1};
  std::string error;
  ASSERT_TRUE(pool.acquire(request("client-a", 1, 10), error));
  ASSERT_TRUE(pool.release(request("client-a", 1, 10), release_disposition_e::shutdown, error));
  const auto next = pool.acquire(request("client-b", 1, 20), error);
  ASSERT_TRUE(next.has_value());
  EXPECT_EQ(next->seat.seat_id, "seat-1");
}

TEST(TerminalSessionSeatPool, DisappearedOwnedAccountFailsClosed) {
  auto backend = std::make_unique<fake_backend>();
  auto *backend_raw = backend.get();
  backend->discovered = {
    {.seat_id = "seat-1", .account_name = "VibeshineSeat1", .account_sid = "sid-1",
     .connection = connection_state_e::disconnected, .managed = true},
  };
  pool_t pool {std::move(backend), 2};
  std::string error;
  ASSERT_TRUE(pool.acquire(request("client-a", 1, 10), error));
  ASSERT_TRUE(pool.release(request("client-a", 1, 10), release_disposition_e::retain, error));
  backend_raw->discovered.clear();
  EXPECT_FALSE(pool.acquire(request("client-a", 1, 11), error));
  EXPECT_NE(error.find("no longer discoverable"), std::string::npos);
}

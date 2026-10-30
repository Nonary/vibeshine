// Runs the control stream's graceful hang-up over real ENet sockets on
// loopback, with the client reading only after the host is done, as a busy
// Moonlight receive thread may.
#include <enet/enet.h>
#include <gtest/gtest.h>
#include <chrono>
#include <cstring>
#include <thread>

namespace {
  struct enet_pair_t {
    ENetHost *server = nullptr;
    ENetHost *client = nullptr;
    ENetPeer *server_peer = nullptr;
    ENetPeer *client_peer = nullptr;

    enet_pair_t() {
      EXPECT_EQ(enet_initialize(), 0);
      ENetAddress address;
      std::memset(&address, 0, sizeof address);
      enet_address_set_host(&address, "127.0.0.1");
      enet_address_set_port(&address, 0);
      server = enet_host_create(AF_INET, &address, 4, 2, 0, 0);
      if (!server) return;

      ENetAddress bound;
      std::memset(&bound, 0, sizeof bound);
      bound.addressLength = sizeof bound.address;
      getsockname(server->socket, (struct sockaddr *) &bound.address, &bound.addressLength);
      client = enet_host_create(AF_INET, nullptr, 1, 2, 0, 0);
      if (!client) return;
      client_peer = enet_host_connect(client, &bound, 2, 0);

      ENetEvent event;
      for (int i = 0; i < 400 && (!server_peer || client_peer->state != ENET_PEER_STATE_CONNECTED); ++i) {
        if (enet_host_service(server, &event, 5) > 0 && event.type == ENET_EVENT_TYPE_CONNECT) {
          server_peer = event.peer;
        }
        enet_host_service(client, &event, 5);
      }
    }

    ~enet_pair_t() {
      if (client) enet_host_destroy(client);
      if (server) enet_host_destroy(server);
      enet_deinitialize();
    }

    bool connected() const {
      return server_peer && client_peer && client_peer->state == ENET_PEER_STATE_CONNECTED;
    }

    void send_termination() {
      static constexpr char termination[] = "termination";
      auto packet = enet_packet_create(termination, sizeof termination, ENET_PACKET_FLAG_RELIABLE);
      ASSERT_EQ(enet_peer_send(server_peer, 0, packet), 0);
      enet_host_flush(server);
    }

    // Services both hosts until the client sees the termination or a hang-up.
    ENetEventType first_client_event() {
      // Let everything the host sent reach the client's socket before it reads.
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
      ENetEvent event;
      for (int i = 0; i < 400; ++i) {
        enet_host_service(server, &event, 1);
        while (enet_host_service(client, &event, 1) > 0) {
          if (event.type == ENET_EVENT_TYPE_RECEIVE) {
            enet_packet_destroy(event.packet);
            return ENET_EVENT_TYPE_RECEIVE;
          }
          if (event.type == ENET_EVENT_TYPE_DISCONNECT) return ENET_EVENT_TYPE_DISCONNECT;
        }
      }
      return ENET_EVENT_TYPE_NONE;
    }
  };
}  // namespace

// Why the host must not hang up at once: the client's ENet empties its unread
// queue when the disconnect arrives, taking the termination with it.
TEST(EnetTerminationDelivery, ImmediateHangUpLosesUnreadTermination) {
  enet_pair_t pair;
  ASSERT_TRUE(pair.connected());
  pair.send_termination();
  enet_peer_disconnect_now(pair.server_peer, 0);
  EXPECT_EQ(pair.first_client_event(), ENET_EVENT_TYPE_DISCONNECT);
}

// The host's graceful sequence delivers the termination, and the client's own
// hang-up then reaches the retired peer.
TEST(EnetTerminationDelivery, HangUpAfterDeliveryKeepsTermination) {
  enet_pair_t pair;
  ASSERT_TRUE(pair.connected());
  pair.send_termination();
  enet_peer_timeout(pair.server_peer, 0, 0, 0);
  enet_peer_disconnect_later(pair.server_peer, 0);
  ASSERT_EQ(pair.first_client_event(), ENET_EVENT_TYPE_RECEIVE);

  // Moonlight hangs up as soon as it reads the termination.
  enet_peer_disconnect_now(pair.client_peer, 0);
  ENetEvent event;
  bool server_saw_hang_up = false;
  for (int i = 0; i < 400 && !server_saw_hang_up; ++i) {
    server_saw_hang_up = enet_host_service(pair.server, &event, 5) > 0 &&
                         event.type == ENET_EVENT_TYPE_DISCONNECT && event.peer == pair.server_peer;
  }
  EXPECT_TRUE(server_saw_hang_up);
}

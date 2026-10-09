#!/usr/bin/env python3
"""Execute production RTSP pending cleanup with deterministic runtime fakes.

The C++ methods are extracted unchanged from the working tree. The harness
supplies the scheduler, display backend, and shared-runtime boundary so it can
exercise timeout/cancellation ordering without a host, GPU, or RTSP socket.
"""

import argparse
from pathlib import Path
import re
import subprocess
import tempfile


def extract(source: str, signature: str) -> str:
    """Read one complete C++ function/block, ignoring braces in comments."""
    start = source.index(signature)
    opening = source.index("{", start)
    tokens = re.compile(r'//[^\n]*|/\*.*?\*/|"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'|[{}]', re.S)
    depth = 0
    for token in tokens.finditer(source, opening):
        if token.group() == "{":
            depth += 1
        elif token.group() == "}":
            depth -= 1
            if depth == 0:
                return source[start:token.end()]
    raise ValueError(f"Unterminated production function: {signature}")


parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("root", nargs="?", type=Path, default=Path(__file__).resolve().parents[2])
parser.add_argument("compiler", nargs="?", default="clang++")
args = parser.parse_args()
rtsp = (args.root / "src/rtsp.cpp").read_text()
nvhttp = (args.root / "src/nvhttp.cpp").read_text()
remote = (args.root / "src/remote_session.cpp").read_text()

methods = "\n".join(extract(rtsp, signature) for signature in (
    "    void expire_pending_locked(",
    "    void expire_pending() {",
    "    void notify_expired_pending(",
    "    void finalize_abandoned_pending(",
    "    void cancel_pending_launch(",
    "    void session_clear(",
    "    bool has_pending_launch_or_startup() {",
    "    bool has_pending_launches() {",
    "    bool pending_hdr_active() {",
    "    std::shared_ptr<launch_session_t> take_pending_launch(",
    "    std::vector<std::shared_ptr<launch_session_t>> take_all_pending_launches() {",
))
owner_methods = "\n".join(extract(nvhttp, signature) for signature in (
    "    std::string remote_role_owner_key(",
    "    void remember_remote_owner(",
    "    void forget_remote_owner(",
    "  void notify_remote_input_transport_lost(",
    "  void notify_remote_monitor_released(",
))
monitor_policy = extract(remote, "  bool disconnect_monitor_after_stream(")
startup_publication = extract(rtsp, "        if (!startup_failed) {")

program = r'''
#include <algorithm>
#include <array>
#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdint>
#include <deque>
#include <functional>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>
using namespace std::chrono_literals;
using guid_t = std::array<std::uint8_t, 16>;
struct quiet_log_t {
  template<class T> quiet_log_t &operator<<(const T &) { return *this; }
};
#define BOOST_LOG(level) quiet_log_t{}

namespace remote_session {
  enum class role_e : std::uint8_t { none, input, monitor, game };
}
namespace fake {
  struct monitor_t { std::uint64_t generation; bool retryable = false; bool release_fails = false; };
  struct call_t { std::string uuid; std::uint64_t generation; };
  struct finalize_t { std::string reason; std::optional<guid_t> guid; };
  std::mutex lifecycle_gate;
  std::shared_mutex config_gate;
  std::atomic_int reservations {0};
  std::unordered_map<std::string, monitor_t> monitors;
  std::vector<call_t> release_calls, lost_calls;
  std::vector<finalize_t> finalizers;
  std::function<bool()> pending_activity;
  bool capture_active = false;
  int config_clears = 0, config_applies = 0, idle_finalizations = 0;

  void require_cleanup_context() {
    assert(reservations.load() > 0);
    // Probe from another thread: trying to relock std::mutex from its owner
    // would be undefined. Every ownership/config callback needs this gate.
    bool unlocked = false;
    std::thread probe([&] {
      unlocked = lifecycle_gate.try_lock();
      if (unlocked) lifecycle_gate.unlock();
    });
    probe.join();
    assert(!unlocked);
  }
  bool stream_activity() { return capture_active || (pending_activity && pending_activity()); }
}
namespace config {
  struct video_t {
    bool remote_monitor_disconnect_on_stream_end = false;
    bool remote_monitor_disconnect_on_client_disconnect = false;
  } video;
  void clear_runtime_config_overrides() {
    fake::require_cleanup_context();
    ++fake::config_clears;
  }
  void apply_config_now() {
    fake::require_cleanup_context();
    std::unique_lock lock(fake::config_gate);
    ++fake::config_applies;
  }
}
namespace remote_display_topology {
  struct topology_t {
    std::size_t managed_client_identity_count() const { return fake::monitors.size(); }
  } topology;
  topology_t &instance() { return topology; }
}
namespace nvhttp {
  struct remote_role_owner_t {
    remote_session::role_e role;
    std::uint64_t generation;
    std::string client_uuid;
  };
  std::mutex remote_role_owners_mutex;
  std::unordered_map<std::string, remote_role_owner_t> remote_role_owners;
  std::mutex &stream_lifecycle_mutex() { return fake::lifecycle_gate; }
  bool has_stream_session_activity() { return fake::stream_activity(); }
''' + owner_methods + r'''
}
namespace remote_session {
''' + monitor_policy + r'''
  bool release_monitor(std::string_view uuid, std::uint64_t generation, std::string_view) {
    fake::require_cleanup_context();
    fake::release_calls.push_back({std::string(uuid), generation});
    auto it = fake::monitors.find(std::string(uuid));
    // Model the coordinator's generation-fenced backend response. The tests
    // also compile NVHTTP's actual forget method, independently of this fake.
    if (it == fake::monitors.end() || it->second.generation != generation) return true;
    if (it->second.release_fails) return false;
    fake::monitors.erase(it);
    return true;
  }
  void notify_monitor_transport_lost(std::string_view uuid, std::uint64_t generation) {
    fake::require_cleanup_context();
    fake::lost_calls.push_back({std::string(uuid), generation});
    auto it = fake::monitors.find(std::string(uuid));
    if (it != fake::monitors.end() && it->second.generation == generation) it->second.retryable = true;
  }
}
namespace stream {
  struct session_t {};
  namespace session {
    struct cleanup_reservation_t {
      cleanup_reservation_t() { ++fake::reservations; }
      ~cleanup_reservation_t() { --fake::reservations; }
      cleanup_reservation_t(const cleanup_reservation_t &) = delete;
      cleanup_reservation_t &operator=(const cleanup_reservation_t &) = delete;
    };
    struct shared_runtime_finalize_context_t { std::optional<guid_t> virtual_display_guid_bytes; };
    bool finalize_shared_runtime_if_idle(std::string_view reason, const shared_runtime_finalize_context_t &context) {
      fake::require_cleanup_context();
      fake::finalizers.push_back({std::string(reason), context.virtual_display_guid_bytes});
      if (fake::stream_activity() || !fake::monitors.empty()) return false;
      ++fake::idle_finalizations;
      config::clear_runtime_config_overrides();
      config::apply_config_now();
      return true;
    }
  }
}
namespace rtsp_stream {
  struct launch_session_t {
    std::uint32_t id;
    remote_session::role_e role;
    std::string client_uuid;
    std::uint64_t role_generation;
    guid_t virtual_display_guid_bytes {};
    bool ownership_transferred_to_stream = false;
    bool hdr = false;
  };
  bool effective_hdr_requested(const launch_session_t &launch) { return launch.hdr; }
  struct server_t {
    struct pending_launch_t {
      std::shared_ptr<launch_session_t> session;
      std::chrono::steady_clock::time_point expires_at;
    };
    std::mutex pending_launches_mutex;
    std::unordered_map<std::uint32_t, pending_launch_t> pending_launches;
    std::deque<std::function<void()>> tasks;
    std::optional<guid_t> abandoned_startup_virtual_display_guid_bytes;
    int startups = 0, publications = 0;
    bool pending_hdr = false;
    server_t() { fake::pending_activity = [this] { return has_pending_launch_or_startup(); }; }
    ~server_t() { fake::pending_activity = {}; }
    int startup_count() const { return startups; }
    void set_pending_vulkan_hdr_layer_stream(bool value) { pending_hdr = value; }
    template<class F> void post(F &&fn) { tasks.emplace_back(std::forward<F>(fn)); }
    void run_one() {
      assert(!tasks.empty());
      auto task = std::move(tasks.front());
      tasks.pop_front();
      task();
    }
    void drain() { while (!tasks.empty()) run_one(); }
    void insert(const std::shared_ptr<stream::session_t> &, const std::string &, bool) {
      ++publications;
      fake::capture_active = true;
    }
    void publish_startup(const std::shared_ptr<launch_session_t> &pending_launch, bool startup_failed) {
      auto server = this;
      auto stream_session = std::make_shared<stream::session_t>();
      const auto client_uuid = pending_launch->client_uuid;
      const bool stream_hdr_enabled = false;
''' + startup_publication + r'''
    }
''' + methods + r'''
  };
}

using remote_session::role_e;
using rtsp_stream::launch_session_t;
using rtsp_stream::server_t;

void reset() {
  assert(fake::reservations == 0);
  assert(!fake::pending_activity);
  fake::monitors.clear();
  fake::release_calls.clear();
  fake::lost_calls.clear();
  fake::finalizers.clear();
  nvhttp::remote_role_owners.clear();
  fake::capture_active = false;
  fake::config_clears = fake::config_applies = fake::idle_finalizations = 0;
  config::video = {};
}
bool owns(std::string_view uuid, role_e role, std::uint64_t generation) {
  const auto it = nvhttp::remote_role_owners.find(nvhttp::remote_role_owner_key(uuid, role));
  return it != nvhttp::remote_role_owners.end() && it->second.generation == generation;
}
void remember(std::string_view uuid, role_e role, std::uint64_t generation) {
  nvhttp::remember_remote_owner(uuid, role, generation);
  if (role == role_e::monitor) fake::monitors.insert_or_assign(std::string(uuid), fake::monitor_t {generation});
}
std::shared_ptr<launch_session_t> add(server_t &server, std::uint32_t id, role_e role,
    std::string uuid, std::uint64_t generation, bool expired = true, bool hdr = false) {
  auto launch = std::make_shared<launch_session_t>();
  launch->id = id;
  launch->role = role;
  launch->client_uuid = std::move(uuid);
  launch->role_generation = generation;
  launch->virtual_display_guid_bytes[0] = static_cast<std::uint8_t>(id);
  launch->hdr = hdr;
  server.pending_launches.emplace(id, server_t::pending_launch_t {
    launch, std::chrono::steady_clock::now() + (expired ? -1h : 1h)
  });
  return launch;
}

void expiry_is_deferred_across_both_gates() {
  reset();
  server_t server;
  remember("input", role_e::input, 7);
  std::weak_ptr<launch_session_t> lifetime = add(server, 1, role_e::input, "input", 7);
  {
    std::unique_lock lifecycle(fake::lifecycle_gate);
    std::unique_lock config_write(fake::config_gate);
    server.expire_pending();
    assert(server.pending_launches.empty());
    assert(server.tasks.size() == 1);
    assert(fake::reservations == 1);
    assert(!lifetime.expired());
    assert(owns("input", role_e::input, 7));
    assert(fake::finalizers.empty());
    assert(fake::config_applies == 0);
  }
  server.drain();
  assert(lifetime.expired());
  assert(fake::reservations == 0);
  assert(!owns("input", role_e::input, 7));
  assert(fake::finalizers.size() == 1);
  assert(fake::finalizers[0].reason == "rtsp_pending_launch_expired");
  assert(fake::finalizers[0].guid->at(0) == 1);
  assert(fake::idle_finalizations == 1);
  assert(fake::config_applies > 0);
  server.expire_pending();
  assert(server.tasks.empty());
  assert(fake::reservations == 0);
}

void expiry_boundary_and_roleless_finalization() {
  reset();
  server_t server;
  const auto deadline = std::chrono::steady_clock::now();
  add(server, 1, role_e::game, "game", 0);
  add(server, 2, role_e::none, "anonymous", 0);
  add(server, 3, role_e::game, "future", 0, false, true);
  server.pending_launches.at(1).expires_at = deadline;
  server.pending_launches.at(2).expires_at = deadline - 1s;
  server.pending_launches.at(3).expires_at = deadline + 1h;
  std::vector<std::shared_ptr<launch_session_t>> expired;
  {
    std::lock_guard lock(server.pending_launches_mutex);
    server.expire_pending_locked(deadline, &expired);
  }
  assert(expired.size() == 2);
  assert(server.pending_launches.size() == 1);
  assert(server.pending_launches.contains(3));
  server.notify_expired_pending(expired);
  server.drain();
  assert(fake::finalizers.size() == 2);
  assert(fake::release_calls.empty() && fake::lost_calls.empty());
  assert(server.pending_hdr);
  assert(fake::idle_finalizations == 0); // The unexpired launch is still an owner.
  server.cancel_pending_launch("test_cancel_remaining");
  assert(fake::finalizers.size() == 3);
  assert(fake::idle_finalizations == 1);
  assert(!server.pending_hdr);
  assert(fake::reservations == 0);
}

void monitor_retention_policy_matrix() {
  for (bool stream_end : {false, true}) for (bool client_disconnect : {false, true}) {
    reset();
    server_t server;
    config::video.remote_monitor_disconnect_on_stream_end = stream_end;
    config::video.remote_monitor_disconnect_on_client_disconnect = client_disconnect;
    remember("monitor", role_e::monitor, 9);
    remember("peer", role_e::monitor, 11);
    add(server, 1, role_e::monitor, "monitor", 9);
    server.expire_pending();
    server.drain();
    assert(fake::finalizers.size() == 1);
    assert(owns("peer", role_e::monitor, 11));
    assert(!fake::monitors.at("peer").retryable);
    if (stream_end) {
      assert(fake::release_calls.size() == 1);
      assert(fake::release_calls[0].generation == 9);
      assert(fake::lost_calls.empty());
      assert(!owns("monitor", role_e::monitor, 9));
      assert(!fake::monitors.contains("monitor"));
    } else {
      assert(fake::release_calls.empty());
      assert(fake::lost_calls.size() == 1);
      assert(fake::lost_calls[0].generation == 9);
      assert(owns("monitor", role_e::monitor, 9));
      assert(fake::monitors.at("monitor").retryable);
    }
    assert(fake::reservations == 0);
  }
}

void newer_generations_survive_queued_expiry() {
  for (const auto role : {role_e::input, role_e::monitor}) for (bool release : {false, true}) {
    reset();
    server_t server;
    config::video.remote_monitor_disconnect_on_stream_end = release;
    remember("client", role, 7);
    add(server, 1, role, "client", 7);
    server.expire_pending();
    {
      std::unique_lock lifecycle(fake::lifecycle_gate);
      remember("client", role, 8);
      add(server, 2, role, "client", 8, false);
    }
    server.drain();
    assert(owns("client", role, 8));
    assert(server.pending_launches.contains(2));
    if (role == role_e::monitor) {
      assert(fake::monitors.at("client").generation == 8);
      assert(!fake::monitors.at("client").retryable);
    }
    assert(fake::finalizers.size() == 1);
    assert(fake::config_applies == 0);
    assert(fake::reservations == 0);
  }
}

void failed_monitor_release_retains_owner() {
  reset();
  server_t server;
  config::video.remote_monitor_disconnect_on_stream_end = true;
  remember("monitor", role_e::monitor, 4);
  fake::monitors.at("monitor").release_fails = true;
  add(server, 1, role_e::monitor, "monitor", 4);
  server.expire_pending();
  server.drain();
  assert(fake::release_calls.size() == 1);
  assert(owns("monitor", role_e::monitor, 4));
  assert(fake::monitors.contains("monitor"));
  assert(fake::finalizers.size() == 1);
  assert(fake::idle_finalizations == 0);
  assert(fake::config_applies == 0);
}

void startup_transfers_cleanup_even_after_registry_expiry() {
  for (const auto role : {role_e::input, role_e::monitor}) for (bool draining : {false, true}) {
    reset();
    server_t server;
    config::video.remote_monitor_disconnect_on_stream_end = true;
    remember("client", role, 7);
    auto canonical_launch = add(server, 1, role, "client", 7);
    {
      std::unique_lock lifecycle(fake::lifecycle_gate);
      std::shared_lock config_read(fake::config_gate);
      // The timer can remove the pending row after startup validation but
      // before this worker publishes the stream. Its callback must observe
      // the handoff on this same canonical object after taking the gate.
      server.expire_pending();
      server.publish_startup(canonical_launch, false);
      assert(canonical_launch->ownership_transferred_to_stream);
      assert(server.publications == 1);
      if (draining) server.publications = 0;
    }
    server.drain();
    assert(owns("client", role, 7));
    assert(fake::release_calls.empty() && fake::lost_calls.empty());
    assert(fake::finalizers.size() == 1);
    assert(fake::idle_finalizations == 0);
    assert(fake::reservations == 0);
  }
  reset();
  server_t server;
  config::video.remote_monitor_disconnect_on_stream_end = true;
  remember("failed", role_e::monitor, 7);
  auto canonical_launch = add(server, 1, role_e::monitor, "failed", 7);
  {
    std::unique_lock lifecycle(fake::lifecycle_gate);
    server.expire_pending();
    server.publish_startup(canonical_launch, true);
  }
  assert(!canonical_launch->ownership_transferred_to_stream);
  server.drain();
  assert(!owns("failed", role_e::monitor, 7));
  assert(fake::release_calls.size() == 1);
  assert(fake::idle_finalizations == 1);
}

void cancellation_uses_the_same_ownership_transition() {
  for (bool release : {false, true}) {
    reset();
    server_t server;
    config::video.remote_monitor_disconnect_on_stream_end = release;
    config::video.remote_monitor_disconnect_on_client_disconnect = true;
    remember("monitor", role_e::monitor, 4);
    remember("input", role_e::input, 5);
    remember("transferred", role_e::input, 6);
    add(server, 1, role_e::monitor, "monitor", 4, false);
    add(server, 2, role_e::input, "input", 5, false);
    add(server, 3, role_e::game, "game", 0, false);
    auto transferred = add(server, 4, role_e::input, "transferred", 6, false);
    {
      std::unique_lock lifecycle(fake::lifecycle_gate);
      server.publish_startup(transferred, false);
    }
    server.cancel_pending_launch("test_cancel");
    assert(server.pending_launches.empty());
    assert(server.tasks.empty());
    assert(!owns("input", role_e::input, 5));
    assert(owns("transferred", role_e::input, 6));
    assert(owns("monitor", role_e::monitor, 4) == !release);
    assert(fake::finalizers.size() == 4);
    for (const auto &entry : fake::finalizers) assert(entry.reason == "test_cancel");
    assert(!server.abandoned_startup_virtual_display_guid_bytes);
    assert(fake::reservations == 0);
  }
}

void attachment_does_not_retire_remote_ownership() {
  reset();
  server_t server;
  config::video.remote_monitor_disconnect_on_stream_end = true;
  remember("monitor", role_e::monitor, 9);
  add(server, 1, role_e::monitor, "monitor", 9, false);
  server.session_clear(1);
  assert(server.pending_launches.empty());
  assert(owns("monitor", role_e::monitor, 9));
  assert(fake::release_calls.empty() && fake::lost_calls.empty());
  assert(fake::finalizers.size() == 1);
  assert(fake::finalizers[0].reason == "rtsp_launch_attached");
  assert(fake::reservations == 0);
}

void cleanup_reservations_and_startup_context_survive_queueing() {
  reset();
  {
    server_t server;
    server.startups = 1;
    add(server, 1, role_e::game, "first", 0);
    server.expire_pending();
    add(server, 2, role_e::game, "second", 0);
    server.expire_pending();
    assert(fake::reservations == 2);
    server.run_one();
    assert(fake::reservations == 1);
    assert(server.abandoned_startup_virtual_display_guid_bytes->at(0) == 1);
    assert(fake::idle_finalizations == 0);
    server.startups = 0;
    server.run_one();
    assert(fake::reservations == 0);
    assert(!server.abandoned_startup_virtual_display_guid_bytes);
    assert(fake::idle_finalizations == 1);
    server.notify_expired_pending({nullptr});
    server.drain();
    assert(fake::finalizers.size() == 2);
  }
  reset();
  {
    server_t server;
    add(server, 1, role_e::game, "shutdown", 0);
    server.expire_pending();
    assert(fake::reservations == 1);
    // Discarding a queued handler must release its reservation too.
  }
  assert(fake::reservations == 0);
}

int main() {
  expiry_is_deferred_across_both_gates();
  expiry_boundary_and_roleless_finalization();
  monitor_retention_policy_matrix();
  newer_generations_survive_queued_expiry();
  failed_monitor_release_retains_owner();
  startup_transfers_cleanup_even_after_registry_expiry();
  cancellation_uses_the_same_ownership_transition();
  attachment_does_not_retire_remote_ownership();
  cleanup_reservations_and_startup_context_survive_queueing();
  std::cout << "Production RTSP pending lifecycle: deferred cleanup, reservations, retention, "
               "generation fencing, startup handoff, cancellation and finalization passed\n";
}
'''

with tempfile.TemporaryDirectory(prefix="rtsp-pending-lifecycle-") as directory:
    source = Path(directory) / "test.cpp"
    binary = Path(directory) / "test"
    source.write_text(program)
    subprocess.run(
        [args.compiler, "-std=c++20", "-pthread", "-Wall", "-Wextra", "-Werror", str(source), "-o", str(binary)],
        check=True,
        timeout=60,
    )
    # A regression that invokes cleanup synchronously under either gate must
    # fail the test as a timeout instead of hanging the complete test runner.
    subprocess.run([str(binary)], check=True, timeout=15)

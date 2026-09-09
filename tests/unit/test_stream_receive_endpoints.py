#!/usr/bin/env python3
"""Run the production UDP receive loop with concurrent staged completions.

The socket boundary completes both datagrams before invoking either handler,
as asynchronous IO permits. --source accepts an older stream.cpp for regression
verification. No network access or Boost installation is required.
"""
import argparse
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--source", type=Path, default=ROOT / "src/stream.cpp")
parser.add_argument("--compiler", default="c++")
args = parser.parse_args()
source = args.source.read_text()
start = source.index("  void recvThread(broadcast_ctx_t &ctx) {")
end = source.index("\n  }", start) + len("\n  }")

program = r'''
#include <algorithm>
#include <array>
#include <atomic>
#include <cassert>
#include <cstdint>
#include <deque>
#include <functional>
#include <iostream>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <sstream>
#include <string>
#include <tuple>
#include <variant>
#include <vector>
using namespace std::string_view_literals;
namespace boost::system {
  struct error_code {
    int value = 0;
    explicit operator bool() const { return value != 0; }
    bool operator==(int other) const { return value == other; }
    std::string message() const { return {}; }
  };
  namespace errc { constexpr int connection_refused = 1, connection_reset = 2; }
}
namespace udp {
  struct address_t {
    int value = 0;
    auto operator<=>(const address_t &) const = default;
    std::string to_string() const { return std::to_string(value); }
  };
  struct endpoint {
    address_t ip;
    unsigned short source_port = 0;
    address_t address() const { return ip; }
    unsigned short port() const { return source_port; }
    bool operator==(const endpoint &) const = default;
  };
  struct operation_t {
    std::span<char> buffer;
    endpoint *peer;
    std::function<void(const boost::system::error_code, size_t)> handler;
  };
  struct socket {
    operation_t pending;
    void async_receive_from(std::span<char> buffer, endpoint &peer, int,
                            std::function<void(const boost::system::error_code, size_t)> handler) {
      pending = {buffer, &peer, std::move(handler)};
    }
  };
}
namespace asio {
  template<size_t N> std::span<char> buffer(std::array<char, N> &value) { return value; }
}
namespace util {
  template<class F> struct fail_guard { F f; ~fail_guard() { f(); } };
  std::string hex_vec(const std::string &) { return {}; }
}
namespace platf { void set_thread_name(const char *) {} }
std::ostringstream logs;
#define BOOST_LOG(level) logs
#define TUPLE_3D_REF(a, b, c, value) auto &[a, b, c] = value
struct SS_PING { char payload[16]; };
using PSS_PING = SS_PING *;
enum class socket_e { video, audio };
using av_session_id_t = std::variant<udp::address_t, std::string>;
template<class T> struct queue_t {
  std::deque<T> entries;
  bool peek() const { return !entries.empty(); }
  std::optional<T> pop() {
    if (entries.empty()) return std::nullopt;
    auto entry = std::move(entries.front()); entries.pop_front(); return entry;
  }
  template<class... Args> void raise(Args &&...args) { entries.emplace_back(std::forward<Args>(args)...); }
};
using message_queue_t = std::shared_ptr<queue_t<std::pair<udp::endpoint, std::string>>>;
using message_queue_queue_t = std::shared_ptr<queue_t<std::tuple<socket_e, av_session_id_t, message_queue_t>>>;
struct event_t { bool stopped = false; bool peek() const { return stopped; } };
auto shutdown_event = std::make_shared<event_t>();
namespace mail {
  constexpr int broadcast_shutdown = 0;
  struct manager_t { template<class T> auto event(int) { return shutdown_event; } };
  auto man = std::make_shared<manager_t>();
}
struct io_t {
  udp::socket *video, *audio;
  std::array<udp::endpoint, 2> senders;
  std::array<std::string, 2> payloads;
  void run() {
    const std::array operations {video->pending, audio->pending};
    // Both receive operations finish before queued completion callbacks run.
    for (size_t i = 0; i < operations.size(); ++i) {
      *operations[i].peer = senders[i];
      std::copy(payloads[i].begin(), payloads[i].end(), operations[i].buffer.begin());
    }
    for (size_t i = 0; i < operations.size(); ++i) operations[i].handler({}, payloads[i].size());
    shutdown_event->stopped = true;
  }
};
struct broadcast_ctx_t {
  message_queue_queue_t message_queue_queue = std::make_shared<message_queue_queue_t::element_type>();
  std::atomic<std::uint64_t> video_recv_count {0}, audio_recv_count {0};
  udp::socket video_sock, audio_sock;
  io_t io_context {&video_sock, &audio_sock, {}, {}};
};
''' + source[start:end] + r'''
int main() {
  for (const bool modern : {false, true}) {
    for (const bool same_address : {false, true}) {
      shutdown_event->stopped = false;
      broadcast_ctx_t ctx;
      const udp::endpoint video {{1}, 51000};
      const udp::endpoint audio {{same_address ? 1 : 2}, 52000};
      const auto video_payload = modern ? std::string(16, 'v') : std::string("PING");
      const auto audio_payload = modern ? std::string(16, 'a') : std::string("PING");
      const av_session_id_t video_id = modern ? av_session_id_t(video_payload) : av_session_id_t(video.address());
      const av_session_id_t audio_id = modern ? av_session_id_t(audio_payload) : av_session_id_t(audio.address());
      auto video_messages = std::make_shared<message_queue_t::element_type>();
      auto audio_messages = std::make_shared<message_queue_t::element_type>();
      ctx.message_queue_queue->raise(socket_e::video, video_id, video_messages);
      ctx.message_queue_queue->raise(socket_e::audio, audio_id, audio_messages);
      ctx.io_context.senders = {video, audio};
      ctx.io_context.payloads = {video_payload, audio_payload};
      recvThread(ctx);
      assert(video_messages->entries.size() == 1 && audio_messages->entries.size() == 1);
      assert(video_messages->entries.front().first == video);
      assert(audio_messages->entries.front().first == audio);
      assert(video_messages->entries.front().second == video_payload);
      assert(audio_messages->entries.front().second == audio_payload);
      assert(ctx.video_recv_count == 1 && ctx.audio_recv_count == 1);
      // Rearmed receives must retain their own endpoint slots as well.
      assert(ctx.video_sock.pending.peer != ctx.audio_sock.pending.peer);
    }
  }
  std::cout << "4 concurrent UDP endpoint regressions passed.\n";
}
'''

with tempfile.TemporaryDirectory(prefix="stream-receive-") as directory:
    directory = Path(directory)
    harness = directory / "test.cpp"
    binary = directory / "test"
    harness.write_text(program)
    subprocess.run([
        args.compiler, "-std=c++20", "-Wall", "-Wextra", "-Werror",
        "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
        str(harness), "-o", str(binary)
    ], check=True)
    subprocess.run([str(binary)], check=True)

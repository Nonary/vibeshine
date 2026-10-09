"""Compile the production snapshot senders, response reader, and reader lock.

Only the byte pipe and connection/write collaborators are replaced. JSON is
parsed by the real nlohmann header; pass its include directories after the
repository and compiler arguments when it is not installed in a system path.
"""
import pathlib
import re
import subprocess
import sys
import tempfile


root = pathlib.Path(sys.argv[1]).resolve() if len(sys.argv) > 1 else pathlib.Path(__file__).resolve().parents[2]
compiler = sys.argv[2] if len(sys.argv) > 2 else "c++"
json_include_directories = sys.argv[3:]
source = (root / "src/platform/windows/ipc/display_settings_client.cpp").read_text()


def extract(signature, is_function=True):
    tokens = re.findall(r"[A-Za-z_]\w*|::|[^\w\s]", signature)
    match = re.search(r"\s*".join(re.escape(token) for token in tokens), source)
    if match is None:
        raise ValueError(f"Production declaration not found: {signature}")
    end = match.end()
    if is_function:
        # Skip parameter defaults such as cancellation_predicate = {} before
        # looking for the body. Do not rewrite any production function text.
        end = source.index("(", match.start()) + 1
        depth = 1
        while depth:
            depth += (source[end] == "(") - (source[end] == ")")
            end += 1
    brace = source.index("{", end)
    depth, end = 1, brace + 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[match.start():end] + (";" if not is_function else "")


def functions(*signatures):
    return "\n\n".join(extract(signature) for signature in signatures)


constants = []
for name in (
    "kSnapshotResultTimeoutMs", "kConnectTimeoutMs", "kSendTimeoutMs",
    "kStaleConnectionRetireTimeoutMs", "kMaxBufferedResponses",
):
    match = re.search(rf"constexpr\s+[\w:]+\s+{name}\s*=\s*[^;]+;", source)
    if match is None:
        raise ValueError(f"Production constant not found: {name}")
    constants.append(match.group())

program = r'''
#include <algorithm>
#include <array>
#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdint>
#include <deque>
#include <functional>
#include <future>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <thread>
#include <utility>
#include <vector>
#include <nlohmann/json.hpp>
#include "src/platform/windows/ipc/display_settings_protocol.h"

struct NullLog {
  template<class T> NullLog &operator<<(const T &) { return *this; }
};
#define BOOST_LOG(level) NullLog{}
namespace platf::dxgi {
  enum class PipeResult { Success, Timeout, Disconnected, Error };
}
namespace platf::display_helper_client {
  using namespace std::chrono_literals;
  using Clock = std::chrono::steady_clock;
''' + "\n".join(constants) + extract("enum class MsgType", False) + extract("enum class ApplyResponseProtocol", False) + r'''
  struct FakePipe;
  struct ConnectionSession {
    std::shared_ptr<FakePipe> pipe;
    std::uint64_t generation = 7;
    std::atomic<ApplyResponseProtocol> protocol {ApplyResponseProtocol::Unknown};
    std::timed_mutex response_mutex;
    std::mutex response_inbox_mutex;
    std::deque<std::vector<uint8_t>> response_inbox;
  };
  using SessionPtr = std::shared_ptr<ConnectionSession>;
''' + functions(
    "static std::atomic<std::uint64_t> &connection_generation(",
    "static bool session_is_current(",
    "static std::atomic<std::uint64_t> &apply_wait_generation(",
    "static std::atomic<std::uint64_t> &next_auxiliary_request_id(",
) + r'''
  namespace fake {
    SessionPtr session;
    bool cancelled = false;
    bool send_succeeds = true;
    int connects = 0;
    int sends = 0;
    int reads = 0;
    int drops = 0;
    std::uint64_t sent_id = 0;
    nlohmann::json sent_json;
    Clock::time_point connect_deadline;
    Clock::time_point send_deadline;
    std::function<void()> on_connect;
    std::function<void()> on_send;
    std::function<void()> on_receive;
  }

  // Probe on another thread: trying a mutex already held by this thread would
  // be undefined behavior, and could hide a missing production reader lease.
  void assert_reader_locked(const SessionPtr &session) {
    std::thread probe([&] {
      const bool acquired = session->response_mutex.try_lock();
      if (acquired) session->response_mutex.unlock();
      assert(!acquired);
    });
    probe.join();
  }
  void assert_reader_released() {
    assert(fake::session->response_mutex.try_lock());
    fake::session->response_mutex.unlock();
  }
  struct FakePipe {
    std::deque<std::vector<uint8_t>> frames;
    platf::dxgi::PipeResult empty_result = platf::dxgi::PipeResult::Timeout;
    platf::dxgi::PipeResult receive(std::array<uint8_t, 65536> &buffer, std::size_t &bytes_read, int timeout_ms) {
      assert_reader_locked(fake::session);
      assert(timeout_ms > 0 && timeout_ms <= 100);
      ++fake::reads;
      if (fake::on_receive) fake::on_receive();
      if (frames.empty()) {
        bytes_read = 0;
        if (empty_result == platf::dxgi::PipeResult::Timeout) std::this_thread::sleep_for(1ms);
        return empty_result;
      }
      auto frame = std::move(frames.front());
      frames.pop_front();
      bytes_read = frame.size();
      assert(bytes_read <= buffer.size());
      std::copy(frame.begin(), frame.end(), buffer.begin());
      return platf::dxgi::PipeResult::Success;
    }
  };
  namespace fake {
    void reset(ApplyResponseProtocol protocol = ApplyResponseProtocol::Unknown) {
      session = std::make_shared<ConnectionSession>();
      session->pipe = std::make_shared<FakePipe>();
      session->protocol = protocol;
      connection_generation() = session->generation;
      apply_wait_generation() = 11;
      cancelled = false;
      send_succeeds = true;
      connects = sends = reads = drops = 0;
      sent_id = 0;
      sent_json = nullptr;
      connect_deadline = send_deadline = {};
      on_connect = on_send = on_receive = {};
    }
  }

  SessionPtr connected_session() {
    ++fake::connects;
    if (fake::on_connect) fake::on_connect();
    return fake::session;
  }
  SessionPtr connected_session_within(Clock::time_point deadline, int, const std::function<bool()> &) {
    fake::connect_deadline = deadline;
    return connected_session();
  }
  bool send_serialized(const SessionPtr &session, MsgType type, const std::vector<uint8_t> &payload,
                       std::optional<int>, std::optional<std::uint64_t>) {
    assert(type == MsgType::SnapshotCurrent);
    assert_reader_locked(session);
    ++fake::sends;
    fake::sent_json = nlohmann::json::parse(payload.begin(), payload.end());
    assert(fake::sent_json.is_object());
    assert(fake::sent_json.at("sunshine_snapshot_id").is_number_unsigned());
    fake::sent_id = fake::sent_json.at("sunshine_snapshot_id").get<std::uint64_t>();
    assert(fake::sent_id != 0);
    if (fake::on_send) fake::on_send();
    return fake::send_succeeds;
  }
  bool send_serialized_within(const SessionPtr &session, MsgType type, const std::vector<uint8_t> &payload,
                              Clock::time_point deadline, std::optional<std::uint64_t> generation,
                              int, void *, bool, const std::function<bool()> &) {
    fake::send_deadline = deadline;
    assert(deadline == fake::connect_deadline);
    return send_serialized(session, type, payload, std::nullopt, generation);
  }
  void drop_connection_if_current(const SessionPtr &session, const char *) {
    assert(session_is_current(session));
    ++fake::drops;
  }
  void drop_connection_if_current_within(const SessionPtr &session, const char *reason, Clock::time_point) {
    drop_connection_if_current(session, reason);
  }
''' + functions(
    "bool is_bufferable_response(",
    "void buffer_response(",
    "std::optional<std::vector<uint8_t>> take_buffered_response(",
    "std::optional<std::uint64_t> response_request_id(",
    "std::optional<bool> wait_for_snapshot_result_locked(",
    "static int remaining_timeout_ms(const std::chrono::steady_clock::time_point &deadline) {",
    "static bool lock_response_reader_until(",
    "bool send_snapshot_current_and_wait(",
    "bool send_snapshot_current_within(",
) + r'''
  std::vector<uint8_t> frame(std::uint64_t request_id, uint8_t success = 1) {
    std::vector<uint8_t> result {static_cast<uint8_t>(MsgType::SnapshotResult), success};
    for (unsigned shift = 0; shift < 64; shift += 8) result.push_back(static_cast<uint8_t>(request_id >> shift));
    result.push_back(display_helper_protocol::kSnapshotRecoveryVersion);
    return result;
  }
  bool snapshot(bool bounded, const std::string &payload = "{}", int timeout_ms = 25) {
    if (bounded) {
      return send_snapshot_current_within(payload, Clock::now() + std::chrono::milliseconds(timeout_ms), [] { return fake::cancelled; });
    }
    return send_snapshot_current_and_wait(payload, timeout_ms);
  }
  void reply(uint8_t success = 1) {
    fake::on_send = [success] { fake::session->pipe->frames.push_back(frame(fake::sent_id, success)); };
  }

  void test_every_protocol_requires_matching_completion() {
    for (const auto protocol : {ApplyResponseProtocol::Unknown, ApplyResponseProtocol::Legacy, ApplyResponseProtocol::V2}) {
      for (const bool bounded : {false, true}) {
        fake::reset(protocol);
        assert(!snapshot(bounded));
        assert(fake::sends == 1 && fake::reads > 0 && fake::drops == 1);
        assert_reader_released();

        fake::reset(protocol);
        reply();
        assert(snapshot(bounded));
        assert(fake::sends == 1 && fake::reads == 1 && fake::drops == 0);
        const auto first_id = fake::sent_id;
        assert(snapshot(bounded));
        assert(fake::sent_id != first_id);
        assert_reader_released();

        fake::reset(protocol);
        reply(0);
        assert(!snapshot(bounded));
        assert(fake::sends == 1 && fake::reads == 1 && fake::drops == 0);
        assert_reader_released();
      }
    }
  }

  void test_stale_untagged_and_unrelated_responses() {
    for (const bool bounded : {false, true}) {
      fake::reset();
      fake::on_send = [] {
        fake::session->pipe->frames.push_back(frame(fake::sent_id - 1));
        fake::session->pipe->frames.push_back({static_cast<uint8_t>(MsgType::SnapshotResult), 1});
      };
      assert(!snapshot(bounded));
      assert(fake::session->response_inbox.size() == 1 && fake::drops == 1);

      fake::reset();
      fake::on_send = [] {
        fake::session->pipe->frames.push_back(frame(fake::sent_id - 1));
        fake::session->pipe->frames.push_back({static_cast<uint8_t>(MsgType::Ping)});
        fake::session->pipe->frames.push_back({static_cast<uint8_t>(MsgType::ApplyResult), 1});
        fake::session->pipe->frames.push_back(frame(fake::sent_id));
      };
      assert(snapshot(bounded));
      assert(fake::reads == 4 && fake::session->response_inbox.size() == 2);
      assert(fake::drops == 0);
    }
  }

  void test_malformed_frames_and_buffered_replies() {
    for (const bool bounded : {false, true}) {
      for (const bool buffered : {false, true}) {
        for (const int malformed : {0, 1, 2, 3}) {
          fake::reset();
          fake::on_send = [=] {
            auto result = frame(fake::sent_id);
            if (malformed == 0) result.resize(1);
            if (malformed == 1) result.resize(9);
            if (malformed == 2) result[1] = 2;
            if (malformed == 3) result.push_back(0);
            if (buffered) fake::session->response_inbox.push_back(std::move(result));
            else fake::session->pipe->frames.push_back(std::move(result));
          };
          assert(!snapshot(bounded));
          // Truncated replies cannot be correlated and exhaust the wait;
          // malformed replies with this token are an immediate failure.
          assert(fake::drops == (malformed < 2 ? 1 : 0));
        }
      }
      for (const uint8_t success : {0, 1}) {
        fake::reset();
        fake::on_send = [success] {
          fake::session->response_inbox.push_back(frame(fake::sent_id - 1));
          fake::session->response_inbox.push_back(frame(fake::sent_id, success));
        };
        assert(snapshot(bounded) == (success == 1));
        assert(fake::reads == 0 && fake::drops == 0);
        assert(fake::session->response_inbox.size() == 1);
      }
    }
  }

  void test_old_helper_and_wrong_recovery_versions_never_authorize() {
    static_assert(display_helper_protocol::kSnapshotRecoveryVersion == 1);
    for (const auto protocol : {ApplyResponseProtocol::Unknown, ApplyResponseProtocol::Legacy, ApplyResponseProtocol::V2}) {
      for (const bool bounded : {false, true}) {
        for (const bool buffered : {false, true}) {
          // A missing version is the exact correlated success an older v2
          // helper emitted after writing only the snapshot. Its APPLY version
          // and matching request ID must not upgrade that into recovery proof.
          for (const int version : {-1, 0, 2, 255}) {
            fake::reset(protocol);
            fake::on_send = [=] {
              auto snapshot_only = frame(fake::sent_id);
              if (version == -1) snapshot_only.pop_back();
              else snapshot_only.back() = static_cast<uint8_t>(version);
              if (buffered) fake::session->response_inbox.push_back(std::move(snapshot_only));
              else fake::session->pipe->frames.push_back(std::move(snapshot_only));
              // A matching incompatible completion is immediately rejected;
              // do not accidentally skip it and accept a later success.
              fake::session->pipe->frames.push_back(frame(fake::sent_id));
            };
            assert(!snapshot(bounded));
            assert(fake::reads == (buffered ? 0 : 1));
            assert(fake::session->pipe->frames.size() == 1);
            assert(fake::drops == 0);
            assert_reader_released();
          }
        }
      }
    }
  }

  void test_real_json_tokenization() {
    for (const bool bounded : {false, true}) {
      for (const auto *payload : {"{broken", "null", "42", "true", "\"text\""}) {
        fake::reset();
        assert(!snapshot(bounded, payload));
        assert(fake::sends == 0 && fake::reads == 0 && fake::drops == 0);
        assert_reader_released();
      }
      fake::reset();
      reply();
      assert(snapshot(bounded, ""));
      assert(fake::sent_json.size() == 1);

      fake::reset();
      reply();
      assert(snapshot(bounded, R"(["DISPLAY\\1","\u2603", "\"quoted\""])"));
      assert(fake::sent_json.at("exclude_devices") == nlohmann::json::array({"DISPLAY\\1", "\xe2\x98\x83", "\"quoted\""}));

      fake::reset();
      reply();
      assert(snapshot(bounded, R"({"exclude_devices":["keep"],"other":{"enabled":true},"sunshine_snapshot_id":0})"));
      assert(fake::sent_json.at("exclude_devices") == nlohmann::json::array({"keep"}));
      assert(fake::sent_json.at("other").at("enabled") == true);
      assert(fake::sent_json.at("sunshine_snapshot_id") == fake::sent_id);
    }
  }

  void test_cancellation_supersession_and_retired_connections() {
    fake::reset();
    fake::cancelled = true;
    assert(!snapshot(true));
    assert(fake::connects == 0 && fake::sends == 0);

    for (const bool bounded : {false, true}) {
      for (const bool retire_connection : {false, true}) {
        for (const int stage : {0, 1, 2}) {
          fake::reset();
          reply();
          const auto supersede = [retire_connection] {
            if (retire_connection) ++connection_generation();
            else ++apply_wait_generation();
          };
          if (stage == 0) fake::on_connect = supersede;
          if (stage == 1) fake::on_send = [supersede] {
            fake::session->pipe->frames.push_back(frame(fake::sent_id));
            supersede();
          };
          if (stage == 2) fake::on_receive = supersede;
          assert(!snapshot(bounded));
          assert(fake::sends == (stage == 0 ? 0 : 1));
          assert(fake::drops == 0);
          assert_reader_released();
        }
      }
    }
    for (const int stage : {0, 1, 2}) {
      fake::reset();
      reply();
      if (stage == 0) fake::on_connect = [] { fake::cancelled = true; };
      if (stage == 1) fake::on_send = [] {
        fake::session->pipe->frames.push_back(frame(fake::sent_id));
        fake::cancelled = true;
      };
      if (stage == 2) fake::on_receive = [] { fake::cancelled = true; };
      assert(!snapshot(true));
      assert(fake::drops == 0);
      assert_reader_released();
    }
  }

  void test_reader_lock_budget_and_transport_failures() {
    for (const bool bounded : {false, true}) {
      fake::reset();
      std::promise<void> held;
      std::promise<void> release;
      auto released = release.get_future();
      std::thread owner([&] {
        std::lock_guard lock(fake::session->response_mutex);
        held.set_value();
        released.wait();
      });
      held.get_future().wait();
      const auto started = Clock::now();
      assert(!snapshot(bounded));
      assert(Clock::now() - started < 1s);
      assert(fake::sends == 0 && fake::reads == 0 && fake::drops == 0);
      release.set_value();
      owner.join();
      assert_reader_released();

      fake::reset();
      fake::send_succeeds = false;
      assert(!snapshot(bounded));
      assert(fake::sends == 1 && fake::reads == 0 && fake::drops == 0);
      assert_reader_released();

      for (const auto failure : {platf::dxgi::PipeResult::Disconnected, platf::dxgi::PipeResult::Error}) {
        fake::reset();
        fake::session->pipe->empty_result = failure;
        assert(!snapshot(bounded));
        assert(fake::reads == 1 && fake::drops == 1);
        assert_reader_released();
      }
      fake::reset();
      fake::on_connect = [] { fake::session.reset(); };
      assert(!snapshot(bounded));
      assert(fake::sends == 0);

      fake::reset();
      assert(!snapshot(bounded, "{}", 0));
      assert(fake::connects == 0 && fake::sends == 0);
    }
    fake::reset();
    reply();
    const auto started = Clock::now();
    assert(send_snapshot_current_within("{}", started + 1h, {}));
    assert(fake::connect_deadline <= Clock::now() + std::chrono::milliseconds(kSnapshotResultTimeoutMs));
    assert(fake::send_deadline == fake::connect_deadline);

    // A successful receive can return after its deadline if the reader is
    // descheduled. Both entry points must reject that otherwise valid reply.
    for (const bool bounded : {false, true}) {
      fake::reset();
      fake::on_receive = [] { std::this_thread::sleep_for(30ms); };
      reply();
      assert(!snapshot(bounded, "{}", 10));
      assert(fake::reads == 1 && fake::drops == 1);
      assert_reader_released();
    }
  }
}

int main() {
  using namespace platf::display_helper_client;
  test_every_protocol_requires_matching_completion();
  test_stale_untagged_and_unrelated_responses();
  test_malformed_frames_and_buffered_replies();
  test_old_helper_and_wrong_recovery_versions_never_authorize();
  test_real_json_tokenization();
  test_cancellation_supersession_and_retired_connections();
  test_reader_lock_budget_and_transport_failures();
}
'''

with tempfile.TemporaryDirectory(prefix="vibeshine-snapshot-ipc-") as directory:
    temp = pathlib.Path(directory)
    cpp = temp / "snapshot_ipc.cpp"
    binary = temp / "snapshot_ipc"
    cpp.write_text(program)
    include_flags = [flag for directory in json_include_directories for flag in ("-I", directory)]
    subprocess.run(
        [compiler, "-std=c++20", "-Wall", "-Wextra", "-Werror", "-pthread", "-I", str(root), *include_flags,
         str(cpp), "-o", str(binary)],
        check=True,
    )
    subprocess.run([str(binary)], check=True, timeout=15)

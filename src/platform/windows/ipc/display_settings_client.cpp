/**
 * @file src/platform/windows/ipc/display_settings_client.cpp
 */
#ifdef _WIN32

  // standard
  #include <algorithm>
  #include <array>
  #include <atomic>
  #include <chrono>
  #include <cstdint>
  #include <deque>
  #include <mutex>
  #include <optional>
  #include <string>
  #include <unordered_map>
  #include <vector>

  // local
  #include "display_settings_client.h"
  #include "src/globals.h"
  #include "src/logging.h"
  #include "src/platform/windows/ipc/display_settings_protocol.h"
  #include "src/platform/windows/ipc/pipes.h"

namespace platf::display_helper_client {

  namespace {
    constexpr int kConnectTimeoutMs = 2000;
    constexpr int kSendTimeoutMs = 5000;
    constexpr int kShutdownIpcTimeoutMs = 500;
    // Baseline capture is on the stream-start path. The helper's commit lease is
    // shorter than the client wait so an expired capture is fenced before the
    // host proceeds with display enumeration.
    constexpr int kSnapshotCurrentCommitLeaseMs = 750;
    constexpr int kSnapshotCurrentResultTimeoutMs = 1000;

    using MsgType = platf::display_helper_protocol::MsgType;
    using ResultStatus = platf::display_helper_protocol::ResultStatus;

    std::uint64_t next_request_id() {
      static std::atomic<std::uint64_t> next {1};
      auto id = next.fetch_add(1, std::memory_order_relaxed);
      if (id == 0) {
        id = next.fetch_add(1, std::memory_order_relaxed);
      }
      return id;
    }

    struct PendingCorrelatedResult {
      MsgType type;
      platf::display_helper_protocol::CorrelatedResult result;
    };

    std::deque<PendingCorrelatedResult> &pending_correlated_results() {
      static std::deque<PendingCorrelatedResult> results;
      return results;
    }

    std::uint64_t &pipe_connection_serial() {
      static std::uint64_t serial = 0;
      return serial;
    }

    std::unordered_map<std::uint64_t, std::uint64_t> &verification_connections() {
      static std::unordered_map<std::uint64_t, std::uint64_t> connections;
      return connections;
    }

    void clear_connection_scoped_state_locked() {
      pending_correlated_results().clear();
      verification_connections().clear();
    }

    void note_new_connection_locked() {
      clear_connection_scoped_state_locked();
      auto &serial = pipe_connection_serial();
      ++serial;
      if (serial == 0) {
        ++serial;
      }
    }

    std::optional<ResultStatus> take_pending_result(MsgType type, std::uint64_t request_id) {
      auto &results = pending_correlated_results();
      const auto found = std::find_if(results.begin(), results.end(), [&](const PendingCorrelatedResult &pending) {
        return pending.type == type && pending.result.request_id == request_id;
      });
      if (found == results.end()) {
        return std::nullopt;
      }
      const auto status = found->result.status;
      results.erase(found);
      return status;
    }

    void preserve_pending_result(MsgType type, platf::display_helper_protocol::CorrelatedResult result) {
      constexpr std::size_t kMaxPendingResults = 32;
      auto &results = pending_correlated_results();
      if (results.size() >= kMaxPendingResults) {
        results.pop_front();
      }
      results.push_back(PendingCorrelatedResult {type, result});
    }

    std::uint64_t system_tick_ms() {
      return static_cast<std::uint64_t>(::GetTickCount64());
    }

    bool shutdown_requested() {
      if (!mail::man) {
        return false;
      }
      try {
        auto shutdown_event = mail::man->event<bool>(mail::shutdown);
        return shutdown_event && shutdown_event->peek();
      } catch (...) {
        return false;
      }
    }

    int effective_connect_timeout() {
      return shutdown_requested() ? kShutdownIpcTimeoutMs : kConnectTimeoutMs;
    }

    int effective_send_timeout() {
      return shutdown_requested() ? kShutdownIpcTimeoutMs : kSendTimeoutMs;
    }

  }  // namespace

  namespace {
    std::optional<ResultStatus> wait_for_correlated_result_locked(
      platf::dxgi::INamedPipe &pipe,
      MsgType expected_type,
      std::uint64_t request_id,
      int result_timeout_ms,
      const char *operation,
      bool log_timeout = true
    ) {
      using namespace std::chrono;

      if (auto pending = take_pending_result(expected_type, request_id)) {
        return pending;
      }

      const auto deadline = steady_clock::now() + milliseconds(std::max(result_timeout_ms, 1));
      std::array<uint8_t, 2048> buffer {};

      while (steady_clock::now() < deadline) {
        const auto now = steady_clock::now();
        auto remaining = duration_cast<milliseconds>(deadline - now);
        if (remaining.count() < 0) {
          remaining = milliseconds(0);
        }
        const int timeout_ms = static_cast<int>(std::max<long long>(remaining.count(), 1LL));
        size_t bytes_read = 0;
        const auto result = pipe.receive(buffer, bytes_read, timeout_ms);

        if (result == platf::dxgi::PipeResult::Timeout) {
          continue;
        }
        if (result != platf::dxgi::PipeResult::Success) {
          BOOST_LOG(error) << "Display helper IPC: failed waiting for " << operation << " result (pipe error)";
          pipe.disconnect();
          clear_connection_scoped_state_locked();
          return std::nullopt;
        }
        if (bytes_read == 0) {
          BOOST_LOG(error) << "Display helper IPC: connection closed while waiting for " << operation << " result";
          pipe.disconnect();
          clear_connection_scoped_state_locked();
          return std::nullopt;
        }

        const uint8_t msg_type = buffer[0];
        const auto received_type = static_cast<MsgType>(msg_type);
        const bool correlated_type =
          received_type == MsgType::SnapshotCurrentResult ||
          received_type == MsgType::DisarmResult ||
          received_type == MsgType::ApplyResultCorrelated ||
          received_type == MsgType::VerificationResultCorrelated;
        if (correlated_type) {
          const auto decoded = platf::display_helper_protocol::decode_correlated_result(
            std::span<const std::uint8_t>(buffer.data() + 1, bytes_read - 1)
          );
          if (!decoded) {
            BOOST_LOG(warning) << "Display helper IPC: ignoring malformed " << operation << " result";
            continue;
          }
          if (received_type == expected_type && decoded->request_id == request_id) {
            return decoded->status;
          }
          BOOST_LOG(debug) << "Display helper IPC: preserving unmatched correlated result type="
                           << static_cast<int>(msg_type) << " request_id=" << decoded->request_id
                           << " while awaiting type=" << static_cast<int>(expected_type)
                           << " request_id=" << request_id;
          preserve_pending_result(received_type, *decoded);
          continue;
        }

        if (msg_type == static_cast<uint8_t>(MsgType::Ping) ||
            msg_type == static_cast<uint8_t>(MsgType::ApplyResult) ||
            msg_type == static_cast<uint8_t>(MsgType::VerificationResult) ||
            msg_type == static_cast<uint8_t>(MsgType::SnapshotCurrentResult) ||
            msg_type == static_cast<uint8_t>(MsgType::DisarmResult)) {
          continue;
        }

        BOOST_LOG(debug) << "Display helper IPC: ignoring unexpected message type=" << static_cast<int>(msg_type)
                         << " while awaiting " << operation << " result";
      }

      if (log_timeout) {
        BOOST_LOG(warning) << "Display helper IPC: timed out waiting for " << operation << " result acknowledgement";
      }
      return std::nullopt;
    }

  }  // namespace

  static bool send_message(
    platf::dxgi::INamedPipe &pipe,
    MsgType type,
    const std::vector<uint8_t> &payload,
    std::optional<int> send_timeout_override_ms = std::nullopt
  ) {
    const bool is_ping = (type == MsgType::Ping || type == MsgType::LogLevel);
    if (!is_ping) {
      BOOST_LOG(info) << "Display helper IPC: sending frame type=" << static_cast<int>(type)
                      << ", payload_len=" << payload.size();
    }
    std::vector<uint8_t> out;
    out.reserve(1 + payload.size());
    out.push_back(static_cast<uint8_t>(type));
    out.insert(out.end(), payload.begin(), payload.end());
    const int timeout_ms = send_timeout_override_ms.value_or(effective_send_timeout());
    const bool ok = pipe.send(out, timeout_ms);
    if (!ok) {
      pipe.disconnect();
      clear_connection_scoped_state_locked();
    }
    if (!is_ping) {
      BOOST_LOG(info) << "Display helper IPC: send result=" << (ok ? "true" : "false");
    }
    return ok;
  }

  // Persistent connection across a stream session. Helper stays alive until
  // successful revert; we reuse the data pipe for APPLY/REVERT.
  static std::unique_ptr<platf::dxgi::INamedPipe> &pipe_singleton() {
    static std::unique_ptr<platf::dxgi::INamedPipe> s_pipe;
    return s_pipe;
  }

  static std::optional<int> &last_log_level_sent() {
    static std::optional<int> level;
    return level;
  }

  // Global mutex to serialize all access to the pipe (connect, reset, send)
  // and prevent interleaved writes on a BYTE-mode pipe.
  static std::timed_mutex &pipe_mutex() {
    static std::timed_mutex m;
    return m;
  }

  // Ensure connected while holding the pipe mutex. Returns true on success.
  static bool ensure_connected_locked(
    std::optional<int> connect_timeout_override_ms = std::nullopt,
    bool allow_during_shutdown = false
  ) {
    if (!allow_during_shutdown && shutdown_requested()) {
      return false;
    }
    auto &pipe = pipe_singleton();
    if (pipe && pipe->is_connected()) {
      return true;
    }
    BOOST_LOG(debug) << "Display helper IPC: connecting to server pipe 'sunshine_display_helper'";
    const int connect_timeout_ms = connect_timeout_override_ms.value_or(effective_connect_timeout());
    const auto connect_start = std::chrono::steady_clock::now();
    auto remaining_ms = [&]() -> int {
      const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - connect_start
      );
      const long long remaining = static_cast<long long>(connect_timeout_ms) - elapsed.count();
      return static_cast<int>(std::max<long long>(0LL, remaining));
    };

    // A disconnected transport is a new connection epoch. Correlated commands
    // never reconnect transparently because their results belong to the exact
    // pipe that accepted the request.
    if (pipe) {
      clear_connection_scoped_state_locked();
      pipe->disconnect();
      pipe.reset();
    }

    // Create fresh pipe - try anonymous first, then named fallback
    if (remaining_ms() > 0) {
      platf::dxgi::AnonymousPipeFactory factory;
      auto base = factory.create_client_with_timeout(
        "sunshine_display_helper",
        remaining_ms()
      );
      pipe = base ? std::make_unique<platf::dxgi::FramedPipe>(std::move(base)) : nullptr;
      if (pipe) {
        if (pipe->is_connected()) {
          note_new_connection_locked();
          return true;
        }
      }
    }
    if (remaining_ms() > 0) {
      BOOST_LOG(debug) << "Display helper IPC: anonymous connect failed; trying named fallback";
      platf::dxgi::NamedPipeFactory factory;
      auto base = factory.create_client_with_timeout(
        "sunshine_display_helper",
        remaining_ms()
      );
      pipe = base ? std::make_unique<platf::dxgi::FramedPipe>(std::move(base)) : nullptr;
      if (pipe) {
        if (pipe->is_connected()) {
          note_new_connection_locked();
          return true;
        }
      }
    }
    BOOST_LOG(warning) << "Display helper IPC: connection failed";
    return false;
  }

  void reset_connection() {
    std::lock_guard<std::timed_mutex> lg(pipe_mutex());
    auto &pipe = pipe_singleton();
    if (pipe) {
      BOOST_LOG(debug) << "Display helper IPC: resetting cached connection";
      pipe->disconnect();
    }
    pipe.reset();
    last_log_level_sent().reset();
    clear_connection_scoped_state_locked();
  }

  std::optional<bool> wait_for_verification_result(std::uint64_t request_id, int timeout_ms) {
    if (request_id == 0) {
      return std::nullopt;
    }
    using namespace std::chrono;
    const auto deadline = steady_clock::now() + milliseconds(std::max(timeout_ms, 1));
    constexpr auto kReceiveSlice = milliseconds(100);

    while (steady_clock::now() < deadline) {
      const auto remaining = duration_cast<milliseconds>(deadline - steady_clock::now());
      const auto slice = std::min(kReceiveSlice, std::max(remaining, milliseconds(1)));
      std::unique_lock<std::timed_mutex> lk(pipe_mutex(), std::defer_lock);
      if (!lk.try_lock_for(slice)) {
        continue;
      }

      // Verification belongs to the exact connection that accepted APPLY. A
      // reconnect cannot produce its result and would only waste the timeout.
      auto &pipe = pipe_singleton();
      const auto owner = verification_connections().find(request_id);
      if (!pipe || !pipe->is_connected() ||
          owner == verification_connections().end() ||
          owner->second != pipe_connection_serial()) {
        verification_connections().erase(request_id);
        return std::nullopt;
      }
      if (const auto result = wait_for_correlated_result_locked(
            *pipe,
            MsgType::VerificationResultCorrelated,
            request_id,
            static_cast<int>(slice.count()),
            "verification",
            false
          )) {
        verification_connections().erase(request_id);
        return *result == ResultStatus::Succeeded;
      }
    }
    {
      std::unique_lock<std::timed_mutex> lk(pipe_mutex(), std::try_to_lock);
      if (lk.owns_lock()) {
        verification_connections().erase(request_id);
      }
    }
    BOOST_LOG(warning) << "Display helper IPC: timed out waiting for correlated verification result";
    return std::nullopt;
  }

  bool send_log_level(int min_log_level) {
    const int clamped = std::clamp(min_log_level, 0, 6);
    std::unique_lock<std::timed_mutex> lk(pipe_mutex());
    if (!ensure_connected_locked()) {
      return false;
    }
    auto &last_level = last_log_level_sent();
    if (last_level && *last_level == clamped) {
      return true;
    }
    std::vector<uint8_t> payload;
    payload.push_back(static_cast<uint8_t>(clamped));
    auto &pipe = pipe_singleton();
    if (pipe && send_message(*pipe, MsgType::LogLevel, payload)) {
      last_level = clamped;
      return true;
    }
    return false;
  }

  ApplyResult send_apply_json(const std::string &json, int result_timeout_ms) {
    BOOST_LOG(debug) << "Display helper IPC: APPLY request queued (json_len=" << json.size() << ")";
    using namespace std::chrono;
    const auto deadline = steady_clock::now() + milliseconds(std::max(result_timeout_ms, 1));
    auto remaining_ms = [&]() -> int {
      const auto remaining = duration_cast<milliseconds>(deadline - steady_clock::now()).count();
      return static_cast<int>(std::max<long long>(remaining, 0));
    };

    std::unique_lock<std::timed_mutex> lk(pipe_mutex(), std::defer_lock);
    if (!lk.try_lock_until(deadline)) {
      BOOST_LOG(warning) << "Display helper IPC: APPLY timed out waiting for the shared pipe";
      return {};
    }
    if (!ensure_connected_locked(remaining_ms())) {
      BOOST_LOG(warning) << "Display helper IPC: APPLY aborted - no connection";
      return {};
    }
    const auto request_id = next_request_id();
    const std::vector<uint8_t> body(json.begin(), json.end());
    const auto payload = platf::display_helper_protocol::encode_correlated_request(
      request_id,
      system_tick_ms() + static_cast<std::uint64_t>(std::max(remaining_ms(), 1)),
      body
    );
    auto &pipe = pipe_singleton();
    if (!pipe) {
      BOOST_LOG(warning) << "Display helper IPC: APPLY aborted - no pipe instance";
      return {};
    }

    if (remaining_ms() <= 0 ||
        !send_message(*pipe, MsgType::ApplyRequest, payload, remaining_ms())) {
      return {};
    }

    if (auto result = wait_for_correlated_result_locked(
          *pipe,
          MsgType::ApplyResultCorrelated,
          request_id,
          remaining_ms(),
          "APPLY"
        )) {
      if (*result == ResultStatus::Succeeded) {
        constexpr std::size_t kMaxVerificationOwners = 32;
        auto &owners = verification_connections();
        if (owners.size() >= kMaxVerificationOwners) {
          owners.erase(owners.begin());
        }
        owners.insert_or_assign(request_id, pipe_connection_serial());
      }
      return ApplyResult {
        .succeeded = *result == ResultStatus::Succeeded,
        .acknowledged = true,
        .request_id = request_id,
      };
    }

    BOOST_LOG(warning) << "Display helper IPC: dropping cached connection after missing APPLY result";
    pipe->disconnect();
    pipe.reset();
    clear_connection_scoped_state_locked();
    return {};
  }

  bool send_revert(const std::string &json_payload) {
    BOOST_LOG(debug) << "Display helper IPC: REVERT request queued";
    std::unique_lock<std::timed_mutex> lk(pipe_mutex());
    if (!ensure_connected_locked()) {
      BOOST_LOG(warning) << "Display helper IPC: REVERT aborted - no connection";
      return false;
    }
    std::vector<uint8_t> payload(json_payload.begin(), json_payload.end());
    auto &pipe = pipe_singleton();
    if (pipe && send_message(*pipe, MsgType::Revert, payload)) {
      return true;
    }
    return false;
  }

  bool send_revert_fast(const std::string &json_payload, int timeout_ms) {
    using namespace std::chrono;
    const auto deadline = steady_clock::now() + milliseconds(std::max(timeout_ms, 1));
    auto remaining_ms = [&]() -> int {
      const auto remaining = duration_cast<milliseconds>(deadline - steady_clock::now()).count();
      return static_cast<int>(std::max<long long>(remaining, 0));
    };

    std::unique_lock<std::timed_mutex> lk(pipe_mutex(), std::defer_lock);
    if (!lk.try_lock_until(deadline) || !ensure_connected_locked(remaining_ms(), true)) {
      return false;
    }
    std::vector<uint8_t> payload(json_payload.begin(), json_payload.end());
    auto &pipe = pipe_singleton();
    return pipe && remaining_ms() > 0 &&
           send_message(*pipe, MsgType::Revert, payload, remaining_ms());
  }

  bool send_export_golden(const std::string &json_payload) {
    BOOST_LOG(debug) << "Display helper IPC: EXPORT_GOLDEN request queued";
    std::unique_lock<std::timed_mutex> lk(pipe_mutex());
    if (!ensure_connected_locked()) {
      BOOST_LOG(warning) << "Display helper IPC: EXPORT_GOLDEN aborted - no connection";
      return false;
    }
    std::vector<uint8_t> payload(json_payload.begin(), json_payload.end());
    auto &pipe = pipe_singleton();
    if (pipe && send_message(*pipe, MsgType::ExportGolden, payload)) {
      return true;
    }
    return false;
  }

  bool send_reset() {
    BOOST_LOG(debug) << "Display helper IPC: RESET request queued";
    std::unique_lock<std::timed_mutex> lk(pipe_mutex());
    if (!ensure_connected_locked()) {
      BOOST_LOG(warning) << "Display helper IPC: RESET aborted - no connection";
      return false;
    }
    std::vector<uint8_t> payload;
    auto &pipe = pipe_singleton();
    if (pipe && send_message(*pipe, MsgType::Reset, payload)) {
      return true;
    }
    return false;
  }

  bool send_disarm_restore() {
    return send_disarm_restore_fast(kSendTimeoutMs) == DisarmResult::Disarmed;
  }

  DisarmResult send_disarm_restore_fast(int timeout_ms) {
    BOOST_LOG(debug) << "Display helper IPC: DISARM (fast) request queued (timeout_ms=" << timeout_ms << ")";
    using namespace std::chrono;
    const auto deadline = steady_clock::now() + milliseconds(std::max(timeout_ms, 1));
    auto remaining_ms = [&]() -> int {
      const auto remaining = duration_cast<milliseconds>(deadline - steady_clock::now()).count();
      return static_cast<int>(std::max<long long>(remaining, 0));
    };

    std::unique_lock<std::timed_mutex> lk(pipe_mutex(), std::defer_lock);
    if (!lk.try_lock_until(deadline)) {
      BOOST_LOG(debug) << "Display helper IPC: DISARM timed out waiting for the shared pipe";
      return DisarmResult::Unavailable;
    }
    if (!ensure_connected_locked(remaining_ms(), true)) {
      return DisarmResult::Unavailable;
    }

    const auto request_id = next_request_id();
    const auto not_after = system_tick_ms() + static_cast<std::uint64_t>(std::max(remaining_ms(), 1));
    auto payload = platf::display_helper_protocol::encode_correlated_request(request_id, not_after);
    auto &pipe = pipe_singleton();
    if (!pipe || remaining_ms() <= 0 ||
        !send_message(*pipe, MsgType::DisarmRequest, payload, remaining_ms())) {
      return DisarmResult::Unavailable;
    }

    const auto result = wait_for_correlated_result_locked(
      *pipe,
      MsgType::DisarmResult,
      request_id,
      remaining_ms(),
      "DISARM"
    );
    if (!result) {
      return DisarmResult::Unavailable;
    }
    if (*result == ResultStatus::Succeeded) {
      return DisarmResult::Disarmed;
    }
    if (*result == ResultStatus::Busy) {
      return DisarmResult::Busy;
    }
    return DisarmResult::Unavailable;
  }

  bool send_snapshot_current(const std::string &json_payload) {
    BOOST_LOG(debug) << "Display helper IPC: SNAPSHOT_CURRENT request queued";
    using namespace std::chrono;
    const auto result_deadline = steady_clock::now() + milliseconds(kSnapshotCurrentResultTimeoutMs);
    auto remaining_ms = [&]() -> int {
      const auto remaining = duration_cast<milliseconds>(result_deadline - steady_clock::now()).count();
      return static_cast<int>(std::max<long long>(remaining, 0));
    };

    std::unique_lock<std::timed_mutex> lk(pipe_mutex(), std::defer_lock);
    if (!lk.try_lock_until(result_deadline)) {
      BOOST_LOG(warning) << "Display helper IPC: SNAPSHOT_CURRENT timed out waiting for the shared pipe";
      return false;
    }
    if (!ensure_connected_locked(remaining_ms())) {
      BOOST_LOG(warning) << "Display helper IPC: SNAPSHOT_CURRENT aborted - no connection";
      return false;
    }

    // Keep the helper's absolute commit lease strictly inside this call's
    // remaining budget, including any time already spent waiting for the pipe.
    // Once this call returns, stream enumeration may begin immediately.
    constexpr int kLeaseSafetyMarginMs = 25;
    const int lease_ms = std::min(
      kSnapshotCurrentCommitLeaseMs,
      remaining_ms() - kLeaseSafetyMarginMs
    );
    if (lease_ms <= 0) {
      BOOST_LOG(warning) << "Display helper IPC: SNAPSHOT_CURRENT budget exhausted before dispatch";
      return false;
    }

    const auto request_id = next_request_id();
    const auto not_after = system_tick_ms() + static_cast<std::uint64_t>(lease_ms);
    const std::vector<uint8_t> body(json_payload.begin(), json_payload.end());
    auto payload = platf::display_helper_protocol::encode_correlated_request(request_id, not_after, body);
    auto &pipe = pipe_singleton();
    if (!pipe || remaining_ms() <= 0 ||
        !send_message(*pipe, MsgType::SnapshotCurrentRequest, payload, remaining_ms())) {
      return false;
    }

    if (auto result = wait_for_correlated_result_locked(
          *pipe,
          MsgType::SnapshotCurrentResult,
          request_id,
          remaining_ms(),
          "SNAPSHOT_CURRENT"
        )) {
      return *result == ResultStatus::Succeeded;
    }

    // Keep the pipe: the helper cannot commit past the absolute lease, and the
    // request ID prevents a late result from satisfying a future snapshot. A
    // reconnect here could make the helper interpret a timeout as a crash.
    BOOST_LOG(warning) << "Display helper IPC: SNAPSHOT_CURRENT was not confirmed within "
                       << kSnapshotCurrentResultTimeoutMs << "ms; continuing stream start.";
    return false;
  }

  bool send_stop() {
    BOOST_LOG(info) << "Display helper IPC: STOP request queued";
    std::unique_lock<std::timed_mutex> lk(pipe_mutex());
    if (!ensure_connected_locked()) {
      BOOST_LOG(warning) << "Display helper IPC: STOP aborted - no connection";
      return false;
    }
    std::vector<uint8_t> payload;
    auto &pipe = pipe_singleton();
    if (pipe && send_message(*pipe, MsgType::Stop, payload)) {
      return true;
    }
    return false;
  }

  bool send_ping() {
    // No logging for ping path to reduce log spam
    std::unique_lock<std::timed_mutex> lk(pipe_mutex());
    if (!ensure_connected_locked()) {
      return false;
    }
    std::vector<uint8_t> payload;
    auto &pipe = pipe_singleton();
    if (pipe && send_message(*pipe, MsgType::Ping, payload)) {
      return true;
    }
    return false;
  }

  bool send_ping_fast(int timeout_ms) {
    using namespace std::chrono;
    const auto deadline = steady_clock::now() + milliseconds(std::max(timeout_ms, 1));
    auto remaining_ms = [&]() -> int {
      const auto remaining = duration_cast<milliseconds>(deadline - steady_clock::now()).count();
      return static_cast<int>(std::max<long long>(remaining, 0));
    };

    std::unique_lock<std::timed_mutex> lk(pipe_mutex(), std::defer_lock);
    if (!lk.try_lock_until(deadline)) {
      // Local contention means another healthy IPC operation owns the pipe. It
      // is not evidence that callers may terminate the helper.
      return true;
    }
    if (!ensure_connected_locked(remaining_ms())) {
      return false;
    }
    std::vector<uint8_t> payload;
    auto &pipe = pipe_singleton();
    if (pipe && pipe->is_connected() && remaining_ms() <= 0) {
      // Reaching an already-connected pipe at the edge of this tiny probe
      // budget is evidence of liveness, not permission to terminate it.
      return true;
    }
    if (pipe && remaining_ms() > 0 && send_message(*pipe, MsgType::Ping, payload, remaining_ms())) {
      return true;
    }
    return false;
  }
}  // namespace platf::display_helper_client

#endif

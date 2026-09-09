#include "global_fps.h"
#include "global_fps_policy.h"
#include "src/config.h"
#include "src/logging.h"

#include <atomic>
#include <cerrno>
#include <cstdlib>
#include <fcntl.h>
#include <filesystem>
#include <future>
#include <mutex>
#include <optional>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>

extern char **environ;

namespace platf::global_fps {
  namespace {
    std::mutex mutex;
    bool managed_steam = false;
    // A disabled policy also belongs to the first stream. Joins must not
    // change it, even if the app exits before the shared runtime stops.
    std::optional<std::uint32_t> stream_limit;
    std::atomic_bool active {false};
    std::jthread monitor;

    void stop_locked() {
      if (monitor.joinable()) {
        monitor.request_stop();
        monitor.join();
      }
    }
  }

  void set_managed_steam(bool managed) {
    std::lock_guard lock(mutex);
    managed_steam = managed;
  }

  bool is_active() { return active.load(); }

  bool is_available() {
#ifdef VIBESHINE_FRAME_LIMITER_MANIFEST
    std::error_code error;
    return std::filesystem::is_regular_file(VIBESHINE_FRAME_LIMITER_MANIFEST, error);
#else
    return false;
#endif
  }

  void stop() {
    std::lock_guard lock(mutex);
    stop_locked();
    stream_limit.reset();
  }

  void start(int fps, bool virtual_display) {
    std::lock_guard lock(mutex);
    if (active.load()) return;
    stop_locked();
    if (!stream_limit) {
      const auto requested_limit = config::frame_limiter.fps_limit_millihz ?
                                     config::frame_limiter.fps_limit_millihz :
                                     fps > 0 && fps <= 1000 ? static_cast<std::uint32_t>(fps) * 1000 : 0;
      const bool enabled = selected(config::frame_limiter.provider, managed_steam) &&
                           (config::frame_limiter.enable ||
                            (virtual_display && config::frame_limiter.virtual_display_limiter_enabled()));
      stream_limit = enabled && requested_limit <= maximum_limit_millihz ? requested_limit : 0;
    }
    // Retry a failed helper with the original stream policy. Only the final
    // shared stop releases that policy for a later stream or reconnect.
    const auto limit = *stream_limit;
    if (!limit) return;
    if (!is_available()) {
      BOOST_LOG(warning) << "Global Vulkan frame limiting requires the native package's implicit Vulkan layer.";
      return;
    }

    const bool machine_host = std::getenv("VIBESHINE_MACHINE_HOST") != nullptr;
    std::promise<bool> readiness;
    auto ready_result = readiness.get_future();
    // Linux parent-death signals are tied to the spawning thread. Spawn in
    // this persistent monitor, not in a transient stream-start callback.
    monitor = std::jthread([machine_host, limit, readiness = std::move(readiness)](std::stop_token token) mutable {
      std::string executable = machine_host ?
        "/usr/libexec/vibeshine/vibeshine-session-exec" :
        "/usr/libexec/vibeshine/vibeshine-global-fps";
      std::string operation = "global-fps";
      std::string value = std::to_string(limit);
      char *arguments[] = {executable.data(), machine_host ? operation.data() : value.data(),
                           machine_host ? value.data() : nullptr, nullptr};
      int output[2];
      if (pipe2(output, O_CLOEXEC) != 0) { readiness.set_value(false); return; }
      posix_spawn_file_actions_t actions;
      if (posix_spawn_file_actions_init(&actions) != 0) {
        close(output[0]); close(output[1]); readiness.set_value(false); return;
      }
      const bool actions_ok =
        posix_spawn_file_actions_adddup2(&actions, output[1], STDOUT_FILENO) == 0 &&
        posix_spawn_file_actions_addclose(&actions, output[0]) == 0 &&
        posix_spawn_file_actions_addclose(&actions, output[1]) == 0;
      pid_t child = -1;
      const int spawn_error = actions_ok ? posix_spawn(&child, executable.c_str(), &actions, nullptr, arguments, environ) : EINVAL;
      posix_spawn_file_actions_destroy(&actions);
      close(output[1]);
      if (spawn_error != 0) {
        close(output[0]);
        BOOST_LOG(error) << "Cannot start the global Vulkan limiter helper: " << spawn_error;
        readiness.set_value(false);
        return;
      }
      struct pollfd descriptor {output[0], POLLIN, 0};
      char ready[6] {};
      std::size_t received = 0;
      const auto ready_deadline = monotonic_ns() + 5000000000;
      while (received < sizeof(ready) && !token.stop_requested() && monotonic_ns() < ready_deadline) {
        const int result = poll(&descriptor, 1, 100);
        if (result < 0 && errno != EINTR) break;
        if (result <= 0) continue;
        const auto count = read(output[0], ready + received, sizeof(ready) - received);
        if (count <= 0) { if (count < 0 && errno == EINTR) continue; break; }
        received += static_cast<std::size_t>(count);
      }
      close(output[0]);
      const bool started = received == sizeof(ready) && std::string_view(ready, 6) == "ready\n";
      active.store(started);
      readiness.set_value(started);
      bool terminating = false;
      std::int64_t deadline = 0;
      int status;
      while (true) {
        const auto result = waitpid(child, &status, WNOHANG);
        if (result == child || (result < 0 && errno != EINTR)) break;
        if ((token.stop_requested() || !started) && !terminating) {
          (void) kill(child, SIGTERM);
          deadline = monotonic_ns() + 1000000000;
          terminating = true;
        }
        if (terminating && monotonic_ns() >= deadline) {
          (void) kill(child, SIGKILL);
          while (waitpid(child, &status, 0) < 0 && errno == EINTR) {}
          break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
      }
      active.store(false);
    });
    const bool started = ready_result.get();
    if (!started) {
      stop_locked();
      BOOST_LOG(error) << "The global Vulkan limiter helper did not publish its session lease.";
      return;
    }
    BOOST_LOG(info) << "Global Vulkan frame limiter enabled at " << limit / 1000.0 << " FPS.";
  }
}  // namespace platf::global_fps

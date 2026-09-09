// Runs as the desktop user, directly or through the generation-bound broker.
#include "src/platform/linux/global_fps_policy.h"

#include <cerrno>
#include <charconv>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>
#ifdef __linux__
  #include <sys/prctl.h>
#endif

namespace {
  volatile std::sig_atomic_t stopping = 0;
  void stop(int) { stopping = 1; }

  int directory_at(int parent, const char *name) {
    (void) mkdirat(parent, name, 0700);
    const int descriptor = openat(parent, name, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    struct stat attributes {};
    if (descriptor >= 0 && fstat(descriptor, &attributes) == 0 &&
        S_ISDIR(attributes.st_mode) && attributes.st_uid == getuid() && !(attributes.st_mode & 0022)) {
      return descriptor;
    }
    if (descriptor >= 0) close(descriptor);
    return -1;
  }
}

int main(int argc, char **argv) {
  using namespace platf::global_fps;
  if (argc != 2 || !getuid() || getuid() != geteuid()) return 64;
  std::uint32_t limit = 0;
  const std::string_view token(argv[1]);
  const auto parsed = std::from_chars(token.data(), token.data() + token.size(), limit);
  if (parsed.ec != std::errc {} || parsed.ptr != token.data() + token.size() ||
      !limit || limit > maximum_limit_millihz) return 64;

  const pid_t parent = getppid();
#ifdef __linux__
  if (parent <= 1 || prctl(PR_SET_PDEATHSIG, SIGTERM) != 0 || getppid() != parent) return 126;
#endif
  struct sigaction action {};
  action.sa_handler = stop;
  sigemptyset(&action.sa_mask);
  if (sigaction(SIGTERM, &action, nullptr) != 0 || sigaction(SIGINT, &action, nullptr) != 0 ||
      sigaction(SIGHUP, &action, nullptr) != 0) return 126;
  sigset_t signals;
  sigemptyset(&signals);
  sigaddset(&signals, SIGTERM);
  sigaddset(&signals, SIGINT);
  sigaddset(&signals, SIGHUP);
  if (sigprocmask(SIG_UNBLOCK, &signals, nullptr) != 0) return 126;

  const char *home_directory = std::getenv("HOME");
  if (!home_directory || home_directory[0] != '/') return 126;
  const int home_fd = open(home_directory, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
  if (home_fd < 0) return 126;
  const int cache_fd = directory_at(home_fd, ".cache");
  close(home_fd);
  if (cache_fd < 0) return 126;
  const int state_fd = directory_at(cache_fd, "vibeshine");
  close(cache_fd);
  if (state_fd < 0) return 126;
  const int descriptor = openat(state_fd, "frame-limiter.state",
    O_WRONLY | O_CREAT | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK, 0600);
  close(state_fd);
  struct stat attributes {};
  if (descriptor < 0) return 126;
  if (fstat(descriptor, &attributes) != 0 || !S_ISREG(attributes.st_mode) ||
      attributes.st_uid != getuid() || attributes.st_nlink != 1 || (attributes.st_mode & 0022)) {
    close(descriptor);
    return 126;
  }

  // The previous broker connection can close before its desktop service has
  // finished shutting down. Give that owner a bounded window to release its
  // lock; never overwrite or clear a still-owned lease.
  const auto lock_deadline = monotonic_ns() + 1000000000;
  while (flock(descriptor, LOCK_EX | LOCK_NB) != 0) {
    if ((errno != EWOULDBLOCK && errno != EAGAIN) || stopping ||
        getppid() != parent || monotonic_ns() >= lock_deadline) {
      close(descriptor);
      return 126;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(25));
  }
  if (fchmod(descriptor, 0600) != 0) { close(descriptor); return 126; }

  bool ready = false;
  while (!stopping && getppid() == parent) {
    const lease_t lease {1, limit, monotonic_ns() + lease_duration_ns};
    if (pwrite(descriptor, &lease, sizeof(lease), 0) != sizeof(lease) ||
        ftruncate(descriptor, sizeof(lease)) != 0) break;
    if (!ready) {
      std::puts("ready");
      std::fflush(stdout);
      ready = true;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(250));
  }
  const lease_t disabled {};
  (void) pwrite(descriptor, &disabled, sizeof(disabled), 0);
  close(descriptor);
  return ready ? 0 : 126;
}

#include "../../../../src/platform/linux/maintenance_cli.h"

#include <cstdlib>
#include <initializer_list>
#include <string>

static void check(std::initializer_list<const char *> input,
                  std::initializer_list<const char *> expected, bool recognized = true) {
  std::vector<char *> argv;
  for (auto value : input) {
    argv.push_back(const_cast<char *>(value));
  }
  const auto result = platf::linux_cli::command(static_cast<int>(argv.size()), argv.data());
  if (result.has_value() != recognized || (result && result->size() != expected.size())) {
    std::abort();
  }
  if (result) {
    auto actual = result->begin();
    for (auto value : expected) {
      if (std::string {*actual++} != value) {
        std::abort();
      }
    }
  }
}

int main() {
  constexpr auto helper = "/usr/libexec/vibeshine/vibeshine-machine-host";
  check({"vibeshine", "configure", "alice"}, {helper, "configure", "alice"});
  // Arguments remain literal and cannot become shell commands or helper options.
  check({"vibeshine", "configure", "alice; touch /tmp/unwanted"}, {helper, "configure", "alice; touch /tmp/unwanted"});
  check({"vibeshine", "configure", "--help"}, {});
  check({"vibeshine", "configure"}, {});
  check({"vibeshine", "configure", "alice", "bob"}, {});
  check({"vibeshine", "migrate"}, {helper, "configure-auto"});
  check({"vibeshine", "reset"}, {helper, "reset"});
  check({"vibeshine", "reset", "anything"}, {});
  check({"vibeshine", "authorize-commands"}, {helper, "authorize-commands"});
  check({"vibeshine", "driver", "status"}, {"/usr/libexec/vibeshine/vibeshine-drm-install", "status"});
  check({"vibeshine", "driver", "install"}, {"/usr/libexec/vibeshine/vibeshine-drm-install", "install"});
  check({"vibeshine", "driver", "arbitrary-operation"}, {});
  check({"vibeshine", "status"}, {"/usr/bin/systemctl", "--no-pager", "--full", "status",
                                  "vibeshine-session-controller.service", "vibeshine-session-exec.socket", "vibeshine.service"});
  check({"vibeshine", "logs"}, {"/usr/bin/journalctl", "--no-pager", "-n", "200",
                                "-u", "vibeshine-session-controller.service", "-u", "vibeshine-session-exec@.service", "-u", "vibeshine.service"});
  check({"vibeshine"}, {}, false);
  check({"vibeshine", "/var/lib/vibeshine/vibeshine.conf"}, {}, false);
  check({"vibeshine", "--version"}, {}, false);
  check({"vibeshine", "encoder=nvenc"}, {}, false);

  char name[] = "vibeshine";
  char paths[] = "paths";
  char extra[] = "unexpected";
  char *argv[] = {name, paths, extra};
  if (platf::linux_cli::dispatch(2, argv) != 0 || platf::linux_cli::dispatch(3, argv) != 2) {
    return 1;
  }
  std::puts("PASS: Linux maintenance dispatch");
}

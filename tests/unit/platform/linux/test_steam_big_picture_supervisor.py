#!/usr/bin/env python3
"""Exercise Steam bootstrap/handoff lifetimes in the production app supervisor.

The installed Steam executable path alone is redirected to a deterministic
forking fixture. Capability hardening, subreaping, signals, and watchdogs run
unchanged. Requires an unprivileged Linux user and libcap development headers.
"""
from pathlib import Path
import ctypes
import json
import os
import signal
import subprocess
import sys
import tempfile
import time

ROOT = Path(__file__).resolve().parents[4]
if not sys.platform.startswith("linux") or os.getuid() == 0:
    print("SKIP: Steam supervisor regression requires an unprivileged Linux user")
    sys.exit(77)

# Adopt fixture daemons when a supervisor exits, so this manager-free harness
# can explicitly reap them after testing the systemd cleanup handoff.
assert ctypes.CDLL(None, use_errno=True).prctl(36, 1, 0, 0, 0) == 0


def function(source, signature):
    start = source.index(signature)
    brace = source.index("{", start)
    end, depth = brace + 1, 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end]

with tempfile.TemporaryDirectory(prefix="steam-supervisor-") as temporary:
    temporary = Path(temporary)
    fixture = temporary / "steam"
    fixture_source = temporary / "steam.c"
    fixture_source.write_text(r'''
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>

int main(int argc, char **argv) {
  if (argc != 2 || (strcmp(argv[1], "steam://open/bigpicture") &&
                    strcmp(argv[1], "steam://close/bigpicture"))) return 20;
  const char *mode = getenv("VIBESHINE_STEAM_FIXTURE");
  if (!mode) return 21;
  if (!strcmp(mode, "handoff")) { puts("handoff"); return 0; }
  int ready[2];
  if (pipe(ready)) return 22;
  const pid_t child = fork();
  if (child < 0) return 23;
  if (!child) {
    close(ready[0]);
    if (!strcmp(mode, "setsid")) {
      if (setsid() < 0) _exit(24);
      const pid_t daemon = fork();
      if (daemon < 0) _exit(25);
      if (daemon) _exit(0);
    }
    if (dprintf(STDOUT_FILENO, "%ld\n", (long) getpid()) < 0 ||
        write(ready[1], "1", 1) != 1) _exit(26);
    close(ready[1]);
    for (;;) pause();
  }
  close(ready[1]);
  char started;
  if (read(ready[0], &started, 1) != 1) return 27;
  close(ready[0]);
  return !strcmp(mode, "failure") ? 7 : 0;
}
''')
    source = (ROOT / "packaging/linux/vibeshine-app-supervisor.c").read_text()
    assert source.count('"/usr/bin/steam"') == 1
    source = source.replace('"/usr/bin/steam"', json.dumps(str(fixture)))
    supervisor_source = temporary / "supervisor.c"
    supervisor_source.write_text(source)
    supervisor = temporary / "supervisor"
    compiler = os.environ.get("CC", "cc")
    flags = ["-std=gnu11", "-Wall", "-Wextra", "-Werror", "-fsanitize=undefined",
             "-fno-sanitize-recover=undefined"]
    subprocess.run([compiler, *flags, str(fixture_source), "-o", str(fixture)], check=True)
    subprocess.run([compiler, *flags, str(supervisor_source), "-lcap", "-o", str(supervisor)], check=True)

    def launch(mode, option="--steam-big-picture", action="open"):
        return subprocess.Popen([str(supervisor), option, str(fixture), f"steam://{action}/bigpicture"],
                                env=dict(os.environ, VIBESHINE_STEAM_FIXTURE=mode),
                                stdin=subprocess.PIPE, stdout=subprocess.PIPE)

    def absent(pid):
        try:
            os.kill(pid, 0)
            return False
        except ProcessLookupError:
            return True

    def finish(process, child=None):
        if process.poll() is None:
            process.terminate()
            try:
                process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait(timeout=2)
        if child and not absent(child):
            os.kill(child, signal.SIGKILL)
        if child:
            try:
                os.waitpid(child, 0)
            except ChildProcessError:
                pass
        process.stdin.close()
        process.stdout.close()

    for action in ("open", "close"):
        process = launch("handoff", action=action)
        try:
            assert process.stdout.readline() == b"handoff\n"
            assert process.wait(timeout=2) == 0
        finally:
            finish(process)

    for mode in ("cold", "setsid"):
        process = launch(mode)
        child = None
        try:
            child = int(process.stdout.readline())
            time.sleep(0.2)
            assert process.poll() is None, "successful Steam bootstrap killed its daemon"
            status = Path(f"/proc/{child}/status").read_text()
            assert f"PPid:\t{process.pid}\n" in status, "daemon was not adopted by the supervisor"
            os.kill(child, signal.SIGTERM)
            assert process.wait(timeout=2) == 0
            assert absent(child)
        finally:
            finish(process, child)
    print("PASS: existing-client open/close and cold/setsid daemon adoption")

    for cancellation in ("watchdog", "signal"):
        process = launch("setsid")
        child = None
        try:
            child = int(process.stdout.readline())
            time.sleep(0.2)
            assert process.poll() is None
            if cancellation == "watchdog":
                process.stdin.close()
            else:
                process.terminate()
            assert process.wait(timeout=5) == 128 + signal.SIGTERM
            # After its bootstrap has exited, supervisor shutdown delegates
            # descendant cleanup to the unit's KillMode=control-group. This
            # harness has no systemd manager, so finish() removes the fixture.
            # In particular, never send a signal to the stale bootstrap PGID.
        finally:
            finish(process, child)

    for mode, option, result in (("failure", "--steam-big-picture", 7),
                                 ("cold", "--", 0)):
        process = launch(mode, option=option)
        child = None
        try:
            child = int(process.stdout.readline())
            assert process.wait(timeout=5) == result
            if option == "--":
                assert absent(child), "default launcher left descendants running"
        finally:
            finish(process, child)
    print("PASS: bounded watchdog/TERM/failure exit and unchanged default cleanup")

    invalid = [
        ["--steam-big-picture", "/usr/bin/true"],
        ["--steam-big-picture", str(fixture), "steam://install/480"],
        ["--steam-big-picture", str(fixture), "steam://open/bigpicture", "extra"],
        ["--unknown", str(fixture), "steam://open/bigpicture"],
    ]
    for arguments in invalid:
        assert subprocess.run([str(supervisor), *arguments], stdin=subprocess.DEVNULL).returncode == 2
    print("PASS: descendant mode accepts only the fixed Steam Big Picture actions")

    # Exercise the real systemd-run argv builder. The process execution
    # boundary records arguments; identity/environment and scope construction
    # are the same code the broker uses after validating the selected session.
    broker = (ROOT / "packaging/linux/vibeshine-session-broker.c").read_text()
    builder = temporary / "builder.c"
    builder.write_text(r'''
#define _GNU_SOURCE
#include <assert.h>
#include <limits.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
struct session_identity {
  const char *home, *user, *runtime, *wayland_display, *x_display, *xauthority;
  unsigned long generation;
};
static const char fixed_path[] = "/usr/local/bin:/usr/bin:/bin";
static const char application_supervisor_path[] = "/usr/libexec/vibeshine/vibeshine-app-supervisor";
static const unsigned int steam_launch_attempts = 2;
static volatile sig_atomic_t termination_signal = 0;
static char recorded[72][PATH_MAX + 64];
static int count;
static int supervise_user_service(const char *unit, char *const arguments[]) {
  assert(!strncmp(unit, "vibeshine-app-42-", 17));
  count = 0;
  while (arguments[count]) {
    assert(count < 71);
    snprintf(recorded[count], sizeof(recorded[count]), "%s", arguments[count]);
    ++count;
  }
  return 0;
}
''' + function(broker, "static int exec_user_service(") + r'''
static int find(const char *value) {
  for (int i = 0; i < count; ++i) if (!strcmp(recorded[i], value)) return i;
  return -1;
}
int main(void) {
  const struct session_identity desktop = {
    .home = "/home/test", .user = "test", .runtime = "/run/user/1000",
    .wayland_display = "wayland-0", .x_display = ":1",
    .xauthority = "/run/user/1000/xauth", .generation = 42,
  };
  char *command[] = {"/usr/bin/steam", "steam://open/bigpicture", NULL, NULL};
  for (int action = 0; action < 2; ++action) {
    command[1] = action ? "steam://close/bigpicture" : "steam://open/bigpicture";
    assert(exec_user_service(&desktop, NULL, command, false) == 0);
    const int supervisor = find(application_supervisor_path);
    assert(supervisor > 0 && supervisor + 4 == count);
    assert(!strcmp(recorded[supervisor - 1], "--"));
    assert(!strcmp(recorded[supervisor + 1], "--steam-big-picture"));
    assert(!strcmp(recorded[supervisor + 2], "/usr/bin/steam"));
    assert(!strcmp(recorded[supervisor + 3], command[1]));
    assert(find("--wait") > 0 && find("--pipe") > 0);
    assert(find("--property=ExitType=main") > 0);
    assert(find("--property=KillMode=control-group") > 0);
    assert(find("--property=RemainAfterExit=no") > 0);
    assert(find("HOME=/home/test") > 0 && find("USER=test") > 0);
    assert(find("DBUS_SESSION_BUS_ADDRESS=unix:path=/run/user/1000/bus") > 0);
  }
  command[1] = "steam://install/480";
  assert(exec_user_service(&desktop, NULL, command, false) == 0);
  assert(find("--steam-big-picture") < 0);
  command[1] = "steam://open/bigpicture"; command[2] = "extra";
  assert(exec_user_service(&desktop, NULL, command, false) == 0);
  assert(find("--steam-big-picture") < 0);
  command[2] = NULL; command[0] = "/tmp/steam";
  assert(exec_user_service(&desktop, NULL, command, false) == 0);
  assert(find("--steam-big-picture") < 0);
  puts("PASS: real broker builder retains identity, generation and watchdog scope");
}
''')
    builder_binary = temporary / "builder"
    subprocess.run([compiler, *flags, str(builder), "-o", str(builder_binary)], check=True)
    subprocess.run([str(builder_binary)], check=True)

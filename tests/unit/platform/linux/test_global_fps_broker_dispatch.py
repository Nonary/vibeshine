#!/usr/bin/env python3
"""Verify the real broker dispatch with identity and exec boundaries replaced."""
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[4]
source = (ROOT / "packaging/linux/vibeshine-session-broker.c").read_text()


def function(signature):
    start = source.index(signature)
    brace = source.index("{", start)
    end, depth = brace + 1, 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end]


paths_start = source.index("static const char steam_launch_path[]")
paths_end = source.index("// Steam can", paths_start)
program = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>
struct session_identity { char role[16]; };
static int stage, executed, via_user_service;
static bool drop_ok = true, endpoints_ok = true, authority_ok = true;
static char executed_path[256], executed_arguments[4][128];
static bool drop_to_session(const struct session_identity *id) { stage = 1; return drop_ok; }
static bool validate_session_endpoints(const struct session_identity *id) { assert(stage == 1); stage = 2; return endpoints_ok; }
static bool validate_xauthority(const struct session_identity *id) { assert(stage == 2); stage = 3; return authority_ok; }
static bool display_argument_is_safe(const char *v) { return false; }
static bool sink_name_is_safe(const char *v) { return false; }
static size_t layout_channel_count(const char *v) { return 0; }
static bool parse_channel_mapping(const char *v, size_t count, unsigned char *mapping) { return false; }
static bool format_channel_mapping(const unsigned char *v, size_t count, const char *prefix, char *out, size_t size) { return false; }
static bool numeric_suffix(const char *v, const char *prefix) { return false; }
static bool steam_direct_arguments_are_safe(int argc, char **argv) { return false; }
static bool artwork_request_is_safe(const char *v, const char *prefix, unsigned long maximum) { return false; }
static bool command_is_authorized(const char *role, const char *cmd, gid_t gid, char *out, size_t size) { return false; }
static int record_exec(const char *path, char *const argv[]);
static int exec_user_service(const struct session_identity *id, const char *directory, char *const argv[], bool retry) {
  assert(!directory && !retry); ++via_user_service;
  (void) record_exec(argv[0], argv); return 127;
}
static int record_exec(const char *path, char *const argv[]) {
  assert(stage == 3); ++executed;
  snprintf(executed_path, sizeof(executed_path), "%s", path);
  unsigned i = 0;
  for (; argv[i]; ++i) {
    assert(i < 3);
    snprintf(executed_arguments[i], sizeof(executed_arguments[i]), "%s", argv[i]);
  }
  executed_arguments[i][0] = 0;
  errno = ENOENT; return -1;
}
static void discard_error(const char *message) {}
#define execv record_exec
#define perror discard_error
''' + source[paths_start:paths_end] + function("static bool parse_number(") + function(
    "static bool global_fps_arguments_are_safe("
) + function("static int execute_request(") + r'''
int main(void) {
  struct session_identity desktop = {.role = "desktop"}, greeter = {.role = "greeter"};
  char *args[] = {"vibeshine-session-exec", "global-fps", "60000", NULL, NULL};
  assert(execute_request(3, args, &greeter, 1000) == 126 && stage == 0 && !executed);
  const char *invalid[] = {"", "0", "01", " 1", "1 ", "+1", "-1", "1.0", "1e3", "1000001", "999999999999999999999999", "1;exit", "1\n"};
  for (unsigned i = 0; i < sizeof(invalid)/sizeof(invalid[0]); ++i) {
    args[2] = (char *) invalid[i];
    assert(execute_request(3, args, &desktop, 1000) == 126 && stage == 0 && !executed);
  }
  args[2] = "60000"; args[3] = "extra";
  assert(execute_request(4, args, &desktop, 1000) == 126 && !stage && !executed);
  args[3] = NULL;
  assert(execute_request(2, args, &desktop, 1000) == 126 && !stage && !executed);
  const char *valid[] = {"1", "59940", "60000", "1000000"};
  for (unsigned i = 0; i < sizeof(valid)/sizeof(valid[0]); ++i) {
    args[2] = (char *) valid[i]; stage = executed = via_user_service = 0;
    assert(execute_request(3, args, &desktop, 1000) == 127 && stage == 3 && executed == 1);
    assert(via_user_service == 1);
    assert(!strcmp(executed_path, "/usr/libexec/vibeshine/vibeshine-global-fps"));
    assert(!strcmp(executed_arguments[0], executed_path));
    assert(!strcmp(executed_arguments[1], args[2]) && !executed_arguments[2][0]);
  }
  for (int failure = 1; failure <= 3; ++failure) {
    stage = executed = via_user_service = 0;
    drop_ok = failure != 1; endpoints_ok = failure != 2; authority_ok = failure != 3;
    assert(execute_request(3, args, &desktop, 1000) == 126 && stage == failure && !executed && !via_user_service);
  }
  drop_ok = endpoints_ok = authority_ok = true;
  stage = executed = via_user_service = 0; args[1] = "steam-big-picture"; args[2] = "open";
  assert(execute_request(3, args, &greeter, 1000) == 126 && !stage && !executed);
  args[2] = "other";
  assert(execute_request(3, args, &desktop, 1000) == 126 && !stage && !executed);
  args[2] = "open"; args[3] = "extra";
  assert(execute_request(4, args, &desktop, 1000) == 126 && !stage && !executed);
  args[3] = NULL;
  assert(execute_request(2, args, &desktop, 1000) == 126 && !stage && !executed);
  for (int close = 0; close < 2; ++close) {
    stage = executed = via_user_service = 0;
    args[2] = close ? "close" : "open";
    assert(execute_request(3, args, &desktop, 1000) == 127 && executed == 1);
    assert(via_user_service == 1);
    assert(!strcmp(executed_path, "/usr/bin/steam"));
    assert(!strcmp(executed_arguments[1], close ? "steam://close/bigpicture" : "steam://open/bigpicture"));
  }
  puts("PASS: global FPS broker bounds, desktop scope, privilege ordering and supervised dispatch");
}
'''

with tempfile.TemporaryDirectory(prefix="global-fps-broker-") as directory:
    directory = Path(directory)
    harness, binary = directory / "test.c", directory / "test"
    harness.write_text(program)
    subprocess.run([
        "cc", "-std=gnu11", "-Wall", "-Wextra", "-Werror", "-Wno-unused-parameter",
        str(harness), "-o", str(binary)
    ], check=True)
    subprocess.run([str(binary)], check=True)

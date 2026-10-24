/**
 * @file tools/steam_ui_helper.cpp
 * @brief Interactive-session helper that connects Steam Big Picture to Vibeshine.
 *
 * Launched by the machine host in the console session. It verifies that 127.0.0.1:8080 belongs to
 * Steam's own steamwebhelper.exe, injects assets/steam/big_picture_controls.js into Steam's shared
 * JS context over CDP, pushes stream status from the host and forwards button presses back over a
 * named pipe. See src/steam_big_picture_controls_protocol.h for the message formats.
 *
 * Not implemented yet: the CDP client, endpoint verification and pipe client arrive in phase 2.
 */
#include <cstdio>
#include <cstring>

int main(int argc, char **argv) {
  if (argc > 1 && std::strcmp(argv[1], "--version") == 0) {
    std::puts("sunshine_steam_ui_helper protocol 1 (not implemented)");
    return 0;
  }
  std::fputs("sunshine_steam_ui_helper: not implemented yet\n", stderr);
  return 1;
}

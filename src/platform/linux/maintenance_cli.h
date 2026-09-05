/**
 * @file src/platform/linux/maintenance_cli.h
 * @brief Native-package maintenance commands, dispatched before host startup.
 */
#pragma once

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <optional>
#include <string_view>
#include <vector>

#include <unistd.h>

namespace platf::linux_cli {
  inline constexpr const char *help =
    "Linux maintenance (native packages):\n"
    "  vibeshine paths                    Show settings and program locations\n"
    "  vibeshine status                   Show machine service status\n"
    "  sudo vibeshine logs                 Show recent service logs\n"
    "  sudo vibeshine configure USER       Select the desktop owner and migrate settings\n"
    "  sudo vibeshine migrate              Prepare existing settings after an upgrade\n"
    "  sudo vibeshine authorize-commands   Approve commands in the application list\n"
    "  sudo vibeshine driver install       Install/update the virtual-display driver\n"
    "  sudo vibeshine driver status        Show virtual-display driver status\n"
    "  sudo vibeshine reset                Erase settings and pairings; ends active streams\n";

  // A missing optional means this is a normal host invocation. An empty vector
  // denotes a recognized command with invalid arguments; it must never fall
  // through to configuration parsing or start another host.
  inline std::optional<std::vector<const char *>> command(int argc, char **argv) {
    if (argc < 2) {
      return std::nullopt;
    }
    const std::string_view name {argv[1]};
    constexpr auto machine = "/usr/libexec/vibeshine/vibeshine-machine-host";
    if (name == "configure") {
      if (argc == 3 && argv[2][0] != '\0' && argv[2][0] != '-') {
        return std::vector<const char *> {machine, "configure", argv[2]};
      }
    } else if (name == "migrate" || name == "authorize-commands" || name == "reset") {
      if (argc == 2) {
        return std::vector<const char *> {machine, name == "migrate" ? "configure-auto" : argv[1]};
      }
    } else if (name == "driver") {
      if (argc == 3 && (std::string_view {argv[2]} == "install" || std::string_view {argv[2]} == "status")) {
        return std::vector<const char *> {"/usr/libexec/vibeshine/vibeshine-drm-install", argv[2]};
      }
    } else if (name == "status") {
      if (argc == 2) {
        return std::vector<const char *> {"/usr/bin/systemctl", "--no-pager", "--full", "status",
                                        "vibeshine-session-controller.service", "vibeshine-session-exec.socket", "vibeshine.service"};
      }
    } else if (name == "logs") {
      if (argc == 2) {
        return std::vector<const char *> {"/usr/bin/journalctl", "--no-pager", "-n", "200",
                                        "-u", "vibeshine-session-controller.service", "-u", "vibeshine-session-exec@.service",
                                        "-u", "vibeshine.service"};
      }
    } else if (name != "paths" && name != "maintenance-help") {
      return std::nullopt;
    }
    return std::vector<const char *> {};
  }

  inline std::optional<int> dispatch(int argc, char **argv) {
    auto arguments = command(argc, argv);
    if (!arguments) {
      return std::nullopt;
    }
    if (argc == 2 && std::string_view {argv[1]} == "paths") {
      std::puts("Native Linux package locations:\n"
                "  Settings, credentials, pairings, apps: /var/lib/vibeshine\n"
                "  Configuration file: /var/lib/vibeshine/vibeshine.conf\n"
                "  Administrator policy: /etc/vibeshine\n"
                "  Temporary session data: /run/vibeshine\n"
                "  Programs: /usr/bin/vibeshine, /usr/libexec/vibeshine\n"
                "  Assets: /usr/share/vibeshine\n"
                "  Logs: sudo vibeshine logs\n"
                "  Legacy user settings (imported once): ~/.config/vibeshine");
      return 0;
    }
    if (argc == 2 && std::string_view {argv[1]} == "maintenance-help") {
      std::fputs(help, stdout);
      return 0;
    }
    if (arguments->empty()) {
      std::fputs(help, stderr);
      return 2;
    }
    arguments->push_back(nullptr);
    // Fixed executables and separate arguments: no shell, PATH search, or
    // privilege escalation. Administrative helpers enforce their own UID check.
    execv(arguments->front(), const_cast<char *const *>(arguments->data()));
    std::fprintf(stderr, "Vibeshine: cannot execute %s: %s\n", arguments->front(), std::strerror(errno));
    return 1;
  }
}  // namespace platf::linux_cli

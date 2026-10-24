/**
 * @file tools/steam_ui_helper/main.cpp
 * @brief Interactive-session helper that connects Steam Big Picture to Vibeshine.
 *
 * Runs as the signed-in user. It checks that 127.0.0.1:8080 belongs to Steam's own steamwebhelper.exe,
 * injects assets/steam/big_picture_controls.js into Steam's shared JS context over CDP, shows the stream
 * status there and reports button presses. See src/steam_big_picture_controls_protocol.h.
 *
 * Usage:
 *   sunshine_steam_ui_helper --version
 *   sunshine_steam_ui_helper --check [--script <path>] [fault options]
 *       Find everything in the running Steam without patching it, print the report and exit.
 *   sunshine_steam_ui_helper --fixture <status.json> [--script <path>] [fault options]
 *       Development mode: the status comes from the fixture file (re-read when it changes). Messages
 *       are printed as JSON lines, and actions are applied to the fixture status in memory so Steam
 *       shows their effect. Ctrl+C removes the patches from Steam and exits.
 *   Fault options: --simulate-missing=a,b  --simulate-crash=a,b  --max-errors=N
 *
 * The pipe mode the service uses arrives in a later phase.
 */
#include "controller.h"
#include "src/steam_big_picture_controls_protocol.h"
#include "steam_link_windows.h"

#include <windows.h>

#include <chrono>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <mutex>
#include <optional>
#include <sstream>
#include <stop_token>
#include <string>
#include <thread>
#include <vector>

namespace {
  using steam_ui_helper::json;
  namespace protocol = platf::steam::big_picture::protocol;

  std::stop_source stop_source;

  BOOL WINAPI on_console_event(DWORD) {
    stop_source.request_stop();
    return TRUE;
  }

  struct arguments_t {
    bool version = false;
    bool check = false;
    std::filesystem::path fixture;
    std::filesystem::path script;
    json inject_options = json::object();
  };

  std::vector<std::string> split_list(const std::string &value) {
    std::vector<std::string> items;
    std::stringstream stream(value);
    for (std::string item; std::getline(stream, item, ',');) {
      if (!item.empty()) {
        items.push_back(item);
      }
    }
    return items;
  }

  std::optional<arguments_t> parse(int argc, char **argv) {
    arguments_t args;
    for (int i = 1; i < argc; ++i) {
      const std::string arg = argv[i];
      const auto suffix = [&](const char *prefix) -> std::optional<std::string> {
        return arg.starts_with(prefix) ? std::optional(arg.substr(std::strlen(prefix))) : std::nullopt;
      };
      if (arg == "--version") {
        args.version = true;
      } else if (arg == "--check") {
        args.check = true;
      } else if (arg == "--fixture" && i + 1 < argc) {
        args.fixture = argv[++i];
      } else if (arg == "--script" && i + 1 < argc) {
        args.script = argv[++i];
      } else if (const auto missing = suffix("--simulate-missing=")) {
        args.inject_options["simulateMissing"] = split_list(*missing);
      } else if (const auto crash = suffix("--simulate-crash=")) {
        args.inject_options["simulateCrash"] = split_list(*crash);
      } else if (const auto errors = suffix("--max-errors=")) {
        args.inject_options["maxErrors"] = std::stoi(*errors);
      } else {
        return std::nullopt;
      }
    }
    return args;
  }

  std::filesystem::path executable_dir() {
    std::wstring path(32768, L'\0');
    path.resize(GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size())));
    return std::filesystem::path(path).parent_path();
  }

  std::string read_file(const std::filesystem::path &path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
      throw std::runtime_error("cannot read " + path.string());
    }
    std::stringstream contents;
    contents << in.rdbuf();
    return contents.str();
  }

  std::mutex print_mutex;

  void print(std::ostream &out, const json &message) {
    const auto now = std::time(nullptr);
    std::tm local {};
    localtime_s(&local, &now);
    char stamp[16];
    std::strftime(stamp, sizeof(stamp), "%H:%M:%S", &local);
    std::lock_guard lock(print_mutex);
    out << stamp << ' ' << message.dump() << std::endl;
  }

  // Development mode stands in for the service: apply an action to the fixture status.
  void apply_action(json &status, const json &action) {
    if (action.value("action", std::string {}) == protocol::action::end_stream) {
      status["appRunning"] = false;
      status["appName"] = "";
      status["clients"] = json::array();
      status["streams"] = json::array();
      return;
    }
    if (const auto clients = status.find("clients"); clients != status.end() && clients->is_array()) {
      const auto client = action.value("client", std::string {});
      json remaining = json::array();
      for (const auto &entry : *clients) {
        if (!entry.is_object() || entry.value("id", std::string {}) != client) {
          remaining.push_back(entry);
        }
      }
      status["clients"] = remaining;
      if (remaining.empty()) {
        status["streams"] = json::array();
      }
    }
  }

  int run_fixture(const arguments_t &args, const std::string &script) {
    steam_ui_helper::windows_steam_link_t link;
    std::mutex status_mutex;
    json status = json::object();
    steam_ui_helper::controller_t *controller_ref = nullptr;
    steam_ui_helper::controller_t controller(link, {
                                                     .script = script,
                                                     .inject_options = args.inject_options,
                                                     .emit = [&](const json &message) {
                                                       print(std::cout, message);
                                                       if (message.value("type", std::string {}) != protocol::message::action) {
                                                         return;
                                                       }
                                                       std::lock_guard lock(status_mutex);
                                                       apply_action(status, message);
                                                       controller_ref->set_status(status);
                                                     },
                                                   });
    controller_ref = &controller;

    std::jthread watcher([&](std::stop_token stop) {
      std::optional<std::filesystem::file_time_type> seen;
      while (!stop.stop_requested()) {
        std::error_code error;
        const auto modified = std::filesystem::last_write_time(args.fixture, error);
        if (!error && modified != seen) {
          seen = modified;
          try {
            auto loaded = json::parse(read_file(args.fixture));
            {
              std::lock_guard lock(status_mutex);
              status = loaded;
            }
            controller.set_status(std::move(loaded));
            print(std::cout, {{"type", "log"}, {"level", "info"}, {"message", "loaded " + args.fixture.string()}});
          } catch (const std::exception &failure) {
            print(std::cout, {{"type", "log"}, {"level", "warning"}, {"message", std::string("fixture: ") + failure.what()}});
          }
        }
        for (int i = 0; i < 5 && !stop.stop_requested(); ++i) {
          std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
      }
    });

    controller.run(stop_source.get_token());
    return 0;
  }

  int run_check(const arguments_t &args, const std::string &script) {
    steam_ui_helper::windows_steam_link_t link;
    steam_ui_helper::controller_t controller(link, {
                                                     .script = script,
                                                     .inject_options = args.inject_options,
                                                     .emit = [](const json &message) {
                                                       print(std::cerr, message);
                                                     },
                                                   });
    try {
      std::cout << controller.check().dump(2) << std::endl;
      return 0;
    } catch (const std::exception &error) {
      std::cerr << "check failed: " << error.what() << std::endl;
      return 1;
    }
  }
}  // namespace

int main(int argc, char **argv) {
  SetConsoleOutputCP(CP_UTF8);
  const auto args = parse(argc, argv);
  if (!args) {
    std::cerr << "usage: sunshine_steam_ui_helper --version | --check | --fixture <status.json> [--script <path>]"
                 " [--simulate-missing=a,b] [--simulate-crash=a,b] [--max-errors=N]"
              << std::endl;
    return 2;
  }
  if (args->version) {
    std::cout << "sunshine_steam_ui_helper protocol " << protocol::version << std::endl;
    return 0;
  }
  if (!args->check && args->fixture.empty()) {
    std::cerr << "sunshine_steam_ui_helper: the service pipe mode is not implemented yet; use --check or --fixture" << std::endl;
    return 2;
  }
  const auto script_path = args->script.empty() ? executable_dir().parent_path() / "assets" / "steam" / "big_picture_controls.js" : args->script;
  std::string script;
  try {
    script = read_file(script_path);
  } catch (const std::exception &error) {
    std::cerr << "sunshine_steam_ui_helper: " << error.what() << std::endl;
    return 2;
  }
  SetConsoleCtrlHandler(on_console_event, TRUE);
  return args->check ? run_check(*args, script) : run_fixture(*args, script);
}

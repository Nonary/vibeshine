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
 *   sunshine_steam_ui_helper --pipe <name> --host-pid <pid>
 *       How the service runs it: messages go both ways over the service's pipe, which must be served
 *       by exactly that process. When the pipe closes the helper removes its patches and exits.
 */
#include "controller.h"
#include "src/platform/windows/steam_big_picture_pipe.h"
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
    std::string pipe;
    DWORD host_pid = 0;
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
      } else if (arg == "--pipe" && i + 1 < argc) {
        args.pipe = argv[++i];
      } else if (arg == "--host-pid" && i + 1 < argc) {
        args.host_pid = static_cast<DWORD>(std::stoul(argv[++i]));
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
      throw std::runtime_error("cannot read " + steam_ui_helper::narrow(path.native()));
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
            print(std::cout, {{"type", "log"}, {"level", "info"}, {"message", "loaded " + steam_ui_helper::narrow(args.fixture.native())}});
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

  int run_pipe(const arguments_t &args) {
    // The host launches us into the interactive console session; refuse to run anywhere else.
    DWORD session = 0;
    if (args.host_pid == 0 || !ProcessIdToSessionId(GetCurrentProcessId(), &session) || session == 0 || session != WTSGetActiveConsoleSessionId()) {
      return 2;
    }
    const std::wstring name(args.pipe.begin(), args.pipe.end());
    HANDLE pipe = CreateFileW(name.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, FILE_FLAG_OVERLAPPED | SECURITY_SQOS_PRESENT | SECURITY_IDENTIFICATION, nullptr);
    if (pipe == INVALID_HANDLE_VALUE) {
      return 2;
    }
    ULONG server_pid = 0;
    if (!GetNamedPipeServerProcessId(pipe, &server_pid) || server_pid != args.host_pid) {
      CloseHandle(pipe);
      return 2;
    }
    int result = 0;
    {
      platf::steam::big_picture::framed_pipe_t channel(pipe);
      channel.write({{"type", protocol::message::hello}, {"protocol", protocol::version}, {"pid", GetCurrentProcessId()}});

      std::optional<json> config;
      const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
      while (!config && !channel.closed() && std::chrono::steady_clock::now() < deadline) {
        if (auto message = channel.read(std::chrono::milliseconds(250)); message && message->value("type", std::string {}) == protocol::message::config) {
          config = std::move(message);
        }
      }
      std::string script;
      try {
        if (!config || config->value("protocol", 0) != protocol::version) {
          throw std::runtime_error("no usable configuration from the host");
        }
        // Sent as UTF-8, like every string in the protocol's JSON.
        script = read_file(std::filesystem::path(steam_ui_helper::widen(config->value("script", std::string {}))));
      } catch (const std::exception &error) {
        channel.write({{"type", protocol::message::log}, {"level", "error"}, {"message", std::string("the Steam UI helper cannot start: ") + error.what()}});
        result = 2;
      }
      if (result == 0) {
        auto inject_options = config->value("inject_options", json::object());
        inject_options["features"] = config->value("features", json::object());
        steam_ui_helper::windows_steam_link_t link;
        steam_ui_helper::controller_t controller(link, {
                                                         .script = script,
                                                         .inject_options = std::move(inject_options),
                                                         .emit = [&channel](const json &message) {
                                                           channel.write(message);
                                                         },
                                                       });
        std::jthread reader([&](std::stop_token stop) {
          while (!stop.stop_requested() && !channel.closed()) {
            const auto message = channel.read(std::chrono::milliseconds(250));
            if (!message) {
              continue;
            }
            const auto type = message->value("type", std::string {});
            if (type == protocol::message::status) {
              controller.set_status(message->value("status", json::object()));
            } else if (type == protocol::message::check) {
              controller.request_check(message->value("id", std::uint64_t {0}));
            }
          }
          // The host closed the pipe: it is stopping or the feature was turned off.
          stop_source.request_stop();
        });
        controller.run(stop_source.get_token());
      }
    }
    CloseHandle(pipe);
    return result;
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
  if (!args->pipe.empty()) {
    return run_pipe(*args);
  }
  if (!args->check && args->fixture.empty()) {
    std::cerr << "sunshine_steam_ui_helper: use --check, --fixture or --pipe" << std::endl;
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

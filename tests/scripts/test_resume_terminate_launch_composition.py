#!/usr/bin/env python3
"""Exercise the production NVHTTP launch gate for hidden Resume/Terminate tiles.

The catalogue, process lookup, remote owner state, and request arguments are
deterministic test adapters; this does not start a server, RTSP transport, or
real application.
"""

import argparse
from pathlib import Path
import subprocess
import tempfile


PREAMBLE = r'''
#include "src/remote_session.h"
#include <algorithm>
#include <cassert>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

using namespace std::string_literals;
using args_t = std::multimap<std::string, std::string>;
std::string get_arg(const args_t &args, const char *key, const char *fallback = nullptr) {
  auto it = args.find(key);
  return it == args.end() ? std::string {fallback} : it->second;
}
namespace util { int from_view(const std::string &value) { return std::stoi(value); } }
namespace config { struct { bool app_resume_terminate = true; } video; }
namespace proc {
  struct resolved_t : remote_session::app_t { bool remote_monitor_profile; };
  struct process_t {
    std::vector<remote_session::app_t> apps;
    std::vector<remote_session::app_t> get_apps() const { return apps; }
    std::optional<resolved_t> resolve_app(const std::string &id, const std::string &uuid) const {
      const auto it = std::find_if(apps.begin(), apps.end(), [&](const auto &app) {
        return !uuid.empty() ? app.uuid == uuid : std::to_string(app.id) == id;
      });
      return it == apps.end() ? std::nullopt : std::optional {resolved_t {*it, it->monitor_profile}};
    }
  } proc;
}
auto remote_configured_catalogue(const std::vector<remote_session::app_t> &apps) { return apps; }
remote_session::cached_game_tiles_t cached_running_game_tiles;
struct identity_t { std::string uuid; };
struct remote_role_gate_snapshot_t {
  remote_session::owner_t owner;
  bool active {};
};
remote_role_gate_snapshot_t remote_gate_state;
remote_role_gate_snapshot_t remote_role_gate_snapshot_for_client(const std::string &) { return remote_gate_state; }
std::set<remote_session::control_e> removed_controls;
bool system_remote_app_removed(remote_session::control_e control) { return removed_controls.contains(control); }
struct tree_t {
  int status {};
  std::string message;
  void put(const char *key, int value) { if (std::string {key} == "root.<xmlattr>.status_code") status = value; }
  void put(const char *key, const char *value) { if (std::string {key} == "root.<xmlattr>.status_message") message = value; }
};
struct outcome_t {
  int status {};
  std::string message;
  remote_session::control_e control {};
};
'''


TESTS = r'''
// Status 0 means the request passed every gate and reached dispatch().
int main() {
  using remote_session::control_e;
  const remote_session::app_t game {42, "game-uuid", "Game", false};
  proc::proc.apps = {game, {77, "other-uuid", "Other", false}};
  const remote_session::game_t active {.running = true, .owner_uuid = "owner", .app = game};
  const auto projection = remote_session::project({.uuid = "viewer", .paired = true}, active, {}, proc::proc.apps, false);
  cached_running_game_tiles.remember("viewer", projection.catalogue.front().id, game);
  const auto running_copy = projection.catalogue.front().id;
  const std::string turned_off = "This app has been turned off on the host";

  for (const auto id : {remote_session::secondary_resume_id, remote_session::resume_id}) {
    assert(route("viewer", id, 42).control == control_e::resume);
  }
  for (const auto id : {remote_session::secondary_terminate_id, remote_session::terminate_id}) {
    assert(route("viewer", id, 42).control == control_e::terminate);
  }

  // The default keeps every cached control launchable.
  for (const auto id : {remote_session::secondary_resume_id, remote_session::secondary_terminate_id}) {
    assert(route("viewer", id, 42).status == 0);
  }

  // With the setting off, cached Resume and Terminate tiles are refused.
  config::video.app_resume_terminate = false;
  for (const auto id : {remote_session::secondary_resume_id, remote_session::resume_id,
                        remote_session::secondary_terminate_id, remote_session::terminate_id}) {
    const auto result = route("viewer", id, 42);
    assert(result.status == 404 && result.message == turned_off);
  }
  // A cached running-game copy and the Remote Input and Monitor tiles still work.
  auto result = route("viewer", running_copy, 42);
  assert(result.control == control_e::running_game && result.status == 0);
  for (const auto id : {remote_session::secondary_input_id, remote_session::secondary_monitor_id,
                        remote_session::input_id, remote_session::monitor_id}) {
    assert(route("viewer", id, 42).status == 0);
  }

  // Any active Remote Input or Remote Monitor brings the pair back.
  remote_gate_state.active = true;
  for (const auto id : {remote_session::secondary_resume_id, remote_session::secondary_terminate_id}) {
    assert(route("viewer", id, 42).status == 0);
  }

  // A retained Remote Monitor can always be resumed, even with no game running.
  remote_gate_state.owner = {.role = remote_session::role_e::monitor, .retained = true};
  assert(route("viewer", remote_session::resume_id, 0).status == 0);
  remote_gate_state = {};

  // A removed built-in reports its own reason first.
  removed_controls = {control_e::input};
  result = route("viewer", remote_session::secondary_input_id, 42);
  assert(result.status == 404 && result.message == "This system application has been removed");
}
'''


def fixture(source: str) -> str:
    launch = source.index('void launch(')

    start = source.index('    auto appid_str =', launch)
    end = source.index('    const bool removed_builtin =', start)
    end = source.index(';', end) + 1
    launch_prefix = source[start:end]

    start = source.index('      const auto remote_gate = remote_role_gate_snapshot_for_client(identity.uuid);', launch)
    end = source.index('      const auto decision = remote_session::dispatch(', start)
    synthetic_gate = source[start:end]

    def returning(block: str, control: str) -> str:
        return block.replace('      return;', '      return {tree.status, tree.message, ' + control + '};')

    return PREAMBLE + r'''
outcome_t route(const std::string &client, int id, int current_appid) {
  args_t args {{"appid", std::to_string(id)}};
  tree_t tree;
  const identity_t request_identity {client};
''' + returning(launch_prefix, 'remote_session::control_e::none') + r'''
  if (synthetic_control == remote_session::control_e::none) return {tree.status, tree.message, synthetic_control};
  const auto &identity = request_identity;
''' + returning(synthetic_gate, 'synthetic_control') + r'''
  return {tree.status, tree.message, synthetic_control};
}
''' + TESTS


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument('--source', type=Path, required=True)
    parser.add_argument('--compiler', required=True)
    args = parser.parse_args()
    source = (args.source / 'src/nvhttp.cpp').read_text()
    with tempfile.TemporaryDirectory() as directory:
        test_source = Path(directory) / 'resume-terminate.cpp'
        executable = Path(directory) / 'resume-terminate'
        test_source.write_text(fixture(source))
        subprocess.run([
            args.compiler, '-std=c++23', '-O0', '-pthread', '-I' + str(args.source),
            str(args.source / 'src/remote_session.cpp'), str(test_source), '-o', str(executable),
        ], check=True)
        subprocess.run([str(executable)], check=True)


if __name__ == '__main__':
    main()

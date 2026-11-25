"""Replay production composition and owner release with a simulated compositor."""

import pathlib
import subprocess
import sys
import tempfile

root = pathlib.Path(sys.argv[1]).resolve()
compiler = sys.argv[2]
includes = [path for path in sys.argv[3].split(";") if path]

source = (root / "src/platform/linux/private_display.cpp").read_text()


def function(signature):
    start = source.index("  bool " + signature + "(")
    end = source.index("\n  }", start)
    return source[start:end + len("\n  }")]


program = r'''
#include <atomic>
#include <cassert>
#include <cmath>
#include <iostream>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <vector>
#include <nlohmann/json.hpp>
#include "src/platform/linux/private_display_restore_policy.h"
#include "src/virtual_display_scale.h"

#define BOOST_LOG(level) std::cerr

namespace config {
enum class hdr_option_e { disabled, automatic };
struct video_t { struct dd_t {
  using hdr_option_e = config::hdr_option_e;
  hdr_option_e hdr_option = hdr_option_e::automatic;
  int virtual_display_scale_percent = 100;
} dd; } video;
}
namespace display_device {
struct Resolution { unsigned int width, height; };
struct Rational { unsigned int numerator, denominator; };
using FloatingPoint = Rational;
}
namespace display_target {
struct lease_t { bool begin_mutation() const { return true; } };
lease_t leases() { return {}; }
}
namespace remote_display_topology {
struct node_t {
  std::string id;
  std::string device_id;
  bool physical = false, preexisting = false, primary = false;
  int x = 0, y = 0;
  struct { int width = 1920, height = 1080, refresh_hz = 60; bool hdr = false; } configured_mode;
};
}
namespace platf::linux_private_display {
using json = nlohmann::json;
struct state_t {
  std::mutex mutex;
  std::atomic<unsigned> cleanup_generation {};
  struct { void cancel(unsigned) {} } restore_dispatcher;
  std::map<std::string, std::string> reservations;
  std::set<std::string> newly_connected_reservations;
} manager;
json topology;
std::vector<std::string> captured_names, config_arguments, detached;
std::set<std::string> managed_names, drm_disconnected;
bool detach_publishes = true, lose_survivor_before_detach = false;
bool restore_allowed() { return true; }
bool process_shutdown_preserve_requested() { return false; }
bool revert() { return true; }
state_t &state() { return manager; }
std::string client_reservation_identity(const std::string &client) { return client; }
bool connected(const json &output) { return output.value("connected", false); }
bool enabled(const json &output) { return output.value("enabled", false); }
const json *find_output(const json &current, const std::string &name) {
  for (const auto &output : current["outputs"]) {
    if (output.value("name", std::string {}) == name) return &output;
  }
  return nullptr;
}
std::optional<json> query_configuration() { return topology; }
std::vector<std::string> capture_output_names() { return captured_names; }
template<class Predicate>
bool wait_for_configuration(Predicate predicate, bool = false) {
  const bool matched = predicate(topology);
  if (matched && lose_survivor_before_detach)
    for (auto &output : topology["outputs"]) if (output.value("name", std::string {}) == "DP-3")
      output["enabled"] = false;
  return matched;
}
bool snapshot_configuration_if_needed(state_t &, const json &) { return true; }
std::string best_mode_id(const json &, auto, auto) { return "mode"; }
bool is_managed_output(const std::string &name) { return managed_names.contains(name); }
bool admit_requested_mode(std::optional<json> &, const std::string &, auto, auto) { return true; }
bool mode_matches_refresh(const json &, const std::string &, auto) { return true; }
bool output_hdr_capable(const json *, const std::string &) { return false; }
std::optional<double> retained_scale(state_t &, const std::string &) { return std::nullopt; }
void remember_scale(state_t &, const std::string &, const json *) {}
std::set<std::string> private_output_set() { return {"Virtual-1", "Virtual-2"}; }
std::map<std::string, std::string> managed_connector_identities() {
  std::map<std::string, std::string> identities;
  for (const auto &name : managed_names) identities.emplace(name, name);
  return identities;
}
bool managed_connector_identity_matches(const std::string &name, const std::string &identity) {
  return name == identity && managed_names.contains(name);
}
bool managed_connector_verified_disconnected(const std::string &name, const std::string &identity) {
  const auto *output = find_output(topology, name);
  return managed_connector_identity_matches(name, identity) && drm_disconnected.contains(name) &&
    (!output || !connected(*output));
}
bool disconnect_managed_output(const std::string &name) {
  detached.push_back(name);
  for (auto &output : topology["outputs"]) if (output.value("name", std::string {}) == name) {
    output["connected"] = !detach_publishes;
    output["enabled"] = !detach_publishes;
  }
  if (detach_publishes) drm_disconnected.insert(name);
  return true;
}
bool execute_configuration(const std::vector<std::string> &arguments, std::string_view) {
  config_arguments.insert(config_arguments.end(), arguments.begin(), arguments.end());
  return true;
}
'''

program += function("remote_apply_composed_topology")
program += function("remote_remove_owned_display")
program += r'''
}  // namespace platf::linux_private_display

int main(int argc, char **argv) {
  using namespace platf::linux_private_display;
  assert(argc == 2);
  const std::string scenario = argv[1];
  topology = {{"outputs", json::array({
    {{"name", "HDMI-A-1"}, {"connected", true}, {"enabled", true}},
    {{"name", "DP-3"}, {"connected", true}, {"enabled", true}},
    {{"name", "Virtual-1"}, {"connected", true}, {"enabled", true}},
    {{"name", "Virtual-2"}, {"connected", true}, {"enabled", true}},
    {{"name", "Virtual-9"}, {"connected", true}, {"enabled", true}},
  })}};
  managed_names = {"Virtual-1", "Virtual-2", "Virtual-9"};
  manager.reservations = {{"departing", "Virtual-1"}, {"remaining", "Virtual-2"}};

  if (scenario == "returning-output") {
    captured_names = {"HDMI-A-1", "DP-3", "Virtual-1"};
    assert(remote_apply_composed_topology({{.id = "HDMI-A-1", .physical = true, .primary = true}}));
    assert(std::ranges::none_of(config_arguments, [](const auto &argument) {
      return argument == "output.DP-3.disable" || argument == "output.Virtual-1.disable" ||
             argument == "output.Virtual-2.disable" || argument == "output.Virtual-9.disable";
    }));
    assert(detached.empty());
    assert(remote_remove_owned_display("departing"));
    assert(detached == std::vector<std::string>{"Virtual-1"});
    assert(manager.reservations.contains("remaining"));
    assert(enabled(*find_output(topology, "DP-3")));
  } else if (scenario == "shared-owner") {
    manager.reservations["remaining"] = "Virtual-1";
    assert(remote_remove_owned_display("departing"));
    assert(detached.empty() && manager.reservations.contains("remaining"));
  } else if (scenario == "stale-private-survivor") {
    captured_names = {"Virtual-9"};
    assert(!remote_remove_owned_display("departing"));
    assert(detached.empty() && manager.reservations.contains("departing"));
  } else if (scenario == "disappearing-survivor") {
    captured_names = {"DP-3"};
    lose_survivor_before_detach = true;
    assert(!remote_remove_owned_display("departing"));
    assert(detached.empty() && manager.reservations.contains("departing"));
  } else if (scenario == "unverified-detach") {
    captured_names = {"DP-3"};
    detach_publishes = false;
    assert(!remote_remove_owned_display("departing"));
    assert(detached == std::vector<std::string>{"Virtual-1"});
    assert(manager.reservations.contains("departing"));
  } else if (scenario == "already-detached") {
    drm_disconnected.insert("Virtual-1");
    for (auto &output : topology["outputs"]) if (output["name"] == "Virtual-1") {
      output["connected"] = false;
      output["enabled"] = false;
    }
    assert(remote_remove_owned_display("departing"));
    assert(detached.empty() && !manager.reservations.contains("departing"));
  } else if (scenario == "last-scanout") {
    manager.reservations.erase("remaining");
    captured_names = {"Virtual-1"};
    assert(remote_remove_owned_display("departing"));
    assert(detached.empty() && connected(*find_output(topology, "Virtual-1")));
  } else if (scenario == "physical-reservation") {
    manager.reservations["departing"] = "DP-3";
    captured_names = {"HDMI-A-1"};
    assert(!remote_remove_owned_display("departing"));
    assert(detached.empty() && manager.reservations.contains("departing"));
  } else {
    assert(false);
  }
}
'''

with tempfile.TemporaryDirectory() as directory:
    test_source = pathlib.Path(directory) / "retirement.cpp"
    binary = test_source.with_suffix("")
    test_source.write_text(program)
    subprocess.run(
        [compiler, "-std=c++20", "-pthread", "-I", str(root),
         *[f"-I{path}" for path in includes], str(test_source), "-o", str(binary)],
        check=True,
    )
    for scenario in (
        "returning-output", "shared-owner", "stale-private-survivor",
        "disappearing-survivor", "unverified-detach", "already-detached",
        "last-scanout", "physical-reservation",
    ):
        subprocess.run([str(binary), scenario], check=True)
        print(f"PASS {scenario}", flush=True)

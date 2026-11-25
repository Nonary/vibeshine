"""Replay production composition and owner release with a simulated compositor."""

import pathlib
import subprocess
import sys
import tempfile

root = pathlib.Path(sys.argv[1]).resolve()
compiler = sys.argv[2]
includes = [path for path in sys.argv[3].split(";") if path]

source = (root / "src/platform/linux/private_display.cpp").read_text()
launch_source = (root / "src/nvhttp.cpp").read_text()
coordinator_source = (root / "src/remote_display_topology.cpp").read_text()


def definition(text, signature, indent="  ", terminator="}"):
    start = text.index(indent + signature)
    end_marker = "\n" + indent + terminator
    end = text.index(end_marker, start)
    return text[start:end + len(end_marker)] + "\n"


program = r'''
#include <atomic>
#include <cassert>
#include <cmath>
#include <filesystem>
#include <iostream>
#include <memory>
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
  std::vector<std::string> virtual_display_outputs;
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
namespace rtsp_stream {
struct launch_session_t {
  std::string normal_vdd_owner_uuid, client_uuid;
  bool normal_vdd_identity_newly_reserved = true;
  unsigned normal_vdd_identity_token = 1;
};
}
namespace remote_display_topology {
struct mode_t {};
struct coordinator_t {
  struct owner_t {
    bool normal_game, remote_monitor;
    std::optional<mode_t> normal_requested_mode;
    std::uint64_t normal_game_token;
    bool normal_release_pending = false;
    std::set<unsigned> normal_capture_references;
  };
  std::mutex mutex_;
  std::map<std::string, owner_t> clients_ {
    {"departing", {true, false, mode_t {}, 1}}, {"remaining", {false, true, std::nullopt, 0}},
  };
  bool display_mutation_deferred_locked() const { return false; }
  bool rollback_normal_game_identity(const std::string &client, std::uint64_t token);
  bool reapply_composed_topology() const { return !clients_.contains("departing"); }
  std::vector<std::string> protected_remote_monitor_client_ids() const { return {"remaining"}; }
};
coordinator_t &instance() { static coordinator_t coordinator; return coordinator; }
'''
program += definition(coordinator_source, "bool coordinator_t::rollback_normal_game_identity(")
program += r'''
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
namespace fs = std::filesystem;
fs::path fixture_drm_root;
'''
has_provenance = "struct reservation_t {" in source
if has_provenance:
    program += definition(source, "struct reservation_t {", "    ", "};")
else:
    program += "using reservation_t = std::string;\n"
program += r'''
struct state_t {
  std::mutex mutex;
  std::atomic<unsigned> cleanup_generation {};
  struct { void cancel(unsigned) {} } restore_dispatcher;
  std::map<std::string, reservation_t> reservations;
  std::optional<json> snapshot;
  std::set<std::string> newly_connected_reservations;
} manager;
json topology;
std::vector<std::string> captured_names, config_arguments, detached;
std::set<std::string> managed_names, drm_disconnected;
std::vector<std::string> configured_names, broker_connected;
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
bool snapshot_configuration_if_needed(state_t &state, const json &current) {
  if (!state.snapshot) state.snapshot = current;
  return true;
}
std::vector<std::string> configured_outputs() { return configured_names; }
bool connect_managed_output(const std::string &name) {
  broker_connected.push_back(name);
  return true;
}
std::optional<json> wait_for_output_publication(const std::string &name) {
  const auto *output = find_output(topology, name);
  return output && connected(*output) ? std::optional {topology} : std::nullopt;
}
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
  for (const auto &name : managed_names) {
    std::error_code error;
    const auto identity = fs::canonical(fixture_drm_root / ("card0-" + name), error);
    if (!error) identities.emplace(name, identity.string());
  }
  return identities;
}
bool managed_connector_identity_matches(const std::string &name, const std::string &identity) {
  const auto identities = managed_connector_identities();
  const auto it = identities.find(name);
  return it != identities.end() && identity == it->second;
}
bool managed_connector_verified_disconnected(const std::string &name, const std::string &identity) {
  const auto *output = find_output(topology, name);
  return managed_connector_identity_matches(name, identity) && drm_disconnected.contains(name) &&
    (!output || !connected(*output));
}
bool disconnect_managed_output(const std::string &name) {
  if (!managed_names.contains(name)) return true;
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

if has_provenance:
    program += definition(source, "std::optional<reservation_t> probe_reservation_connector(", "    ").replace(
        '{"/sys/class/drm"}', '{fixture_drm_root}'
    )
    program += definition(source, "bool reservation_connector_matches(", "    ")
program += definition(source, "std::optional<std::string> reserve_output(", "    ")
program += definition(source, "bool remote_create_or_reclaim(")
program += definition(source, "bool remote_apply_composed_topology(")
if "static bool release_remote_reservation(" in source:
    program += definition(source, "static bool release_remote_reservation(")
    program += definition(source, "bool remote_release_unadmitted_display(")
program += definition(source, "bool remote_remove_owned_display(")
program += "} // namespace platf::linux_private_display\n"
program += definition(launch_source, "void rollback_linux_normal_display_identity(", "    ")
program += r'''
int main(int argc, char **argv) {
  using namespace platf::linux_private_display;
  assert(argc == 3);
  const std::string scenario = argv[1];
  topology = {{"outputs", json::array({
    {{"name", "HDMI-A-1"}, {"connected", true}, {"enabled", true}},
    {{"name", "DP-3"}, {"connected", true}, {"enabled", true}},
    {{"name", "Virtual-1"}, {"connected", true}, {"enabled", true}},
    {{"name", "Virtual-2"}, {"connected", true}, {"enabled", true}},
    {{"name", "Virtual-9"}, {"connected", true}, {"enabled", true}},
  })}};
  managed_names = {"Virtual-1", "Virtual-2", "Virtual-9"};
  fixture_drm_root = fs::path {argv[2]} / scenario;
  fs::create_directories(fixture_drm_root);
  const auto add_connector = [&](const std::string &name, bool managed, const std::string &card = "card0") {
    const auto relative = managed ? "devices/faux/vibeshine/" : "devices/pci/gpu/";
    const auto identity = fixture_drm_root / relative / (card + "-" + name);
    fs::create_directories(identity);
    fs::create_directory_symlink(identity, fixture_drm_root / (card + "-" + name));
  };
  const auto set_connected = [](const std::string &name, bool active) {
    for (auto &output : topology["outputs"]) {
      if (output["name"] != name) continue;
      output["connected"] = active;
      output["enabled"] = active;
    }
  };
  for (const auto &name : managed_names) add_connector(name, true);
  add_connector("DP-3", false);
  add_connector("HDMI-A-1", false);

  if (scenario == "configured-external") {
    configured_names = {"DP-3"};
    config::video.dd.virtual_display_outputs = configured_names;
    assert(remote_create_or_reclaim("departing", {}));
    assert(manager.reservations.contains("departing"));
    captured_names = {"HDMI-A-1"};
    assert(remote_remove_owned_display("departing"));
    assert(!manager.reservations.contains("departing"));
    assert(remote_create_or_reclaim("replacement", {}));
    assert(manager.reservations.contains("replacement"));
    assert(config_arguments.empty() && detached.empty() && broker_connected.empty());
    assert(enabled(*find_output(topology, "DP-3")));
    return 0;
  }
  if (scenario == "ambiguous-configured-output") {
    configured_names = {"DP-3"};
    config::video.dd.virtual_display_outputs = configured_names;
    add_connector("DP-3", false, "card1");
    assert(!remote_create_or_reclaim("departing", {}));
    assert(manager.reservations.empty() && broker_connected.empty());
    return 0;
  }

  if (scenario == "unexpected-external-in-pool") {
    configured_names = {"DP-3"};
    assert(!remote_create_or_reclaim("departing", {}));
    assert(manager.reservations.empty() && broker_connected.empty());
    return 0;
  }

  configured_names = {"Virtual-1", "Virtual-2"};
  assert(remote_create_or_reclaim("departing", {}));
  if (scenario == "shared-owner") {
    assert(reserve_output(manager, "remaining", false) == "Virtual-1");
  } else if (scenario != "last-scanout") {
    assert(remote_create_or_reclaim("remaining", {}));
  }
  broker_connected.clear();

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
    captured_names = {"Virtual-1"};
    assert(remote_remove_owned_display("departing"));
    assert(detached.empty() && connected(*find_output(topology, "Virtual-1")));
  } else if (scenario == "managed-identity-loss") {
    captured_names = {"DP-3"};
    fs::remove(fixture_drm_root / "card0-Virtual-1");
    assert(!remote_remove_owned_display("departing"));
    assert(detached.empty() && manager.reservations.contains("departing"));
    add_connector("Virtual-1", false);
    managed_names.erase("Virtual-1");
    assert(!remote_remove_owned_display("departing"));
    assert(detached.empty() && manager.reservations.contains("departing"));
  } else if (scenario == "failed-launch-active-peer" || scenario == "late-publication") {
    if (scenario == "late-publication") set_connected("Virtual-1", false);
    auto launch = std::make_shared<rtsp_stream::launch_session_t>();
    launch->client_uuid = "departing";
    rollback_linux_normal_display_identity(launch);
    assert(!remote_display_topology::instance().clients_.contains("departing"));
    assert(remote_display_topology::instance().clients_.contains("remaining"));
    assert(!manager.reservations.contains("departing") && manager.reservations.contains("remaining"));
    assert(detached.empty() && config_arguments.empty() && manager.snapshot);
    assert(enabled(*find_output(topology, "Virtual-2")));
    if (scenario == "late-publication") set_connected("Virtual-1", true);
    assert(remote_create_or_reclaim("replacement", {}));
    assert(manager.reservations.contains("replacement") && manager.reservations.contains("remaining"));
    assert(detached.empty() && enabled(*find_output(topology, "Virtual-1")));
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
    for scenario in sys.argv[4:] or (
        "returning-output", "shared-owner", "stale-private-survivor",
        "disappearing-survivor", "unverified-detach", "already-detached",
        "last-scanout", "configured-external", "ambiguous-configured-output",
        "unexpected-external-in-pool", "managed-identity-loss", "failed-launch-active-peer", "late-publication",
    ):
        subprocess.run([str(binary), scenario, directory], check=True)
        print(f"PASS {scenario}", flush=True)

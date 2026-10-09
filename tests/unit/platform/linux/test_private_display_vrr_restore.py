"""Execute the production restore planner, readback checker, and broker VRR gate.

Only HDR capability discovery/private connector inventory are fixtures. No real
display is changed. An optional git ref exercises the same cases before a fix.
"""
import pathlib
import subprocess
import sys
import tempfile

root = pathlib.Path(sys.argv[1]).resolve()
compiler = sys.argv[2]
json_includes = sys.argv[3].split(";")
source_ref = sys.argv[4] if len(sys.argv) > 4 else None


def source(path):
    if source_ref:
        return subprocess.check_output(["git", "-C", str(root), "show", f"{source_ref}:{path}"], text=True)
    return (root / path).read_text()


def function(text, signature):
    start = text.index(signature)
    brace = text.index("{", start)
    depth, end = 1, brace + 1
    while depth:
        depth += (text[end] == "{") - (text[end] == "}")
        end += 1
    return text[start:end]


display = source("src/platform/linux/private_display.cpp")
broker = source("packaging/linux/vibeshine-session-broker.c")
fragments = "\n".join([
    function(display, "const json *find_output("),
    function(display, "bool connected("),
    function(display, "bool enabled("),
    function(display, "struct phased_configuration_t") + ";",
    function(display, "std::vector<std::string> output_activation_arguments("),
    function(display, "phased_configuration_t restore_arguments("),
    function(broker, "static bool safe_text("),
    function(broker, "static bool display_argument_is_safe("),
])

program = r'''
#include <algorithm>
#include <cstring>
#include <iostream>
#include <map>
#include <regex.h>
#include <string>
#include <vector>
#include <nlohmann/json.hpp>
#include "src/platform/linux/private_display_restore_policy.h"
#include "src/platform/linux/private_display_snapshot_policy.h"
using json = nlohmann::json;
using namespace platf::linux_private_display;
struct quiet_log { template<class T> quiet_log &operator<<(const T &) { return *this; } };
#define BOOST_LOG(level) quiet_log{}
bool output_hdr_capable(const json *, const std::string &) { return false; }
std::set<std::string> private_output_set() { return {"Virtual-1"}; }
''' + fragments + r'''
int failures = 0;
void check(bool value, const std::string &message) {
  if (!value) { std::cerr << "FAIL: " << message << '\n'; ++failures; }
}
bool contains(const std::vector<std::string> &args, const std::string &arg) {
  return std::ranges::find(args, arg) != args.end();
}
json baseline_output(const std::string &name, bool active, int preference) {
  return {{"name",name}, {"connected",true}, {"enabled",active},
    {"currentModeId","60"}, {"priority",1}, {"rotation",1}, {"scale",1.0},
    {"pos",{{"x",0},{"y",0}}}, {"size",{{"width",1920},{"height",1080}}},
    {"modes", {{{"id","60"}, {"refreshRate",60.0}, {"size",{{"width",1920},{"height",1080}}}}}},
    {"vrrPolicy",preference}};
}
int main() {
  for (const auto &[preference, spelling] : std::map<int,std::string>{{0,"never"},{1,"always"},{2,"automatic"}}) {
    for (const auto name : {"DP-1", "HDMI-A-1"}) {
      auto saved_output = baseline_output(name, true, preference);
      json saved {{"outputs", {saved_output}}};
      auto current = saved;
      current["outputs"][0]["vrrPolicy"] = 1;
      auto plan = restore_arguments(saved, current, {});
      const auto command = std::string("output.") + name + ".vrrpolicy." + spelling;
      check(contains(plan.activate, command), "activation restores " + command);
      check(contains(plan.guard_activate, command), "guard restores " + command);
      check(display_argument_is_safe(command.c_str()), "broker admits " + command);
      if (preference != 1) {
        check(!restore_policy::snapshot_matches(saved, current), "activation readback rejects VRR drift");
        check(!restore_policy::snapshot_matches(saved, current, true), "final readback rejects VRR drift");
      }
      current["outputs"][0]["vrrPolicy"] = preference;
      check(restore_policy::snapshot_matches(saved, current, true), "restored VRR verifies");
      current["outputs"][0].erase("vrrPolicy");
      check(!restore_policy::snapshot_matches(saved, current, true), "known VRR requires readback");

      // An explicitly selected physical/dummy output may originally be off.
      // Restore its policy in the final disable phase without enabling it.
      saved["outputs"][0]["enabled"] = false;
      saved["outputs"].push_back(baseline_output("eDP-1", true, 2));
      current = saved;
      current["outputs"][0]["enabled"] = true;
      current["outputs"][0]["vrrPolicy"] = 1;
      plan = restore_arguments(saved, current, {});
      check(contains(plan.deactivate, command), "disabled baseline restores " + command);
      check(!contains(plan.activate, std::string("output.") + name + ".enable"), "disabled baseline remains off");
      current["outputs"][0]["enabled"] = false;
      if (preference != 1) check(!restore_policy::snapshot_matches(saved, current, true), "disabled output VRR drift remains pending");
      current["outputs"][0]["vrrPolicy"] = preference;
      check(restore_policy::snapshot_matches(saved, current, true), "disabled output policy verifies");

      saved_output.erase("vrrPolicy");
      auto legacy = output_activation_arguments(saved_output, &saved_output, name);
      check(std::ranges::none_of(legacy, [](const auto &arg) { return arg.find(".vrrpolicy.") != std::string::npos; }), "legacy snapshot invents no VRR setting");
    }
  }
  for (const auto suffix : {"auto", "0", "Never", "always.extra", "automatic ", "never;touch /tmp/x", "never\noutput.eDP-1.enable"}) {
    const auto command = std::string("output.DP-1.vrrpolicy.") + suffix;
    check(!display_argument_is_safe(command.c_str()), "broker rejects malformed VRR " + command);
  }
  if (failures) return 1;
  std::cout << "Production VRR restoration, exact readback, and broker allowlist passed\n";
}
'''

with tempfile.TemporaryDirectory(prefix="vibeshine-vrr-restore-") as temporary:
    work = pathlib.Path(temporary)
    if source_ref:
        # Readback and validation must come from the same revision as the plan.
        for name in ("private_display_restore_policy.h", "private_display_snapshot_policy.h", "private_display_mode_policy.h"):
            path = pathlib.Path("src/platform/linux") / name
            target = work / path
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_text(source(str(path)))
    translation_unit = work / "restore.cpp"
    translation_unit.write_text(program)
    executable = work / "restore"
    subprocess.run([compiler, "-std=c++20", "-Wall", "-Wextra", "-Werror", "-I", str(work), "-I", str(root),
                    *[item for path in json_includes for item in ("-I", path)],
                    str(translation_unit), "-o", str(executable)], check=True)
    subprocess.run([str(executable)], check=True)

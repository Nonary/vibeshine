"""Exercise production Linux snapshot storage and capture admission with injected I/O failures."""
import pathlib
import subprocess
import sys
import tempfile

root = pathlib.Path(sys.argv[1]).resolve()
compiler = sys.argv[2]
includes = [path for argument in sys.argv[3:] for path in argument.split(';') if path]


def function(text, signature):
    start = text.index(signature)
    brace = text.index('{', start)
    depth, end = 1, brace + 1
    while depth:
        depth += (text[end] == '{') - (text[end] == '}')
        end += 1
    return text[start:end]


storage = (root / 'src/state_storage.cpp').read_text()
display = (root / 'src/platform/linux/private_display.cpp').read_text()
program = r'''
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include "src/linux_display_snapshot_storage.h"
#include "src/state_storage_policy.h"
using json = nlohmann::json;
using namespace platf::linux_private_display;
struct quiet_log { template<class T> quiet_log &operator<<(const T &) { return *this; } };
#define BOOST_LOG(level) quiet_log{}
namespace statefile {
namespace fs = std::filesystem;
namespace pt = boost::property_tree;
pt::ptree disk;
policy::load_result_e read_status = policy::load_result_e::loaded;
bool fail_write = false, visible_failed_write = false;
int writes = 0, reads = 0;
void migrate_recent_state_keys() {}
const std::string &vibeshine_state_path() { static const std::string path="shared-state.json"; return path; }
std::mutex &state_mutex() { static std::mutex mutex; return mutex; }
policy::load_result_e load_tree_for_read(const fs::path &, pt::ptree &tree) {
  ++reads;
  if (read_status == policy::load_result_e::loaded) tree = disk;
  return read_status;
}
void write_tree(const fs::path &, const pt::ptree &tree) {
  ++writes;
  if (!fail_write || visible_failed_write) disk = tree;
  if (fail_write) throw std::runtime_error("injected durable write failure");
}
''' + function(storage, '  bool save_linux_display_snapshot(') + '\n' + function(storage, '  linux_display_snapshot_read_result_t read_linux_display_snapshot(') + r'''
}
struct state_t {
  bool snapshot_loaded=false, snapshot_legacy_record=false;
  std::optional<json> snapshot;
  std::map<std::string,std::string> reservations;
};
std::string binding = "1000:desktop";
std::string snapshot_owner() { return binding; }
std::set<std::string> private_output_set() { return {"Virtual-1"}; }
''' + '\n'.join(function(display, signature) for signature in (
    '    bool persist_snapshot(', '    bool load_snapshot_if_needed(', '    bool snapshot_configuration_if_needed(')) + r'''
int failures=0;
void check(bool result, const char *message) { if (!result) { ++failures; std::cerr << "FAIL: " << message << '\n'; } }
json topology(const std::string &name="DP-1") {
  return {{"outputs", {{{"name",name},{"enabled",true},{"connected",true},{"currentModeId","60"},
    {"modes", {{{"id","60"},{"refreshRate",60.0},{"size",{{"width",1920},{"height",1080}}}}}}}}}};
}
std::string record(const std::string &owner, const json &saved, bool pending=true) {
  return json{{"version",1},{"owner",owner},{"restore_pending",pending},{"topology",saved}}.dump();
}
int main() {
  using status = statefile::linux_display_snapshot_status_e;
  const auto original = topology();
  const auto desktop_record = record("1000:desktop", original);
  statefile::disk.put("root.linux_display_topology", desktop_record);
  statefile::disk.put("root.unrelated", "retained");
  // Cold boot into another compositor must migrate, never overwrite, the old owner.
  binding="472:greeter";
  state_t greeter;
  check(load_snapshot_if_needed(greeter) && !greeter.snapshot, "foreign desktop is not applied to greeter");
  check(snapshot_policy::prepare_startup(greeter.snapshot, topology("HDMI-A-1"), private_output_set(),
      [](const json &saved) { return persist_snapshot(saved, false); }) == snapshot_policy::startup_action_e::ready,
      "idle greeter can persist its own baseline");
  check(statefile::read_linux_display_snapshot("1000:desktop").contents == desktop_record, "legacy desktop pending record survives greeter idle write");
  check(statefile::read_linux_display_snapshot(binding).status == status::loaded, "greeter baseline independently reloads");
  check(!statefile::disk.get_child_optional("root.linux_display_topology"), "legacy key migrates only after validated save");
  check(statefile::disk.get<std::string>("root.unrelated") == "retained", "unrelated machine settings preserved");
  check(snapshot_configuration_if_needed(greeter, topology("HDMI-A-1")), "greeter can arm its own stream");
  check(statefile::save_linux_display_snapshot(binding, std::nullopt), "greeter completion clears its own record");
  check(statefile::read_linux_display_snapshot(binding).status == status::missing, "completed greeter intent is absent");
  check(statefile::read_linux_display_snapshot("1000:desktop").contents == desktop_record, "greeter completion retains desktop obligation");
  binding="1000:desktop";
  state_t desktop;
  check(load_snapshot_if_needed(desktop) && desktop.snapshot == original, "returning desktop reloads original intent");
  check(snapshot_configuration_if_needed(desktop, topology("HDMI-A-1")) && desktop.snapshot == original,
        "new preparation cannot replace original with current subset");
  check(statefile::save_linux_display_snapshot(binding, std::nullopt), "desktop completion clears own record");

  for (const auto failure : {statefile::policy::load_result_e::failed, statefile::policy::load_result_e::corrupt}) {
    statefile::disk.clear(); statefile::disk.put("root.linux_display_topology", desktop_record);
    const auto before=statefile::disk;
    statefile::read_status=failure;
    state_t unavailable;
    const auto writes=statefile::writes;
    check(!snapshot_configuration_if_needed(unavailable, topology()), "read failure or corruption prevents hotplug admission");
    check(!unavailable.snapshot_loaded && !unavailable.snapshot, "failed read is not cached as missing");
    check(statefile::read_linux_display_snapshot(binding).status == status::failed, "corrupt direct primary state remains failed");
    check(!statefile::save_linux_display_snapshot(binding, std::nullopt), "failed read cannot clear unknown intent");
    check(statefile::writes == writes && statefile::disk == before, "read failure performs no write");
    statefile::read_status=statefile::policy::load_result_e::loaded;
    check(load_snapshot_if_needed(unavailable) && unavailable.snapshot == original, "read retries and recovers original");
  }
  for (const auto &malformed : {std::string("{"), record(binding, json{{"outputs","invalid"}}), record("472:greeter", json{{"outputs","invalid"}})}) {
    statefile::disk.clear(); statefile::disk.put("root.linux_display_topology", malformed);
    const auto before=statefile::disk;
    state_t unavailable;
    check(!snapshot_configuration_if_needed(unavailable, topology()), "invalid owned or foreign intent blocks fresh capture");
    check(!statefile::save_linux_display_snapshot(binding, std::nullopt), "invalid record is not silently erased");
    check(statefile::disk == before, "malformed evidence retained");
  }
  statefile::disk.clear();
  statefile::disk.put("root.linux_display_topology", desktop_record);
  auto wrong = record("472:greeter", topology("HDMI-A-1"));
  check(!statefile::save_linux_display_snapshot(binding, wrong), "caller cannot store another owner's record");
  // Contradictory legacy and owner-keyed originals require operator recovery.
  statefile::disk.put("root.linux_display_topologies." + statefile::linux_display_snapshot_storage::key(binding), record(binding, topology("DP-2")));
  check(statefile::read_linux_display_snapshot(binding).status == status::failed, "conflicting originals are not silently superseded");
  check(!statefile::save_linux_display_snapshot(binding, std::nullopt), "conflicting originals are not cleared");

  for (bool visible_failure : {false,true}) {
    statefile::disk.clear();
    statefile::fail_write=true; statefile::visible_failed_write=visible_failure;
    state_t fresh;
    check(!snapshot_configuration_if_needed(fresh, topology()), "failed durable save never acknowledges admission");
    check(!fresh.snapshot, "failed capture remains unarmed");
    statefile::fail_write=false; statefile::visible_failed_write=false;
    check(snapshot_configuration_if_needed(fresh, topology()), "successful durable retry admits capture");
  }
  statefile::fail_write=true;
  const auto before=statefile::disk;
  check(!statefile::save_linux_display_snapshot(binding, std::nullopt), "failed completion persistence retains pending status");
  check(statefile::disk == before, "pre-rename failed clear preserves original");
  statefile::fail_write=false;
  check(statefile::save_linux_display_snapshot(binding, std::nullopt), "subsequent completion can retire record");
  if (failures) return 1;
  std::cout << "Production owner migration, pending authority, read failure, and durable admission passed\n";
}
'''

with tempfile.TemporaryDirectory(prefix='vibeshine-snapshot-storage-') as temporary:
    work = pathlib.Path(temporary)
    source = work / 'storage.cpp'
    source.write_text(program)
    executable = work / 'storage'
    subprocess.run([compiler, '-std=c++20', '-Wall', '-Wextra', '-Werror', '-I', str(root),
                    *[item for path in includes for item in ('-I', path)], str(source), '-o', str(executable)], check=True)
    subprocess.run([str(executable)], check=True)

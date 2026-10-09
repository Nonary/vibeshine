"""Run the production recovery planner/transaction against deterministic display and storage failures."""
import pathlib
import subprocess
import sys
import tempfile

root = pathlib.Path(sys.argv[1]).resolve()
compiler = sys.argv[2]
includes = sys.argv[3].split(';')
display = (root / 'src/platform/linux/private_display.cpp').read_text()


def function(signature):
    start = display.index(signature)
    brace = display.index('{', start)
    depth, end = 1, brace + 1
    while depth:
        depth += (display[end] == '{') - (display[end] == '}')
        end += 1
    return display[start:end]


program = r'''
#include <algorithm>
#include <chrono>
#include <iostream>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>
#include <nlohmann/json.hpp>
#include "src/platform/linux/private_display_snapshot_policy.h"
#include "src/platform/linux/private_display_restore_transaction.h"
#include "src/platform/linux/private_display_recovery_policy.h"
using json=nlohmann::json;
using namespace platf::linux_private_display;
struct quiet_log { template<class T> quiet_log &operator<<(const T &) { return *this; } };
#define BOOST_LOG(level) quiet_log{}
struct state_t { std::optional<json> snapshot; std::map<std::string,std::string> reservations; std::set<std::string> newly_connected_reservations; };
json observed;
std::optional<json> durable;
bool fail_configure=false, fail_retire=false, fail_clear=false, load_known=true, allow=true, cancel_on_capture=false;
int writes=0, clears=0, configurations=0, retirements=0;
struct context_t { std::chrono::steady_clock::time_point deadline; };
context_t *restore_context=nullptr;
constexpr auto helper_reply_timeout=std::chrono::seconds(10);
bool process_shutdown_preserve_requested(){return false;}
bool restore_allowed(){return allow;}
std::string snapshot_owner(){return "1000:desktop";}
std::set<std::string> private_output_set(){return {"Virtual-1"};}
std::vector<std::string> discover_managed_outputs(){return {"Virtual-1"};}
bool managed_connector_identity_matches(const std::string&,const std::string&){return true;}
std::optional<json> query_configuration(){return observed;}
const json *find_output(const json&,const std::string&);
bool connected(const json&);
bool enabled(const json&);
bool output_hdr_capable(const json*,const std::string&){return true;}
void remember_reserved_scales(state_t&,const json&){}
bool load_snapshot_if_needed(state_t&){return load_known;}
bool persist_snapshot(const json &snapshot){durable=snapshot;++writes;return true;}
namespace statefile {
bool save_linux_display_snapshot(const std::string&, std::nullopt_t){
  ++clears;
  if (fail_clear) return false;
  durable.reset();return true;
}
}
bool execute_configuration(const std::vector<std::string>& arguments,const char*){
  ++configurations;
  if (fail_configure) return false;
  for (const auto &arg:arguments) for (auto &output:observed["outputs"]) {
    const auto prefix="output."+output["name"].get<std::string>()+".";
    if (arg==prefix+"enable") output["enabled"]=true;
    if (arg==prefix+"disable") output["enabled"]=false;
  }
  return true;
}
bool wait_for_snapshot_activation(const json& snapshot,bool final=false){return allow && restore_policy::snapshot_matches(snapshot,observed,final);}
bool wait_for_capture_publication(const std::string& name){
  if (cancel_on_capture) allow=false;
  const auto*p=find_output(observed,name);return p && connected(*p)&&enabled(*p);
}
template<class Predicate> bool wait_for_configuration(Predicate predicate,bool){return predicate(observed);}
bool disconnect_managed_output(const std::string&name){
  ++retirements;
  if (fail_retire) return false;
  for(auto&o:observed["outputs"])if(o["name"]==name){o["connected"]=false;o["enabled"]=false;}
  return true;
}
namespace display_power {struct dpms_state_t{};std::optional<std::map<std::string,dpms_state_t>> query_dpms_states(std::chrono::steady_clock::time_point){return std::map<std::string,dpms_state_t>{};}}
recovery_policy::connector_state_t recovery_connector_state(const std::string&,const std::map<std::string,display_power::dpms_state_t>&){return {true,true,true,true,true,true};}
''' + '\n'.join(function(signature) + (';' if signature.startswith('    struct ') else '') for signature in (
    '    const json *find_output(', '    bool connected(', '    bool enabled(',
    '    json physical_topology_signature(', '    struct phased_configuration_t',
    '    std::vector<std::string> output_activation_arguments(', '    phased_configuration_t restore_arguments(',
    '  static bool revert_locked(')) + r'''
int failures=0;
void check(bool value,const char*message){if(!value){++failures;std::cerr<<"FAIL: "<<message<<'\n';}}
json output(std::string name,bool active=true){return {{"name",name},{"connected",true},{"enabled",active},
  {"currentModeId","60"},{"modes",{{{"id","60"},{"refreshRate",60.0},{"size",{{"width",1920},{"height",1080}}}}}},
  {"size",{{"width",1920},{"height",1080}}},{"pos",{{"x",0},{"y",0}}},{"rotation",1},{"scale",1.0},{"priority",1},{"hdr",false}};}
json topology(std::initializer_list<json> outputs){return {{"outputs",outputs}};}
void reset(){fail_configure=fail_retire=fail_clear=cancel_on_capture=false;load_known=allow=true;writes=clears=configurations=retirements=0;}
int main(){
  const auto original=topology({output("DP-1"),output("DP-2")});
  auto partial=topology({output("DP-1"),output("DP-2",false),output("Virtual-1")});
  partial["outputs"][1]["connected"]=false;
  state_t manager;
  restore_transaction::result_e failure;
  const std::map<std::string,std::string> identities{{"Virtual-1","verified-owner"}};
  reset();manager.snapshot=original;durable=original;observed=partial;
  check(!revert_locked(manager,&failure,recovery_policy::target_policy_e::automatic_recovery,nullptr,&identities),"automatic partial visibility is not exact completion");
  check(failure==restore_transaction::result_e::baseline_pending,"successful subset cleanup reports pending authority");
  check(manager.snapshot==original&&durable==original&&writes==0&&clears==0,"partial cleanup preserves original memory and durable record");
  check(retirements==1&&!find_output(observed,"Virtual-1")->value("connected",true),"verified live physical guard permits owned connector retirement");
  check(snapshot_policy::capture(manager.snapshot,observed,private_output_set(),false,[](const json&s){return persist_snapshot(s);})&&manager.snapshot==original&&writes==0,
        "new prepare after partial cleanup retains authority");
  observed["outputs"][1]["connected"]=true;
  check(revert_locked(manager,&failure,recovery_policy::target_policy_e::automatic_recovery),"returned disabled original monitor can complete exact recovery");
  check(restore_policy::snapshot_matches(original,observed,true)&&!manager.snapshot&&!durable,"only complete original readback clears obligation");

  const auto absent=topology({output("DP-2")});
  for(bool fail:{true,false}){
    reset();manager.snapshot=absent;durable=absent;observed=topology({output("HDMI-A-1"),output("Virtual-1")});fail_configure=fail;
    check(!revert_locked(manager,&failure),"unavailable original remains pending with live unrelated monitor");
    check(manager.snapshot==absent&&durable==absent&&writes==0&&clears==0,"live fallback cannot overwrite authority even before configuration failure");
    check(fail?retirements==0:retirements==1,"safe fallback retirement obeys configuration success");
  }
  reset();manager.snapshot=original;durable=original;observed=topology({output("DP-1"),output("DP-2"),output("Virtual-1")});fail_clear=true;
  check(!revert_locked(manager,&failure)&&failure==restore_transaction::result_e::persistence_failed,"failed durable completion cannot report restored");
  check(manager.snapshot==original&&durable==original,"failed retirement retains pending authority");
  fail_clear=false;check(revert_locked(manager,&failure)&&!manager.snapshot&&!durable,"durable completion retry succeeds");

  reset();manager.snapshot=original;durable=original;observed=partial;cancel_on_capture=true;
  check(!revert_locked(manager,&failure)&&retirements==0&&clears==0&&manager.snapshot==original&&durable==original,"cancelled generation cannot retire connector or intent");
  reset();manager.snapshot=original;durable=original;observed=partial;fail_retire=true;
  check(!revert_locked(manager,&failure)&&failure==restore_transaction::result_e::connector_disconnect_failed&&clears==0&&manager.snapshot==original,"failed connector retirement retains original");

  reset();manager.snapshot.reset();durable.reset();observed=topology({output("HDMI-A-1"),output("Virtual-1")});load_known=false;
  check(!revert_locked(manager,&failure)&&writes==0&&configurations==0&&retirements==0,"unreadable state cannot grant missing-baseline cleanup");
  load_known=true;
  check(revert_locked(manager,&failure)&&writes==1&&retirements==1&&!durable,"known missing baseline can durably guard live-physical orphan cleanup");
  reset();manager.snapshot.reset();durable.reset();observed=topology({output("HDMI-A-1",false),output("Virtual-1")});
  check(!revert_locked(manager,&failure)&&retirements==0&&writes==0,"missing baseline cannot guess a disabled physical target");
  if(failures)return 1;
  std::cout<<"Production restore authority, partial visibility, cancellation, and completion failure tests passed\n";
}
'''

with tempfile.TemporaryDirectory(prefix='vibeshine-recovery-authority-') as temporary:
    work=pathlib.Path(temporary)
    source=work/'recovery.cpp'
    source.write_text(program)
    executable=work/'recovery'
    subprocess.run([compiler,'-std=c++20','-Wall','-Wextra','-Werror','-Wno-missing-field-initializers','-I',str(root),
                    *[item for path in includes for item in ('-I',path)],str(source),'-o',str(executable)],check=True)
    subprocess.run([str(executable)],check=True)

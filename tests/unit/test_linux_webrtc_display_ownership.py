"""Execute full Linux WebRTC startup and its shared RTSP display ownership path.

The production launch builder, startup, NVHTTP ownership helpers, private output
reservation/preparation/removal and complete coordinator are compiled unchanged.
Only encoder, transport and compositor effects use deterministic fakes. These
cases came from independent reproductions of missing browser ownership and wrong
resume identity; cache turnover and rollback exercise the complete startup too.
Usage: python3 test_linux_webrtc_display_ownership.py REPO CXX [JSON_INCLUDE ...]
"""
import pathlib
import subprocess
import sys
import tempfile

repo = pathlib.Path(sys.argv[1]).resolve()
compiler = sys.argv[2]

def get(path):
    return (repo / path).read_text()

def definition(source, signature):
    start = source.index(signature)
    brace = source.index("{", start)
    depth, end = 1, brace + 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end]


w = get("src/webrtc_stream.cpp")
p = get("src/platform/linux/private_display.cpp")
nv = get("src/nvhttp.cpp")
program=r'''
#include <algorithm>
#include <array>
#include <atomic>
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <ranges>
#include <set>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>
#include <nlohmann/json.hpp>
#include "src/remote_display_topology.h"
#include "src/remote_session.h"
#include "src/platform/linux/private_display_resume_policy.h"
#include "src/platform/linux/private_display_cleanup_policy.h"
#ifndef __linux__
#define __linux__
#endif
#undef _WIN32
#undef SUNSHINE_ENABLE_WEBRTC
using json=nlohmann::json;
struct NullLog {template<class T> NullLog& operator<<(const T&){return *this;}};
#define BOOST_LOG(x) NullLog{}
namespace util {
 template<class F> struct guard {F f;bool active=true; ~guard(){if(active)f();}void disable(){active=false;}};
 template<class F> auto fail_guard(F f){return guard<F>{f};}
 int from_view(const std::string&s){return std::stoi(s);}
 template<class T> std::string hex_vec(const T&){return "00";}
}
namespace crypto {using aes_t=std::vector<unsigned char>;namespace cipher {struct gcm_t{};} auto rand(int n){return aes_t(n);}}
int RAND_bytes(void*,int){return 1;}
namespace uuid_util {struct uuid_t {static uuid_t generate(){return {};}std::string string(){static int i=0;return "random-"+std::to_string(++i);}};}
namespace config {
 struct video_t {enum class virtual_display_mode_e {disabled,shared,per_client}; enum class virtual_display_layout_e {extend}; struct dd_t {enum class config_option_e {disabled};}; virtual_display_mode_e virtual_display_mode=virtual_display_mode_e::per_client;} video;
 struct runtime_output_override_lease_t {std::string output;}; std::string output;
 void merge_config_overrides(auto&,const auto&){} bool adapter_config_overrides_compatible_with_active(const auto&){return true;}
 void set_runtime_config_overrides(auto){} void clear_runtime_config_overrides(){} void apply_config_now(){} void mark_deferred_reload(){}
 void maybe_apply_deferred(){} int acquire_apply_read_gate(){return 0;}void record_active_adapter_config(){}
 auto set_runtime_output_name_override_with_lease(std::string s){output=s;return runtime_output_override_lease_t{s};}
 bool clear_runtime_output_name_override_if_lease(const runtime_output_override_lease_t&){return true;}
}
namespace rtsp_stream {
''' + definition(get('src/rtsp.h'),'struct launch_session_t') + r''';
 bool has_pending_launch_or_startup(){return false;}
 bool effective_hdr_requested(const launch_session_t&s){return s.enable_hdr&&!s.prefer_sdr_10bit&&!s.force_sdr;}
}
namespace proc {
 struct ctx_t {
  std::string id="1", uuid="app",name="App",cmd,playnite_id,frame_generation_provider;
  bool virtual_screen=true,playnite_fullscreen=false,gen1_framegen_fix=false,gen2_framegen_fix=false,frame_generation_enabled=false,lossless_scaling_framegen=false;
  std::optional<int> lossless_scaling_target_fps,lossless_scaling_rtss_limit;
  std::optional<config::video_t::virtual_display_mode_e> virtual_display_mode_override;
  std::optional<config::video_t::virtual_display_layout_e> virtual_display_layout_override;
  std::optional<config::video_t::dd_t::config_option_e> dd_config_option_override;
  std::optional<std::string> output_name_override;
  std::unordered_map<std::string,std::string> config_overrides;
 };
 struct active_t {std::string client_uuid; std::uint64_t normal_vdd_identity_token=0;};
 struct Process {
  int app_id=0;active_t active;bool requests_virtual=true;
  bool running(){return app_id>0;}int current_app_id(){return app_id;}
  std::optional<ctx_t> resolve_app(int id){ctx_t c;c.id=std::to_string(id);c.virtual_screen=requests_virtual;return c;}
  active_t active_session_guard(){return active;}
  int execute(int id,std::shared_ptr<rtsp_stream::launch_session_t>s){app_id=id; active={s->client_uuid,s->normal_vdd_identity_token};return 0;}
 }proc;
}
namespace nvhttp {std::mutex gate;std::mutex& stream_lifecycle_mutex(){return gate;}std::unordered_map<std::string,std::string>get_client_config_overrides(const std::string&){return {};}}
namespace platf::display_power {std::shared_ptr<void> acquire(){return std::make_shared<int>(1);}}
namespace platf::linux_private_display {
 struct state_t {std::mutex mutex;std::optional<json> snapshot;std::map<std::string,std::string>reservations;std::set<std::string>newly_connected_reservations;std::atomic_uint64_t cleanup_generation{0};struct {void cancel(uint64_t){}}restore_dispatcher;} manager;
 state_t& state(){return manager;}
 json screen={{"outputs",json::array()}};std::vector<std::string>disconnected;std::vector<std::string>pool={"Virtual-1","Virtual-2","Virtual-3","Virtual-4"};
 std::optional<json>query_configuration(){return screen;}
 const json* find_output(const json&c,const std::string&name){for(auto&o:c["outputs"])if(o.value("name",std::string{})==name)return &o;return nullptr;}
 json& output(const std::string&name){for(auto&o:screen["outputs"])if(o.value("name",std::string{})==name)return o;throw 1;}
 bool connected(const json&o){return o.value("connected",false);}bool enabled(const json&o){return o.value("enabled",false);}
 bool is_managed_output(const std::string&){return true;}
 const auto& configured_outputs(){return pool;}
 bool snapshot_configuration_if_needed(state_t&m,const json&c){m.snapshot=c;return true;}
 bool connect_managed_output(const std::string&name){output(name)["connected"]=true;return true;}
 bool wait_for_output_publication(const std::string&){return true;}
 bool disconnect_managed_output(const std::string&name){disconnected.push_back(name);output(name)["connected"]=false;output(name)["enabled"]=false;return true;}
 bool process_shutdown_preserve_requested(){return false;}
 bool output_hdr_capable(const json*,const std::string&){return false;}
 void remember_scale(state_t&,const std::string&,const json*){}
 void cancel_scheduled_revert(){}void restore_after_failed_preparation(){}
 std::vector<std::string> capture_output_names(){std::vector<std::string>v;for(auto&o:screen["outputs"])if(connected(o)&&enabled(o))v.push_back(o["name"]);return v;}
 namespace restore_policy {enum class connection_result_e{failed,published};template<class A,class B,class C,class D>auto connect_with_snapshot_and_publication(A&,B,C connect,D publish){return connect()&&publish()?connection_result_e::published:connection_result_e::failed;}}
 struct prepare_result_t {bool requested=false,active=false;std::string output_name,error;};
''' + '\n'.join(definition(p,s) for s in [
 'std::string reservation_identity(', 'std::string client_reservation_identity(', 'std::optional<std::string> reserve_output(',
 'prepare_result_t prepare_session(', 'bool remote_create_or_reclaim(', 'bool remote_remove_owned_display(']) + r'''
 void reset(){manager.reservations.clear();manager.newly_connected_reservations.clear();screen["outputs"]=json::array();disconnected.clear();for(auto&n:pool)screen["outputs"].push_back({{"name",n},{"connected",false},{"enabled",false}});}
}
namespace platf::linux_display {
 struct result {std::string output_name;bool owns_output;std::string error;};
 struct Backend {
  result prepare_session(rtsp_stream::launch_session_t&s,bool idle,bool changes){auto r=linux_private_display::prepare_session(s,idle,changes);return {r.output_name,r.active,r.error};}
  bool apply_session(rtsp_stream::launch_session_t&s){linux_private_display::output(s.virtual_display_device_id)["enabled"]=true;return true;}
  bool revert(){return true;}
 };Backend&backend(){static Backend b;return b;}
}
namespace safe {struct mail_raw_t{};}
namespace video {struct config_t {int dynamicRange=0,framerate=60;bool prefer_sdr_10bit=false,force_sdr=false;}; bool probe_encoders(){return false;}void capture(auto,auto,auto){} }
namespace audio {struct config_t{};void capture(auto,auto,auto){} }
namespace stream::session {std::atomic_uint running_game_sessions{0};void arm_shared_runtime_cleanup(auto){}}
namespace webrtc_stream {
''' + definition(get('src/webrtc_stream.h'),'struct SessionOptions') + r''';
 int kDefaultWidth=1920,kDefaultHeight=1080,kDefaultFps=60,kDefaultAudioChannels=2;std::atomic_uint webrtc_launch_session_id{0};
 std::atomic_bool rtsp_sessions_active{false};std::atomic_uint active_sessions{0};
 struct Params{bool hdr=false,prefer_sdr_10bit=false,uses_virtual_display=false;};
 struct Capture {std::mutex mutex;std::condition_variable teardown_cv;std::atomic_bool teardown_in_progress{false},active{false},feedback_shutdown{false};std::atomic_uint pending_session_creations{0};std::optional<int>config_key,app_id,published_bitrate_kbps;std::optional<Params>stream_start_params;std::optional<config::runtime_output_override_lease_t>output_override_lease;std::shared_ptr<void>normal_display_capture;std::shared_ptr<safe::mail_raw_t>mail;std::shared_ptr<rtsp_stream::launch_session_t>launch_session;std::thread video_thread,audio_thread;}webrtc_capture;
 std::optional<int>snapshot_rtsp_capture_config(){return {};}
 Params compute_stream_start_params(const SessionOptions&,int){return {};}
 bool resolve_prefer_10bit_sdr(const SessionOptions&){return false;}
 video::config_t build_video_config(const SessionOptions&,bool){return {};}
 audio::config_t build_audio_config(const SessionOptions&){return {};}
 void apply_rtsp_video_overrides(auto&,const auto&){}void apply_rtx_hdr_stream_policy(auto&){}
 int build_capture_config_key(int,const video::config_t&,const SessionOptions&){return 1;}
 void acquire_webrtc_frame_limiter_locked(const Params&){}
 std::optional<std::string>validate_requested_video_capabilities(const SessionOptions&){return {};}
''' + definition(w,'bool has_active_or_pending_sessions()') + definition(w,'bool has_capture_active()') + definition(w,'std::shared_ptr<rtsp_stream::launch_session_t> build_launch_session(') + '\n' + definition(w,'std::optional<std::string> start_webrtc_capture(') + r'''
 void stop(){if(webrtc_capture.video_thread.joinable())webrtc_capture.video_thread.join();if(webrtc_capture.audio_thread.joinable())webrtc_capture.audio_thread.join();webrtc_capture.active=false;webrtc_capture.normal_display_capture.reset();webrtc_capture.pending_session_creations=0;}
}
namespace remote_session {
''' + definition(get('src/remote_session.cpp'),'dispatch_t dispatch(') + definition(get('src/remote_session.cpp'),'bool joins_existing_game_output(') + r'''
}
void callbacks(){
 using namespace platf::linux_private_display;
 remote_display_topology::instance().set_runtime_callbacks({
 .create_or_reclaim=[](const auto&id,const auto&,const auto&m){return remote_create_or_reclaim(id,m);},
 .apply_composed_topology=[](const auto&nodes){
   // Simulate only enabling desired outputs; intentionally do NOT disable
   // omitted outputs. Thus H1's disconnected capture cannot be caused by this mock.
   for(auto&n:nodes)output(manager.reservations.at("client:"+n.id))["enabled"]=true;
   return true;
 },
 .exact_target_has_current_mode_and_dxgi=[](const auto&id,const auto&)->std::optional<std::string>{return manager.reservations.at("client:"+id);},
 .remove_owned_display=remote_remove_owned_display,
 .client_identity_capacity=[](){return 4u;}
 });
}
void reset(){stream::session::running_game_sessions=0;webrtc_stream::stop();remote_display_topology::instance().shutdown(true);platf::linux_private_display::reset();proc::proc.app_id=0;proc::proc.active={};proc::proc.requests_virtual=true;config::video.virtual_display_mode=config::video_t::virtual_display_mode_e::per_client;config::video.dd={};callbacks();}

'''

program=program.replace('struct dd_t {enum class config_option_e {disabled};};', 'struct dd_t {enum class config_option_e {disabled};bool config_revert_on_disconnect=false;int paused_virtual_display_timeout_secs=0;std::chrono::milliseconds config_revert_delay{0};} dd;')
program=program.replace('namespace nvhttp {std::mutex', 'namespace nvhttp {'+definition(get('src/nvhttp.h'),'enum class linux_normal_identity_result_e')+';bool has_stream_session_activity(){return false;}void refresh_remote_monitor_baseline(bool){} std::mutex')
program=program.replace('namespace safe {', 'namespace nvhttp {'+definition(nv,'linux_normal_identity_result_e reserve_linux_normal_display_identity(')+definition(nv,'void rollback_linux_normal_display_identity(')+'} namespace safe {')
program=program.replace('void reset(){manager.reservations.clear();', 'bool publish_current_session_state(rtsp_stream::launch_session_t&s){return connected(output(s.virtual_display_device_id))&&enabled(output(s.virtual_display_device_id));} void reset(){manager.reservations.clear();')
program=program.replace('struct Backend {','int schedules=0,applies=0;bool fail_apply=false,throw_apply=false;struct Backend {void schedule_revert(auto,auto){++schedules;}')
program=program.replace('bool apply_session(rtsp_stream::launch_session_t&s){linux_private_display::output', 'bool apply_session(rtsp_stream::launch_session_t&s){++applies;if(throw_apply)throw std::runtime_error("test apply exception");if(fail_apply)return false;linux_private_display::output')
program=program.replace('bool probe_encoders(){return false;}', 'bool probe_failure=false; bool probe_encoders(){return probe_failure;}')
program=program.replace('std::optional<int>snapshot_rtsp_capture_config(){return {};}',
    definition(w,'struct RtspCaptureConfig') + ';' + definition(w,'struct RtspCaptureOwner') +
    ';std::mutex rtsp_config_mutex;std::optional<RtspCaptureConfig>rtsp_capture_config;std::optional<RtspCaptureOwner>rtsp_capture_owner;' +
    ''.join(definition(w, signature) for signature in (
        'std::optional<RtspCaptureConfig> snapshot_rtsp_capture_config()',
        'std::optional<RtspCaptureOwner> snapshot_rtsp_capture_owner()',
        'void clear_rtsp_capture_config()', 'void set_rtsp_capture_config(',
        'void set_rtsp_capture_owner(', 'void clear_rtsp_capture_source()', 'void set_rtsp_sessions_active(',
    )))
program=program.replace('void apply_rtsp_video_overrides(auto&,const auto&){}', definition(w,'void apply_rtsp_video_overrides('))
program=program.replace('void reset(){stream::session::running_game_sessions=0;webrtc_stream::stop();', 'void reset(){stream::session::running_game_sessions=0;webrtc_stream::stop();webrtc_stream::set_rtsp_sessions_active(false);platf::linux_display::schedules=0;platf::linux_display::applies=0;platf::linux_display::fail_apply=false;platf::linux_display::throw_apply=false;video::probe_failure=false;')
program += r'''
struct RtspCapture {
  std::string device_uuid = "requester", normal_display_owner;
  std::uint64_t normal_display_token {};
  std::shared_ptr<void> normal_display_capture;
};
std::shared_ptr<RtspCapture> retain_rtsp_capture(const rtsp_stream::launch_session_t &launch_session) {
  auto session = std::make_shared<RtspCapture>();
''' + definition(get("src/stream.cpp"), "if (launch_session.role == remote_session::role_e::game)") + r'''
  return session;
}
'''
join_start = nv.index("    const bool joining_existing_game_output =")
join_end = nv.index("\n      );", join_start) + len("\n      );")
owner_start = nv.index("    const auto display_owner = proc::proc.active_session_guard();")
owner_end = nv.index("#endif", owner_start)
program += r'''
bool rtsp_joins_existing(const bool no_active_sessions, const bool retained_game_output_ready) {
  const auto active_game = proc::proc.active_session_guard();
  const bool game_capture_active = stream::session::running_game_sessions.load() != 0 || webrtc_stream::has_capture_active();
''' + nv[join_start:join_end] + r'''
  return joining_existing_game_output;
}
void stage_rtsp_resume(const std::shared_ptr<rtsp_stream::launch_session_t> &launch_session, bool joining_existing_game_output) {
''' + nv[owner_start:owner_end] + definition(nv, "if (joining_existing_game_output)") + r'''
}
void secondary_rtsp_ownership() {
  using namespace platf::linux_private_display;
  auto &coord = remote_display_topology::instance();
  ::reset();
  auto primary = std::make_shared<rtsp_stream::launch_session_t>();
  primary->client_uuid="A";primary->client_name="A";primary->virtual_display=true;
  auto prepared=prepare_session(*primary,true,true);assert(prepared.active);output(prepared.output_name)["enabled"]=true;
  assert(nvhttp::reserve_linux_normal_display_identity(primary)==nvhttp::linux_normal_identity_result_e::ready);
  const auto token=primary->normal_vdd_identity_token;
  proc::proc.app_id=1;proc::proc.active={"A",token};
  auto first=coord.retain_normal_game_capture("A",token);assert(first);
  auto second=std::make_shared<rtsp_stream::launch_session_t>();second->client_uuid="B";second->virtual_display=true;
  stream::session::running_game_sessions=1;assert(rtsp_joins_existing(false,false));
  stage_rtsp_resume(second,true);
  assert(!second->virtual_display && second->normal_vdd_identity_token==token);
  auto capture=retain_rtsp_capture(*second);assert(capture->normal_display_capture);
  assert(capture->normal_display_owner=="A" && capture->normal_display_token==token);
  first.reset();coord.release_normal_game_identity("A",token);
  assert(manager.reservations.contains("client:A"));
  capture.reset();coord.release_drained_normal_game_identities();assert(!manager.reservations.contains("client:A"));
  ::reset();
  prepared=prepare_session(*primary,true,true);assert(prepared.active);output(prepared.output_name)["enabled"]=true;
  primary->normal_vdd_identity_token=0;assert(nvhttp::reserve_linux_normal_display_identity(primary)==nvhttp::linux_normal_identity_result_e::ready);
  proc::proc.app_id=1;proc::proc.active={"A",primary->normal_vdd_identity_token};
  coord.complete_restored_normal_game_cleanup(true);manager.reservations.clear();output(prepared.output_name)["enabled"]=false;output(prepared.output_name)["connected"]=false;
  second=std::make_shared<rtsp_stream::launch_session_t>();second->client_uuid="B";second->virtual_display=true;
  assert(!rtsp_joins_existing(true,false));
  assert(coord.activate_or_resume("C","Monitor",{},7).ready);
  webrtc_stream::set_rtsp_sessions_active(true); // Monitor-only activity is not a game source.
  assert(stream::session::running_game_sessions==0);
  assert(!rtsp_joins_existing(false,false));stage_rtsp_resume(second,false);
  const auto recreated=prepare_session(*second,true,false);assert(recreated.active);
  assert(nvhttp::reserve_linux_normal_display_identity(second)==nvhttp::linux_normal_identity_result_e::ready_composed);
  assert(second->normal_vdd_owner_uuid=="A" && second->normal_vdd_identity_token==primary->normal_vdd_identity_token);
  capture=retain_rtsp_capture(*second);assert(capture->normal_display_capture);capture.reset();
  assert(manager.reservations.contains("client:A") && manager.reservations.contains("client:C"));
  // Browser Resume must also prepare A while C is the only RTSP transport.
  webrtc_stream::SessionOptions browser;browser.resume=true;
  assert(!webrtc_stream::start_webrtc_capture(browser));
  assert(webrtc_stream::webrtc_capture.launch_session->normal_vdd_identity_token==primary->normal_vdd_identity_token);
  assert(webrtc_stream::webrtc_capture.normal_display_capture);
  assert(manager.reservations.contains("client:C"));
  ::reset();
  std::cout<<"Secondary RTSP holds the exact live output; idle cross-client Resume recreates the original suspended owner.\n";
}
'''
stream_source = get("src/stream.cpp")
publish_start = stream_source.index("      const bool first_game_session =")
publish_end = stream_source.index("#endif", publish_start)
program += r'''
struct RtspPublication {
  remote_session::role_e remote_role = remote_session::role_e::game;
  struct { video::config_t monitor; audio::config_t audio; } config;
  std::string normal_display_owner;
  std::uint64_t normal_display_token {};
};
void publish_first_game(RtspPublication &session) {
  using namespace stream::session;
''' + stream_source[publish_start:publish_end] + r'''
}
void finish_game_publication(RtspPublication &session) {
  using namespace stream::session;
''' + definition(stream_source, "if (session.remote_role == remote_session::role_e::game && --running_game_sessions == 0)") + r'''
}
void browser_first_handoff() {
  using namespace webrtc_stream;
  using namespace platf::linux_private_display;
  auto &coord = remote_display_topology::instance();
  for (bool prior_game : {false, true}) {
    ::reset();
    assert(coord.activate_or_resume("monitor", "Retained Monitor", {}, 3).ready);
    set_rtsp_sessions_active(true);
    if (prior_game) {
      RtspPublication old;
      old.config.monitor.framerate=24; old.normal_display_owner="old-game"; old.normal_display_token=12;
      publish_first_game(old);
      assert(snapshot_rtsp_capture_config()->video.framerate==24);
      finish_game_publication(old);
      assert(!snapshot_rtsp_capture_config() && !snapshot_rtsp_capture_owner());
    }
    SessionOptions browser;browser.app_id=1;browser.client_uuid="A";browser.fps=60;
    // Starting a new app while RTSP is active retains its existing restriction;
    // here the test browser starts first, before the monitor transport returns.
    set_rtsp_sessions_active(false);
    assert(!start_webrtc_capture(browser));
    const auto token=webrtc_capture.launch_session->normal_vdd_identity_token;
    assert(token && webrtc_capture.normal_display_capture);
    auto secondary=std::make_shared<rtsp_stream::launch_session_t>();secondary->client_uuid="B";
    stage_rtsp_resume(secondary,true);
    auto rtsp_capture=retain_rtsp_capture(*secondary);assert(rtsp_capture->normal_display_capture);
    RtspPublication joined;
    joined.config.monitor.framerate=30;
    joined.normal_display_owner=rtsp_capture->normal_display_owner;
    joined.normal_display_token=rtsp_capture->normal_display_token;
    publish_first_game(joined);set_rtsp_sessions_active(true);
    assert(!snapshot_rtsp_capture_config());
    assert(snapshot_rtsp_capture_owner());
    assert(snapshot_rtsp_capture_owner()->normal_display_owner=="A");
    assert(snapshot_rtsp_capture_owner()->normal_display_token==token);
    video::config_t active_browser;active_browser.framerate=60;
    apply_rtsp_video_overrides(active_browser,snapshot_rtsp_capture_config());
    assert(active_browser.framerate==60); // RTSP metadata must not change active browser FPS.
    stop();
    assert(!start_webrtc_capture(browser));
    assert(webrtc_capture.normal_display_capture);
    coord.release_normal_game_identity("A",token);
    rtsp_capture.reset();finish_game_publication(joined);
    coord.release_drained_normal_game_identities();
    assert(manager.reservations.contains("client:A"));
    stop();coord.release_drained_normal_game_identities();
    assert(!manager.reservations.contains("client:A"));
    assert(manager.reservations.contains("client:monitor"));
  }
  ::reset();
  std::cout<<"Browser-first RTSP handoff republishes exact ownership without replacing browser FPS, including prior-game turnover.\n";
}
'''
program += r'''
void cache_turnover(){
 using namespace webrtc_stream;using namespace platf::linux_private_display;
 auto&coord=remote_display_topology::instance();
 auto seed=[](){
  auto s=std::make_shared<rtsp_stream::launch_session_t>();s->client_uuid="A";s->client_name="A";s->virtual_display=true;s->width=1920;s->height=1080;s->fps=60;
  auto prepared=prepare_session(*s,true,true);assert(prepared.active);output(prepared.output_name)["enabled"]=true;
  assert(nvhttp::reserve_linux_normal_display_identity(s)==nvhttp::linux_normal_identity_result_e::ready);
  return s;
 };
 ::reset();auto owner=seed();
 auto r1=coord.retain_normal_game_capture("A",owner->normal_vdd_identity_token);assert(r1);
 set_rtsp_capture_config({}, {}, "A", owner->normal_vdd_identity_token);set_rtsp_sessions_active(true);stream::session::running_game_sessions=1;
 assert(coord.activate_or_resume("C","Unrelated Monitor",{},9).ready);
 coord.release_normal_game_identity("A",owner->normal_vdd_identity_token);r1.reset();coord.release_drained_normal_game_identities();
 assert(!manager.reservations.contains("client:A"));assert(manager.reservations.contains("client:C"));
 stream::session::running_game_sessions=0;
 assert(!start_webrtc_capture({}));assert(webrtc_capture.normal_display_capture);
 assert(!manager.reservations.contains("client:A"));assert(manager.reservations.contains("client:C"));
 std::cout<<"PASS: cached metadata does not retain departed A; Monitor-only activity gets independent owned browser capture.\n";
 ::reset();owner=seed();
 r1=coord.retain_normal_game_capture("A",owner->normal_vdd_identity_token);auto r2=coord.retain_normal_game_capture("A",owner->normal_vdd_identity_token);assert(r1&&r2&&r1!=r2);
 proc::proc.app_id=1;proc::proc.active={"A",owner->normal_vdd_identity_token};
 set_rtsp_capture_config({}, {}, "A", owner->normal_vdd_identity_token);set_rtsp_sessions_active(true);stream::session::running_game_sessions=1;
 r1.reset();coord.release_drained_normal_game_identities();assert(manager.reservations.contains("client:A"));assert(coord.has_idle_display_cleanup_owner());
 auto error=start_webrtc_capture({});
 assert(!error);assert(webrtc_capture.normal_display_capture);std::cout<<"PASS: same-owner RTSP R2 keeps capture joinable after original R1 ends.\n";
 ::reset();
}

int main(){
 using namespace webrtc_stream;using namespace platf::linux_private_display;
 browser_first_handoff();
 secondary_rtsp_ownership();
 cache_turnover();
 auto&coord=remote_display_topology::instance();::reset();
 assert(coord.activate_or_resume("B","Monitor B",{2560,1440,120},1).ready);assert(coord.activate_or_resume("C","Monitor C",{},2).ready);
 coord.transport_lost("B",1);coord.transport_lost("C",2);
 set_rtsp_sessions_active(true); // Only independent Monitor transports are active.
 assert(stream::session::running_game_sessions==0);
 SessionOptions fresh;fresh.client_uuid="B";
 assert(!start_webrtc_capture(fresh));
 auto target=webrtc_capture.launch_session->virtual_display_device_id;
 assert(target==manager.reservations.at("client:B"));assert(config::output==target);
 assert(webrtc_capture.launch_session->normal_vdd_identity_token!=0);assert(webrtc_capture.normal_display_capture);
 assert(platf::linux_display::applies==0); // composed Monitor mode wins over different default WebRTC mode
 assert(coord.explicit_release("B",1,"Disconnect Monitor"));
 assert(webrtc_capture.active);assert(connected(output(target)));assert(manager.reservations.contains("client:B"));assert(manager.reservations.contains("client:C"));
 coord.release_normal_game_identity("B",webrtc_capture.launch_session->normal_vdd_identity_token);
 assert(connected(output(target)));assert(manager.reservations.contains("client:B"));
 stop();coord.release_drained_normal_game_identities();
 assert(!connected(output(target)));assert(!manager.reservations.contains("client:B"));
 std::cout<<"H1 fixed: fresh desktop token, capture survives Monitor release/app-end, B retires after capture ends with C alive; composed mode not reapplied.\n";
 ::reset();
 // Paused app exists with correct reservation established via exported production helper.
 auto rtsp=std::make_shared<rtsp_stream::launch_session_t>();rtsp->id=100;rtsp->client_uuid="A";rtsp->client_name="A";rtsp->width=1920;rtsp->height=1080;rtsp->fps=60;rtsp->virtual_display=true;
 auto original=prepare_session(*rtsp,true,true);assert(original.active);output(original.output_name)["enabled"]=true;manager.newly_connected_reservations.clear();
 assert(nvhttp::reserve_linux_normal_display_identity(rtsp)==nvhttp::linux_normal_identity_result_e::ready);
 auto token=rtsp->normal_vdd_identity_token;assert(token);proc::proc.app_id=1;proc::proc.active={"A",token};
 SessionOptions resume;resume.resume=true;
 assert(!start_webrtc_capture(resume));
 auto resumed=webrtc_capture.launch_session;
 assert(config::output==original.output_name);assert(resumed->client_uuid!="A");assert(resumed->normal_vdd_owner_uuid=="A");assert(resumed->virtual_display_device_id==original.output_name);
 assert(resumed->normal_vdd_identity_token==token);assert(!resumed->normal_vdd_identity_newly_reserved);assert(webrtc_capture.normal_display_capture);
 assert(manager.reservations.size()==1);assert(coord.managed_client_identity_ids()==std::vector<std::string>{"A"});stop();
 resume.client_uuid="different-paired";assert(!start_webrtc_capture(resume));assert(webrtc_capture.launch_session->virtual_display_device_id==original.output_name);assert(manager.reservations.size()==1);stop();
 std::cout<<"H2 fixed: omitted/different UUID resumes reuse A and exact token.\n";
 // Resume encoder failure preserves A/token without generic restoration (paused preference).
 video::probe_failure=true;assert(start_webrtc_capture(resume));assert(manager.reservations.contains("client:A"));assert(coord.managed_client_identity_ids()==std::vector<std::string>{"A"});assert(platf::linux_display::schedules==0);video::probe_failure=false;
 std::cout<<"Failed paused resume preserves retained A and does not force restoration.\n";
 // A verified timeout leaves A's logical token for future private resumes.
 // Choosing physical/shared output must not try to capture that removed output.
 coord.complete_restored_normal_game_cleanup(true);manager.reservations.clear();
 output(original.output_name)["connected"]=false;output(original.output_name)["enabled"]=false;
 proc::proc.requests_virtual=false;config::video.virtual_display_mode=config::video_t::virtual_display_mode_e::disabled;
 assert(!start_webrtc_capture(resume));assert(!webrtc_capture.normal_display_capture);assert(!webrtc_capture.launch_session->virtual_display);
 assert(coord.managed_client_identity_count()==1);
 // An actual active physical browser source is joinable without A's dormant token.
 auto physical_join=std::make_shared<rtsp_stream::launch_session_t>();physical_join->client_uuid="secondary";physical_join->virtual_display=true;
 assert(rtsp_joins_existing(false,false));stage_rtsp_resume(physical_join,true);
 assert(physical_join->normal_vdd_identity_token==0);
 assert(!retain_rtsp_capture(*physical_join)->normal_display_capture);
 stop();
 rtsp_stream::launch_session_t physical{};physical.role=remote_session::role_e::game;
 assert(!retain_rtsp_capture(physical)->normal_display_capture);
 physical.virtual_display=true;physical.virtual_display_mode_override=config::video_t::virtual_display_mode_e::shared;
 assert(!retain_rtsp_capture(physical)->normal_display_capture);
 std::cout<<"Physical/shared Resume ignores the suspended private-output token in WebRTC and RTSP capture ownership.\n";
 ::reset();
 // Fresh startup failures roll back token; no capture owns it.
 for(int kind=0;kind<3;++kind){
  ::reset();SessionOptions options;options.client_uuid="failed";
  platf::linux_display::fail_apply=kind==0;video::probe_failure=kind==1;platf::linux_display::throw_apply=kind==2;
  bool rejected=false;try{rejected=start_webrtc_capture(options).has_value();}catch(const std::runtime_error&){rejected=true;}
  assert(rejected);assert(!webrtc_capture.active);assert(coord.managed_client_identity_count()==0);assert(!manager.reservations.contains("client:failed"));assert(platf::linux_display::schedules==1);
 }
 std::cout<<"Fresh apply/encoder/exception failures roll back token and schedule guarded restoration once.\n";
 ::reset();
 // Actual shared capture publication APIs with appid=0 and exact RTSP lease.
 auto owner=std::make_shared<rtsp_stream::launch_session_t>();owner->client_uuid="D";owner->client_name="Desktop";owner->virtual_display=true;owner->width=1920;owner->height=1080;owner->fps=60;
 auto desktop=prepare_session(*owner,true,true);assert(desktop.active);output(desktop.output_name)["enabled"]=true;
 assert(nvhttp::reserve_linux_normal_display_identity(owner)==nvhttp::linux_normal_identity_result_e::ready);
 auto rtsp_capture=coord.retain_normal_game_capture("D",owner->normal_vdd_identity_token);assert(rtsp_capture);
 set_rtsp_capture_config({}, {}, "D", owner->normal_vdd_identity_token);set_rtsp_sessions_active(true);stream::session::running_game_sessions=1;
 assert(!start_webrtc_capture({}));assert(webrtc_capture.normal_display_capture!=nullptr);assert(manager.reservations.size()==1);
 coord.release_normal_game_identity("D",owner->normal_vdd_identity_token);rtsp_capture.reset();set_rtsp_sessions_active(false);coord.release_drained_normal_game_identities();
 assert(coord.managed_client_identity_count()==1);assert(connected(output(desktop.output_name)));
 stop();coord.release_drained_normal_game_identities();assert(coord.managed_client_identity_count()==0);
 std::cout<<"RTSP appid0 exact lease survives RTSP drain through WebRTC; last capture releases it.\n";
 ::reset();
}
'''

with tempfile.TemporaryDirectory(prefix="linux-webrtc-display-ownership-") as directory:
    source = pathlib.Path(directory) / "test.cpp"
    binary = pathlib.Path(directory) / "test"
    source.write_text(program)
    command = [compiler, "-std=c++20", "-pthread", "-I", str(repo)]
    for include in sys.argv[3:]:
        command.extend(["-I", include])
    command.extend([str(source), str(repo / "src/remote_display_topology.cpp"), "-o", str(binary)])
    subprocess.run(command, check=True)
    subprocess.run([str(binary)], check=True, timeout=40)

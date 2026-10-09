"""Exercise production PipeWire format publication/frame acceptance and KMS HDR checks.

The fixtures replace compositor/DRM I/O, never configure a real display. PipeWire
format events deliberately omit PAUSED: format and stream state are independent
callbacks in upstream stream.c::impl_port_set_param/impl_send_command.
"""
import hashlib
import pathlib
import subprocess
import sys
import tempfile

root = pathlib.Path(sys.argv[1]).resolve()
compiler = sys.argv[2]
source_ref = sys.argv[3] if len(sys.argv) > 3 else None


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


pipewire = source("src/platform/linux/pipewire.cpp")
shared_state = function(pipewire, "struct shared_state_t") + ";"
# Retain parsing and the complete production publication block. The remainder
# only acknowledges buffer types and metadata to PipeWire.
publish = function(pipewire, "static void on_param_changed(")
publish = publish[:publish.index("      uint64_t drm_format = 0;")] + "}\n"
snapshot = function(pipewire, "platf::capture_e snapshot(")
is_hdr = "virtual " + function(pipewire, "bool is_hdr() override").replace(" override", "")
capture_valid = function(pipewire, "virtual bool capture_format_valid()")
negotiation = pipewire[pipewire.index("      // Wait for pipewire negotiation to finish"):pipewire.index("    platf::capture_e snapshot(")]
gamescope = source("src/platform/linux/gamescopegrab.cpp")
gamescope_is_hdr = function(gamescope, "bool is_hdr() override")
gamescope_valid = function(gamescope, "bool capture_format_valid() override")

program = r'''
#include <atomic>
#include <cassert>
#include <chrono>
#include <functional>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include "src/platform/linux/pipewire_capture_format.h"
#include "src/platform/linux/gamescope_hdr_policy.h"
using namespace std::literals;
struct quiet_log { template<class T> quiet_log &operator<<(const T &) { return *this; } };
#define BOOST_LOG(level) quiet_log{}
constexpr int SPA_PARAM_Format = 1, SPA_MEDIA_TYPE_video = 2, SPA_MEDIA_SUBTYPE_raw = 3;
constexpr int SPA_VIDEO_COLOR_PRIMARIES_BT2020 = 9, SPA_VIDEO_TRANSFER_SMPTE2084 = 14;
constexpr int SPA_VIDEO_FORMAT_xBGR_210LE = 2, SPA_VIDEO_COLOR_RANGE_0_255 = 1, SPA_VIDEO_COLOR_MATRIX_RGB = 1;
enum pw_stream_state { PW_STREAM_STATE_UNCONNECTED };
struct raw_format {
  int format {}, color_primaries {}, transfer_function {}, color_range {}, color_matrix {};
  struct { int width {}, height {}; } size;
  struct { int num {}, denom {1}; } framerate, max_framerate;
};
struct spa_pod { raw_format raw; };
int spa_format_parse(const spa_pod *, int *type, int *subtype) { *type=SPA_MEDIA_TYPE_video; *subtype=SPA_MEDIA_SUBTYPE_raw; return 0; }
int spa_format_video_raw_parse(const spa_pod *pod, raw_format *raw) { *raw = pod->raw; return 0; }
namespace pipewire {
''' + shared_state + r'''
struct stream_data_t {
  void *current_buffer {};
  struct { int media_type {}, media_subtype {}; struct { raw_format raw; } info; } format;
  std::shared_ptr<shared_state_t> shared = std::make_shared<shared_state_t>();
};
''' + publish + r'''
}
namespace platf {
  enum class capture_e { ok, reinit, timeout, interrupted };
  struct img_t { virtual ~img_t() = default; };
}
namespace egl {
  struct img_descriptor_t: platf::img_t {
    char *data {};
    std::optional<int> pts, seq, pw_flags;
    struct { int fds[4] {-1,-1,-1,-1}; } sd;
    void reset() {}
  };
}
using pull_free_image_cb_t = std::function<bool(std::shared_ptr<platf::img_t> &)>;
using pipewire::capture_format_snapshot_t;
struct display_t {
  std::shared_ptr<pipewire::shared_state_t> shared_state;
  pipewire::capture_format_snapshot_t capture_format_;
  int width {1920}, height {1080};
  int env_width {}, env_height {}, logical_height {}, logical_width {}, env_logical_height {}, env_logical_width {};
  void verify_and_update_display_parameters() {}
  bool negotiated_size_ready(int, int) { return true; }
  auto negotiated_size_settle_time() { return 0ms; }
  struct { std::function<void()> hook; void fill_img(egl::img_descriptor_t *img) { if (hook) hook(); img->sd.fds[0] = 7; } } pipewire;
  bool wait_for_frame(auto) { return true; }
''' + capture_valid + r'''
  bool is_buffer_redundant(auto) { return false; }
  void update_metadata(auto, int) {}
  int initialize_current_format() {
''' + negotiation + snapshot + is_hdr + r'''
};
struct gamescope_display_t: display_t {
  bool hdr_requested_ {true};
  struct node_t { bool capable {true}; bool hdr_capable() { return capable; } };
  std::optional<node_t> node_ {node_t{}};
''' + gamescope_is_hdr + gamescope_valid + r'''
};
pipewire::capture_format_t as_capture_format(const raw_format &f) {
  return {f.size.width, f.size.height, f.format, f.color_primaries, f.transfer_function, f.color_range, f.color_matrix};
}
void publish(pipewire::stream_data_t &data, const raw_format &format) {
  const spa_pod pod {format};
  pipewire::on_param_changed(&data, SPA_PARAM_Format, &pod);
}
int main() {
  const raw_format sdr {1, 1, 1, 1, 1, {1920,1080}};
  auto hdr = sdr; hdr.format = 2; hdr.color_primaries = 9; hdr.transfer_function = 14;
  unsigned scenarios = 0;
  for (const auto &initial : {sdr, hdr}) {
    for (unsigned change = 0; change != 10; ++change) {
      pipewire::stream_data_t data;
      publish(data, initial);
      display_t display;
      display.shared_state = data.shared;
      assert(display.initialize_current_format() == 0);
      std::shared_ptr<platf::img_t> image;
      const pull_free_image_cb_t allocate = [](auto &img) { img = std::make_shared<egl::img_descriptor_t>(); return true; };
      assert(display.snapshot(allocate, image, 10ms, false) == platf::capture_e::ok);
      auto updated = initial;
      switch (change) {
        case 0: break; // Duplicate event is harmless.
        case 1: updated = initial.color_primaries == 9 ? sdr : hdr; break;
        case 2: ++updated.format; break;
        case 3: ++updated.color_primaries; break;
        case 4: ++updated.transfer_function; break;
        case 5: ++updated.color_range; break;
        case 6: ++updated.color_matrix; break;
        case 7: updated.size = {2560,1440}; break;
        case 8: updated.framerate.num = 120; break; // Pacing is not encoder format.
        case 9: ++updated.format; break; // A->B->A still invalidates queued buffers.
      }
      publish(data, updated);
      if (change == 9) publish(data, initial);
      assert(!data.shared->stream_dead.load()); // No state_changed event in this fixture.
      const bool changed = change != 0 && change != 8;
      const auto status = display.snapshot(allocate, image, 10ms, false);
      assert(status == (changed ? platf::capture_e::reinit : platf::capture_e::ok));
      // Encoder metadata stays tied to the old frame generation until reinit.
      assert(display.is_hdr() == (initial.color_primaries == 9));
      ++scenarios;
    }
  }
  // A format callback occurring inside fill_img must reject that first frame.
  pipewire::stream_data_t data;
  publish(data, sdr);
  display_t display;
  display.shared_state = data.shared;
  assert(display.initialize_current_format() == 0);
  display.pipewire.hook = [&] { publish(data, hdr); };
  std::shared_ptr<platf::img_t> image;
  const pull_free_image_cb_t allocate = [](auto &img) { img = std::make_shared<egl::img_descriptor_t>(); return true; };
  assert(display.snapshot(allocate, image, 10ms, false) == platf::capture_e::reinit);
  display.pipewire.hook = {};
  assert(display.initialize_current_format() == 0); // New encoder adopts the negotiated format.
  assert(display.is_hdr());
  assert(display.snapshot(allocate, image, 10ms, false) == platf::capture_e::ok);
  // Startup takes a coherent new size and HDR profile together.
  auto resized_hdr = hdr; resized_hdr.size = {2560,1440};
  publish(data, resized_hdr);
  assert(display.initialize_current_format() == 0);
  assert(display.width == 2560 && display.height == 1440 && display.is_hdr());
  assert(display.snapshot(allocate, image, 10ms, false) == platf::capture_e::ok);

  // Gamescope's profile proof and metadata use the exact encoder generation.
  gamescope_display_t gamescope;
  gamescope.shared_state = data.shared;
  assert(gamescope.initialize_current_format() == 0);
  assert(gamescope.is_hdr() && gamescope.capture_format_valid());
  assert(gamescope.snapshot(allocate, image, 10ms, false) == platf::capture_e::ok);
  // The inherited snapshot must dispatch Gamescope's stricter profile check
  // even when neither dimensions nor negotiated-format generation changed.
  gamescope.node_->capable = false;
  assert(gamescope.snapshot(allocate, image, 10ms, false) == platf::capture_e::reinit);
  gamescope.node_->capable = true;
  assert(gamescope.snapshot(allocate, image, 10ms, false) == platf::capture_e::ok);
  publish(data, sdr);
  assert(gamescope.is_hdr()); // Pending negotiation cannot relabel an old frame.
  assert(gamescope.snapshot(allocate, image, 10ms, false) == platf::capture_e::reinit);
  assert(gamescope.initialize_current_format() == 0);
  assert(!gamescope.is_hdr() && !gamescope.capture_format_valid());
  assert(gamescope.snapshot(allocate, image, 10ms, false) == platf::capture_e::reinit);
  gamescope.hdr_requested_ = false;
  assert(gamescope.capture_format_valid());
  assert(gamescope.snapshot(allocate, image, 10ms, false) == platf::capture_e::ok);
  std::cout << scenarios + 9 << " PipeWire/Gamescope production format/frame scenarios passed\n";
}
'''

kms = source("src/platform/linux/kmsgrab.cpp")
refresh = function(kms, "inline capture_e refresh(")
hdr_check = refresh[refresh.index("        // Check for a change in HDR metadata"):refresh.index("        if (pending_exported_frame)")]
kms_program = r'''
#include <cassert>
#include <cstdint>
#include <iostream>
#include <optional>
#include <string_view>
using namespace std::literals;
struct quiet_log { template<class T> quiet_log &operator<<(const T &) { return *this; } };
#define BOOST_LOG(level) quiet_log{}
enum class capture_e { ok, reinit };
struct display_t {
  std::optional<uint32_t> connector_id {42};
  std::optional<uint64_t> hdr_metadata_blob_id;
  struct {
    std::optional<uint64_t> current;
    int queries {};
    auto connector_props(uint32_t id) { assert(id == 42); ++queries; return current; }
    auto prop_value_by_name(auto props, std::string_view name) { assert(name == "HDR_OUTPUT_METADATA"); return props; }
  } card;
  capture_e refresh() {
''' + hdr_check + r'''
    return capture_e::ok;
  }
};
int main() {
  unsigned scenarios = 0;
  for (const auto before : {std::optional<uint64_t>{}, std::optional<uint64_t>{0}, std::optional<uint64_t>{77}}) {
    for (const auto after : {std::optional<uint64_t>{}, std::optional<uint64_t>{0}, std::optional<uint64_t>{77}, std::optional<uint64_t>{88}}) {
      display_t display;
      display.hdr_metadata_blob_id = before;
      display.card.current = after;
      assert(display.refresh() == (before == after ? capture_e::ok : capture_e::reinit));
      assert(display.card.queries == 1);
      ++scenarios;
    }
  }
  std::cout << scenarios << " KMS production HDR identity scenarios passed\n";
}
'''

with tempfile.TemporaryDirectory(prefix="vibeshine-capture-format-") as directory:
    for name, fixture in (("pipewire", program), ("kms", kms_program)):
        cpp = pathlib.Path(directory) / f"{name}.cpp"
        binary = pathlib.Path(directory) / name
        cpp.write_text(fixture)
        subprocess.run([compiler, "-std=c++20", "-pthread", "-I", str(root), str(cpp), "-o", str(binary)], check=True)
        subprocess.run([str(binary)], check=True)
print("Source SHA256", hashlib.sha256(pipewire.encode()).hexdigest(), hashlib.sha256(kms.encode()).hexdigest())

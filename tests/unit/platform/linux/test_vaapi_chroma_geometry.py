"""Exercise VAAPI destination import and rendering with inert VA/GL edges."""
import pathlib
import subprocess
import sys
import tempfile

root = pathlib.Path(sys.argv[1]).resolve()
compiler = sys.argv[2] if len(sys.argv) > 2 else "c++"


def body(path, signature):
    source = (root / path).read_text()
    start = source.index("{", source.index(signature))
    end = start
    depth = 0
    for end in range(start, len(source)):
        depth += int(source[end] == "{") - int(source[end] == "}")
        if depth == 0:
            return source[start:end + 1]
    raise ValueError(f"Unterminated C++ body: {signature}")


fixture = r'''
#include "src/platform/linux/dmabuf_surface.h"

#include <algorithm>
#include <array>
#include <cassert>
#include <cmath>
#include <memory>
#include <utility>

enum AVPixelFormat { AV_PIX_FMT_NV12, AV_PIX_FMT_P010 };
struct AVFrame { int width, height; void *buf[1] {}; void *data[4] {}; };
struct AVHWFramesContext { AVPixelFormat sw_format; };
struct AVBufferRef { void *data; };
int av_hwframe_get_buffer(AVBufferRef *, AVFrame *, int) { return 0; }

namespace va {
using VASurfaceID = unsigned;
constexpr int SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2 = 1;
constexpr int EXPORT_SURFACE_WRITE_ONLY = 2;
constexpr int EXPORT_SURFACE_SEPARATE_LAYERS = 4;
struct DRMPRIMESurfaceDescriptor PRIME_DESCRIPTOR;
}

va::DRMPRIMESurfaceDescriptor exported;
int vaExportSurfaceHandle(int, va::VASurfaceID, int memory, int flags, va::DRMPRIMESurfaceDescriptor *prime) {
  assert(memory == 1 && flags == 6);
  *prime = exported;
  return 0;
}

const char *vaErrorStr(int) { return "test VA error"; }

using GLenum = unsigned;
using GLint = int;
constexpr GLenum GL_TEXTURE0 = 0, GL_TEXTURE_2D = 1, GL_FRAMEBUFFER = 2;
constexpr GLenum GL_COLOR_ATTACHMENT0 = 3, GL_COLOR_ATTACHMENT1 = 4;
constexpr GLenum GL_FRAMEBUFFER_COMPLETE = 5, GL_TRIANGLES = 6;

struct NullLog {
  template<class T> NullLog &operator<<(const T &) { return *this; }
};
#define BOOST_LOG(level) NullLog()
using namespace std::literals;
namespace util {
struct Hex {
  std::string_view to_string_view() const { return "0"; }
};
Hex hex(unsigned) { return {}; }
}

namespace gl {
using frame_buf_t = std::array<int, 2>;
struct Rect {
  int x, y, width, height;
  bool operator==(const Rect &) const = default;
};
struct Context {
  std::array<Rect, 2> viewport {};
  int pass = 0;
  int draws = 0;
  std::array<float, 2> delta {};
  void ActiveTexture(GLenum) {}
  void BindTexture(GLenum, int) {}
  void BindFramebuffer(GLenum, int) {}
  void DrawBuffers(int, const GLenum *) {}
  GLenum CheckFramebufferStatus(GLenum) { return GL_FRAMEBUFFER_COMPLETE; }
  void UseProgram(int index) { pass = index; }
  void Uniform2f(GLint, float x, float y) {
    assert(pass == 1);
    delta = {x, y};
  }
  void Viewport(int x, int y, int width, int height) { viewport[pass] = {x, y, width, height}; }
  void DrawArrays(GLenum, int, int) { ++draws; }
  void Flush() {}
} ctx;
struct Program {
  int index;
  int handle() const { return index; }
};
}

using file_t = int;
namespace platf {
struct img_t {};
}
namespace egl {
struct img_descriptor_t: platf::img_t {
  surface_descriptor_t sd;
  int crtc_gamma_lut = 0;
  int crtc_gamma_lut_serial = 0;
  std::uint64_t sequence = 0;
};

struct nv12_img_t {
  static constexpr std::size_t num_fds = 4;
  gl::frame_buf_t buf {};
};
using nv12_t = std::shared_ptr<nv12_img_t>;
std::array<surface_descriptor_t, 2> target;
std::optional<nv12_t> import_target(int, std::array<file_t, 4> &&fds,
                                   const surface_descriptor_t &y, const surface_descriptor_t &uv) {
  assert(fds[0] == 17 && fds[1] == 23);
  target = {y, uv};
  return std::make_shared<nv12_img_t>();
}

struct RGB { int tex[1] = {1}; };
using rgb_t = std::shared_ptr<RGB>;
rgb_t create_blank(const platf::img_t &) { return std::make_shared<RGB>(); }

bool fail_direct;
bool fail_upload;
int direct_attempts;
int upload_attempts;
int source_width, source_height;
void check_rgb_descriptor(const surface_descriptor_t &source) {
  assert(source.width == source_width && source.height == source_height);
  assert(source.pitches[0] == 8960 && source.offsets[0] == 128);
  assert(source.fds[0] == 37 && source.modifier == 0 && source.direct_import_required);
}
std::optional<rgb_t> import_source(int, const surface_descriptor_t &source) {
  ++direct_attempts;
  check_rgb_descriptor(source);
  if (fail_direct) {
    return std::nullopt;
  }
  return std::make_shared<RGB>();
}
std::optional<rgb_t> upload_source(int, const surface_descriptor_t &source) {
  ++upload_attempts;
  check_rgb_descriptor(source);
  if (fail_upload) {
    return std::nullopt;
  }
  return std::make_shared<RGB>();
}

class sws_t {
public:
  static std::optional<sws_t> make(int, int, int width, int height, AVPixelFormat) {
    return sws_t {.out_width = width, .out_height = height};
  }
  void include_partial_chroma(int frame_width, int frame_height) PARTIAL_CHROMA
  int convert(gl::frame_buf_t &fb) GL_CONVERT
  void load_vram(const img_descriptor_t &, int, int, int texture) { loaded_texture = texture; }
  void apply_output_lut(int, int) {}

  int out_width;
  int out_height;
  int offsetX = 0;
  int offsetY = 0;
  int loaded_texture = 0;
  GLint chroma_coord_delta_location = 1;
  int partial_chroma_width = 0;
  int partial_chroma_height = 0;
  std::array<gl::Program, 2> program {{{0}, {1}}};
};
}

struct VA {
  std::unique_ptr<AVFrame> hwframe;
  AVFrame *frame;
  int va_display = 0;
  struct Display { int get() { return 0; } } display;
  int width = 2190;
  int height = 1179;
  int offset_x = 0;
  int offset_y = 0;
  egl::sws_t sws {};
  egl::nv12_t nv12;
  egl::rgb_t rgb;
  std::uint64_t sequence = 0;

  int set_frame(AVFrame *frame, AVBufferRef *hw_frames_ctx_buf) SET_FRAME
  int convert(platf::img_t &img) VA_CONVERT
};

void check_geometry(AVPixelFormat format, int width, int height, int exported_height) {
  // Use separate layer FDs, unequal pitches, and nonzero offsets. The producer
  // owns this layout regardless of luma size or chroma subsampling.
  exported = {};
  exported.width = width;
  exported.height = exported_height;
  exported.num_objects = exported.num_layers = 2;
  exported.objects[0].fd = 17;
  exported.objects[1].fd = 23;
  auto plane_codes = format == AV_PIX_FMT_NV12 ? std::array {0x20203852u, 0x38385247u}
                                              : std::array {0x20363152u, 0x32335247u};
  for (int plane = 0; plane < 2; ++plane) {
    exported.objects[plane].drm_format_modifier = 0x100000000000001ULL;
    exported.layers[plane].drm_format = plane_codes[plane];
    exported.layers[plane].num_planes = 1;
    exported.layers[plane].object_index[0] = plane;
    exported.layers[plane].pitch[0] = (format == AV_PIX_FMT_NV12 ? 2304 : 4608) + 256 * plane;
    exported.layers[plane].offset[0] = 4096 + 4096 * plane;
  }

  gl::ctx = {};
  VA va;
  va.width = width;
  va.height = height;
  AVHWFramesContext frames {format};
  AVBufferRef buffer {&frames};
  assert(va.set_frame(new AVFrame {width, height}, &buffer) == 0);
  assert(va.frame->width == width && va.frame->height == height);
  assert(egl::target[0].width == width && egl::target[0].height == exported_height);
  assert(egl::target[1].width == (width + 1) / 2 && egl::target[1].height == (exported_height + 1) / 2);
  for (int plane = 0; plane < 2; ++plane) {
    const auto &layer = egl::target[plane];
    assert(layer.fourcc == plane_codes[plane] && layer.modifier == exported.objects[plane].drm_format_modifier);
    assert(layer.fds[0] == exported.objects[plane].fd && layer.fds[1] == -1);
    assert(layer.pitches[0] == exported.layers[plane].pitch[0] && layer.offsets[0] == exported.layers[plane].offset[0]);
  }

  egl::img_descriptor_t img;
  img.sequence = 1;
  img.sd.width = egl::source_width = width;
  img.sd.height = egl::source_height = height;
  img.sd.fds[0] = 37;
  img.sd.pitches[0] = 8960;
  img.sd.offsets[0] = 128;
  img.sd.direct_import_required = true;

  egl::direct_attempts = egl::upload_attempts = 0;
  egl::fail_direct = egl::fail_upload = false;
  assert(va.convert(img) == 0);
  assert(egl::direct_attempts == 1 && egl::upload_attempts == 0);
  assert((gl::ctx.viewport[0] == gl::Rect {0, 0, width, height}));
  assert((gl::ctx.viewport[1] == gl::Rect {0, 0, (width + 1) / 2, (height + 1) / 2}));
  auto rendered = gl::ctx.viewport;
  auto sample_delta = gl::ctx.delta;
  for (int axis = 0; axis < 2; ++axis) {
    const int luma = axis == 0 ? width : height;
    const int chroma = (luma + 1) / 2;
    for (int index : {0, chroma / 2, chroma - 1}) {
      const float base = (index + 0.5f) / chroma;
      // The production UV shader adds base * delta, before clamping at the image edge.
      const float adjusted = base + base * sample_delta[axis];
      assert(std::abs(adjusted - (2 * index + 1.0f) / luma) < 0.000001f);
    }
  }

  egl::fail_direct = true;
  ++img.sequence;
  assert(va.convert(img) == 0);
  assert(egl::direct_attempts == 2 && egl::upload_attempts == 1);
  assert(gl::ctx.viewport == rendered && gl::ctx.delta == sample_delta);

  egl::fail_upload = true;
  const int draws = gl::ctx.draws;
  ++img.sequence;
  assert(va.convert(img) == -1);
  assert(gl::ctx.draws == draws);
}

int main() {
  for (auto format : {AV_PIX_FMT_NV12, AV_PIX_FMT_P010}) {
    check_geometry(format, 2190, 1179, 1179);
    check_geometry(format, 2190, 1180, 1180);
    check_geometry(format, 2190, 1179, 1184);
    check_geometry(format, 2191, 1180, 1180);
  }

  gl::ctx = {};
  gl::frame_buf_t target {};
  egl::sws_t sws {.out_width = 2190, .out_height = 1179};
  assert(sws.convert(target) == 0);
  assert(gl::ctx.viewport[1].height == 589); // Other GL callers keep their existing target geometry.
  sws.include_partial_chroma(2190, 1179);
  assert(sws.convert(target) == 0 && gl::ctx.delta[1] != 0);
  sws.out_height = 1177;
  sws.offsetY = 1;
  assert(sws.convert(target) == 0);
  assert((gl::ctx.viewport[1] == gl::Rect {0, 0, 1095, 588}));
  assert(gl::ctx.delta[1] == 0); // An interior letterboxed viewport keeps its existing geometry.
}
'''

vaapi = "src/platform/linux/vaapi.cpp"
graphics = "src/platform/linux/graphics.cpp"
for marker, shape in {
    "PRIME_DESCRIPTOR": (vaapi, "struct DRMPRIMESurfaceDescriptor"),
    "PARTIAL_CHROMA": (graphics, "void sws_t::include_partial_chroma("),
    "GL_CONVERT": (graphics, "int sws_t::convert("),
    "SET_FRAME": (vaapi, "int set_frame(AVFrame *frame"),
    "VA_CONVERT": (vaapi, "int convert(platf::img_t &img) override {\n      auto &descriptor"),
}.items():
    # The old converter has no opt-in method. Keep the baseline runnable so the
    # odd export test detects the preexisting truncation by behavior.
    replacement = ("{}" if marker == "PARTIAL_CHROMA" and
                   shape[1] not in (root / shape[0]).read_text() else body(*shape))
    fixture = fixture.replace(marker, replacement)

with tempfile.TemporaryDirectory(prefix="vaapi-chroma-test-") as temp:
    source = pathlib.Path(temp) / "test.cpp"
    binary = pathlib.Path(temp) / "test"
    source.write_text(fixture)
    subprocess.run([compiler, "-std=c++23", "-Wall", "-Wextra", "-Werror",
                    "-Wno-sign-compare", "-Wno-unused-parameter", "-I", str(root),
                    str(source), "-o", str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
print("VAAPI chroma import, conversion, and CPU fallback fixture passed")

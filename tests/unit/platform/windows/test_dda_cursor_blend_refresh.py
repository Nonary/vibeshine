#!/usr/bin/env python3
"""Replay the Desktop Duplication snapshots through cursor-blend changes on a still desktop.

Whether frames carry the pointer can change while neither the desktop nor the
pointer does: Ctrl+Alt+Shift+N, or host compositing for the client-side cursor.
The fixture extracts both production snapshot bodies (system memory and video
memory) unchanged and runs them against modeled DXGI/D3D11 objects that record
which desktop image a texture holds and whether the pointer was blended onto it.
The pointer is a separate hardware pointer (DXGI reports it apart from the
desktop image), so only the snapshot can add it to or remove it from a frame.
Use --source-tree for negative controls and --emit-only to inspect the replay.
"""
import argparse
from pathlib import Path
import re
import subprocess
import tempfile


def function(source, marker):
    start = source.index(marker)
    # The selected definitions end at their original indentation.
    indent = re.match(r" *", marker).group()
    return source[start:source.index("\n" + indent + "}", start) + len(indent) + 2]


def generate(root):
    read = lambda path: (root / path).read_text()
    ram = function(read("src/platform/windows/display_ram.cpp"), "  capture_e display_ddup_ram_t::snapshot(")
    vram = function(read("src/platform/windows/display_vram.cpp"), "  capture_e display_ddup_vram_t::snapshot(")
    code = r'''
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <deque>
#include <functional>
#include <iostream>
#include <memory>
#include <optional>
#include <span>
#include <string_view>
#include <tuple>
#include <utility>
#include <variant>
#include <vector>
using namespace std::literals;

struct Log {template<class T> Log &operator<<(const T &) {return *this;}};
#define BOOST_LOG(level) Log {}
#define FAILED(hr) ((hr) < 0)
using HRESULT = long;
using UINT = unsigned;
constexpr HRESULT S_OK = 0;
constexpr int W = 4, H = 4, PITCH = W * 4;

struct LARGE_INTEGER {long long QuadPart = 0;};
struct POINT {long x = 0, y = 0;};
struct DXGI_OUTDUPL_POSITION {POINT Position; int Visible = 0;};
struct DXGI_OUTDUPL_FRAME_INFO {
  LARGE_INTEGER LastPresentTime, LastMouseUpdateTime;
  UINT AccumulatedFrames = 0;
  int RectsCoalesced = 0, ProtectedContentMaskedOut = 0;
  DXGI_OUTDUPL_POSITION PointerPosition;
  UINT TotalMetadataBufferSize = 0, PointerShapeBufferSize = 0;
};
struct DXGI_OUTDUPL_POINTER_SHAPE_INFO {UINT Type = 0, Width = 0, Height = 0, Pitch = 0; POINT HotSpot;};
enum DXGI_FORMAT {DXGI_FORMAT_UNKNOWN = 0, DXGI_FORMAT_B8G8R8A8_UNORM = 87};
enum D3D11_USAGE {D3D11_USAGE_DEFAULT, D3D11_USAGE_STAGING};
enum D3D11_MAP {D3D11_MAP_READ = 1};
constexpr UINT D3D11_CPU_ACCESS_READ = 0x20000;
struct DXGI_SAMPLE_DESC {UINT Count = 0, Quality = 0;};
struct D3D11_TEXTURE2D_DESC {
  UINT Width = 0, Height = 0, MipLevels = 0, ArraySize = 0;
  DXGI_FORMAT Format = DXGI_FORMAT_UNKNOWN;
  DXGI_SAMPLE_DESC SampleDesc;
  D3D11_USAGE Usage = D3D11_USAGE_DEFAULT;
  UINT BindFlags = 0, CPUAccessFlags = 0, MiscFlags = 0;
};
struct D3D11_MAPPED_SUBRESOURCE {void *pData = nullptr; UINT RowPitch = 0, DepthPitch = 0;};
struct D3D11_VIEWPORT {float TopLeftX = 0, TopLeftY = 0, Width = 0, Height = 0, MinDepth = 0, MaxDepth = 0;};
struct GUID {int id;};
constexpr GUID IID_ID3D11Texture2D {1};
std::string_view dxgi_format_to_string(DXGI_FORMAT) {return "format";}
long long qpc_counter() {return 1000;}
std::chrono::nanoseconds qpc_time_difference(long long a, long long b) {return std::chrono::nanoseconds(a - b);}

namespace util {
  template<class T> struct buffer_t {
    std::vector<T> data;
    explicit buffer_t(std::size_t size): data(size) {}
    T *begin() {return data.data();}
    T *end() {return data.data() + data.size();}
    std::size_t size() const {return data.size();}
  };
  struct hex_t {std::string_view to_string_view() const {return "0";}};
  template<class T> hex_t hex(const T &) {return {};}
}
namespace host_cursor::tracker {
  template<class... A> void note_dxgi_pointer_shape(A &&...) {}
}

// Modeled GPU content: the desktop image a texture holds and whether the pointer was blended onto it.
struct Texture {
  int desktop = 0;
  bool pointer = false;
  std::vector<std::uint8_t> bytes;
  void GetDesc(D3D11_TEXTURE2D_DESC *desc) const {*desc = {}; desc->Width = W; desc->Height = H; desc->Format = DXGI_FORMAT_B8G8R8A8_UNORM;}
};
std::deque<Texture> textures;
Texture *new_texture() {return &textures.emplace_back();}
struct ID3D11RenderTargetView {Texture *texture;};
struct ID3D11ShaderResourceView {};
std::deque<ID3D11RenderTargetView> views;
struct Shader {};
struct Blend {};
struct KeyedMutex {};

// Like util::safe_ptr, without ownership: everything modeled lives until exit.
template<class T> struct ptr_t {
  T *p = nullptr;
  ptr_t() = default;
  ptr_t(T *q): p(q) {}
  T *get() const {return p;}
  T **operator&() {return &p;}
  T *operator->() const {return p;}
  T *release() {return std::exchange(p, nullptr);}
  void reset(T *q = nullptr) {p = q;}
  explicit operator bool() const {return p;}
};
using texture2d_t = ptr_t<Texture>;

struct Resource {
  Texture *desktop = nullptr;
  HRESULT QueryInterface(GUID, void **out) {*out = desktop; return S_OK;}
};
struct resource_t: ptr_t<Resource> {
  using pointer = Resource *;
  using ptr_t::ptr_t;
};

struct Device {
  HRESULT CreateTexture2D(const D3D11_TEXTURE2D_DESC *, const void *, Texture **out) {*out = new_texture(); return S_OK;}
};
struct Context {
  ID3D11RenderTargetView *target = nullptr;
  void CopyResource(Texture *dst, Texture *src) {dst->desktop = src->desktop; dst->pointer = src->pointer;}
  void VSSetShader(Shader *, void *, UINT) {}
  void PSSetShader(Shader *, void *, UINT) {}
  void OMSetRenderTargets(UINT count, ID3D11RenderTargetView *const *rtv, void *) {target = count ? rtv[0] : nullptr;}
  void OMSetBlendState(Blend *, const float *, UINT) {}
  void PSSetShaderResources(UINT, UINT, ID3D11ShaderResourceView *const *) {}
  void RSSetViewports(UINT, const D3D11_VIEWPORT *) {}
  void Draw(UINT, UINT) {if (target) target->texture->pointer = true;}
  void ClearRenderTargetView(ID3D11RenderTargetView *rtv, const float *) {rtv->texture->desktop = 0; rtv->texture->pointer = false;}
  HRESULT Map(Texture *texture, UINT, D3D11_MAP, UINT, D3D11_MAPPED_SUBRESOURCE *out) {
    texture->bytes.assign(H * PITCH, 0);
    texture->bytes[0] = texture->desktop;
    texture->bytes[1] = texture->pointer;
    out->pData = texture->bytes.data();
    out->RowPitch = PITCH;
    return S_OK;
  }
  void Unmap(Texture *, UINT) {}
};
Device gpu;
Context context;

namespace platf {
  enum class capture_e {ok, reinit, timeout, interrupted, error};
  struct img_t {
    virtual ~img_t() = default;
    std::uint8_t *data = nullptr;
    std::vector<std::uint8_t> storage;
    std::optional<std::chrono::steady_clock::time_point> frame_timestamp, host_processing_timestamp, capture_pacing_timestamp;
  };
  using pull_free_image_cb_t = std::function<bool(std::shared_ptr<img_t> &)>;

  namespace dxgi {
    // One scripted AcquireNextFrame() result.
    struct step_t {
      capture_e status = capture_e::timeout;
      DXGI_OUTDUPL_FRAME_INFO info;
      Texture *desktop = nullptr;
    };
    struct Duplication {
      HRESULT GetFramePointerShape(UINT size, void *data, UINT *required, DXGI_OUTDUPL_POINTER_SHAPE_INFO *info) {
        std::memset(data, 0xff, size);
        *required = size;
        *info = {};
        info->Type = 2;
        info->Width = info->Height = 2;
        info->Pitch = 8;
        return S_OK;
      }
    };
    struct duplication_t {
      Duplication output;
      Duplication *dup = &output;
      Resource resource;
      std::deque<step_t> steps;
      std::chrono::milliseconds waited {-1};
      capture_e next_frame(DXGI_OUTDUPL_FRAME_INFO &info, std::chrono::milliseconds wait, Resource **res) {
        waited = wait;
        auto step = steps.front();
        steps.pop_front();
        if (step.status != capture_e::ok) {
          // AcquireNextFrame() does not define the frame info it leaves behind on a timeout.
          info.LastPresentTime.QuadPart = info.LastMouseUpdateTime.QuadPart = 0x5a5a;
          info.AccumulatedFrames = 9;
          info.PointerShapeBufferSize = 64;
          return step.status;
        }
        info = step.info;
        resource.desktop = step.desktop;
        *res = &resource;
        return capture_e::ok;
      }
    };

    struct display_base_t {
      ptr_t<Device> device {&gpu};
      ptr_t<Context> device_ctx {&context};
      UINT width = W, height = H, width_before_rotation = W, height_before_rotation = H;
      int display_rotation = 0;
      DXGI_FORMAT capture_format = DXGI_FORMAT_UNKNOWN;
      duplication_t dup;
      std::optional<bool> last_cursor_blended;
    };

    struct cursor_t {
      std::vector<std::uint8_t> img_data;
      DXGI_OUTDUPL_POINTER_SHAPE_INFO shape_info;
      int x = 0, y = 0;
      bool visible = false;
    };
    void blend_cursor(const cursor_t &, img_t &img) {img.data[1] = 1;}

    struct display_ddup_ram_t: display_base_t {
      cursor_t cursor;
      texture2d_t texture;
      D3D11_MAPPED_SUBRESOURCE img_info;
      static std::shared_ptr<img_t> alloc() {return std::make_shared<img_t>();}
      int complete_img(img_t *img, bool) {
        if (!img->data) {
          img->storage.assign(H * PITCH, 0);
          img->data = img->storage.data();
        }
        return 0;
      }
      int dummy_img(img_t *img) {
        complete_img(img, true);
        std::fill(img->storage.begin(), img->storage.end(), 0);
        return 0;
      }
      capture_e snapshot(const pull_free_image_cb_t &pull_free_image_cb, std::shared_ptr<platf::img_t> &img_out, std::chrono::milliseconds timeout, bool cursor_visible);
    };

    struct img_d3d_t: img_t {
      texture2d_t capture_texture;
      ptr_t<ID3D11RenderTargetView> capture_rt;
      ptr_t<KeyedMutex> capture_mutex;
      bool blank = true;
    };
    struct texture_lock_helper {
      texture_lock_helper(KeyedMutex *) {}
      texture_lock_helper(std::nullptr_t) {}
      bool lock() {return true;}
    };
    struct gpu_cursor_t {
      texture2d_t texture;
      ptr_t<ID3D11ShaderResourceView> input_res;
      D3D11_VIEWPORT cursor_view;
      bool visible = false;
      void set_pos(long, long, int, int, int, bool visible_) {visible = visible_;}
    };
    template<class B> std::vector<std::uint8_t> make_cursor_alpha_image(const B &, const DXGI_OUTDUPL_POINTER_SHAPE_INFO &) {return std::vector<std::uint8_t>(4, 1);}
    template<class B> std::vector<std::uint8_t> make_cursor_xor_image(const B &, const DXGI_OUTDUPL_POINTER_SHAPE_INFO &) {return {};}
    bool set_cursor_texture(Device *, gpu_cursor_t &cursor, std::vector<std::uint8_t> &&image, const DXGI_OUTDUPL_POINTER_SHAPE_INFO &) {
      cursor.texture.reset(image.empty() ? nullptr : new_texture());
      return true;
    }

    struct display_ddup_vram_t: display_base_t {
      ptr_t<Shader> cursor_vs, cursor_ps;
      ptr_t<Blend> blend_alpha, blend_invert, blend_disable;
      gpu_cursor_t cursor_alpha, cursor_xor;
      texture2d_t old_surface_delayed_destruction;
      std::chrono::steady_clock::time_point old_surface_timestamp;
      std::variant<std::monostate, texture2d_t, std::shared_ptr<platf::img_t>> last_frame_variant;
      static std::shared_ptr<img_t> alloc() {return std::make_shared<img_d3d_t>();}
      int complete_img(img_t *base, bool) {
        auto img = static_cast<img_d3d_t *>(base);
        if (!img->capture_texture) {
          img->capture_texture.reset(new_texture());
          img->capture_rt.reset(&views.emplace_back(ID3D11RenderTargetView {img->capture_texture.get()}));
        }
        return 0;
      }
      capture_e snapshot(const pull_free_image_cb_t &pull_free_image_cb, std::shared_ptr<platf::img_t> &img_out, std::chrono::milliseconds timeout, bool cursor_visible);
    };

// RAM
// VRAM
  }  // namespace dxgi
}  // namespace platf
using namespace platf;
using namespace platf::dxgi;

int failures = 0, checks = 0;
void check(bool ok, const char *backend, const char *message) {
  ++checks;
  if (!ok) {
    ++failures;
    std::cerr << "FAIL [" << backend << "]: " << message << '\n';
  }
}

// What a frame shows: the desktop image and whether the pointer is in it.
std::pair<int, bool> shows(const display_ddup_ram_t &, const std::shared_ptr<img_t> &img) {return {img->data[0], img->data[1] != 0};}
std::pair<int, bool> shows(const display_ddup_vram_t &, const std::shared_ptr<img_t> &img) {
  auto texture = static_cast<img_d3d_t &>(*img).capture_texture.get();
  return {texture->desktop, texture->pointer};
}

step_t timeout() {return {};}
step_t no_update() {return {capture_e::ok, {}, nullptr};}  // a frame acquired with neither image nor pointer changes
step_t desktop(Texture *image) {
  step_t step {capture_e::ok, {}, image};
  step.info.LastPresentTime.QuadPart = 900;
  step.info.AccumulatedFrames = 1;
  return step;
}
step_t pointer(bool visible, bool shape) {
  step_t step {capture_e::ok, {}, nullptr};
  step.info.LastMouseUpdateTime.QuadPart = 900;
  step.info.PointerPosition.Position = {1, 1};
  step.info.PointerPosition.Visible = visible;
  step.info.PointerShapeBufferSize = shape ? 32 : 0;
  return step;
}
step_t both(Texture *image, bool visible, bool shape) {
  auto step = desktop(image);
  auto mouse = pointer(visible, shape);
  step.info.LastMouseUpdateTime = mouse.info.LastMouseUpdateTime;
  step.info.PointerPosition = mouse.info.PointerPosition;
  step.info.PointerShapeBufferSize = mouse.info.PointerShapeBufferSize;
  return step;
}

template<class Display> void run(const char *backend) {
  Display display;
  std::shared_ptr<img_t> frame;
  int pulled = 0;
  const pull_free_image_cb_t pull = [&](std::shared_ptr<img_t> &img) {
    ++pulled;
    img = Display::alloc();
    return true;
  };
  auto snap = [&](step_t step, bool cursor_visible) {
    display.dup.steps = {step};
    frame.reset();
    return display.snapshot(pull, frame, 200ms, cursor_visible);
  };
  auto shows_ = [&](int image, bool with_pointer) {
    return frame && shows(display, frame) == std::pair {image, with_pointer};
  };
  auto first = new_texture(), second = new_texture();
  first->desktop = 1;
  second->desktop = 2;

  check(snap(timeout(), true) == capture_e::timeout && !frame && pulled == 0, backend, "a capture with no frame yet has nothing to rebuild");
  check(snap(both(first, true, true), true) == capture_e::ok && shows_(1, true), backend, "the first frame blends the hardware pointer");

  // Compositing stops, then resumes, while neither the desktop nor the pointer changes.
  check(snap(timeout(), false) == capture_e::ok && shows_(1, false), backend, "a still desktop gets a frame without the pointer when compositing stops");
  check(display.dup.waited == 0ms, backend, "the change does not wait for a desktop or pointer update");
  check(frame && frame->frame_timestamp && frame->host_processing_timestamp, backend, "the rebuilt frame is stamped, so it can anchor frame pacing");
  check(snap(timeout(), false) == capture_e::timeout && !frame, backend, "no further frames until something changes");
  check(display.dup.waited == 200ms, backend, "without a change, the snapshot waits as before");
  check(snap(timeout(), true) == capture_e::ok && shows_(1, true), backend, "a still desktop gets the pointer back when compositing resumes");
  check(display.dup.waited == 0ms, backend, "resuming does not wait for an update either");
  check(snap(timeout(), true) == capture_e::timeout, backend, "resumed compositing also stops producing frames once the frame is right");

  // An acquired frame with no updates follows the same rule.
  check(snap(no_update(), false) == capture_e::ok && shows_(1, false), backend, "an acquired frame without updates follows compositing stopping");
  check(snap(no_update(), false) == capture_e::timeout, backend, "an acquired frame without updates is otherwise a timeout");
  check(snap(no_update(), true) == capture_e::ok && shows_(1, true), backend, "an acquired frame without updates follows compositing resuming");

  // A new desktop image while the pointer is left out is the one a later rebuild uses.
  check(snap(desktop(second), false) == capture_e::ok && shows_(2, false), backend, "a desktop update without compositing has no pointer");
  check(snap(timeout(), true) == capture_e::ok && shows_(2, true), backend, "the rebuild blends the pointer onto the latest desktop");

  // A hidden pointer looks the same either way, so a change produces no frame.
  check(snap(pointer(false, false), true) == capture_e::ok && shows_(2, false), backend, "a hidden pointer is not blended");
  check(snap(timeout(), false) == capture_e::timeout && display.dup.waited == 200ms, backend, "with the pointer hidden, compositing stopping produces no frame");
  check(snap(timeout(), true) == capture_e::timeout, backend, "with the pointer hidden, compositing resuming produces no frame");
  check(snap(pointer(true, false), true) == capture_e::ok && shows_(2, true), backend, "the pointer is blended again once it reappears");
}

int main() {
  run<display_ddup_ram_t>("system memory");
  run<display_ddup_vram_t>("video memory");
  std::cout << "Desktop Duplication cursor blend refresh: failures=" << failures << ", checks=" << checks << '\n';
  return failures ? 1 : 0;
}
'''
    code = code.replace("// RAM\n", ram + "\n", 1)
    return code.replace("// VRAM\n", vram + "\n", 1)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("repository", type=Path)
    parser.add_argument("compiler", nargs="?", default="c++")
    parser.add_argument("--source-tree", type=Path)
    parser.add_argument("--emit-only", type=Path)
    args = parser.parse_args()
    code = generate(args.source_tree or args.repository)
    if args.emit_only:
        args.emit_only.write_text(code)
        print("Prepared production source replay:", args.emit_only)
        return
    with tempfile.TemporaryDirectory(prefix="dda-cursor-replay-") as temp:
        source = Path(temp) / "test.cpp"
        exe = Path(temp) / "test"
        source.write_text(code)
        subprocess.run([args.compiler, "-std=c++23", "-O0", "-Wall", "-Wextra", str(source), "-o", str(exe)], check=True)
        subprocess.run([str(exe)], check=True, timeout=8)


if __name__ == "__main__":
    main()

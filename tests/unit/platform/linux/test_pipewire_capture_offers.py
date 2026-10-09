"""Run the actual PipeWire offer loops and SPA filter without a live compositor."""
import pathlib
import shlex
import subprocess
import sys
import tempfile

root = pathlib.Path(sys.argv[1]).resolve()
compiler = sys.argv[2]
source = (root / "src/platform/linux/pipewire.cpp").read_text()
start = source.index("        int n_params = 0;")
end = source.index('        BOOST_LOG(debug) << "[pipewire] Connect PW stream', start)
offers = source[start:end]

program = r'''
#include <array>
#include <cassert>
#include <cstdint>
#include <iostream>
#include <optional>
#include <spa/param/video/format-utils.h>
#include <spa/pod/filter.h>
#include "src/platform/linux/pipewire_format.h"
namespace platf { enum class mem_type_e { system, vaapi, vulkan, cuda }; }
using namespace pipewire;
constexpr int MAX_PARAMS = 200;
struct format_map_t { int32_t pw_format; };
constexpr std::array<format_map_t, 7> format_map {{
  {SPA_VIDEO_FORMAT_xBGR_210LE}, {SPA_VIDEO_FORMAT_ARGB_210LE},
  {SPA_VIDEO_FORMAT_ABGR_210LE}, {SPA_VIDEO_FORMAT_RGBA_102LE},
  {SPA_VIDEO_FORMAT_BGRA_102LE}, {SPA_VIDEO_FORMAT_BGRA}, {SPA_VIDEO_FORMAT_BGRx},
}};
struct dmabuf_format_info_t { int32_t format; const uint64_t *modifiers; int n_modifiers; };
struct result_t { int offers; std::optional<int> selected; };
bool is_sdr_format(int32_t format) { return memory_format_supported(format); }
result_t negotiate(platf::mem_type_e mem_type, bool display_is_nvidia, bool force_sdr_formats_, bool force_hdr10_formats_,
                   int modifier_count, int32_t producer_format, bool producer_dma) {
  const uint64_t modifier = 0;
  std::array<dmabuf_format_info_t, 7> catalog;
  for (std::size_t i = 0; i < catalog.size(); ++i) catalog[i] = {format_map[i].pw_format, &modifier, modifier_count};
  const auto *dmabuf_infos = catalog.data();
  const int n_dmabuf_infos = catalog.size();
  constexpr uint32_t width = 2560, height = 1440, refresh_rate = 60;
  constexpr bool negotiate_maxframerate_ = true, gamescope_requested_size_ = false;
  std::array<uint8_t, 32768> storage {};
  auto pod_builder = SPA_POD_BUILDER_INIT(storage.data(), storage.size());
''' + offers + r'''
  std::array<uint8_t, 2048> producer_storage {};
  auto producer_builder = SPA_POD_BUILDER_INIT(producer_storage.data(), producer_storage.size());
  spa_pod_frame frame;
  const auto size = SPA_RECTANGLE(width, height);
  spa_pod_builder_push_object(&producer_builder, &frame, SPA_TYPE_OBJECT_Format, SPA_PARAM_EnumFormat);
  spa_pod_builder_add(&producer_builder,
                     SPA_FORMAT_mediaType, SPA_POD_Id(SPA_MEDIA_TYPE_video),
                     SPA_FORMAT_mediaSubtype, SPA_POD_Id(SPA_MEDIA_SUBTYPE_raw),
                     SPA_FORMAT_VIDEO_format, SPA_POD_Id(producer_format),
                     SPA_FORMAT_VIDEO_size, SPA_POD_Rectangle(&size), 0);
  if (producer_dma) spa_pod_builder_add(&producer_builder, SPA_FORMAT_VIDEO_modifier, SPA_POD_Long(modifier), 0);
  const auto *producer = static_cast<const spa_pod *>(spa_pod_builder_pop(&producer_builder, &frame));
  result_t result {n_params, {}};
  for (int i = 0; i < n_params; ++i) {
    assert(params[i]);
    std::array<uint8_t, 2048> filtered_storage {};
    auto filter_builder = SPA_POD_BUILDER_INIT(filtered_storage.data(), filtered_storage.size());
    spa_pod *filtered = nullptr;
    if (spa_pod_filter(&filter_builder, &filtered, producer, params[i]) < 0) continue;
    spa_pod_fixate(filtered);
    spa_video_info_raw raw {};
    assert(spa_format_video_raw_parse(filtered, &raw) >= 0);
    result.selected = raw.format;
    return result;
  }
  return result;
}
int main() {
  unsigned checks = 0;
  for (const auto memory : {platf::mem_type_e::system, platf::mem_type_e::cuda, platf::mem_type_e::vaapi, platf::mem_type_e::vulkan}) {
    for (const bool nvidia : {false, true}) {
      for (const int modifiers : {0, 1}) {
        const bool imports_dma = modifiers > 0 && (memory == platf::mem_type_e::vaapi || memory == platf::mem_type_e::vulkan ||
                                                  (memory == platf::mem_type_e::cuda && nvidia));
        for (const bool force_sdr : {false, true}) {
          assert(!negotiate(memory, nvidia, force_sdr, false, modifiers, SPA_VIDEO_FORMAT_xBGR_210LE, false).selected);
          assert(negotiate(memory, nvidia, force_sdr, false, modifiers, SPA_VIDEO_FORMAT_BGRA, false).selected == SPA_VIDEO_FORMAT_BGRA);
          assert(negotiate(memory, nvidia, force_sdr, false, modifiers, SPA_VIDEO_FORMAT_BGRx, false).selected == SPA_VIDEO_FORMAT_BGRx);
          assert(negotiate(memory, nvidia, force_sdr, false, modifiers, SPA_VIDEO_FORMAT_xBGR_210LE, true).selected.has_value() == (imports_dma && !force_sdr));
          checks += 4;
        }
        const auto hdr = negotiate(memory, nvidia, false, true, modifiers, SPA_VIDEO_FORMAT_xBGR_210LE, true);
        assert(hdr.selected.has_value() == imports_dma);
        assert(hdr.offers == (imports_dma ? 1 : 0));
        assert(!negotiate(memory, nvidia, false, true, modifiers, SPA_VIDEO_FORMAT_xBGR_210LE, false).selected);
        assert(!negotiate(memory, nvidia, false, true, modifiers, SPA_VIDEO_FORMAT_BGRA, false).selected);
        checks += 4;
      }
    }
  }
  std::cout << checks << " production offer-loop/SPA checks passed\n";
}
'''

includes = shlex.split(subprocess.check_output(["pkg-config", "--cflags", "libpipewire-0.3"], text=True))
with tempfile.TemporaryDirectory(prefix="vibeshine-pipewire-offers-") as directory:
    cpp = pathlib.Path(directory) / "offers.cpp"
    binary = pathlib.Path(directory) / "offers"
    cpp.write_text(program)
    subprocess.run([compiler, "-std=c++20", "-I", str(root), *includes, str(cpp), "-o", str(binary)], check=True)
    subprocess.run([str(binary)], check=True)

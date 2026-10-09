/**
 * @file src/platform/linux/pipewire_format.h
 * @brief PipeWire video format negotiation shared with focused SPA tests.
 */
#pragma once

#include <array>
#include <cstdint>
#include <limits>
#include <spa/param/video/format.h>
#include <spa/pod/builder.h>

namespace pipewire {
  inline constexpr bool memory_format_supported(int32_t format) {
    // The software and CUDA memory converters consume byte-packed BGR pixels.
    // RGB10 is supported only through an importable DMA-BUF, not SHM/MemPtr.
    return format == SPA_VIDEO_FORMAT_BGRA || format == SPA_VIDEO_FORMAT_BGRx;
  }

  inline void add_framerate_parameters(spa_pod_builder *builder, bool negotiate_maxframerate) {
    const auto variable_rate = SPA_FRACTION(0, 1);
    spa_pod_builder_add(builder, SPA_FORMAT_VIDEO_framerate, SPA_POD_Fraction(&variable_rate), 0);
    if (negotiate_maxframerate) {
      // Prefer an unpaced stream, but accept compositors such as KWin whose
      // maximum-rate range starts at 1 fps. A zero-only range has no overlap
      // with those producers and rejects every otherwise compatible format.
      const auto maximum_rate = SPA_FRACTION(std::numeric_limits<int32_t>::max(), 1);
      spa_pod_builder_add(builder, SPA_FORMAT_VIDEO_maxFramerate,
                          SPA_POD_CHOICE_RANGE_Fraction(&variable_rate, &variable_rate, &maximum_rate), 0);
    }
  }

  struct capture_format_options_t {
    bool negotiate_maxframerate = true;
    bool gamescope_requested_size = false;
    bool force_hdr10 = false;
  };

  inline spa_pod *build_capture_format_parameter(spa_pod_builder *b, uint32_t width, uint32_t height,
                                                int32_t format, const uint64_t *modifiers, int n_modifiers,
                                                capture_format_options_t options = {}) {
    const bool dmabuf = n_modifiers > 0 && modifiers != nullptr;
    // A DMA format catalog entry with zero modifiers also creates a memory
    // offer. Guard the pod itself so neither offer loop can advertise CPU10.
    if (n_modifiers < 0 || (n_modifiers > 0 && !modifiers) ||
        (!dmabuf && !memory_format_supported(format)) ||
        (options.force_hdr10 && (!dmabuf || format != SPA_VIDEO_FORMAT_xBGR_210LE))) {
      return nullptr;
    }

    spa_pod_frame object_frame;
    spa_pod_frame modifier_frame;
    const std::array<spa_rectangle, 3> sizes {{
      SPA_RECTANGLE(width, height),
      SPA_RECTANGLE(1, 1),
      SPA_RECTANGLE(8192, 4096),
    }};

    spa_pod_builder_push_object(b, &object_frame, SPA_TYPE_OBJECT_Format, SPA_PARAM_EnumFormat);
    spa_pod_builder_add(b, SPA_FORMAT_mediaType, SPA_POD_Id(SPA_MEDIA_TYPE_video), 0);
    spa_pod_builder_add(b, SPA_FORMAT_mediaSubtype, SPA_POD_Id(SPA_MEDIA_SUBTYPE_raw), 0);
    spa_pod_builder_add(b, SPA_FORMAT_VIDEO_format, SPA_POD_Id(format), 0);
    spa_pod_builder_add(b, SPA_FORMAT_VIDEO_size, SPA_POD_CHOICE_RANGE_Rectangle(&sizes[0], &sizes[1], &sizes[2]), 0);
    if (options.gamescope_requested_size) {
      // Valve's private SPA_FORMAT_VIDEO_requested_size capture bounds.
      spa_pod_builder_add(b, 0x70000, SPA_POD_Rectangle(&sizes[0]), 0);
    }
    add_framerate_parameters(b, options.negotiate_maxframerate);

    if (format == SPA_VIDEO_FORMAT_xBGR_210LE) {
      spa_pod_builder_add(b, SPA_FORMAT_VIDEO_colorPrimaries, SPA_POD_Id(SPA_VIDEO_COLOR_PRIMARIES_BT2020), 0);
      // Stable SPA wire value; the SMPTE2084 enum name arrived in PipeWire 1.6.
      spa_pod_builder_add(b, SPA_FORMAT_VIDEO_transferFunction, SPA_POD_Id(14), 0);
      if (options.force_hdr10) {
        spa_pod_builder_add(b, SPA_FORMAT_VIDEO_colorMatrix, SPA_POD_Id(SPA_VIDEO_COLOR_MATRIX_RGB), 0);
        spa_pod_builder_add(b, SPA_FORMAT_VIDEO_colorRange, SPA_POD_Id(SPA_VIDEO_COLOR_RANGE_0_255), 0);
      }
    }

    if (dmabuf) {
      spa_pod_builder_prop(b, SPA_FORMAT_VIDEO_modifier, SPA_POD_PROP_FLAG_MANDATORY | SPA_POD_PROP_FLAG_DONT_FIXATE);
      spa_pod_builder_push_choice(b, &modifier_frame, SPA_CHOICE_Enum, 0);
      spa_pod_builder_long(b, modifiers[0]);
      for (int i = 0; i < n_modifiers; ++i) {
        spa_pod_builder_long(b, modifiers[i]);
      }
      spa_pod_builder_pop(b, &modifier_frame);
    }

    return static_cast<spa_pod *>(spa_pod_builder_pop(b, &object_frame));
  }
}  // namespace pipewire

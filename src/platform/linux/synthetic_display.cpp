/**
 * @file src/platform/linux/synthetic_display.cpp
 * @brief Blank Linux capture source for Remote Input and encoder probes.
 */
// standard includes
#include <algorithm>
#include <limits>

// local includes
#include "graphics.h"
#include "synthetic_display.h"
#include "src/video.h"
#ifdef SUNSHINE_BUILD_CUDA
  #include "cuda.h"
#endif
#ifdef SUNSHINE_BUILD_VAAPI
  #include "vaapi.h"
#endif

namespace platf {
  class synthetic_display_t final: public platf::display_t {
  public:
    synthetic_display_t(const platf::mem_type_e type, const video::config_t &config, const bool capture_frames):
        type(type),
        capture_frames(capture_frames),
        frame_rate(std::max(1, config.framerate)),
        hdr(config.dynamicRange > 0 && !config.force_sdr) {
      width = std::max(1, config.width);
      height = std::max(1, config.height);
      logical_width = width;
      logical_height = height;
      env_width = width;
      env_height = height;
      env_logical_width = width;
      env_logical_height = height;
    }

    platf::capture_e capture(
      const push_captured_image_cb_t &push,
      const pull_free_image_cb_t &pull,
      bool *
    ) override {
      return capture_frames ? capture_synthetic_black(push, pull, frame_rate) : platf::capture_e::error;
    }

    std::shared_ptr<platf::img_t> alloc_img() override {
      auto img = std::make_shared<egl::img_descriptor_t>();
      img->width = width;
      img->height = height;
      img->data = nullptr;
      img->pixel_pitch = 4;
      img->row_pitch = width * img->pixel_pitch;
      img->serial = std::numeric_limits<decltype(img->serial)>::max();
      img->sequence = 0;
      img->sd = {};
      img->sd.width = width;
      img->sd.height = height;
      std::fill_n(img->sd.fds, 4, -1);
      return img;
    }

    int dummy_img(platf::img_t *) override {
      return 0;
    }

    std::unique_ptr<platf::avcodec_encode_device_t> make_avcodec_encode_device(platf::pix_fmt_e) override {
      switch (type) {
#ifdef SUNSHINE_BUILD_VAAPI
        case platf::mem_type_e::vaapi:
          // Sequence-zero descriptors already use a blank EGL texture in the
          // VAAPI VRAM converter; no capture target or CUDA context is needed.
          return va::make_avcodec_encode_device(width, height, true);
#endif
#ifdef SUNSHINE_BUILD_CUDA
        case platf::mem_type_e::cuda:
          return ::cuda::make_avcodec_gl_encode_device(width, height, 0, 0);
#endif
        default:
          return nullptr;
      }
    }

    std::unique_ptr<platf::nvenc_encode_device_t> make_nvenc_encode_device([[maybe_unused]] platf::pix_fmt_e pix_fmt) override {
#ifdef SUNSHINE_BUILD_CUDA
      if (type == platf::mem_type_e::cuda) {
        return ::cuda::make_nvenc_gl_encode_device(width, height, 0, 0, pix_fmt);
      }
#endif
      return nullptr;
    }

    bool is_hdr() override {
      return hdr;
    }

    bool get_hdr_metadata(SS_HDR_METADATA &metadata) override {
      if (!hdr) {
        metadata = {};
        return false;
      }

      metadata = {};
      metadata.displayPrimaries[0] = {35400, 14600};
      metadata.displayPrimaries[1] = {8500, 39850};
      metadata.displayPrimaries[2] = {6550, 2300};
      metadata.whitePoint = {15635, 16450};
      metadata.maxDisplayLuminance = 1000;
      metadata.minDisplayLuminance = 1;
      metadata.maxContentLightLevel = 1000;
      metadata.maxFrameAverageLightLevel = 250;
      metadata.maxFullFrameLuminance = 400;
      return true;
    }

  private:
    platf::mem_type_e type;
    bool capture_frames;
    int frame_rate;
    bool hdr;
  };

  std::shared_ptr<display_t> make_synthetic_display(
    const mem_type_e type,
    [[maybe_unused]] const video::config_t &config,
    [[maybe_unused]] const bool capture_frames
  ) {
    switch (type) {
#ifdef SUNSHINE_BUILD_VAAPI
      case mem_type_e::vaapi:
#endif
#ifdef SUNSHINE_BUILD_CUDA
      case mem_type_e::cuda:
#endif
#if defined(SUNSHINE_BUILD_VAAPI) || defined(SUNSHINE_BUILD_CUDA)
        return std::make_shared<synthetic_display_t>(type, config, capture_frames);
#endif
      default:
        return {};
    }
  }
}  // namespace platf

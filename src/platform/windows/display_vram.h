/**
 * @file src/platform/windows/display_vram.h
 * @brief DXGI/D3D11 VRAM image structures and utilities for Windows platform display capture.
 */
#pragma once

// standard includes
#include <memory>

// local includes
#include "display.h"

// platform includes
#include <d3d11.h>
#include <d3d11_4.h>
#include <dxgi.h>
#include <winrt/base.h>

namespace platf::dxgi {

  class wgc_frame_sync_t {
  public:
    virtual ~wgc_frame_sync_t() = default;
    virtual void register_consumer(ID3D11Fence *fence, uint64_t value) = 0;
    virtual void mark_failed() = 0;
  };

  /**
   * @brief Direct3D-backed image container used for WGC/DXGI capture paths.
   *
   * Extends platf::img_t with Direct3D 11 resources required for capture and
   * inter-process texture sharing.
   */
  struct img_d3d_t: public platf::img_t {
    texture2d_t capture_texture;  ///< Staging/CPU readable or GPU shared texture.
    render_target_t capture_rt;  ///< Render target bound when copying / compositing.
    keyed_mutex_t capture_mutex;  ///< Keyed mutex for cross-process synchronization.
    std::shared_ptr<winrt::handle> encoder_texture_handle;  ///< Shared handle owner opened by encoder devices.
    std::shared_ptr<winrt::handle> wgc_ready_fence_handle;  ///< Helper producer-ready fence for direct-ring WGC frames.
    wgc_frame_sync_t *wgc_frame_sync = nullptr;  ///< Frame-lifetime completion tracker owned by the aliasing frame owner.
    uint64_t wgc_ready_value = 0;  ///< Fence generation that makes this ring texture readable.
    bool dummy = false;  ///< True if placeholder prior to first successful frame.
    bool blank = true;  ///< True if contains no desktop or cursor content.
    uint32_t id = 0;  ///< Monotonically increasing identifier.
    DXGI_FORMAT format;  ///< Underlying DXGI texture format.
  };

}  // namespace platf::dxgi

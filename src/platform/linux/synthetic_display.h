/**
 * @file src/platform/linux/synthetic_display.h
 * @brief Backend-matched blank Linux capture sources.
 */
#pragma once

// standard includes
#include <memory>

namespace video {
  struct config_t;
}

namespace platf {
  class display_t;
  enum class mem_type_e;

  // Reuse the selected backend's blank-image conversion without opening a
  // desktop capture target. Unsupported or unbuilt backends return null.
  std::shared_ptr<display_t> make_synthetic_display(mem_type_e type, const video::config_t &config, bool capture_frames);
}  // namespace platf

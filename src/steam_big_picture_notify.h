/**
 * @file src/steam_big_picture_notify.h
 * @brief Lifecycle hook for Vibeshine's stream controls inside Steam Big Picture.
 */
#pragma once

namespace platf::steam::big_picture {
  /**
   * @brief Wake the Steam Big Picture controls after a stream, client or app change.
   * Only sets a flag; safe to call under lifecycle locks. A no-op off Windows.
   */
#ifdef _WIN32
  void notify_changed();
#else
  inline void notify_changed() {
  }
#endif
}  // namespace platf::steam::big_picture

/**
 * @file globals.h
 * @brief Declarations for globally accessible variables and functions.
 */
#pragma once

// local includes
#include "entry_handler.h"
#include "thread_pool.h"

/**
 * @brief A thread pool for processing tasks.
 */
extern thread_pool_util::ThreadPool task_pool;

/**
 * @brief A boolean flag to indicate whether the cursor should be displayed.
 */
extern bool display_cursor;

#ifdef _WIN32
  // Declare global singleton used for NVIDIA control panel modifications
  #include "platform/windows/nvprefs/nvprefs_interface.h"

/**
 * @brief A global singleton used for NVIDIA control panel modifications.
 */
extern nvprefs::nvprefs_interface nvprefs_instance;
#endif

/**
 * @brief Handles process-wide communication.
 */
namespace mail {
#define MAIL(x) \
  constexpr auto x = std::string_view { \
    #x \
  }

  /**
   * @brief A process-wide communication mechanism.
   */
  extern safe::mail_t man;

  // Global mail
  MAIL(shutdown);
  MAIL(broadcast_shutdown);
  MAIL(video_packets);
  MAIL(audio_packets);
  MAIL(switch_display);

  // Local mail
  MAIL(touch_port);
  MAIL(idr);
  MAIL(invalidate_ref_frames);
  MAIL(gamepad_feedback);
  MAIL(hdr);
  MAIL(dynamic_bitrate);  // Runtime encoder bitrate change (kbps), posted from the HTTP /bitrate handler
  MAIL(live_bitrate);  // 1 while the encoder applies a runtime bitrate without rebuilding, 0 once it cannot; only ever falls
  MAIL(link_rate_bitrate);  // Host link rate adaptation bitrate step (kbps), applied without forcing a keyframe
#undef MAIL

}  // namespace mail

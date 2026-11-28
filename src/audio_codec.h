/**
 * @file src/audio_codec.h
 * @brief Lightweight audio codec declarations shared by audio and RTSP policies.
 */
#pragma once

#include <cstddef>

namespace audio {
  inline constexpr std::size_t MAX_DOLBY_PACKET_SIZE = 2560;  ///< Maximum 640 kbit/s AC-3 access unit at 48 kHz.

  enum class dolby_codec_e { opus, ac3, eac3 };
}  // namespace audio

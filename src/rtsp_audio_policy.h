/**
 * @file src/rtsp_audio_policy.h
 * @brief RTSP response policy for negotiated audio codecs.
 */
#pragma once

#include "audio_codec.h"

namespace rtsp_stream {
  /**
   * @brief Preserve the legacy Opus response; identify explicitly negotiated Dolby codecs.
   */
  constexpr bool should_send_audio_codec_header(audio::dolby_codec_e codec) noexcept {
    return codec != audio::dolby_codec_e::opus;
  }
}  // namespace rtsp_stream

/**
 * @file src/video_hdr_dynamic.h
 * @brief HDR10+ dynamic metadata between an encode session, its device and its packets.
 */
#pragma once

// standard includes
#include <atomic>
#include <cstdint>
#include <string_view>

// local includes
#include "hdr_dynamic_metadata.h"
#include "video.h"

namespace video {
  /// Frames carrying the static stand-in, with no measurement yet, before a stream says so.
  inline constexpr std::uint64_t hdr_dynamic_unmeasured_frames_warning = 120;

  /**
   * @brief HDR10+ static levels from the stream's HDR10 metadata, in nits as the encoders'
   *        mastering-display and content-light SEI take them.
   */
  hdr_dynamic::static_levels_t hdr_dynamic_static_levels(const SS_HDR_METADATA &metadata);

  /**
   * @brief Hand new HDR10 metadata to `session`'s HDR10+ stream, if it has one.
   */
  void set_hdr_dynamic_static_levels(encode_session_t &session, const SS_HDR_METADATA &metadata);

  /**
   * @brief Say once per `warned` that HDR10 streams from `encoder_name` carry no HDR10+
   *        metadata although the setting asks for it.
   */
  void warn_hdr_dynamic_unsupported(std::atomic_bool &warned, std::string_view encoder_name);

  /**
   * @brief Give `session` HDR10+ dynamic metadata when `enabled` (the setting) asks for it and
   *        the stream is PQ 10-bit HEVC or AV1 from an encoder whose device measures its frames.
   * @param video_format `config_t::videoFormat`.
   * @param colorspace The stream's colorspace.
   * @return True when the session carries it from now on. With `enabled` false the device is
   *         never asked, so it creates nothing.
   */
  bool attach_hdr_dynamic_stream(
    encode_session_t &session,
    bool enabled,
    int video_format,
    const sunshine_colorspace_t &colorspace,
    hdr_dynamic::encoder_e encoder,
    std::string_view encoder_name
  );

  /**
   * @brief Put `session`'s HDR10+ units into `packet` as it leaves the encoder.
   * @details The packet's bytes are copied once, into the packet itself. A frame that cannot
   *          carry them goes out unchanged, with one warning per stream, and so does every
   *          frame once the stream stopped. A stream that has carried only the static stand-in
   *          for hdr_dynamic_unmeasured_frames_warning frames says so once.
   */
  void splice_hdr_dynamic_units(encode_session_t &session, packet_raw_t &packet);

  /**
   * @brief Tell `session`'s device that the encoder took the last converted frame as
   *        `frame_index`, so its statistics pass reports under that frame.
   */
  void run_hdr_dynamic_pass(encode_session_t &session, std::int64_t frame_index);
}  // namespace video

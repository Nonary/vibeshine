/**
 * @file src/video_hdr_dynamic.cpp
 * @brief HDR10+ dynamic metadata between an encode session, its device and its packets.
 */
// this include
#include "video_hdr_dynamic.h"

// standard includes
#include <memory>
#include <utility>

// local includes
#include "logging.h"

using namespace std::literals;

namespace video {
  hdr_dynamic::static_levels_t hdr_dynamic_static_levels(const SS_HDR_METADATA &metadata) {
    return {metadata.maxContentLightLevel, metadata.maxDisplayLuminance, metadata.maxFrameAverageLightLevel};
  }

  void set_hdr_dynamic_static_levels(encode_session_t &session, const SS_HDR_METADATA &metadata) {
    if (session.hdr_dynamic) {
      session.hdr_dynamic->set_static_levels(hdr_dynamic_static_levels(metadata));
    }
  }

  void warn_hdr_dynamic_unsupported(std::atomic_bool &warned, std::string_view encoder_name) {
    if (!warned.exchange(true)) {
      BOOST_LOG(warning) << "HDR10+ dynamic metadata is not supported with the "sv << encoder_name
                         << " encoder; its HDR10 streams carry static metadata only"sv;
    }
  }

  bool attach_hdr_dynamic_stream(
    encode_session_t &session,
    bool enabled,
    int video_format,
    const sunshine_colorspace_t &colorspace,
    hdr_dynamic::encoder_e encoder,
    std::string_view encoder_name
  ) {
    session.hdr_dynamic.reset();
    const auto gate = hdr_dynamic::gate(enabled, video_format, colorspace_is_hdr(colorspace), colorspace.bit_depth, encoder);
    if (gate == hdr_dynamic::gate_e::unsupported) {
      static std::atomic_bool warned = false;
      warn_hdr_dynamic_unsupported(warned, encoder_name);
      return false;
    }
    if (gate != hdr_dynamic::gate_e::carry) {
      return false;
    }

    auto *device = session.encode_device();
    auto stream = std::make_shared<hdr_dynamic::stream_t>(
      *hdr_dynamic::codec_for(video_format),
      device && device->hdr_metadata_valid ? hdr_dynamic_static_levels(device->hdr_metadata) : hdr_dynamic::static_levels_t {}
    );
    if (!device || !device->attach_hdr_dynamic_metadata(stream)) {
      BOOST_LOG(error) << "HDR10+ dynamic metadata: the "sv << encoder_name
                       << " encoder cannot measure this stream's frames; it carries static HDR10 metadata only"sv;
      return false;
    }
    session.hdr_dynamic = std::move(stream);
    BOOST_LOG(info) << "HDR10+ dynamic metadata: on for this stream"sv;
    return true;
  }

  void splice_hdr_dynamic_units(encode_session_t &session, packet_raw_t &packet) {
    if (!session.hdr_dynamic) {
      return;
    }
    auto &stream = *session.hdr_dynamic;

    using enum hdr_dynamic::stream_t::splice_e;
    hdr_dynamic::stream_t::insertion_t insertion;
    switch (stream.prepare(packet.frame_index(), {packet.data(), packet.data_size()}, insertion)) {
      case spliced:
        if (!packet.insert_data(insertion.offset, insertion.units)) {
          BOOST_LOG(warning) << "HDR10+ dynamic metadata is off for the rest of this stream: a packet could not take the metadata"sv;
          stream.stop();
          return;
        }
        if (stream.counters().stand_in == hdr_dynamic_unmeasured_frames_warning) {
          BOOST_LOG(warning) << "HDR10+ dynamic metadata: "sv << hdr_dynamic_unmeasured_frames_warning
                             << " frames went out without a measurement; they carry a stand-in from the static HDR10 levels"sv;
        }
        break;
      case no_insert_point:
        if (stream.counters().no_insert_point == 1) {
          BOOST_LOG(warning) << "HDR10+ dynamic metadata: frame "sv << packet.frame_index()
                             << " has no place for it and goes out without; later frames like it are not reported"sv;
        }
        break;
      case empty:
      case stopped:
        break;
    }
  }

  void run_hdr_dynamic_pass(encode_session_t &session, std::int64_t frame_index) {
    if (!session.hdr_dynamic) {
      return;
    }
    if (auto *device = session.encode_device()) {
      device->run_hdr_dynamic_metadata_pass(frame_index);
    }
  }
}  // namespace video

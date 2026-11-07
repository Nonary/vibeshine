/**
 * @file tests/unit/test_video_hdr_dynamic.cpp
 * @brief HDR10+ dynamic metadata between encode sessions, their devices and their packets.
 */
#include "../tests_common.h"
#include "src/hdr_dynamic_metadata.h"
#include "src/video_hdr_dynamic.h"

#include <cstdint>
#include <cstring>
#include <memory>
#include <optional>
#include <span>
#include <sstream>
#include <string>
#include <vector>

#include <boost/core/null_deleter.hpp>
#include <boost/log/core.hpp>
#include <boost/log/sinks/sync_frontend.hpp>
#include <boost/log/sinks/text_ostream_backend.hpp>
#include <boost/make_shared.hpp>

namespace hdr = video::hdr_dynamic;
using namespace std::literals;

namespace {
  using bytes_t = std::vector<std::uint8_t>;

  /// An HEVC AUD and P slice, as encoders emit inter frames.
  const bytes_t hevc_frame {0, 0, 0, 1, 35 << 1, 0x01, 0x50, 0, 0, 0, 1, 1 << 1, 0x01, 0xD0, 0x12};

  constexpr video::sunshine_colorspace_t hdr10 {video::colorspace_e::bt2020, false, 10};

  /// What a stream of its own, with `levels`, makes of `frame` as frame `frame_index`.
  bytes_t reference_splice(std::int64_t frame_index, const hdr::static_levels_t &levels = {}) {
    hdr::stream_t stream {hdr::codec_e::hevc, levels};
    bytes_t out;
    EXPECT_EQ(stream.splice(frame_index, hevc_frame, out), hdr::stream_t::splice_e::spliced);
    EXPECT_GT(out.size(), hevc_frame.size());
    return out;
  }

  /// A frame of measured statistics, unlike any stand-in.
  hdr::frame_stats_t measured_stats() {
    hdr::frame_stats_t stats;
    stats.min_pq = 0.0f;
    stats.max_pq = hdr::nits_to_pq(600.0f);
    stats.mean_pq = hdr::nits_to_pq(50.0f);
    stats.mean_nits = 50.0f;
    for (std::size_t i = 0; i < stats.percentiles_pq.size(); ++i) {
      stats.percentiles_pq[i] = stats.max_pq * static_cast<float>(i + 1) / static_cast<float>(stats.percentiles_pq.size());
    }
    return stats;
  }

  struct fake_device_t: platf::encode_device_t {
    int convert(platf::img_t &) override {
      return 0;
    }

    bool attach_hdr_dynamic_metadata(std::shared_ptr<hdr::stream_t> attached) override {
      ++attaches;
      stream = std::move(attached);
      return measures;
    }

    void run_hdr_dynamic_metadata_pass(std::int64_t frame_index) override {
      passes.push_back(frame_index);
    }

    bool measures = true;  ///< Whether it has a statistics pass.
    int attaches = 0;
    std::shared_ptr<hdr::stream_t> stream;
    std::vector<std::int64_t> passes;
  };

  struct fake_session_t: video::encode_session_t {
    explicit fake_session_t(platf::encode_device_t *device):
        device {device} {
    }

    int convert(platf::img_t &) override {
      return 0;
    }

    void request_idr_frame() override {
    }

    void request_normal_frame() override {
    }

    bool invalidate_ref_frames(int64_t, int64_t) override {
      return true;
    }

    platf::encode_device_t *encode_device() override {
      return device;
    }

    platf::encode_device_t *device;
  };

  /// A packet that takes no insertion, as when its buffer cannot be allocated.
  struct full_packet_t: video::packet_raw_generic {
    using packet_raw_generic::packet_raw_generic;

    bool insert_data(size_t, std::span<const uint8_t>) override {
      return false;
    }
  };

  bytes_t bytes_of(video::packet_raw_t &packet) {
    return {packet.data(), packet.data() + packet.data_size()};
  }

  bool attach_hevc(fake_session_t &session) {
    return video::attach_hdr_dynamic_stream(session, true, 1, hdr10, hdr::encoder_e::hardware, "fake"sv);
  }

  /// The log records written while it lives.
  class log_capture_t {
  public:
    log_capture_t() {
      auto backend = boost::make_shared<backend_t>();
      backend->add_stream(boost::shared_ptr<std::ostream>(&stream, boost::null_deleter()));
      backend->auto_flush(true);
      sink = boost::make_shared<sink_t>(backend);
      boost::log::core::get()->add_sink(sink);
    }

    ~log_capture_t() {
      boost::log::core::get()->remove_sink(sink);
    }

    /// Records containing `text`.
    int count(const std::string &text) const {
      int found = 0;
      std::istringstream lines {stream.str()};
      for (std::string line; std::getline(lines, line);) {
        found += line.find(text) != std::string::npos ? 1 : 0;
      }
      return found;
    }

  private:
    using backend_t = boost::log::sinks::text_ostream_backend;
    using sink_t = boost::log::sinks::synchronous_sink<backend_t>;

    std::ostringstream stream;
    boost::shared_ptr<sink_t> sink;
  };
}  // namespace

TEST(VideoHdrDynamicAttach, TheSettingOffNeverReachesTheDevice) {
  fake_device_t device;
  fake_session_t session {&device};

  EXPECT_FALSE(video::attach_hdr_dynamic_stream(session, false, 1, hdr10, hdr::encoder_e::hardware, "fake"sv));
  EXPECT_EQ(device.attaches, 0) << "the device creates no shader, buffers or GPU work";
  EXPECT_FALSE(session.hdr_dynamic);

  video::run_hdr_dynamic_pass(session, 3);
  EXPECT_TRUE(device.passes.empty());
  video::packet_raw_generic packet {bytes_t {hevc_frame}, 3, false};
  video::splice_hdr_dynamic_units(session, packet);
  EXPECT_EQ(bytes_of(packet), hevc_frame);
}

TEST(VideoHdrDynamicAttach, OnlyPq10BitHevcAndAv1FromAMeasuringEncoderCarryIt) {
  struct case_t {
    const char *name;
    int video_format;
    video::sunshine_colorspace_t colorspace;
    hdr::encoder_e encoder;
    std::optional<hdr::codec_e> codec;
  };

  const case_t cases[] {
    {"H.264 HDR10", 0, hdr10, hdr::encoder_e::hardware, std::nullopt},
    {"HEVC SDR", 1, {video::colorspace_e::rec709, false, 8}, hdr::encoder_e::hardware, std::nullopt},
    {"HEVC 10-bit SDR", 1, {video::colorspace_e::bt2020sdr, false, 10}, hdr::encoder_e::hardware, std::nullopt},
    {"HEVC PQ 8-bit", 1, {video::colorspace_e::bt2020, false, 8}, hdr::encoder_e::hardware, std::nullopt},
    {"HEVC HDR10 software", 1, hdr10, hdr::encoder_e::software, std::nullopt},
    {"AV1 HDR10 PyroWave", 2, hdr10, hdr::encoder_e::pyrowave, std::nullopt},
    {"HEVC HDR10", 1, hdr10, hdr::encoder_e::hardware, hdr::codec_e::hevc},
    {"AV1 HDR10", 2, hdr10, hdr::encoder_e::hardware, hdr::codec_e::av1},
    {"AV1 HDR10 full range", 2, {video::colorspace_e::bt2020, true, 10}, hdr::encoder_e::hardware, hdr::codec_e::av1},
  };

  for (const auto &c : cases) {
    SCOPED_TRACE(c.name);
    fake_device_t device;
    fake_session_t session {&device};
    EXPECT_EQ(video::attach_hdr_dynamic_stream(session, true, c.video_format, c.colorspace, c.encoder, "fake"sv), c.codec.has_value());
    EXPECT_EQ(device.attaches, c.codec ? 1 : 0);
    ASSERT_EQ(static_cast<bool>(session.hdr_dynamic), c.codec.has_value());
    if (c.codec) {
      EXPECT_EQ(session.hdr_dynamic->codec(), *c.codec);
      EXPECT_EQ(device.stream, session.hdr_dynamic) << "the device measures for the session's stream";
    }
  }
}

TEST(VideoHdrDynamicAttach, AnEncoderThatCannotMeasureCarriesNone) {
  fake_device_t device;
  device.measures = false;
  fake_session_t session {&device};
  EXPECT_FALSE(attach_hevc(session));
  EXPECT_EQ(device.attaches, 1);
  EXPECT_FALSE(session.hdr_dynamic);

  fake_session_t without_device {nullptr};
  EXPECT_FALSE(attach_hevc(without_device));
  EXPECT_FALSE(without_device.hdr_dynamic);
}

TEST(VideoHdrDynamicAttach, TheStandInStartsFromTheDeviceStaticLevels) {
  fake_device_t device;
  device.hdr_metadata_valid = true;
  device.hdr_metadata.maxContentLightLevel = 1500;
  device.hdr_metadata.maxDisplayLuminance = 1000;
  device.hdr_metadata.maxFrameAverageLightLevel = 300;
  fake_session_t session {&device};
  ASSERT_TRUE(attach_hevc(session));

  const auto expected = reference_splice(0, {1500, 1000, 300});
  ASSERT_NE(expected, reference_splice(0)) << "the levels must show in the units";
  video::packet_raw_generic packet {bytes_t {hevc_frame}, 0, true};
  video::splice_hdr_dynamic_units(session, packet);
  EXPECT_EQ(bytes_of(packet), expected);

  // New HDR10 metadata reaches the stand-in too.
  SS_HDR_METADATA updated {};
  updated.maxContentLightLevel = 4000;
  video::set_hdr_dynamic_static_levels(session, updated);
  video::packet_raw_generic next {bytes_t {hevc_frame}, 1, false};
  video::splice_hdr_dynamic_units(session, next);
  EXPECT_EQ(bytes_of(next), reference_splice(1, {4000, 0, 0}));
}

TEST(VideoHdrDynamicPass, IsNamedAfterTheFrameTheEncoderTook) {
  fake_device_t device;
  fake_session_t session {&device};
  ASSERT_TRUE(attach_hevc(session));

  for (const std::int64_t frame : {0, 1, 2, 7}) {
    video::run_hdr_dynamic_pass(session, frame);
  }
  EXPECT_EQ(device.passes, (std::vector<std::int64_t> {0, 1, 2, 7}));

  // The device reports the statistics under the name it was given: frame 5's packet takes
  // them, frame 4's, encoded before, does not.
  device.stream->push(5, measured_stats());
  video::packet_raw_generic before {bytes_t {hevc_frame}, 4, false};
  video::splice_hdr_dynamic_units(session, before);
  EXPECT_EQ(bytes_of(before), reference_splice(4));
  EXPECT_EQ(session.hdr_dynamic->counters().samples, 0u);
  video::packet_raw_generic measured {bytes_t {hevc_frame}, 5, false};
  video::splice_hdr_dynamic_units(session, measured);
  EXPECT_NE(bytes_of(measured), reference_splice(5));
  EXPECT_EQ(session.hdr_dynamic->counters().samples, 1u);
}

TEST(VideoHdrDynamicSplice, AvcodecPacketTakesTheUnitsAndKeepsItsFrameAndFlags) {
  fake_device_t device;
  fake_session_t session {&device};
  ASSERT_TRUE(attach_hevc(session));

  video::packet_raw_avcodec packet;
  ASSERT_EQ(av_new_packet(packet.av_packet, static_cast<int>(hevc_frame.size())), 0);
  std::memcpy(packet.av_packet->data, hevc_frame.data(), hevc_frame.size());
  packet.av_packet->pts = 42;
  packet.av_packet->flags |= AV_PKT_FLAG_KEY;

  video::splice_hdr_dynamic_units(session, packet);

  const auto expected = reference_splice(42);
  ASSERT_EQ(packet.data_size(), expected.size());
  EXPECT_EQ(bytes_of(packet), expected);
  EXPECT_EQ(packet.frame_index(), 42);
  EXPECT_TRUE(packet.is_idr());
  // The packet owns the new bytes, padded with zeros as FFmpeg expects.
  ASSERT_NE(packet.av_packet->buf, nullptr);
  EXPECT_EQ(packet.av_packet->buf->data, packet.av_packet->data);
  ASSERT_GE(packet.av_packet->buf->size, expected.size() + AV_INPUT_BUFFER_PADDING_SIZE);
  for (std::size_t at = expected.size(); at < expected.size() + AV_INPUT_BUFFER_PADDING_SIZE; ++at) {
    EXPECT_EQ(packet.av_packet->data[at], 0) << at;
  }
}

TEST(VideoHdrDynamicSplice, GenericPacketTakesTheUnitsAndKeepsItsFrameAndFlags) {
  fake_device_t device;
  fake_session_t session {&device};
  ASSERT_TRUE(attach_hevc(session));

  video::packet_raw_generic packet {bytes_t {hevc_frame}, 7, true};
  video::splice_hdr_dynamic_units(session, packet);

  EXPECT_EQ(bytes_of(packet), reference_splice(7));
  EXPECT_EQ(packet.frame_index(), 7);
  EXPECT_TRUE(packet.is_idr());
}

TEST(VideoHdrDynamicSplice, PacketsInsertAtAnyOffsetAndRefuseOnePastTheEnd) {
  const bytes_t units {0xA1, 0xA2, 0xA3};
  const bytes_t frame {1, 2, 3, 4};
  for (const std::size_t offset : {std::size_t {0}, std::size_t {2}, frame.size()}) {
    SCOPED_TRACE(offset);
    bytes_t expected {frame};
    expected.insert(expected.begin() + static_cast<std::ptrdiff_t>(offset), units.begin(), units.end());

    video::packet_raw_generic generic {bytes_t {frame}, 1, false};
    EXPECT_TRUE(generic.insert_data(offset, units));
    EXPECT_EQ(bytes_of(generic), expected);

    video::packet_raw_avcodec avcodec;
    ASSERT_EQ(av_new_packet(avcodec.av_packet, static_cast<int>(frame.size())), 0);
    std::memcpy(avcodec.av_packet->data, frame.data(), frame.size());
    EXPECT_TRUE(avcodec.insert_data(offset, units));
    EXPECT_EQ(bytes_of(avcodec), expected);
  }

  video::packet_raw_generic generic {bytes_t {frame}, 1, false};
  EXPECT_FALSE(generic.insert_data(frame.size() + 1, units));
  EXPECT_EQ(bytes_of(generic), frame);
  video::packet_raw_avcodec avcodec;
  ASSERT_EQ(av_new_packet(avcodec.av_packet, static_cast<int>(frame.size())), 0);
  std::memcpy(avcodec.av_packet->data, frame.data(), frame.size());
  EXPECT_FALSE(avcodec.insert_data(frame.size() + 1, units));
  EXPECT_EQ(bytes_of(avcodec), frame);
}

TEST(VideoHdrDynamicSplice, AStoppedStreamLeavesPacketsAlone) {
  fake_device_t device;
  fake_session_t session {&device};
  ASSERT_TRUE(attach_hevc(session));
  session.hdr_dynamic->stop();

  video::packet_raw_generic packet {bytes_t {hevc_frame}, 1, false};
  video::splice_hdr_dynamic_units(session, packet);
  EXPECT_EQ(bytes_of(packet), hevc_frame);
}

TEST(VideoHdrDynamicSplice, APacketThatCannotTakeTheUnitsStopsTheStreamLoudly) {
  fake_device_t device;
  fake_session_t session {&device};
  ASSERT_TRUE(attach_hevc(session));
  log_capture_t log;

  full_packet_t packet {bytes_t {hevc_frame}, 1, false};
  video::splice_hdr_dynamic_units(session, packet);
  EXPECT_EQ(bytes_of(packet), hevc_frame);
  EXPECT_TRUE(session.hdr_dynamic->stopped());
  EXPECT_EQ(log.count("off for the rest of this stream"), 1);
}

TEST(VideoHdrDynamicSplice, AFrameWithoutAnInsertPointGoesOutUnchangedWithOneWarning) {
  fake_device_t device;
  fake_session_t session {&device};
  ASSERT_TRUE(attach_hevc(session));
  log_capture_t log;

  const bytes_t no_start_code {9, 9, 9, 9};
  for (std::int64_t frame = 0; frame < 3; ++frame) {
    video::packet_raw_generic packet {bytes_t {no_start_code}, frame, false};
    video::splice_hdr_dynamic_units(session, packet);
    EXPECT_EQ(bytes_of(packet), no_start_code);
  }
  EXPECT_EQ(session.hdr_dynamic->counters().no_insert_point, 3u);
  EXPECT_EQ(log.count("has no place for it"), 1);
}

TEST(VideoHdrDynamicSplice, SaysOnceWhenNoMeasurementArrives) {
  fake_device_t device;
  fake_session_t session {&device};
  ASSERT_TRUE(attach_hevc(session));
  log_capture_t log;

  const auto splice_frames = [&](std::int64_t from, std::int64_t to) {
    for (auto frame = from; frame < to; ++frame) {
      video::packet_raw_generic packet {bytes_t {hevc_frame}, frame, false};
      video::splice_hdr_dynamic_units(session, packet);
    }
  };
  const auto limit = static_cast<std::int64_t>(video::hdr_dynamic_unmeasured_frames_warning);
  splice_frames(0, limit - 1);
  EXPECT_EQ(log.count("without a measurement"), 0);
  splice_frames(limit - 1, limit);
  EXPECT_EQ(log.count("without a measurement"), 1);
  splice_frames(limit, 2 * limit + 1);
  EXPECT_EQ(log.count("without a measurement"), 1) << "said once";
}

TEST(VideoHdrDynamicSplice, StaysQuietWhenMeasurementsArrive) {
  fake_device_t device;
  fake_session_t session {&device};
  ASSERT_TRUE(attach_hevc(session));
  log_capture_t log;

  const auto limit = static_cast<std::int64_t>(video::hdr_dynamic_unmeasured_frames_warning);
  for (std::int64_t frame = 0; frame < 2 * limit; ++frame) {
    if (frame == 2) {
      device.stream->push(frame, measured_stats());
    }
    video::packet_raw_generic packet {bytes_t {hevc_frame}, frame, false};
    video::splice_hdr_dynamic_units(session, packet);
  }
  EXPECT_EQ(session.hdr_dynamic->counters().stand_in, 2u);
  EXPECT_EQ(log.count("without a measurement"), 0);
}

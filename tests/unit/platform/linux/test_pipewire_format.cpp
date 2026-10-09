/**
 * @file tests/unit/platform/linux/test_pipewire_format.cpp
 * @brief Exercise capture format compatibility using PipeWire's SPA filter.
 */
#include "../../../tests_common.h"

#include <array>
#include <spa/param/video/format-utils.h>
#include <spa/pod/filter.h>
#include <src/platform/linux/pipewire_format.h>

namespace {
  struct format_pod_t {
    std::array<uint8_t, 1024> storage {};
    spa_pod_builder builder = SPA_POD_BUILDER_INIT(storage.data(), static_cast<uint32_t>(storage.size()));
    spa_pod_frame frame {};

    explicit format_pod_t(int32_t format = SPA_VIDEO_FORMAT_BGRA) {
      const auto size = SPA_RECTANGLE(2560, 1440);
      spa_pod_builder_push_object(&builder, &frame, SPA_TYPE_OBJECT_Format, SPA_PARAM_EnumFormat);
      spa_pod_builder_add(&builder,
                          SPA_FORMAT_mediaType, SPA_POD_Id(SPA_MEDIA_TYPE_video),
                          SPA_FORMAT_mediaSubtype, SPA_POD_Id(SPA_MEDIA_SUBTYPE_raw),
                          SPA_FORMAT_VIDEO_format, SPA_POD_Id(format),
                          SPA_FORMAT_VIDEO_size, SPA_POD_Rectangle(&size), 0);
    }

    const spa_pod *finish() {
      return static_cast<spa_pod *>(spa_pod_builder_pop(&builder, &frame));
    }

    void add_producer_rates(uint32_t minimum, uint32_t maximum) {
      const auto variable_rate = SPA_FRACTION(0, 1);
      const auto minimum_rate = SPA_FRACTION(minimum, 1);
      const auto maximum_rate = SPA_FRACTION(maximum, 1);
      spa_pod_builder_add(&builder,
                          SPA_FORMAT_VIDEO_framerate, SPA_POD_Fraction(&variable_rate),
                          SPA_FORMAT_VIDEO_maxFramerate,
                          SPA_POD_CHOICE_RANGE_Fraction(&maximum_rate, &minimum_rate, &maximum_rate), 0);
    }
  };

  int negotiate(const spa_pod *producer, const spa_pod *consumer, spa_video_info_raw &format) {
    std::array<uint8_t, 2048> storage {};
    auto builder = SPA_POD_BUILDER_INIT(storage.data(), storage.size());
    spa_pod *result = nullptr;
    const int status = spa_pod_filter(&builder, &result, producer, consumer);
    if (status < 0) {
      return status;
    }
    spa_pod_fixate(result);
    return spa_format_video_raw_parse(result, &format);
  }
}  // namespace

TEST(PipewireFormat, AcceptsKwinFiniteMaximumWithVariableFrameRate) {
  // Native SteamOS KWin offers these rates for its 2560x1440, 119 Hz output.
  format_pod_t producer;
  producer.add_producer_rates(1, 119);
  format_pod_t consumer;
  pipewire::add_framerate_parameters(&consumer.builder, true);
  spa_video_info_raw format {};
  ASSERT_GE(negotiate(producer.finish(), consumer.finish(), format), 0);
  EXPECT_EQ(format.framerate.num, 0u);
  EXPECT_EQ(format.framerate.denom, 1u);
  EXPECT_EQ(format.max_framerate.num, 119u);
  EXPECT_EQ(format.max_framerate.denom, 1u);
}

TEST(PipewireFormat, PreservesUnpacedProducerSupport) {
  format_pod_t producer;
  producer.add_producer_rates(0, 0);
  format_pod_t consumer;
  pipewire::add_framerate_parameters(&consumer.builder, true);
  spa_video_info_raw format {};
  ASSERT_GE(negotiate(producer.finish(), consumer.finish(), format), 0);
  EXPECT_EQ(format.framerate.num, 0u);
  EXPECT_EQ(format.max_framerate.num, 0u);
}

TEST(PipewireFormat, CanOmitOptionalMaximumForGamescope) {
  format_pod_t consumer;
  pipewire::add_framerate_parameters(&consumer.builder, false);
  const auto *format = consumer.finish();
  EXPECT_NE(spa_pod_find_prop(format, nullptr, SPA_FORMAT_VIDEO_framerate), nullptr);
  EXPECT_EQ(spa_pod_find_prop(format, nullptr, SPA_FORMAT_VIDEO_maxFramerate), nullptr);
}

TEST(PipewireFormat, ZeroOnlyMaximumCannotPreserveKwinFiniteRate) {
  format_pod_t producer;
  producer.add_producer_rates(1, 119);
  format_pod_t consumer;
  consumer.add_producer_rates(0, 0);
  spa_video_info_raw format {};
  const auto status = negotiate(producer.finish(), consumer.finish(), format);
  if (status >= 0) {
    // SPA 1.0.5 accepts these disjoint fractional ranges but fixates the
    // consumer's zero maximum. Other versions reject them. Either outcome
    // prevents the old zero-only request from preserving KWin's finite rate.
    EXPECT_EQ(format.max_framerate.num, 0u);
    EXPECT_EQ(format.max_framerate.denom, 1u);
  }
}

TEST(PipewireFormat, MemoryOffersRejectPackedTenBitAndPreserveByteBgr) {
  for (const auto pixel_format : {SPA_VIDEO_FORMAT_xBGR_210LE, SPA_VIDEO_FORMAT_ARGB_210LE,
                                 SPA_VIDEO_FORMAT_ABGR_210LE, SPA_VIDEO_FORMAT_RGBA_102LE,
                                 SPA_VIDEO_FORMAT_BGRA_102LE, SPA_VIDEO_FORMAT_BGRA, SPA_VIDEO_FORMAT_BGRx}) {
    std::array<uint8_t, 2048> storage {};
    auto builder = SPA_POD_BUILDER_INIT(storage.data(), storage.size());
    const auto *consumer = pipewire::build_capture_format_parameter(&builder, 2560, 1440, pixel_format, nullptr, 0);
    if (pixel_format != SPA_VIDEO_FORMAT_BGRA && pixel_format != SPA_VIDEO_FORMAT_BGRx) {
      EXPECT_EQ(consumer, nullptr);
      continue;
    }
    ASSERT_NE(consumer, nullptr);
    EXPECT_EQ(spa_pod_find_prop(consumer, nullptr, SPA_FORMAT_VIDEO_modifier), nullptr);
    format_pod_t byte_producer {pixel_format};
    format_pod_t packed_producer {SPA_VIDEO_FORMAT_xBGR_210LE};
    spa_video_info_raw negotiated {};
    ASSERT_GE(negotiate(byte_producer.finish(), consumer, negotiated), 0);
    EXPECT_EQ(negotiated.format, pixel_format);
    EXPECT_LT(negotiate(packed_producer.finish(), consumer, negotiated), 0);
  }
}

TEST(PipewireFormat, ZeroModifierDmaCatalogEntryCannotBecomePackedMemoryOffer) {
  std::array<uint8_t, 2048> storage {};
  auto builder = SPA_POD_BUILDER_INIT(storage.data(), storage.size());
  const uint64_t modifier = 0; // A valid linear DMA-BUF modifier, if advertised.
  EXPECT_EQ(pipewire::build_capture_format_parameter(&builder, 2560, 1440, SPA_VIDEO_FORMAT_xBGR_210LE, &modifier, 0), nullptr);
  EXPECT_EQ(pipewire::build_capture_format_parameter(&builder, 2560, 1440, SPA_VIDEO_FORMAT_xBGR_210LE, nullptr, 1), nullptr);
  EXPECT_NE(pipewire::build_capture_format_parameter(&builder, 2560, 1440, SPA_VIDEO_FORMAT_BGRx, &modifier, 0), nullptr);
}

TEST(PipewireFormat, ImportableTenBitDmaRequiresProducerModifierAndKeepsHdrProfile) {
  std::array<uint8_t, 2048> storage {};
  auto builder = SPA_POD_BUILDER_INIT(storage.data(), storage.size());
  const uint64_t modifier = 0;
  const auto *consumer = pipewire::build_capture_format_parameter(&builder, 2560, 1440,
                                                                 SPA_VIDEO_FORMAT_xBGR_210LE, &modifier, 1,
                                                                 {.force_hdr10 = true});
  ASSERT_NE(consumer, nullptr);
  const auto *modifier_property = spa_pod_find_prop(consumer, nullptr, SPA_FORMAT_VIDEO_modifier);
  ASSERT_NE(modifier_property, nullptr);
  EXPECT_NE(modifier_property->flags & SPA_POD_PROP_FLAG_MANDATORY, 0u);

  format_pod_t memory_producer {SPA_VIDEO_FORMAT_xBGR_210LE};
  spa_video_info_raw negotiated {};
  EXPECT_LT(negotiate(memory_producer.finish(), consumer, negotiated), 0);

  format_pod_t dma_producer {SPA_VIDEO_FORMAT_xBGR_210LE};
  spa_pod_builder_add(&dma_producer.builder, SPA_FORMAT_VIDEO_modifier, SPA_POD_Long(modifier), 0);
  ASSERT_GE(negotiate(dma_producer.finish(), consumer, negotiated), 0);
  EXPECT_EQ(negotiated.format, SPA_VIDEO_FORMAT_xBGR_210LE);
  EXPECT_EQ(negotiated.modifier, modifier);
  EXPECT_EQ(negotiated.color_primaries, SPA_VIDEO_COLOR_PRIMARIES_BT2020);
  EXPECT_EQ(static_cast<int>(negotiated.transfer_function), 14); // SMPTE2084 on older SPA headers.
  EXPECT_EQ(negotiated.color_matrix, SPA_VIDEO_COLOR_MATRIX_RGB);
  EXPECT_EQ(negotiated.color_range, SPA_VIDEO_COLOR_RANGE_0_255);
}

TEST(PipewireFormat, ForcedHdrHasNoMemoryFallback) {
  std::array<uint8_t, 2048> storage {};
  auto builder = SPA_POD_BUILDER_INIT(storage.data(), storage.size());
  for (const auto format : {SPA_VIDEO_FORMAT_BGRA, SPA_VIDEO_FORMAT_BGRx, SPA_VIDEO_FORMAT_xBGR_210LE}) {
    EXPECT_EQ(pipewire::build_capture_format_parameter(&builder, 2560, 1440, format, nullptr, 0, {.force_hdr10 = true}), nullptr);
  }
}

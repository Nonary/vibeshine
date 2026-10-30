/**
 * @file tests/unit/test_hdr_dynamic_metadata.cpp
 * @brief HDR10+ payload, carriage, splice, statistics and frame synchronisation.
 */
#include "../tests_common.h"
#include "src/hdr_dynamic_metadata.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <optional>
#include <span>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace hdr = video::hdr_dynamic;

namespace {
  using bytes_t = std::vector<std::uint8_t>;

  bytes_t concat(std::initializer_list<bytes_t> parts) {
    bytes_t out;
    for (const auto &part : parts) {
      out.insert(out.end(), part.begin(), part.end());
    }
    return out;
  }

  /// MSB-first reader for the independent parsers below, written from the syntax tables
  /// (ST 2094-40 as FFmpeg parses it, H.265 7.3, AV1 5.3 and 5.8).
  class reader_t {
  public:
    explicit reader_t(std::span<const std::uint8_t> data):
        data {data} {}

    std::uint32_t bits(std::size_t count) {
      EXPECT_LE(at + count, data.size() * 8) << "read past the end";
      std::uint32_t value = 0;
      for (std::size_t i = 0; i < count && at < data.size() * 8; ++i, ++at) {
        value = (value << 1) | ((data[at / 8] >> (7 - at % 8)) & 1u);
      }
      return value;
    }

    /// Only zero padding to the next byte boundary may remain.
    void finish() {
      const auto left = data.size() * 8 - at;
      EXPECT_LT(left, 8u) << left << " bits left over";
      EXPECT_EQ(bits(left), 0u) << "padding must be zero";
    }

  private:
    std::span<const std::uint8_t> data;
    std::size_t at = 0;
  };

  struct parsed_hdr10plus_t {
    std::uint32_t application_version = 0;
    std::uint32_t num_windows = 0;
    std::uint32_t target = 0;
    bool target_actual_peak = false;
    std::array<std::uint32_t, 3> maxscl {};
    std::uint32_t average_maxrgb = 0;
    std::vector<std::pair<std::uint32_t, std::uint32_t>> distribution;
    std::uint32_t fraction_bright_pixels = 0;
    bool mastering_actual_peak = false;
    bool tone_mapping = false;
    bool saturation_mapping = false;
  };

  parsed_hdr10plus_t parse_hdr10plus(std::span<const std::uint8_t> t35) {
    reader_t r {t35};
    EXPECT_EQ(r.bits(8), 0xB5u) << "itu_t_t35_country_code";
    EXPECT_EQ(r.bits(16), 0x003Cu) << "itu_t_t35_terminal_provider_code";
    EXPECT_EQ(r.bits(16), 0x0001u) << "terminal_provider_oriented_code";
    EXPECT_EQ(r.bits(8), 4u) << "application_identifier";
    parsed_hdr10plus_t parsed;
    parsed.application_version = r.bits(8);
    parsed.num_windows = r.bits(2);
    EXPECT_EQ(parsed.num_windows, 1u) << "this parser covers one window";
    parsed.target = r.bits(27);
    parsed.target_actual_peak = r.bits(1) == 1;
    if (parsed.target_actual_peak) {
      const auto rows = r.bits(5);
      const auto columns = r.bits(5);
      for (std::uint32_t i = 0; i < rows * columns; ++i) {
        r.bits(4);
      }
    }
    for (auto &component : parsed.maxscl) {
      component = r.bits(17);
    }
    parsed.average_maxrgb = r.bits(17);
    const auto count = r.bits(4);
    for (std::uint32_t i = 0; i < count; ++i) {
      const auto percentage = r.bits(7);
      parsed.distribution.emplace_back(percentage, r.bits(17));
    }
    parsed.fraction_bright_pixels = r.bits(10);
    parsed.mastering_actual_peak = r.bits(1) == 1;
    if (parsed.mastering_actual_peak) {
      const auto rows = r.bits(5);
      const auto columns = r.bits(5);
      for (std::uint32_t i = 0; i < rows * columns; ++i) {
        r.bits(4);
      }
    }
    parsed.tone_mapping = r.bits(1) == 1;
    if (parsed.tone_mapping) {
      r.bits(12);
      r.bits(12);
      const auto anchors = r.bits(4);
      for (std::uint32_t i = 0; i < anchors; ++i) {
        r.bits(10);
      }
    }
    parsed.saturation_mapping = r.bits(1) == 1;
    if (parsed.saturation_mapping) {
      r.bits(6);
    }
    r.finish();
    return parsed;
  }

  /// Every SEI message in one Annex B prefix SEI NAL unit, as (payloadType, payload).
  std::vector<std::pair<std::size_t, bytes_t>> parse_hevc_sei(std::span<const std::uint8_t> nal) {
    std::size_t start = 0;
    if (nal.size() >= 4 && nal[0] == 0 && nal[1] == 0 && nal[2] == 0 && nal[3] == 1) {
      start = 4;
    } else if (nal.size() >= 3 && nal[0] == 0 && nal[1] == 0 && nal[2] == 1) {
      start = 3;
    } else {
      ADD_FAILURE() << "no start code";
      return {};
    }
    const auto body = nal.subspan(start);
    EXPECT_EQ(body[0] >> 7, 0) << "forbidden_zero_bit";
    EXPECT_EQ((body[0] >> 1) & 0x3F, 39) << "PREFIX_SEI_NUT";
    EXPECT_EQ(((body[0] & 1) << 5) | (body[1] >> 3), 0) << "nuh_layer_id";
    EXPECT_EQ(body[1] & 7, 1) << "nuh_temporal_id_plus1";
    bytes_t rbsp;
    int zeros = 0;
    for (const auto byte : body.subspan(2)) {
      if (zeros == 2) {
        EXPECT_GT(byte, 2) << "unescaped 00 00 " << int(byte);
        if (byte == 3) {
          zeros = 0;
          continue;  // emulation_prevention_three_byte
        }
      }
      rbsp.push_back(byte);
      zeros = byte == 0 ? zeros + 1 : 0;
    }
    std::vector<std::pair<std::size_t, bytes_t>> messages;
    std::size_t at = 0;
    const auto ff_coded = [&]() {
      std::size_t value = 0;
      while (at < rbsp.size()) {
        const auto byte = rbsp[at++];
        value += byte;
        if (byte != 0xFF) {
          break;
        }
      }
      return value;
    };
    while (at < rbsp.size() && !(at + 1 == rbsp.size() && rbsp[at] == 0x80)) {
      const auto kind = ff_coded();
      const auto size = ff_coded();
      if (at + size > rbsp.size()) {
        ADD_FAILURE() << "payload past the end";
        break;
      }
      messages.emplace_back(kind, bytes_t(rbsp.begin() + at, rbsp.begin() + at + size));
      at += size;
    }
    return messages;
  }

  std::uint64_t leb128(std::span<const std::uint8_t> data, std::size_t &at) {
    std::uint64_t value = 0;
    for (unsigned index = 0; index < 8 && at < data.size(); ++index) {
      const auto byte = data[at++];
      value |= static_cast<std::uint64_t>(byte & 0x7F) << (7 * index);
      if (!(byte & 0x80)) {
        break;
      }
    }
    return value;
  }

  /// (metadata_type, payload) of one AV1 metadata OBU, trailing bits removed like dav1d.
  std::pair<std::uint64_t, bytes_t> parse_av1_metadata(std::span<const std::uint8_t> obu) {
    EXPECT_EQ(obu[0] >> 7, 0) << "obu_forbidden_bit";
    EXPECT_EQ((obu[0] >> 3) & 0x0F, 5) << "OBU_METADATA";
    EXPECT_EQ(obu[0] & 0x04, 0) << "obu_extension_flag";
    EXPECT_EQ(obu[0] & 0x02, 0x02) << "obu_has_size_field";
    std::size_t at = 1;
    const auto size = leb128(obu, at);
    EXPECT_EQ(size, obu.size() - at) << "obu_size";
    const auto kind = leb128(obu, at);
    auto end = obu.size();
    while (end > at && obu[end - 1] == 0) {
      --end;
    }
    EXPECT_EQ(obu[end - 1], 0x80) << "trailing_one_bit";
    return {kind, bytes_t(obu.begin() + at, obu.begin() + end - 1)};
  }

  /// A bright game frame: 0.05 to 4000 nits, mean 180 nits, highlights near 1500.
  hdr::frame_stats_t sample() {
    constexpr std::array<float, 9> nits {0.5f, 2.0f, 5.0f, 20.0f, 80.0f, 200.0f, 600.0f, 900.0f, 1500.0f};
    hdr::frame_stats_t stats;
    stats.min_pq = hdr::nits_to_pq(0.05f);
    stats.mean_pq = hdr::nits_to_pq(60.0f);
    stats.max_pq = hdr::nits_to_pq(4000.0f);
    stats.mean_nits = 180.0f;
    for (std::size_t slot = 0; slot < nits.size(); ++slot) {
      stats.percentiles_pq[slot] = hdr::nits_to_pq(nits[slot]);
    }
    return stats;
  }

  /// What hdr10plus_tool's validation and ST 2094-40 Annex C ask of profile A: application 4
  /// version 1, one window, a zero target display with no curve, none of the optional peak
  /// or saturation fields, no bright-pixel fraction, and the nine percentages in order.
  void expect_profile_a(std::span<const std::uint8_t> t35) {
    const auto parsed = parse_hdr10plus(t35);
    EXPECT_EQ(parsed.application_version, 1u);
    EXPECT_EQ(parsed.num_windows, 1u);
    EXPECT_EQ(parsed.target, 0u) << "targeted_system_display_maximum_luminance must be zero for profile A";
    EXPECT_FALSE(parsed.tone_mapping) << "profile A carries no Bezier curve";
    EXPECT_FALSE(parsed.target_actual_peak);
    EXPECT_FALSE(parsed.mastering_actual_peak);
    EXPECT_FALSE(parsed.saturation_mapping);
    EXPECT_EQ(parsed.fraction_bright_pixels, 0u);
    ASSERT_EQ(parsed.distribution.size(), hdr::percentages.size());
    for (std::size_t slot = 0; slot < hdr::percentages.size(); ++slot) {
      EXPECT_EQ(parsed.distribution[slot].first, hdr::percentages[slot]);
      EXPECT_LE(parsed.distribution[slot].second, 100000u);
    }
    for (const auto value : parsed.maxscl) {
      EXPECT_LE(value, 100000u);
    }
    EXPECT_LE(parsed.average_maxrgb, 100000u);
  }

  bytes_t nal(std::uint8_t kind, bytes_t body) {
    bytes_t unit {0, 0, 0, 1, static_cast<std::uint8_t>(kind << 1), 0x01};
    unit.insert(unit.end(), body.begin(), body.end());
    return unit;
  }

  std::vector<int> nal_types(std::span<const std::uint8_t> frame) {
    std::vector<int> types;
    for (std::size_t at = 0; at + 3 < frame.size(); ++at) {
      if (frame[at] == 0 && frame[at + 1] == 0 && frame[at + 2] == 1) {
        types.push_back((frame[at + 3] >> 1) & 0x3F);
      }
    }
    return types;
  }

  bytes_t obu(std::uint8_t kind, bytes_t body) {
    bytes_t unit {static_cast<std::uint8_t>((kind << 3) | 0x02), static_cast<std::uint8_t>(body.size())};
    unit.insert(unit.end(), body.begin(), body.end());
    return unit;
  }

  bytes_t units_of(hdr::codec_e codec, const hdr::frame_stats_t &stats) {
    auto units = hdr::frame_units(codec, stats);
    EXPECT_TRUE(units.has_value());
    return units.value_or(bytes_t {});
  }
}  // namespace

TEST(HdrDynamicMetadata, PqMatchesSt2084ReferencePoints) {
  for (const auto [nits, pq] : {std::pair {100.0f, 0.508f}, std::pair {1000.0f, 0.752f}, std::pair {10000.0f, 1.0f}}) {
    EXPECT_NEAR(hdr::nits_to_pq(nits), pq, 0.001f) << nits;
    EXPECT_NEAR(hdr::pq_to_nits(hdr::nits_to_pq(nits)), nits, nits * 1e-3f) << nits;
  }
  EXPECT_EQ(hdr::nits_to_pq(std::numeric_limits<float>::quiet_NaN()), 0.0f);
  EXPECT_EQ(hdr::nits_to_pq(-5.0f), 0.0f);
  EXPECT_EQ(hdr::nits_to_pq(std::numeric_limits<float>::infinity()), 1.0f);
  EXPECT_EQ(hdr::pq_to_nits(std::numeric_limits<float>::quiet_NaN()), 0.0f);
}

TEST(HdrDynamicMetadata, Hdr10PlusGoldenBytesFromTheSyntaxTable) {
  // Derived by hand: num_windows 01, a zero 27-bit target (profile A), the nine 7-bit
  // percentages 24 bits apart, everything else zero, 331 bits padded to 42 bytes.
  bytes_t expected {0xB5, 0x00, 0x3C, 0x00, 0x01, 0x04, 0x01, 0x40, 0x00, 0x00, 0x00};
  expected.insert(expected.end(), 8, 0);
  for (const auto byte : {0x24, 0x08, 0, 0, 0x28, 0, 0, 0x50, 0, 0, 0xC8, 0, 0x01, 0x90, 0}) {
    expected.push_back(static_cast<std::uint8_t>(byte));
  }
  for (const auto byte : {0x02, 0x58, 0, 0x02, 0xD0, 0, 0x02, 0xF8, 0, 0x03, 0x18, 0, 0, 0, 0}) {
    expected.push_back(static_cast<std::uint8_t>(byte));
  }
  EXPECT_EQ(hdr::hdr10plus_t {}.to_t35(), expected);
  EXPECT_EQ(expected.size(), 49u);
}

TEST(HdrDynamicMetadata, Hdr10PlusRoundTripsThroughAnIndependentParser) {
  const auto metadata = hdr::hdr10plus_t::from_stats(sample()).value();
  const auto t35 = metadata.to_t35();
  EXPECT_EQ(t35.size(), 49u);
  const auto parsed = parse_hdr10plus(t35);
  EXPECT_EQ(parsed.application_version, 1u);
  EXPECT_EQ(parsed.target, 0u) << "profile A";
  EXPECT_EQ(parsed.maxscl, (std::array<std::uint32_t, 3> {metadata.maxscl, metadata.maxscl, metadata.maxscl}));
  EXPECT_EQ(parsed.average_maxrgb, 1800u) << "180 nits over 10000, in 1/100000";
  ASSERT_EQ(parsed.distribution.size(), 9u);
  for (std::size_t slot = 0; slot < 9; ++slot) {
    EXPECT_EQ(parsed.distribution[slot].first, hdr::percentages[slot]);
    EXPECT_EQ(parsed.distribution[slot].second, metadata.distribution[slot]);
  }
  EXPECT_EQ(parsed.fraction_bright_pixels, 0u);
  EXPECT_FALSE(parsed.tone_mapping);
  EXPECT_FALSE(parsed.saturation_mapping);
}

TEST(HdrDynamicMetadata, Hdr10PlusIsValidProfileAForEveryKindOfFrame) {
  auto bright = sample();
  bright.max_pq = 1.0f;
  bright.percentiles_pq.fill(1.0f);
  bright.mean_nits = 10000.0f;
  const auto stand_in = hdr::frame_stats_t::from_static({});
  for (const auto &stats : {sample(), hdr::frame_stats_t {}, bright, stand_in}) {
    expect_profile_a(hdr::hdr10plus_t::from_stats(stats).value().to_t35());
  }
  // The units as spliced: the SEI and the OBU carry the same profile A payload.
  for (const auto codec : {hdr::codec_e::hevc, hdr::codec_e::av1}) {
    hdr::writer_t writer {codec};
    const auto before = writer.units();
    ASSERT_TRUE(writer.sample(sample()));
    for (const auto &units : {before, writer.units()}) {
      const auto t35 = codec == hdr::codec_e::hevc ? parse_hevc_sei(units).at(0).second : parse_av1_metadata(units).second;
      expect_profile_a(t35);
    }
  }
}

TEST(HdrDynamicMetadata, LuminanceFieldsCapAtTenThousandNits) {
  // Values past the 17-bit field's 10000-nit range are written as 10000 nits.
  hdr::hdr10plus_t metadata;
  metadata.maxscl = 131071;
  metadata.average_maxrgb = 200000;
  metadata.distribution.fill(100001);
  const auto parsed = parse_hdr10plus(metadata.to_t35());
  EXPECT_EQ(parsed.maxscl, (std::array<std::uint32_t, 3> {100000, 100000, 100000}));
  EXPECT_EQ(parsed.average_maxrgb, 100000u);
  for (const auto &[percentage, value] : parsed.distribution) {
    EXPECT_EQ(value, 100000u) << int(percentage);
  }

  // Statistics at or past the PQ peak normalize to exactly 10000 nits.
  auto peak = sample();
  peak.max_pq = 1.0f;
  peak.percentiles_pq.fill(1.0f);
  peak.mean_nits = 10000.0f;
  const auto from_peak = hdr::hdr10plus_t::from_stats(peak).value();
  EXPECT_EQ(from_peak.maxscl, 100000u);
  EXPECT_EQ(from_peak.average_maxrgb, 100000u);
  EXPECT_EQ(from_peak.distribution[8], 100000u);

  // A static peak past PQ's range is held at 10000 nits.
  const auto stand_in = hdr::frame_stats_t::from_static({0xFFFF, 0, 0});
  EXPECT_TRUE(stand_in.is_valid());
  EXPECT_EQ(stand_in.max_pq, 1.0f);
  EXPECT_EQ(hdr::hdr10plus_t::from_stats(stand_in).value().maxscl, 100000u);
}

TEST(HdrDynamicMetadata, MaxsclIsTheMaximumAndTheReservedSlotsAreFixed) {
  const auto metadata = hdr::hdr10plus_t::from_stats(sample()).value();
  // ST 2094-40 maxSCL is the maximum: the 4000-nit peak, not P99's 1500 nits.
  EXPECT_NEAR(static_cast<double>(metadata.maxscl), 40000.0, 8.0);
  EXPECT_EQ(metadata.distribution[1], 0u) << "V1";
  EXPECT_EQ(metadata.distribution[2], 255u) << "V2";
  EXPECT_NEAR(static_cast<double>(metadata.distribution[4]), 800.0, 1.0) << "P50 80 nits";
  for (std::size_t slot = 3; slot < hdr::percentages.size(); ++slot) {
    EXPECT_LE(metadata.distribution[slot], metadata.maxscl) << slot;
  }

  // Statistics whose mean sits above their maximum still never report a maxSCL below the
  // average; a bright 5th percentile stays out of the reserved slot.
  auto stats = sample();
  stats.max_pq = hdr::nits_to_pq(300.0f);
  stats.percentiles_pq.fill(0.1f);
  stats.percentiles_pq[1] = 0.9f;
  stats.mean_nits = 400.0f;
  const auto above = hdr::hdr10plus_t::from_stats(stats).value();
  EXPECT_EQ(above.maxscl, 4000u) << "never below the average";
  EXPECT_EQ(above.distribution[1], 0u);

  auto broken = sample();
  broken.percentiles_pq[3] = std::numeric_limits<float>::quiet_NaN();
  EXPECT_FALSE(hdr::hdr10plus_t::from_stats(broken));
  broken = sample();
  broken.mean_nits = -1.0f;
  EXPECT_FALSE(hdr::hdr10plus_t::from_stats(broken));
  broken = sample();
  broken.mean_nits = 10000.5f;
  EXPECT_FALSE(hdr::hdr10plus_t::from_stats(broken));
  broken = sample();
  broken.max_pq = 1.01f;
  EXPECT_FALSE(hdr::hdr10plus_t::from_stats(broken));
}

TEST(HdrDynamicMetadata, HevcSeiWrapsAndEscapesThePayload) {
  const bytes_t plain {0xB5, 0x00, 0x3C, 0x12, 0x34};
  bytes_t expected {0, 0, 0, 1, 0x4E, 0x01, 0x04, 5};
  expected.insert(expected.end(), plain.begin(), plain.end());
  expected.push_back(0x80);
  EXPECT_EQ(hdr::hevc_t35_sei(plain), expected) << "nothing to escape";

  // The all-zero HDR10+ payload has long zero runs.
  const auto zeros = hdr::hdr10plus_t {}.to_t35();
  const auto zero_nal = hdr::hevc_t35_sei(zeros);
  EXPECT_EQ(bytes_t(zero_nal.begin(), zero_nal.begin() + 8), (bytes_t {0, 0, 0, 1, 0x4E, 0x01, 0x04, 49}));
  EXPECT_GT(zero_nal.size(), 8 + zeros.size() + 1) << "emulation prevention bytes inserted";
  EXPECT_EQ(parse_hevc_sei(zero_nal), (std::vector<std::pair<std::size_t, bytes_t>> {{4, zeros}}));

  const auto hdr10plus = hdr::hdr10plus_t::from_stats(sample()).value().to_t35();
  EXPECT_EQ(parse_hevc_sei(hdr::hevc_t35_sei(hdr10plus)), (std::vector<std::pair<std::size_t, bytes_t>> {{4, hdr10plus}}));

  // payloadSize of 255 or more takes 0xFF continuation bytes.
  const bytes_t long_payload(300, 0xB5);
  const auto long_nal = hdr::hevc_t35_sei(long_payload);
  EXPECT_EQ(bytes_t(long_nal.begin() + 6, long_nal.begin() + 9), (bytes_t {0x04, 0xFF, 45}));
  EXPECT_EQ(parse_hevc_sei(long_nal), (std::vector<std::pair<std::size_t, bytes_t>> {{4, long_payload}}));
}

TEST(HdrDynamicMetadata, EmulationPreventionCoversEveryStartCodePrefix) {
  const std::vector<std::pair<bytes_t, bytes_t>> cases {
    {{0, 0, 0}, {0, 0, 3, 0}},
    {{0, 0, 1}, {0, 0, 3, 1}},
    {{0, 0, 2}, {0, 0, 3, 2}},
    {{0, 0, 3}, {0, 0, 3, 3}},
    {{0, 0, 4}, {0, 0, 4}},
    {{0, 0, 0, 0, 0, 0}, {0, 0, 3, 0, 0, 3, 0, 0}},
    {{0, 1, 0, 0, 0}, {0, 1, 0, 0, 3, 0}},
    {{0, 0, 0x80}, {0, 0, 0x80}},
  };
  for (const auto &[rbsp, escaped] : cases) {
    bytes_t out;
    hdr::escape(rbsp, out);
    EXPECT_EQ(out, escaped);
  }
}

TEST(HdrDynamicMetadata, Av1ObuCarriesThePayloadWithItsSize) {
  const bytes_t plain {0xB5, 0x00, 0x3C, 0x12, 0x34};
  bytes_t expected {0x2A, 7, 0x04};
  expected.insert(expected.end(), plain.begin(), plain.end());
  expected.push_back(0x80);
  EXPECT_EQ(hdr::av1_t35_obu(plain), expected);

  const auto hdr10plus = hdr::hdr10plus_t::from_stats(sample()).value().to_t35();
  EXPECT_EQ(parse_av1_metadata(hdr::av1_t35_obu(hdr10plus)), (std::pair<std::uint64_t, bytes_t> {4, hdr10plus}));
  // Sizes of 128 or more take a second leb128 byte.
  const bytes_t long_payload(200, 0xB5);
  const auto long_obu = hdr::av1_t35_obu(long_payload);
  EXPECT_EQ(bytes_t(long_obu.begin() + 1, long_obu.begin() + 3), (bytes_t {0xCA, 0x01})) << "202 as leb128";
  EXPECT_EQ(parse_av1_metadata(long_obu), (std::pair<std::uint64_t, bytes_t> {4, long_payload}));
}

TEST(HdrDynamicMetadata, HevcInsertOffsetLandsBeforeTheFirstSlice) {
  const auto aud = nal(35, {0x50});
  const auto vps = nal(32, {0x0C, 0x01, 0xFF, 0xFF, 0x01, 0x60});
  const auto sps = nal(33, {0x01, 0x01, 0x60, 0x00, 0x00, 0x03, 0x00, 0x90});
  const auto pps = nal(34, {0xC1, 0x72, 0xB4});
  const auto sei = nal(39, {0x05, 0x02, 0xAA, 0xBB, 0x80});
  const auto idr = nal(19, {0xAF, 0x00, 0x00, 0x03, 0x01, 0x12, 0x34});
  const auto p_slice = nal(1, {0xD0, 0x00, 0x00, 0x03, 0x00, 0x56});

  const auto headers = concat({aud, vps, sps, pps});
  const auto keyframe = concat({headers, idr});
  EXPECT_EQ(hdr::insert_offset(hdr::codec_e::hevc, keyframe), headers.size());

  const auto no_aud = concat({vps, sps, pps, idr});
  EXPECT_EQ(hdr::insert_offset(hdr::codec_e::hevc, no_aud), no_aud.size() - idr.size());

  const auto with_sei = concat({headers, sei, idr});
  EXPECT_EQ(hdr::insert_offset(hdr::codec_e::hevc, with_sei), with_sei.size() - idr.size());

  EXPECT_EQ(hdr::insert_offset(hdr::codec_e::hevc, p_slice), 0u);
  const auto aud_p = concat({aud, p_slice});
  EXPECT_EQ(hdr::insert_offset(hdr::codec_e::hevc, aud_p), aud.size());

  // Three-byte start codes: before the slice's own start code.
  const auto short_codes = concat({bytes_t(aud.begin() + 1, aud.end()), bytes_t(p_slice.begin() + 1, p_slice.end())});
  EXPECT_EQ(hdr::insert_offset(hdr::codec_e::hevc, short_codes), aud.size() - 1);

  const auto hdr10plus = hdr::hdr10plus_t::from_stats(sample()).value().to_t35();
  const auto hdr10plus_nal = hdr::hevc_t35_sei(hdr10plus);
  const auto units = units_of(hdr::codec_e::hevc, sample());
  EXPECT_EQ(units, hdr10plus_nal);
  auto spliced = keyframe;
  ASSERT_TRUE(hdr::splice(hdr::codec_e::hevc, spliced, units));
  EXPECT_EQ(nal_types(spliced), (std::vector<int> {35, 32, 33, 34, 39, 19}));
  EXPECT_EQ(spliced, concat({headers, hdr10plus_nal, idr}));
}

TEST(HdrDynamicMetadata, HevcWithoutAnInsertionPointStaysUnchanged) {
  const auto units = units_of(hdr::codec_e::hevc, sample());
  const auto parameter_sets = concat({nal(32, {0x0C}), nal(33, {0x01}), nal(34, {0xC1})});
  const bytes_t length_prefixed {0, 0, 0, 7, 0x26, 0x01, 0xAF, 0x12, 0x34, 0x56, 0x78};
  auto forbidden = nal(1, {0xD0});
  forbidden[4] |= 0x80;
  const bytes_t truncated {0, 0, 0, 1};
  for (const auto &frame : {parameter_sets, length_prefixed, forbidden, truncated, bytes_t {}}) {
    auto copy = frame;
    EXPECT_FALSE(hdr::splice(hdr::codec_e::hevc, copy, units));
    EXPECT_EQ(copy, frame);
    EXPECT_FALSE(hdr::spliced(hdr::codec_e::hevc, frame, units));
  }
  auto frame = nal(1, {0xD0});
  EXPECT_FALSE(hdr::splice(hdr::codec_e::hevc, frame, {})) << "nothing to insert";
  EXPECT_EQ(frame, nal(1, {0xD0}));
}

TEST(HdrDynamicMetadata, Av1InsertOffsetLandsAfterTheSequenceHeader) {
  const auto td = obu(2, {});
  const auto seq = obu(1, {0x00, 0x00, 0x00, 0x0A, 0x0B, 0xF0, 0x00});
  const auto frame_obu = obu(6, {0x10, 0x00, 0x00, 0x00, 0x01, 0x02});
  const auto keyframe = concat({td, seq, frame_obu});
  EXPECT_EQ(hdr::insert_offset(hdr::codec_e::av1, keyframe), td.size() + seq.size());
  const auto inter = concat({td, frame_obu});
  EXPECT_EQ(hdr::insert_offset(hdr::codec_e::av1, inter), td.size());
  const auto split = concat({td, obu(3, {0x30}), obu(4, {0x00, 0x01})});
  EXPECT_EQ(hdr::insert_offset(hdr::codec_e::av1, split), td.size());
  // An extension byte is skipped with the header.
  auto extended = bytes_t {(1 << 3) | 0x06, 0x00, 1, 0xAA};
  extended.insert(extended.end(), frame_obu.begin(), frame_obu.end());
  EXPECT_EQ(hdr::insert_offset(hdr::codec_e::av1, extended), 4u);

  const auto units = units_of(hdr::codec_e::av1, sample());
  const auto hdr10plus = hdr::hdr10plus_t::from_stats(sample()).value().to_t35();
  EXPECT_EQ(units, hdr::av1_t35_obu(hdr10plus));
  auto spliced = keyframe;
  ASSERT_TRUE(hdr::splice(hdr::codec_e::av1, spliced, units));
  const auto at = td.size() + seq.size();
  EXPECT_EQ(bytes_t(spliced.begin(), spliced.begin() + at), bytes_t(keyframe.begin(), keyframe.begin() + at));
  EXPECT_EQ(parse_av1_metadata(std::span(spliced).subspan(at, units.size())), (std::pair<std::uint64_t, bytes_t> {4, hdr10plus}));
  EXPECT_EQ(bytes_t(spliced.begin() + at + units.size(), spliced.end()), frame_obu);
}

TEST(HdrDynamicMetadata, Av1WithoutAnInsertionPointStaysUnchanged) {
  const auto units = units_of(hdr::codec_e::av1, sample());
  const auto td = obu(2, {});
  const auto headers_only = concat({td, obu(1, {0x00, 0x0A})});
  const auto sizeless = concat({td, bytes_t {1 << 3, 0x00, 0x0A}});
  const auto truncated = concat({td, bytes_t {(1 << 3) | 0x02, 40, 0x00}});
  const auto endless_leb = concat({td, bytes_t {(1 << 3) | 0x02, 0xFF, 0xFF}});
  // A size of zero padded to eight leb128 bytes is still valid; nine are not.
  const auto padded_size = [&](std::size_t bytes) {
    auto frame = td;
    frame.push_back((1 << 3) | 0x02);
    frame.insert(frame.end(), bytes - 1, 0x80);
    frame.push_back(0x00);
    return concat({frame, obu(6, {0x10, 0x20})});
  };
  EXPECT_EQ(hdr::insert_offset(hdr::codec_e::av1, padded_size(8)), td.size() + 9);
  const auto overlong_leb = padded_size(9);
  const auto forbidden = concat({td, bytes_t {0x80 | (6 << 3) | 0x02, 0}});
  const auto extension_cut = concat({td, bytes_t {(1 << 3) | 0x06}});
  for (const auto &frame : {headers_only, sizeless, truncated, endless_leb, overlong_leb, forbidden, extension_cut, bytes_t {}}) {
    auto copy = frame;
    EXPECT_FALSE(hdr::splice(hdr::codec_e::av1, copy, units));
    EXPECT_EQ(copy, frame);
  }
}

TEST(HdrDynamicMetadata, SplicedCopiesAsSpliceInserts) {
  const auto headers = concat({nal(32, {0x0C}), nal(33, {0x01}), nal(34, {0xC1})});
  const auto keyframe = concat({headers, nal(19, {0xAF, 0x12})});
  const auto units = units_of(hdr::codec_e::hevc, sample());
  auto expected = keyframe;
  ASSERT_TRUE(hdr::splice(hdr::codec_e::hevc, expected, units));
  const auto copy = hdr::spliced(hdr::codec_e::hevc, keyframe, units);
  ASSERT_TRUE(copy);
  EXPECT_EQ(*copy, expected);
  EXPECT_EQ(copy->capacity(), keyframe.size() + units.size()) << "one allocation";
  EXPECT_FALSE(hdr::spliced(hdr::codec_e::hevc, keyframe, {}));
  EXPECT_FALSE(hdr::spliced(hdr::codec_e::hevc, headers, units));

  const auto av1 = concat({obu(2, {}), obu(1, {0x0A, 0x0B}), obu(6, {0x10, 0x20})});
  const auto av1_units = units_of(hdr::codec_e::av1, sample());
  auto av1_expected = av1;
  ASSERT_TRUE(hdr::splice(hdr::codec_e::av1, av1_expected, av1_units));
  EXPECT_EQ(hdr::spliced(hdr::codec_e::av1, av1, av1_units), av1_expected);
}

TEST(HdrDynamicMetadata, HistogramStatisticsFollowTheBins) {
  // 990 samples at PQ 0.25, ten at PQ 0.75: P99 still in the dark bin.
  std::vector<std::uint32_t> bins(1024, 0);
  bins[255] = 990;
  bins[767] = 10;
  const auto stats = hdr::frame_stats_t::from_histogram(bins, 0.2495f, 0.7495f).value();
  EXPECT_TRUE(stats.is_valid());
  for (std::size_t slot = 0; slot < 9; ++slot) {
    EXPECT_EQ(stats.percentiles_pq[slot], 0.25f) << slot;
  }
  const double mean_pq = (990.0 * 255.5 + 10.0 * 767.5) / 1000.0 / 1024.0;
  EXPECT_NEAR(stats.mean_pq, mean_pq, 1e-5);
  const double mean_nits = (990.0 * hdr::pq_to_nits(255.5f / 1024.0f) + 10.0 * hdr::pq_to_nits(767.5f / 1024.0f)) / 1000.0;
  EXPECT_NEAR(stats.mean_nits, mean_nits, 0.01);
  EXPECT_GT(stats.mean_nits, hdr::pq_to_nits(stats.mean_pq)) << "linear mean above PQ mean";

  bins[767] = 11;
  const auto brighter = hdr::frame_stats_t::from_histogram(bins, 0.2495f, 0.7495f).value();
  EXPECT_EQ(brighter.percentiles_pq[8], 0.7495f) << "kept at the frame maximum";

  std::vector<std::uint32_t> single(16, 0);
  single[0] = 4;
  EXPECT_EQ(hdr::frame_stats_t::from_histogram(single, 0.0f, 0.0f), hdr::frame_stats_t {}) << "kept inside min and max";

  EXPECT_FALSE(hdr::frame_stats_t::from_histogram(std::vector<std::uint32_t>(1024, 0), 0.0f, 1.0f));
  EXPECT_FALSE(hdr::frame_stats_t::from_histogram(bins, 0.5f, 0.4f));
  EXPECT_FALSE(hdr::frame_stats_t::from_histogram(bins, std::numeric_limits<float>::quiet_NaN(), 1.0f));
}

TEST(HdrDynamicMetadata, TheStandInFollowsTheStaticLevels) {
  const auto near = [](float nits, float expected) {
    return std::abs(nits - expected) <= expected * 2e-3f;
  };
  // MaxCLL first, MaxFALL as the mean.
  auto stats = hdr::frame_stats_t::from_static({800, 1000, 200});
  EXPECT_TRUE(stats.is_valid());
  EXPECT_EQ(stats.min_pq, 0.0f);
  EXPECT_EQ(stats.max_pq, hdr::nits_to_pq(800.0f));
  EXPECT_EQ(stats.percentiles_pq[8], stats.max_pq) << "P99 at the peak";
  EXPECT_TRUE(std::is_sorted(stats.percentiles_pq.begin(), stats.percentiles_pq.end()));
  EXPECT_EQ(stats.mean_nits, 200.0f);
  const auto metadata = hdr::hdr10plus_t::from_stats(stats).value();
  EXPECT_NEAR(static_cast<double>(metadata.maxscl), 8000.0, 2.0);
  EXPECT_EQ(metadata.average_maxrgb, 2000u);

  // Without light levels: the mastering peak, and the spread's own linear mean.
  stats = hdr::frame_stats_t::from_static({0, 600, 0});
  EXPECT_TRUE(near(hdr::pq_to_nits(stats.max_pq), 600.0f));
  EXPECT_GT(stats.mean_nits, 0.0f);
  EXPECT_LT(stats.mean_nits, 600.0f);
  // A MaxFALL above the peak is not taken.
  EXPECT_LT(hdr::frame_stats_t::from_static({500, 1000, 900}).mean_nits, 500.0f);
  // No peak at all: 1000 nits.
  EXPECT_TRUE(near(hdr::pq_to_nits(hdr::frame_stats_t::from_static({}).max_pq), 1000.0f));
}

TEST(HdrDynamicMetadata, MaxsclFallsWithTheFramesAfterABriefPeak) {
  // A dim scene, one frame with a small 10000-nit highlight, then the dim scene again.
  auto dim = sample();
  dim.max_pq = hdr::nits_to_pq(1600.0f);
  auto highlight = dim;
  highlight.max_pq = 1.0f;

  hdr::writer_t writer {hdr::codec_e::hevc};
  ASSERT_TRUE(writer.sample(dim));
  const auto peak = writer.sample(highlight).value();
  ASSERT_FALSE(peak.scene_cut);
  EXPECT_NEAR(static_cast<double>(hdr::hdr10plus_t::from_stats(peak.stats).value().maxscl), 100000.0, 2.0);

  for (int frame = 0; frame < 3; ++frame) {
    SCOPED_TRACE(frame);
    const auto smoothed = writer.sample(dim).value();
    ASSERT_FALSE(smoothed.scene_cut);
    EXPECT_EQ(smoothed.stats.max_pq, dim.max_pq) << "the dark frames' own maximum";
    const auto metadata = hdr::hdr10plus_t::from_stats(smoothed.stats).value();
    // 1600 nits, the frames' own peak: P99 (1500 nits) and the rest stay below it.
    EXPECT_NEAR(static_cast<double>(metadata.maxscl), 16000.0, 2.0);
    EXPECT_EQ(writer.units(), hdr::frame_units(hdr::codec_e::hevc, smoothed.stats).value());
  }
}

TEST(HdrDynamicMetadata, SmoothingFollowsSlowlyAndSnapsAtASceneCut) {
  hdr::stats_smoother_t smoother;
  const auto first = smoother.update(sample()).value();
  EXPECT_TRUE(first.scene_cut) << "the first sample is taken as it is";
  EXPECT_EQ(first.stats, sample());

  // A small change between samples: smoothed, not a cut.
  auto brighter = sample();
  brighter.mean_pq += 0.03f;
  brighter.percentiles_pq[8] += 0.05f;
  const auto step = smoother.update(brighter).value();
  EXPECT_FALSE(step.scene_cut);
  EXPECT_NEAR(step.stats.mean_pq, sample().mean_pq + hdr::stats_smoother_t::alpha * 0.03f, 1e-6);
  EXPECT_LT(step.stats.percentiles_pq[8], brighter.percentiles_pq[8]);

  // A lone peak (cursor, subtitle) is not a cut either, but the maximum is taken at once:
  // maxSCL never sits below a pixel of the frame it describes.
  auto peak = brighter;
  peak.max_pq = 1.0f;
  const auto with_peak = smoother.update(peak).value();
  EXPECT_FALSE(with_peak.scene_cut);
  EXPECT_EQ(with_peak.stats.max_pq, 1.0f);
  EXPECT_LT(with_peak.stats.mean_pq, peak.mean_pq) << "everything else is still smoothed";

  // When the peak is gone the maximum drops with it at once, not over the smoothing's decay.
  const auto after_peak = smoother.update(brighter).value();
  EXPECT_FALSE(after_peak.scene_cut);
  EXPECT_EQ(after_peak.stats.max_pq, brighter.max_pq);

  // Cut to a dark scene: snaps to the new statistics.
  hdr::frame_stats_t dark;
  dark.mean_pq = 0.1f;
  dark.max_pq = 0.3f;
  dark.mean_nits = 0.5f;
  dark.percentiles_pq = {0.02f, 0.03f, 0.04f, 0.06f, 0.1f, 0.14f, 0.2f, 0.25f, 0.3f};
  const auto cut = smoother.update(dark).value();
  EXPECT_TRUE(cut.scene_cut);
  EXPECT_EQ(cut.stats, dark);

  // Invalid samples change nothing: neither the smoothed state nor the cut reference.
  auto nan = dark;
  nan.mean_pq = std::numeric_limits<float>::quiet_NaN();
  EXPECT_FALSE(smoother.update(nan));
  auto bright_but_broken = sample();
  bright_but_broken.min_pq = -0.1f;
  EXPECT_FALSE(smoother.update(bright_but_broken));
  const auto after = smoother.update(dark).value();
  EXPECT_FALSE(after.scene_cut) << "compared with the last valid sample";
  EXPECT_EQ(after.stats, dark);

  smoother.reset();
  const auto fresh = smoother.update(sample()).value();
  EXPECT_TRUE(fresh.scene_cut) << "a reset starts over";
  EXPECT_EQ(fresh.stats, sample());
}

TEST(HdrDynamicMetadata, FrameUnitsRejectInvalidStatistics) {
  auto broken = sample();
  broken.min_pq = -0.1f;
  EXPECT_FALSE(hdr::frame_units(hdr::codec_e::hevc, broken));
  EXPECT_FALSE(hdr::frame_units(hdr::codec_e::av1, broken));
  EXPECT_EQ(hdr::codec_for(1), hdr::codec_e::hevc);
  EXPECT_EQ(hdr::codec_for(2), hdr::codec_e::av1);
  EXPECT_FALSE(hdr::codec_for(0)) << "H.264 carries none";
  EXPECT_FALSE(hdr::codec_for(3));
}

TEST(HdrDynamicMetadata, WriterKeepsTheLastSampleUntilTheNext) {
  hdr::writer_t writer {hdr::codec_e::hevc};
  const auto default_stand_in = hdr::frame_stats_t::from_static({});
  EXPECT_EQ(writer.units(), units_of(hdr::codec_e::hevc, default_stand_in)) << "the first frames carry the stand-in";
  EXPECT_FALSE(writer.measured());
  // An invalid first sample keeps the stand-in.
  auto broken = sample();
  broken.max_pq = std::numeric_limits<float>::quiet_NaN();
  EXPECT_FALSE(writer.sample(broken));
  EXPECT_FALSE(writer.measured());
  // Static levels rebuild the stand-in until the first sample.
  const hdr::static_levels_t levels {450, 600, 0};
  writer.set_static_levels(levels);
  EXPECT_EQ(writer.units(), units_of(hdr::codec_e::hevc, hdr::frame_stats_t::from_static(levels)));
  EXPECT_TRUE(writer.sample(sample()).value().scene_cut);
  EXPECT_TRUE(writer.measured());
  EXPECT_EQ(writer.units(), units_of(hdr::codec_e::hevc, sample()));
  // After it, measurements stand.
  writer.set_static_levels({});
  EXPECT_EQ(writer.units(), units_of(hdr::codec_e::hevc, sample()));

  // Invalid statistics keep the units.
  const auto before = writer.units();
  EXPECT_FALSE(writer.sample(broken));
  EXPECT_EQ(writer.units(), before);

  // A small change is smoothed into the units, not taken as it is.
  auto brighter = sample();
  brighter.mean_pq += 0.03f;
  const auto step = writer.sample(brighter).value();
  EXPECT_FALSE(step.scene_cut);
  EXPECT_EQ(writer.units(), units_of(hdr::codec_e::hevc, step.stats));

  hdr::writer_t av1 {hdr::codec_e::av1};
  EXPECT_EQ(av1.units(), units_of(hdr::codec_e::av1, default_stand_in));
  ASSERT_TRUE(av1.sample(sample()));
  EXPECT_EQ(av1.codec(), hdr::codec_e::av1);
  EXPECT_EQ(av1.units(), units_of(hdr::codec_e::av1, sample()));
}

namespace {
  /// The GPU readout of a set of PQ(maxRGB) samples, binned as the shader bins them.
  std::vector<std::uint32_t> readout_of(std::initializer_list<float> samples) {
    std::vector<std::uint32_t> words(hdr::readout_words, 0);
    std::uint32_t min_code = 0xFFFFFFFF;
    std::uint32_t max_code = 0;
    for (const float pq : samples) {
      const auto code = std::bit_cast<std::uint32_t>(pq);
      min_code = std::min(min_code, code);
      max_code = std::max(max_code, code);
      ++words[2 + std::min(static_cast<std::size_t>(pq * 1024.0f), std::size_t {1023})];
    }
    words[0] = ~min_code;
    words[1] = max_code;
    return words;
  }
}  // namespace

TEST(HdrDynamicMetadata, ReadoutParsesConsistentWordsOnly) {
  const auto words = readout_of({0.1f, 0.2f, 0.2f, 0.75f, 1.0f});
  const auto stats = hdr::parse_readout(words, 5);
  ASSERT_TRUE(stats) << stats.error();
  EXPECT_EQ(stats->min_pq, 0.1f);
  EXPECT_EQ(stats->max_pq, 1.0f);
  EXPECT_TRUE(stats->is_valid());
  EXPECT_EQ(stats->percentiles_pq[8], 1.0f) << "the top bin ends at the maximum";

  // A black frame: the minimum and maximum are both zero.
  const auto black = hdr::parse_readout(readout_of({0.0f, 0.0f}), 2);
  ASSERT_TRUE(black) << black.error();
  EXPECT_EQ(black->max_pq, 0.0f);

  EXPECT_FALSE(hdr::parse_readout(words, 6)) << "a sample missing";
  EXPECT_FALSE(hdr::parse_readout(std::span(words).first(100), 5)) << "short";
  EXPECT_FALSE(hdr::parse_readout(std::vector<std::uint32_t>(hdr::readout_words, 0), 0)) << "a zeroed buffer";
  auto wrong_min = words;
  wrong_min[0] = ~std::bit_cast<std::uint32_t>(0.3f);
  EXPECT_FALSE(hdr::parse_readout(wrong_min, 5)) << "the minimum outside its bin";
  auto wrong_max = words;
  wrong_max[1] = std::bit_cast<std::uint32_t>(0.5f);
  EXPECT_FALSE(hdr::parse_readout(wrong_max, 5)) << "the maximum outside its bin";
  auto nan_max = words;
  nan_max[1] = 0x7FC00000;
  EXPECT_FALSE(hdr::parse_readout(nan_max, 5));
}

TEST(HdrDynamicMetadata, SampleGridHasAtMost240By135Cells) {
  EXPECT_EQ(hdr::sample_grid(3840, 2160), (std::pair<std::uint32_t, std::uint32_t> {240, 135}));
  EXPECT_EQ(hdr::sample_grid(100, 50), (std::pair<std::uint32_t, std::uint32_t> {100, 50}));
  EXPECT_EQ(hdr::sample_grid(0, 0), (std::pair<std::uint32_t, std::uint32_t> {1, 1}));
}

TEST(HdrDynamicMetadataGate, OnlyPq10BitHevcAndAv1FromHardwareEncodersCarryIt) {
  using enum hdr::gate_e;
  using hdr::encoder_e;
  constexpr int h264 = 0;
  constexpr int hevc = 1;
  constexpr int av1 = 2;

  EXPECT_EQ(hdr::gate(true, hevc, true, 10, encoder_e::hardware), carry);
  EXPECT_EQ(hdr::gate(true, av1, true, 10, encoder_e::hardware), carry);

  // The setting is off by default and then nothing else matters.
  for (const auto encoder : {encoder_e::hardware, encoder_e::software, encoder_e::pyrowave}) {
    EXPECT_EQ(hdr::gate(false, hevc, true, 10, encoder), off);
    EXPECT_EQ(hdr::gate(false, av1, true, 10, encoder), off);
  }

  // H.264, SDR and 10-bit SDR never carry it, without a warning.
  EXPECT_EQ(hdr::gate(true, h264, true, 10, encoder_e::hardware), not_hdr10);
  EXPECT_EQ(hdr::gate(true, h264, true, 10, encoder_e::software), not_hdr10);
  for (const auto format : {h264, hevc, av1}) {
    EXPECT_EQ(hdr::gate(true, format, false, 8, encoder_e::hardware), not_hdr10);
    EXPECT_EQ(hdr::gate(true, format, false, 10, encoder_e::hardware), not_hdr10);
    EXPECT_EQ(hdr::gate(true, format, false, 10, encoder_e::pyrowave), not_hdr10);
  }
  EXPECT_EQ(hdr::gate(true, hevc, true, 8, encoder_e::hardware), not_hdr10);

  // An HDR10 stream that should carry it but cannot is reported.
  EXPECT_EQ(hdr::gate(true, hevc, true, 10, encoder_e::software), unsupported);
  EXPECT_EQ(hdr::gate(true, av1, true, 10, encoder_e::software), unsupported);
  for (const auto format : {h264, hevc, av1, 3}) {
    EXPECT_EQ(hdr::gate(true, format, true, 10, encoder_e::pyrowave), unsupported);
  }
}

namespace {
  /// An AUD and a P slice, as encoders emit inter frames.
  bytes_t hevc_frame() {
    return concat({nal(35, {0x50}), nal(1, {0xD0, 0x12})});
  }

  /// The HDR10+ payload a spliced hevc_frame() carries.
  hdr::hdr10plus_t carried(std::span<const std::uint8_t> frame) {
    const auto rest = frame.subspan(nal(35, {0x50}).size());
    // The SEI runs up to the slice's start code.
    std::size_t end = 4;
    while (end + 4 <= rest.size() && !(rest[end] == 0 && rest[end + 1] == 0 && rest[end + 2] == 0 && rest[end + 3] == 1)) {
      ++end;
    }
    const auto messages = parse_hevc_sei(rest.first(end));
    EXPECT_EQ(messages.size(), 1u);
    const auto parsed = parse_hdr10plus(messages.at(0).second);
    hdr::hdr10plus_t metadata;
    metadata.maxscl = parsed.maxscl[0];
    metadata.average_maxrgb = parsed.average_maxrgb;
    for (std::size_t slot = 0; slot < parsed.distribution.size(); ++slot) {
      metadata.distribution[slot] = parsed.distribution[slot].second;
    }
    return metadata;
  }

  /// A scene whose mean sits at mean_nits; two far apart are a scene cut.
  hdr::frame_stats_t scene(float mean_nits) {
    auto stats = sample();
    stats.mean_nits = mean_nits;
    stats.mean_pq = hdr::nits_to_pq(mean_nits);
    return stats;
  }
}  // namespace

TEST(HdrDynamicMetadataStream, FramesBeforeTheFirstSampleCarryTheStandIn) {
  hdr::stream_t stream {hdr::codec_e::hevc, {1200, 1000, 300}};
  bytes_t out;
  ASSERT_EQ(stream.splice(0, hevc_frame(), out), hdr::stream_t::splice_e::spliced);
  const auto metadata = carried(out);
  EXPECT_EQ(metadata.maxscl, 12000u) << "MaxCLL as maxSCL";
  EXPECT_EQ(metadata.average_maxrgb, 3000u) << "MaxFALL as the average";
  EXPECT_EQ(stream.counters().stand_in, 1u);
}

TEST(HdrDynamicMetadataStream, APacketTakesTheNewestSampleAtOrBeforeItsFrame) {
  hdr::stream_t stream {hdr::codec_e::hevc};
  bytes_t out;
  // Samples finish after their frames' packets: frame 1's sample arrives with frame 3's.
  stream.push(1, scene(100.0f));
  stream.push(3, scene(5000.0f));  // A cut: a much brighter scene starts at frame 3.

  ASSERT_EQ(stream.splice(2, hevc_frame(), out), hdr::stream_t::splice_e::spliced);
  EXPECT_EQ(carried(out).average_maxrgb, 1000u) << "frame 2 carries frame 1's sample, never frame 3's";

  ASSERT_EQ(stream.splice(3, hevc_frame(), out), hdr::stream_t::splice_e::spliced);
  EXPECT_EQ(carried(out).average_maxrgb, 50000u) << "the cut is taken as it is from its own frame on";

  // No new sample: the units stay.
  ASSERT_EQ(stream.splice(4, hevc_frame(), out), hdr::stream_t::splice_e::spliced);
  EXPECT_EQ(carried(out).average_maxrgb, 50000u);

  // A late packet for an older frame takes no sample from the future.
  stream.push(6, scene(100.0f));
  ASSERT_EQ(stream.splice(5, hevc_frame(), out), hdr::stream_t::splice_e::spliced);
  EXPECT_EQ(carried(out).average_maxrgb, 50000u);

  const auto counters = stream.counters();
  EXPECT_EQ(counters.frames, 4u);
  EXPECT_EQ(counters.samples, 2u);
  EXPECT_EQ(counters.stand_in, 0u);
}

TEST(HdrDynamicMetadataStream, SamplesAreSmoothedOncePerSampleNotPerFrame) {
  hdr::stream_t stream {hdr::codec_e::hevc};
  hdr::writer_t reference {hdr::codec_e::hevc};
  bytes_t out;
  stream.push(0, sample());
  ASSERT_TRUE(reference.sample(sample()));
  auto brighter = sample();
  brighter.mean_pq += 0.03f;
  brighter.mean_nits = 200.0f;
  stream.push(1, brighter);
  ASSERT_TRUE(reference.sample(brighter));
  for (std::int64_t frame = 1; frame < 5; ++frame) {
    ASSERT_EQ(stream.splice(frame, hevc_frame(), out), hdr::stream_t::splice_e::spliced);
    EXPECT_EQ(out, concat({nal(35, {0x50}), reference.units(), nal(1, {0xD0, 0x12})})) << frame;
  }
}

TEST(HdrDynamicMetadataStream, OutOfOrderAndInvalidSamplesAreDropped) {
  hdr::stream_t stream {hdr::codec_e::hevc};
  bytes_t out;
  stream.push(5, scene(100.0f));
  stream.push(5, scene(9000.0f));  // the same frame again
  stream.push(4, scene(9000.0f));  // an older frame
  auto broken = sample();
  broken.mean_nits = std::numeric_limits<float>::quiet_NaN();
  stream.push(6, broken);
  ASSERT_EQ(stream.splice(6, hevc_frame(), out), hdr::stream_t::splice_e::spliced);
  EXPECT_EQ(carried(out).average_maxrgb, 1000u);
  const auto counters = stream.counters();
  EXPECT_EQ(counters.samples, 1u);
  EXPECT_EQ(counters.invalid_samples, 1u);
}

TEST(HdrDynamicMetadataStream, PendingSamplesAreBounded) {
  hdr::stream_t stream {hdr::codec_e::hevc};
  bytes_t out;
  const auto pushed = static_cast<std::int64_t>(hdr::stream_t::max_pending) + 10;
  for (std::int64_t frame = 0; frame < pushed; ++frame) {
    stream.push(frame, scene(100.0f + static_cast<float>(frame)));
  }
  ASSERT_EQ(stream.splice(pushed, hevc_frame(), out), hdr::stream_t::splice_e::spliced);
  EXPECT_EQ(stream.counters().samples, hdr::stream_t::max_pending);
}

TEST(HdrDynamicMetadataStream, EmptyUnspliceableAndStoppedFramesPassUnchanged) {
  hdr::stream_t stream {hdr::codec_e::hevc};
  bytes_t out {0xEE};
  EXPECT_EQ(stream.splice(0, {}, out), hdr::stream_t::splice_e::empty);
  const auto parameter_sets = concat({nal(32, {0x0C}), nal(33, {0x01})});
  EXPECT_EQ(stream.splice(1, parameter_sets, out), hdr::stream_t::splice_e::no_insert_point);
  EXPECT_EQ(out, bytes_t {0xEE}) << "out untouched";
  EXPECT_EQ(stream.counters(), (hdr::stream_t::counters_t {0, 0, 0, 0, 1})) << "an empty packet is not a frame";

  stream.push(2, sample());
  stream.stop();
  EXPECT_TRUE(stream.stopped());
  EXPECT_EQ(stream.splice(2, hevc_frame(), out), hdr::stream_t::splice_e::stopped);
  EXPECT_EQ(stream.counters().samples, 0u) << "stopping drops the pending samples";
}

TEST(HdrDynamicMetadataStream, PrepareNamesWhatSpliceInserts) {
  hdr::stream_t prepared {hdr::codec_e::hevc, {1200, 1000, 300}};
  hdr::stream_t spliced {hdr::codec_e::hevc, {1200, 1000, 300}};
  const auto frame = hevc_frame();
  for (std::int64_t index = 0; index < 3; ++index) {
    if (index == 1) {
      prepared.push(1, sample());
      spliced.push(1, sample());
    }
    hdr::stream_t::insertion_t insertion;
    ASSERT_EQ(prepared.prepare(index, frame, insertion), hdr::stream_t::splice_e::spliced);
    bytes_t built(frame.begin(), frame.begin() + static_cast<std::ptrdiff_t>(insertion.offset));
    built.insert(built.end(), insertion.units.begin(), insertion.units.end());
    built.insert(built.end(), frame.begin() + static_cast<std::ptrdiff_t>(insertion.offset), frame.end());
    bytes_t out;
    ASSERT_EQ(spliced.splice(index, frame, out), hdr::stream_t::splice_e::spliced);
    EXPECT_EQ(built, out) << index;
  }
  EXPECT_EQ(prepared.counters(), spliced.counters());

  hdr::stream_t::insertion_t untouched {7, {0xEE}};
  EXPECT_EQ(prepared.prepare(3, {}, untouched), hdr::stream_t::splice_e::empty);
  EXPECT_EQ(prepared.prepare(3, concat({nal(32, {0x0C})}), untouched), hdr::stream_t::splice_e::no_insert_point);
  EXPECT_EQ(untouched.offset, 7u);
  EXPECT_EQ(untouched.units, bytes_t {0xEE});
}

TEST(HdrDynamicMetadataStream, Av1FramesGetTheMetadataObu) {
  hdr::stream_t stream {hdr::codec_e::av1};
  bytes_t out;
  stream.push(0, sample());
  const auto frame = concat({obu(2, {}), obu(6, {0x10, 0x20})});
  ASSERT_EQ(stream.splice(0, frame, out), hdr::stream_t::splice_e::spliced);
  EXPECT_EQ(out, concat({obu(2, {}), units_of(hdr::codec_e::av1, sample()), obu(6, {0x10, 0x20})}));
}

// FFmpeg reads the payload back from real encoded frames. Needs ffmpeg (with libx265 and
// libaom) and ffprobe on PATH; skipped without them.
namespace {
#ifdef _WIN32
  constexpr const char *null_device = "NUL";
#else
  constexpr const char *null_device = "/dev/null";
#endif

  bool runs(const std::string &command) {
    return std::system((command + " >" + null_device + " 2>&1").c_str()) == 0;
  }

  std::string quoted(const std::filesystem::path &path) {
    return '"' + path.string() + '"';
  }

  /// One frame of a test pattern; HEVC with an AUD before it, as hardware encoders write it.
  bool encode(hdr::codec_e codec, const std::filesystem::path &path) {
    const std::string encoder = codec == hdr::codec_e::hevc ?
                                  "-c:v libx265 -x265-params log-level=error:aud=1 -f hevc" :
                                  "-c:v libaom-av1 -cpu-used 8 -f obu";
    return runs(
      "ffmpeg -hide_banner -loglevel error -y -f lavfi -i testsrc2=size=64x64:rate=10 -frames:v 1 "
      "-pix_fmt yuv420p10le " +
      encoder + " " + quoted(path)
    );
  }

  using side_data_t = std::vector<std::pair<std::string, std::string>>;

  /// Key-value lines of each side data entry, in ffprobe's order with repeated keys kept.
  std::vector<side_data_t> side_data(const std::filesystem::path &path) {
    const auto command = "ffprobe -hide_banner -loglevel error -show_frames " + quoted(path);
#ifdef _WIN32
    FILE *pipe = _popen(command.c_str(), "r");
#else
    FILE *pipe = popen(command.c_str(), "r");
#endif
    if (!pipe) {
      ADD_FAILURE() << "ffprobe did not start";
      return {};
    }
    std::string text;
    char buffer[4096];
    while (const auto read = std::fread(buffer, 1, sizeof(buffer), pipe)) {
      text.append(buffer, read);
    }
#ifdef _WIN32
    EXPECT_EQ(_pclose(pipe), 0);
#else
    EXPECT_EQ(pclose(pipe), 0);
#endif
    std::vector<side_data_t> entries;
    std::optional<side_data_t> current;
    std::istringstream lines {text};
    for (std::string line; std::getline(lines, line);) {
      while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) {
        line.pop_back();
      }
      if (line == "[SIDE_DATA]") {
        current.emplace();
      } else if (line == "[/SIDE_DATA]") {
        if (current) {
          entries.push_back(std::move(*current));
        }
        current.reset();
      } else if (const auto equals = line.find('='); current && equals != std::string::npos) {
        current->emplace_back(line.substr(0, equals), line.substr(equals + 1));
      }
    }
    return entries;
  }

  std::vector<std::string> values(const side_data_t &entry, const std::string &key) {
    std::vector<std::string> out;
    for (const auto &[name, value] : entry) {
      if (name == key) {
        out.push_back(value);
      }
    }
    return out;
  }

  void expect_ffprobe_reads_hdr10plus(hdr::codec_e codec, const std::string &name) {
    if (!runs("ffprobe -version") || !runs("ffmpeg -version")) {
      GTEST_SKIP() << "ffmpeg and ffprobe are not on PATH";
    }
    const auto dir = std::filesystem::temp_directory_path();
    const auto plain = dir / ("vibeshine-hdr10plus-" + name + ".plain");
    const auto tagged = dir / ("vibeshine-hdr10plus-" + name);
    if (!encode(codec, plain)) {
      GTEST_SKIP() << "ffmpeg cannot encode " << name << " (libx265 or libaom missing)";
    }
    bytes_t stream;
    {
      std::ifstream in {plain, std::ios::binary};
      stream.assign(std::istreambuf_iterator<char> {in}, {});
    }
    ASSERT_FALSE(stream.empty());
    ASSERT_TRUE(hdr::splice(codec, stream, units_of(codec, sample())));
    {
      std::ofstream out {tagged, std::ios::binary};
      out.write(reinterpret_cast<const char *>(stream.data()), static_cast<std::streamsize>(stream.size()));
    }

    const auto entries = side_data(tagged);
    std::error_code ignored;
    std::filesystem::remove(plain, ignored);
    std::filesystem::remove(tagged, ignored);
    const auto entry = std::find_if(entries.begin(), entries.end(), [](const side_data_t &entry) {
      return values(entry, "side_data_type") == std::vector<std::string> {"HDR Dynamic Metadata SMPTE2094-40 (HDR10+)"};
    });
    ASSERT_NE(entry, entries.end()) << "no HDR10+ side data";

    const auto expected = hdr::hdr10plus_t::from_stats(sample()).value();
    const auto fraction = [](std::uint32_t value) {
      return std::to_string(value) + "/100000";
    };
    EXPECT_EQ(values(*entry, "application version"), std::vector<std::string> {"1"});
    EXPECT_EQ(values(*entry, "num_windows"), std::vector<std::string> {"1"});
    EXPECT_EQ(values(*entry, "targeted_system_display_maximum_luminance"), std::vector<std::string> {"0/1"}) << "profile A names no target display";
    EXPECT_EQ(values(*entry, "maxscl"), std::vector<std::string>(3, fraction(expected.maxscl)));
    EXPECT_EQ(values(*entry, "average_maxrgb"), std::vector<std::string> {fraction(expected.average_maxrgb)});
    EXPECT_EQ(values(*entry, "num_distribution_maxrgb_percentiles"), std::vector<std::string> {"9"});
    std::vector<std::string> percentages;
    std::vector<std::string> percentiles;
    for (std::size_t slot = 0; slot < hdr::percentages.size(); ++slot) {
      percentages.push_back(std::to_string(hdr::percentages[slot]));
      percentiles.push_back(fraction(expected.distribution[slot]));
    }
    EXPECT_EQ(values(*entry, "distribution_maxrgb_percentage"), percentages);
    EXPECT_EQ(values(*entry, "distribution_maxrgb_percentile"), percentiles);
    EXPECT_EQ(values(*entry, "fraction_bright_pixels"), std::vector<std::string> {"0/1000"});
  }
}  // namespace

TEST(HdrDynamicMetadataFfprobe, ReadsHdr10PlusFromHevc) {
  expect_ffprobe_reads_hdr10plus(hdr::codec_e::hevc, "frame.hevc");
}

TEST(HdrDynamicMetadataFfprobe, ReadsHdr10PlusFromAv1) {
  expect_ffprobe_reads_hdr10plus(hdr::codec_e::av1, "frame.obu");
}

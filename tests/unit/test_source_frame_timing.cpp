/**
 * @file tests/unit/test_source_frame_timing.cpp
 * @brief Source-image intervals survive encoding and packet scheduling.
 */
#include "../tests_common.h"

#include <src/video_rtp_timing.h>
#include <src/platform/windows/wgc_capture_policy.h>
#include <src/platform/windows/wgc_source_clock.h>

#include <array>
#include <limits>

namespace {
  using namespace std::chrono_literals;
  using source_clock = video::rtp_timing::clock_t;
  using video::rtp_timing::stamp;
  const auto epoch = source_clock::time_point {100s};
}

TEST(SourceFrameTiming, UnevenSourceIntervalsAndRealStallsReachRtp) {
  // Whole 90 kHz ticks: uneven content cadence must survive unchanged,
  // including a real source stall. This is not a fitted capture metronome.
  const std::array offsets {10ms, 17ms, 27ms, 35ms, 85ms};
  const std::array<std::uint32_t, 4> expected_gaps {630, 900, 720, 4500};
  std::uint32_t previous = 0;
  for (std::size_t i = 0; i < offsets.size(); ++i) {
    const auto source = epoch + offsets[i];
    const auto result = stamp(source, epoch + 1s + i * 20ms, epoch);
    EXPECT_EQ(result.source_timestamp, source);
    EXPECT_FALSE(result.synthetic);
    if (i) EXPECT_EQ(result.rtp_timestamp - previous, expected_gaps[i - 1]);
    previous = result.rtp_timestamp;
  }
}

TEST(SourceFrameTiming, EncodingDelayAndPacketizationOrderCannotRetimestampImages) {
  const auto first_image = epoch + 10ms;
  const auto second_image = epoch + 20ms;
  // Packetization can be delayed or an encoder can emit a different frame
  // than the newest submitted one. Each packet retains its own source value.
  const auto second = stamp(second_image, epoch + 300ms, epoch);
  const auto first = stamp(first_image, epoch + 400ms, epoch);
  EXPECT_EQ(first.rtp_timestamp, 900u);
  EXPECT_EQ(second.rtp_timestamp, 1800u);
  EXPECT_EQ(stamp(first_image, epoch + 500ms, epoch).rtp_timestamp, first.rtp_timestamp);
}

TEST(SourceFrameTiming, DuplicateFallbackDoesNotChangeSourceMetadataOrNextGap) {
  const std::optional<source_clock::time_point> missing;
  const auto first = stamp(epoch + 10ms, epoch + 100ms, epoch);
  const auto dupe = stamp(missing, epoch + 15ms, epoch);
  const auto next = stamp(epoch + 20ms, epoch + 200ms, epoch);
  EXPECT_TRUE(dupe.synthetic);
  EXPECT_EQ(dupe.source_timestamp, epoch + 15ms);
  EXPECT_EQ(dupe.rtp_timestamp, 1350u);
  EXPECT_FALSE(missing.has_value());
  EXPECT_EQ(next.rtp_timestamp - first.rtp_timestamp, 900u);
  EXPECT_FALSE(next.synthetic);
}

TEST(SourceFrameTiming, AbsoluteEpochDoesNotChangeIntervals) {
  const auto shifted_epoch = epoch + 24h;
  const auto a = stamp(epoch + 10ms, epoch, epoch);
  const auto b = stamp(epoch + 17ms, epoch, epoch);
  const auto shifted_a = stamp(shifted_epoch + 10ms, shifted_epoch, shifted_epoch);
  const auto shifted_b = stamp(shifted_epoch + 17ms, shifted_epoch, shifted_epoch);
  EXPECT_EQ(b.rtp_timestamp - a.rtp_timestamp, shifted_b.rtp_timestamp - shifted_a.rtp_timestamp);
}

TEST(SourceFrameTiming, RtpWrapAndRoundingPreserveIntervals) {
  using tick = std::chrono::duration<std::int64_t, std::ratio<1, 90000>>;
  const auto near_wrap = std::chrono::duration_cast<source_clock::duration>(tick {0xffffffffLL});
  const auto first = stamp(epoch + near_wrap, epoch, epoch);
  const auto second = stamp(epoch + near_wrap + 10ms, epoch, epoch);
  EXPECT_EQ(first.rtp_timestamp, 0xffffffffu);
  EXPECT_EQ(second.rtp_timestamp - first.rtp_timestamp, 900u);
  EXPECT_EQ(stamp(epoch + 1us, epoch, epoch).rtp_timestamp, 0u);
  EXPECT_EQ(stamp(epoch + 6us, epoch, epoch).rtp_timestamp, 1u);
}

TEST(WgcSourceClock, FixedCorrelationPreservesGapsDespiteCaptureDelayAndClockOffset) {
  using platf::dxgi::wgc_policy::source_clock_t;
  // A deliberately late correlation changes only the epoch, not the gaps.
  const source_clock_t clock {1000000, epoch + 7ms};
  const source_clock_t shifted_clock {1000000, epoch + 20ms};
  const auto difference = [](std::int64_t a, std::int64_t b) { return (a - b) * 100ns; };
  const std::array<std::uint64_t, 4> frames {900000, 983333, 1083333, 1583333};
  const std::array expected {8333300ns, 10ms + 0ns, 50ms + 0ns};
  auto previous = clock.timestamp(frames[0], difference);
  auto shifted_previous = shifted_clock.timestamp(frames[0], difference);
  ASSERT_TRUE(previous);
  ASSERT_TRUE(shifted_previous);
  for (std::size_t i = 1; i < frames.size(); ++i) {
    // Read much later than the source event, on a highly irregular schedule.
    // This value can affect duplicate fallback but cannot affect real timing.
    const auto read_time = epoch + i * i * 100ms;
    const auto current = clock.timestamp(frames[i], difference);
    const auto shifted = shifted_clock.timestamp(frames[i], difference);
    ASSERT_TRUE(current);
    ASSERT_TRUE(shifted);
    EXPECT_EQ(*current - *previous, expected[i - 1]);
    EXPECT_EQ(*shifted - *shifted_previous, expected[i - 1]);
    EXPECT_EQ(stamp(current, read_time, epoch).rtp_timestamp - stamp(previous, epoch, epoch).rtp_timestamp,
              stamp(shifted, read_time, epoch).rtp_timestamp - stamp(shifted_previous, epoch, epoch).rtp_timestamp);
    previous = current;
    shifted_previous = shifted;
  }
}

TEST(WgcSourceClock, MissingOrInvalidSourceTimeCannotBecomeCaptureTime) {
  using platf::dxgi::wgc_policy::source_clock_t;
  const auto difference = [](std::int64_t a, std::int64_t b) { return (a - b) * 100ns; };
  const source_clock_t clock {1000000, epoch};
  EXPECT_FALSE(clock.timestamp(0, difference));
  EXPECT_FALSE(clock.timestamp(std::numeric_limits<std::uint64_t>::max(), difference));
  EXPECT_FALSE(source_clock_t(0, epoch).timestamp(1000, difference));
}

TEST(WgcCapturePolicy, SystemRelativeTimeConvertsToPerformanceCounterTicks) {
  using platf::dxgi::wgc_policy::system_relative_time_to_qpc;
  EXPECT_EQ(system_relative_time_to_qpc(12345678, 10000000), 12345678u);
  EXPECT_EQ(system_relative_time_to_qpc(10000000, 24000000), 24000000u);
  EXPECT_EQ(system_relative_time_to_qpc(15000000, 24000000), 36000000u);
  EXPECT_EQ(system_relative_time_to_qpc(0, 24000000), 0u);
  EXPECT_EQ(system_relative_time_to_qpc(864000000000000, 24000000), 2073600000000000u);
}

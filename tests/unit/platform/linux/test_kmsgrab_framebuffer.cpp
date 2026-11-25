/**
 * @file tests/unit/platform/linux/test_kmsgrab_framebuffer.cpp
 * @brief GEM framebuffer ownership, framebuffer gap, and recovered output
 * checks without a DRM device.
 */
#include "../../../tests_common.h"

#include <algorithm>
#include <chrono>
#include <src/platform/linux/kmsgrab_framebuffer.h>
#include <vector>

namespace {
  TEST(KmsFramebuffer, ClosesDistinctHandlesAndSkipsUnusedPlanes) {
    std::uint32_t handles[] {17, 0, 42, 91};
    std::vector<std::uint32_t> closed;

    platf::kms::close_framebuffer_handles(handles, [&](std::uint32_t handle) {
      closed.push_back(handle);
    });

    EXPECT_EQ(closed, (std::vector<std::uint32_t> {17, 42, 91}));
    EXPECT_TRUE(std::ranges::all_of(handles, [](auto handle) {
      return handle == 0;
    }));

    platf::kms::close_framebuffer_handles(handles, [&](std::uint32_t handle) {
      ADD_FAILURE() << "Closed a handle after ownership was released: " << handle;
    });
  }

  TEST(KmsFramebuffer, SharedPlanesDoNotCloseAReusedHandleNumber) {
    std::uint32_t handles[] {17, 42, 17, 42};
    std::vector<std::uint32_t> closed;
    bool unrelated_handle_17_exists = false;

    platf::kms::close_framebuffer_handles(handles, [&](std::uint32_t handle) {
      // Another caller can allocate the same numeric handle after GEM_CLOSE.
      // A second close for a plane alias would release that unrelated object.
      if (handle == 17) {
        EXPECT_FALSE(unrelated_handle_17_exists);
        unrelated_handle_17_exists = true;
      }
      closed.push_back(handle);
    });

    EXPECT_EQ(closed, (std::vector<std::uint32_t> {17, 42}));
    EXPECT_TRUE(unrelated_handle_17_exists);
    EXPECT_TRUE(std::ranges::all_of(handles, [](auto handle) {
      return handle == 0;
    }));
  }

  using gap_t = platf::kms::framebuffer_gap_t;
  using step_e = gap_t::step_e;
  using namespace std::chrono_literals;

  TEST(KmsFramebufferGap, ShortGapHoldsTheLastFrame) {
    // A modeset clears the plane framebuffer for a few frames. Capture must
    // hold the last frame instead of tearing the capture generation down.
    gap_t gap;
    const auto start = gap_t::clock::time_point {} + 1h;

    EXPECT_EQ(gap.missing(start), step_e::started);
    EXPECT_EQ(gap.missing(start + 16ms), step_e::holding);
    EXPECT_EQ(gap.missing(start + gap_t::limit - 1ms), step_e::holding);
    EXPECT_TRUE(gap.present());
    EXPECT_FALSE(gap.present());
  }

  TEST(KmsFramebufferGap, LastingGapReinitializesOnce) {
    // An output that stays without a framebuffer (for example, a display that
    // is off) must reinitialize instead of freezing.
    gap_t gap;
    const auto start = gap_t::clock::time_point {} + 1h;

    EXPECT_EQ(gap.missing(start), step_e::started);
    EXPECT_EQ(gap.missing(start + gap_t::limit), step_e::expired);
    EXPECT_FALSE(gap.present());

    // A gap that continues after reinitialization is a new gap.
    EXPECT_EQ(gap.missing(start + gap_t::limit + 16ms), step_e::started);
    EXPECT_EQ(gap.missing(start + gap_t::limit + 32ms), step_e::holding);
  }

  TEST(KmsFramebufferGap, ReturnedFramebufferRestartsTheLimit) {
    // Separate short gaps must not add up to the limit.
    gap_t gap;
    const auto start = gap_t::clock::time_point {} + 1h;

    EXPECT_EQ(gap.missing(start), step_e::started);
    EXPECT_EQ(gap.missing(start + gap_t::limit - 1s), step_e::holding);
    EXPECT_TRUE(gap.present());
    EXPECT_EQ(gap.missing(start + gap_t::limit), step_e::started);
    EXPECT_EQ(gap.missing(start + gap_t::limit + gap_t::limit - 1s), step_e::holding);
  }

  constexpr std::uint32_t captured_crtc = 41;
  constexpr std::uint32_t other_crtc = 42;

  TEST(KmsRecoveredFramebuffer, AcceptsTheUnchangedOutput) {
    EXPECT_TRUE(platf::kms::recovered_framebuffer_matches(captured_crtc, captured_crtc, captured_crtc));
    // CRTC export reads no plane, so only the connector binding applies.
    EXPECT_TRUE(platf::kms::recovered_framebuffer_matches(captured_crtc, captured_crtc, std::nullopt));
  }

  TEST(KmsRecoveredFramebuffer, RejectsAPlaneThatMovedToAnotherCrtc) {
    // The saved plane now scans out another output. A frame of the same size
    // would pass the dimensions check.
    EXPECT_FALSE(platf::kms::recovered_framebuffer_matches(captured_crtc, captured_crtc, other_crtc));
  }

  TEST(KmsRecoveredFramebuffer, RejectsAConnectorThatNoLongerDrivesTheCrtc) {
    // The selected connector moved to another CRTC, and another connector
    // took over the captured one.
    EXPECT_FALSE(platf::kms::recovered_framebuffer_matches(captured_crtc, other_crtc, captured_crtc));
    EXPECT_FALSE(platf::kms::recovered_framebuffer_matches(captured_crtc, other_crtc, std::nullopt));
    // An encoder without a CRTC reports CRTC 0.
    EXPECT_FALSE(platf::kms::recovered_framebuffer_matches(captured_crtc, 0, captured_crtc));
  }

  TEST(KmsRecoveredFramebuffer, RejectsAConnectorItCannotConfirm) {
    // A connector that cannot be read, is disconnected, or has no encoder
    // cannot confirm that the framebuffer belongs to the captured output.
    EXPECT_FALSE(platf::kms::recovered_framebuffer_matches(captured_crtc, std::nullopt, captured_crtc));
    EXPECT_FALSE(platf::kms::recovered_framebuffer_matches(captured_crtc, std::nullopt, std::nullopt));
  }
}  // namespace

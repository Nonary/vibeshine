/**
 * @file tests/unit/platform/linux/test_kmsgrab_framebuffer.cpp
 * @brief GEM framebuffer ownership and framebuffer gap checks without a DRM
 * device.
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
  using frame_e = gap_t::frame_e;
  using step_e = gap_t::step_e;
  using namespace std::chrono_literals;

  constexpr auto start = gap_t::clock::time_point {} + 1h;

  TEST(KmsFramebufferGap, CapturesWhileTheOutputHasAFramebuffer) {
    gap_t gap;

    EXPECT_EQ(gap.observe(frame_e::acquired, start), step_e::capture);
    EXPECT_EQ(gap.observe(frame_e::acquired, start + 16ms), step_e::capture);
    EXPECT_FALSE(gap.active());
  }

  TEST(KmsFramebufferGap, ShortGapHoldsTheLastFrame) {
    // A modeset clears the framebuffer for a few frames. Capture must hold
    // the last frame instead of tearing the capture generation down.
    gap_t gap;

    EXPECT_EQ(gap.observe(frame_e::missing, start), step_e::started);
    EXPECT_TRUE(gap.active());
    EXPECT_EQ(gap.observe(frame_e::missing, start + 16ms), step_e::holding);
    EXPECT_EQ(gap.observe(frame_e::missing, start + gap_t::limit - 1ms), step_e::holding);
    EXPECT_TRUE(gap.active());
  }

  TEST(KmsFramebufferGap, ReturnedFramebufferReinitializesOnce) {
    // The modeset can change the plane's CRTC, the connector on it, or the
    // output geometry. A returned framebuffer of the same size does not show
    // that they are unchanged, so it ends the capture generation.
    gap_t gap;

    EXPECT_EQ(gap.observe(frame_e::missing, start), step_e::started);
    EXPECT_EQ(gap.observe(frame_e::acquired, start + 16ms), step_e::recovered);
    EXPECT_FALSE(gap.active());
    EXPECT_EQ(gap.observe(frame_e::acquired, start + 32ms), step_e::capture);
  }

  TEST(KmsFramebufferGap, FailedLookupDoesNotEndTheGap) {
    // The framebuffer ID returns, but its lookup fails while the output is
    // reconfigured. Only a framebuffer that capture read ends the gap, so the
    // next successful lookup still reinitializes instead of being captured.
    gap_t gap;

    EXPECT_EQ(gap.observe(frame_e::missing, start), step_e::started);
    EXPECT_EQ(gap.observe(frame_e::unreadable, start + 16ms), step_e::holding);
    EXPECT_TRUE(gap.active());
    EXPECT_EQ(gap.observe(frame_e::unreadable, start + 32ms), step_e::holding);
    EXPECT_EQ(gap.observe(frame_e::acquired, start + 48ms), step_e::recovered);
  }

  TEST(KmsFramebufferGap, FailedLookupsCountTowardTheLimit) {
    gap_t gap;

    EXPECT_EQ(gap.observe(frame_e::missing, start), step_e::started);
    EXPECT_EQ(gap.observe(frame_e::unreadable, start + gap_t::limit), step_e::expired);
    EXPECT_FALSE(gap.active());
  }

  TEST(KmsFramebufferGap, FailedLookupOutsideAGapSkipsTheFrame) {
    // A framebuffer can be removed between reading the plane and looking up
    // its framebuffer. Outside a gap, that race skips one frame as before and
    // does not start a gap.
    gap_t gap;

    EXPECT_EQ(gap.observe(frame_e::unreadable, start), step_e::skip);
    EXPECT_FALSE(gap.active());
    EXPECT_EQ(gap.observe(frame_e::acquired, start + 16ms), step_e::capture);
  }

  TEST(KmsFramebufferGap, LastingGapReinitializesOnce) {
    // An output that stays without a framebuffer (for example, a display that
    // is off) must reinitialize instead of freezing.
    gap_t gap;

    EXPECT_EQ(gap.observe(frame_e::missing, start), step_e::started);
    EXPECT_EQ(gap.observe(frame_e::missing, start + gap_t::limit), step_e::expired);
    EXPECT_FALSE(gap.active());

    // A gap that continues after reinitialization is a new gap.
    EXPECT_EQ(gap.observe(frame_e::missing, start + gap_t::limit + 16ms), step_e::started);
    EXPECT_EQ(gap.observe(frame_e::missing, start + gap_t::limit + 32ms), step_e::holding);
  }
}  // namespace

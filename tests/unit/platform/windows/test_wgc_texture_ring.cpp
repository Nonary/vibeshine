#ifdef _WIN32

  #include <atomic>
  #include <array>
  #include <thread>
  #include <vector>

  #include <gtest/gtest.h>

  #include "src/platform/windows/ipc/pipes.h"

namespace {
  using platf::dxgi::release_wgc_texture_slot;
  using platf::dxgi::transition_wgc_texture_slot;
  using platf::dxgi::wgc_texture_slot_metadata_t;
  using platf::dxgi::wgc_texture_slot_state;
  using platf::dxgi::wgc_texture_slot_state_e;

  void publish_slot(wgc_texture_slot_metadata_t &slot, LONG64 frame_id) {
    slot.frame_id = frame_id;
    slot.frame_qpc = frame_id * 100;
    ASSERT_TRUE(transition_wgc_texture_slot(slot, wgc_texture_slot_state_e::writing, wgc_texture_slot_state_e::ready));
  }
}  // namespace

TEST(WgcTextureRing, LeasedSlotRequiresMatchingGenerationToRelease) {
  wgc_texture_slot_metadata_t slot {};

  ASSERT_TRUE(transition_wgc_texture_slot(slot, wgc_texture_slot_state_e::free, wgc_texture_slot_state_e::writing));
  publish_slot(slot, 42);
  ASSERT_TRUE(transition_wgc_texture_slot(slot, wgc_texture_slot_state_e::ready, wgc_texture_slot_state_e::leased));

  EXPECT_FALSE(release_wgc_texture_slot(slot, 41));
  EXPECT_EQ(wgc_texture_slot_state(slot), static_cast<LONG>(wgc_texture_slot_state_e::leased));
  EXPECT_TRUE(release_wgc_texture_slot(slot, 42));
  EXPECT_EQ(wgc_texture_slot_state(slot), static_cast<LONG>(wgc_texture_slot_state_e::free));
}

TEST(WgcTextureRing, ProducerCannotReuseLeasedSlots) {
  std::array<wgc_texture_slot_metadata_t, platf::dxgi::WGC_IPC_TEXTURE_SLOT_COUNT> slots {};
  for (size_t index = 0; index < slots.size(); ++index) {
    ASSERT_TRUE(transition_wgc_texture_slot(slots[index], wgc_texture_slot_state_e::free, wgc_texture_slot_state_e::writing));
    publish_slot(slots[index], static_cast<LONG64>(index + 1));
    ASSERT_TRUE(transition_wgc_texture_slot(slots[index], wgc_texture_slot_state_e::ready, wgc_texture_slot_state_e::leased));

    EXPECT_FALSE(transition_wgc_texture_slot(slots[index], wgc_texture_slot_state_e::free, wgc_texture_slot_state_e::writing));
    EXPECT_FALSE(transition_wgc_texture_slot(slots[index], wgc_texture_slot_state_e::ready, wgc_texture_slot_state_e::writing));
  }
}

TEST(WgcTextureRing, ProducerMayReclaimAnUnclaimedReadySlot) {
  wgc_texture_slot_metadata_t slot {};
  ASSERT_TRUE(transition_wgc_texture_slot(slot, wgc_texture_slot_state_e::free, wgc_texture_slot_state_e::writing));
  publish_slot(slot, 10);

  EXPECT_TRUE(transition_wgc_texture_slot(slot, wgc_texture_slot_state_e::ready, wgc_texture_slot_state_e::writing));
  EXPECT_EQ(wgc_texture_slot_state(slot), static_cast<LONG>(wgc_texture_slot_state_e::writing));
}

TEST(WgcTextureRing, OnlyOneConsumerCanClaimReadySlot) {
  wgc_texture_slot_metadata_t slot {};
  ASSERT_TRUE(transition_wgc_texture_slot(slot, wgc_texture_slot_state_e::free, wgc_texture_slot_state_e::writing));
  publish_slot(slot, 7);

  std::atomic<int> claims = 0;
  std::vector<std::thread> consumers;
  consumers.reserve(8);
  for (int index = 0; index < 8; ++index) {
    consumers.emplace_back([&]() {
      if (transition_wgc_texture_slot(slot, wgc_texture_slot_state_e::ready, wgc_texture_slot_state_e::leased)) {
        claims.fetch_add(1, std::memory_order_relaxed);
      }
    });
  }
  for (auto &consumer: consumers) {
    consumer.join();
  }

  EXPECT_EQ(claims.load(std::memory_order_relaxed), 1);
  EXPECT_EQ(wgc_texture_slot_state(slot), static_cast<LONG>(wgc_texture_slot_state_e::leased));
}

#endif

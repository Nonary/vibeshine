/**
 * @file tests/unit/test_display_helper_protocol_and_guard.cpp
 * @brief Platform-neutral tests for correlated display-helper IPC and the
 *        cancel-vs-restore-mutation gate.
 */

#include "../tests_common.h"
#include "src/platform/windows/display_restore_guard.h"
#include "src/platform/windows/ipc/display_settings_protocol.h"

#include <algorithm>
#include <array>
#include <thread>
#include <vector>

TEST(DisplayHelperProtocol, CorrelatedRequestAndResultRoundTrip) {
  using namespace platf::display_helper_protocol;

  const std::array<std::uint8_t, 3> body {0x11, 0x22, 0x33};
  const auto encoded = encode_correlated_request(0x1020304050607080ULL, 987654321ULL, body);
  const auto decoded = decode_correlated_request(encoded);
  ASSERT_TRUE(decoded.has_value());
  EXPECT_EQ(decoded->request_id, 0x1020304050607080ULL);
  EXPECT_EQ(decoded->not_after_tick_ms, 987654321ULL);
  EXPECT_TRUE(std::equal(decoded->body.begin(), decoded->body.end(), body.begin(), body.end()));

  const auto encoded_result = encode_correlated_result(decoded->request_id, ResultStatus::Busy);
  const auto decoded_result = decode_correlated_result(encoded_result);
  ASSERT_TRUE(decoded_result.has_value());
  EXPECT_EQ(decoded_result->request_id, decoded->request_id);
  EXPECT_EQ(decoded_result->status, ResultStatus::Busy);
}

TEST(DisplayHelperProtocol, RejectsTruncatedInvalidAndZeroIdFrames) {
  using namespace platf::display_helper_protocol;

  std::vector<std::uint8_t> truncated(kCorrelatedRequestHeaderSize - 1, 0);
  EXPECT_FALSE(decode_correlated_request(truncated).has_value());

  auto zero_id = encode_correlated_request(1, 100);
  std::fill(zero_id.begin() + 1, zero_id.begin() + 9, 0);
  EXPECT_FALSE(decode_correlated_request(zero_id).has_value());

  auto bad_version = encode_correlated_result(1, ResultStatus::Succeeded);
  bad_version.front() = 0xFF;
  EXPECT_FALSE(decode_correlated_result(bad_version).has_value());

  auto bad_status = encode_correlated_result(1, ResultStatus::Succeeded);
  bad_status.back() = 0xFF;
  EXPECT_FALSE(decode_correlated_result(bad_status).has_value());
}

TEST(DisplayRestoreMutationGuard, DisarmWinsBeforeMutation) {
  display_helper::RestoreMutationGuard guard;
  auto worker = guard.begin_worker();
  bool canceled = false;

  EXPECT_TRUE(guard.try_disarm([&]() {
    canceled = true;
  }));
  EXPECT_TRUE(canceled);
  EXPECT_FALSE(worker.try_begin_mutation([]() {
    return false;
  }));
  EXPECT_FALSE(guard.topology_unconfirmed());
  EXPECT_TRUE(guard.try_begin_capture().has_value());
}

TEST(DisplayRestoreMutationGuard, WorkerStartingAfterCancellationKeepsFence) {
  display_helper::RestoreMutationGuard guard;
  bool canceled = false;
  ASSERT_TRUE(guard.try_disarm([&]() {
    canceled = true;
  }));
  auto worker = guard.begin_worker(canceled);

  EXPECT_FALSE(worker.try_begin_mutation([&]() {
    return canceled;
  }));
  EXPECT_TRUE(guard.try_begin_capture().has_value());
}

TEST(DisplayRestoreMutationGuard, MutationWinsAndDisarmCannotLie) {
  display_helper::RestoreMutationGuard guard;
  bool canceled = false;

  {
    auto worker = guard.begin_worker();
    ASSERT_TRUE(worker.try_begin_mutation([]() {
      return false;
    }));
    EXPECT_TRUE(guard.topology_unconfirmed());
    EXPECT_FALSE(guard.try_disarm([&]() {
      canceled = true;
    }));
    EXPECT_FALSE(canceled);
    EXPECT_FALSE(guard.try_begin_capture().has_value());
  }

  // Worker drain alone cannot prove that a partial topology is safe.
  EXPECT_TRUE(guard.topology_unconfirmed());
  EXPECT_FALSE(guard.try_begin_capture().has_value());
  guard.mark_topology_confirmed();
  EXPECT_TRUE(guard.try_begin_capture().has_value());
}

TEST(DisplayRestoreMutationGuard, ApplySupersedeFencesFollowUpMutation) {
  display_helper::RestoreMutationGuard guard;
  auto worker = guard.begin_worker();
  ASSERT_TRUE(worker.try_begin_mutation([]() {
    return false;
  }));

  bool canceled = false;
  EXPECT_TRUE(guard.supersede_for_apply([&]() {
    canceled = true;
  }));
  EXPECT_TRUE(canceled);
  EXPECT_FALSE(worker.try_begin_mutation([]() {
    return false;
  }));
  EXPECT_FALSE(guard.try_begin_capture().has_value());
}

TEST(DisplayRestoreMutationGuard, VerifiedApplyUnblocksDisarmButPreservesBaseline) {
  display_helper::RestoreMutationGuard guard;
  {
    auto worker = guard.begin_worker();
    ASSERT_TRUE(worker.try_begin_mutation([]() {
      return false;
    }));

    bool canceled = false;
    EXPECT_TRUE(guard.supersede_for_apply([&]() {
      canceled = true;
    }));
    EXPECT_TRUE(canceled);
    guard.mark_superseding_apply_confirmed();

    bool disarmed = false;
    EXPECT_TRUE(guard.try_disarm([&]() {
      disarmed = true;
    }));
    EXPECT_TRUE(disarmed);
    EXPECT_FALSE(guard.try_begin_capture().has_value());
  }

  {
    auto later_restore = guard.begin_worker();
    guard.mark_topology_confirmed();
  }
  EXPECT_TRUE(guard.try_begin_capture().has_value());
}

TEST(DisplayRestoreMutationGuard, SupersededWorkerCannotPublishLateRestoreConfirmation) {
  display_helper::RestoreMutationGuard guard;
  {
    auto worker = guard.begin_worker();
    ASSERT_TRUE(worker.try_begin_mutation([]() {
      return false;
    }));
    ASSERT_TRUE(guard.supersede_for_apply([]() {
    }));
    guard.mark_topology_confirmed();
    EXPECT_FALSE(guard.try_begin_capture().has_value());
  }

  // A later, non-superseded restore cycle can establish a safe baseline again.
  {
    auto worker = guard.begin_worker();
    guard.mark_topology_confirmed();
  }
  EXPECT_TRUE(guard.try_begin_capture().has_value());
}

TEST(DisplayPendingRestoreTracker, StaleAcknowledgementCannotClearNewerRestore) {
  display_helper::PendingRestoreTracker tracker;
  const auto first = tracker.begin_restore();
  ASSERT_TRUE(first != 0);
  const auto second = tracker.begin_restore();
  ASSERT_TRUE(second != 0);
  ASSERT_TRUE(second != first);

  EXPECT_FALSE(tracker.clear_if(first));
  EXPECT_EQ(tracker.current(), second);
  EXPECT_TRUE(tracker.clear_if(second));
  EXPECT_EQ(tracker.current(), 0u);
}

TEST(DisplayPendingRestoreTracker, BusyDiscoveryIsStableUntilConfirmed) {
  display_helper::PendingRestoreTracker tracker;
  const auto discovered = tracker.discover_restore();
  ASSERT_TRUE(discovered != 0);
  EXPECT_EQ(tracker.discover_restore(), discovered);
  EXPECT_EQ(tracker.current(), discovered);
}

TEST(DisplayPendingRestoreTracker, ConcurrentBeginsPublishNewestGeneration) {
  display_helper::PendingRestoreTracker tracker;
  constexpr std::size_t kThreadCount = 32;
  std::array<std::uint64_t, kThreadCount> generations {};
  std::vector<std::thread> threads;
  threads.reserve(kThreadCount);

  for (std::size_t i = 0; i < kThreadCount; ++i) {
    threads.emplace_back([&, i]() {
      generations[i] = tracker.begin_restore();
    });
  }
  for (auto &thread : threads) {
    thread.join();
  }

  const auto newest = *std::max_element(generations.begin(), generations.end());
  EXPECT_EQ(tracker.current(), newest);
  for (const auto generation : generations) {
    if (generation != newest) {
      EXPECT_FALSE(tracker.clear_if(generation));
    }
  }
  EXPECT_TRUE(tracker.clear_if(newest));
}

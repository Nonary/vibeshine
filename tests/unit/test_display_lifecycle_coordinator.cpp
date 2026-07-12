/**
 * @file tests/unit/test_display_lifecycle_coordinator.cpp
 * @brief Windows lifecycle serialization regression tests for stream starts and
 *        final display teardown.
 */
#ifdef _WIN32

  #include "../tests_common.h"
  #include "src/platform/windows/display_helper_integration.h"

  #include <atomic>
  #include <chrono>
  #include <future>
  #include <mutex>
  #include <stdexcept>
  #include <thread>
  #include <vector>

using namespace std::chrono_literals;

TEST(DisplayLifecycleCoordinator, AbandonedStartRunsRetainedTeardown) {
  auto start = display_helper_integration::acquire_display_start_reservation(100ms);
  ASSERT_TRUE(start);

  std::promise<void> teardown_called;
  auto teardown_done = teardown_called.get_future();
  auto teardown = display_helper_integration::try_acquire_display_teardown(
    []() {
      return true;
    },
    [&]() {
      teardown_called.set_value();
    }
  );
  EXPECT_FALSE(teardown);

  start.reset();
  EXPECT_EQ(teardown_done.wait_for(2s), std::future_status::ready);
  auto released = display_helper_integration::acquire_display_start_reservation(2s);
  ASSERT_TRUE(released);
  released->publish_active();
}

TEST(DisplayLifecycleCoordinator, PublishedStartDiscardsStaleTeardown) {
  auto start = display_helper_integration::acquire_display_start_reservation(100ms);
  ASSERT_TRUE(start);

  std::atomic_bool stale_teardown_called {false};
  auto teardown = display_helper_integration::try_acquire_display_teardown(
    []() {
      return true;
    },
    [&]() {
      stale_teardown_called.store(true, std::memory_order_release);
    }
  );
  EXPECT_FALSE(teardown);

  start->publish_active();
  start.reset();

  // Acquiring a new teardown proves the lifecycle gate opened synchronously;
  // the retained callback belonged to the now-active capture and was discarded.
  auto current_teardown = display_helper_integration::try_acquire_display_teardown(
    []() {
      return true;
    }
  );
  ASSERT_TRUE(current_teardown);
  current_teardown.reset();
  EXPECT_FALSE(stale_teardown_called.load(std::memory_order_acquire));
}

TEST(DisplayLifecycleCoordinator, RetainedTeardownsComposeAcrossAbortLease) {
  auto start = display_helper_integration::acquire_display_start_reservation(100ms);
  ASSERT_TRUE(start);

  std::promise<void> first_called;
  std::promise<void> second_called;
  auto first_done = first_called.get_future();
  auto second_done = second_called.get_future();

  EXPECT_FALSE(display_helper_integration::try_acquire_display_teardown([]() {
    return true;
  },
                                                                        [&]() {
                                                                          first_called.set_value();
                                                                          throw std::runtime_error("intentional teardown test failure");
                                                                        }));

  auto abort_lease = start->begin_abort_cleanup();
  ASSERT_TRUE(abort_lease);
  start.reset();

  EXPECT_FALSE(display_helper_integration::try_acquire_display_teardown([]() {
    return true;
  },
                                                                        [&]() {
                                                                          second_called.set_value();
                                                                        }));

  abort_lease.reset();
  EXPECT_EQ(first_done.wait_for(2s), std::future_status::ready);
  EXPECT_EQ(second_done.wait_for(2s), std::future_status::ready);
  auto released = display_helper_integration::acquire_display_start_reservation(2s);
  ASSERT_TRUE(released);
  released->publish_active();
}

TEST(DisplayLifecycleCoordinator, PlatformStartPredicatePreventsStaleResurrection) {
  std::atomic_bool start_called {false};
  std::atomic_bool stop_called {false};

  EXPECT_FALSE(display_helper_integration::claim_platform_streaming_lifecycle([]() {
                 return false;
               }).has_value());
  EXPECT_FALSE(display_helper_integration::run_platform_streaming_start(0, [&]() {
    start_called.store(true, std::memory_order_release);
  }));
  EXPECT_FALSE(start_called.load(std::memory_order_acquire));

  const auto claim = display_helper_integration::claim_platform_streaming_lifecycle([]() {
    return true;
  });
  ASSERT_TRUE(claim);
  EXPECT_TRUE(claim->start_required);
  EXPECT_TRUE(display_helper_integration::run_platform_streaming_start(claim->generation, [&]() {
    start_called.store(true, std::memory_order_release);
  }));
  EXPECT_TRUE(start_called.load(std::memory_order_acquire));

  EXPECT_TRUE(display_helper_integration::release_platform_streaming_lifecycle(claim->generation, [&](bool) {
    stop_called.store(true, std::memory_order_release);
  }));
  EXPECT_TRUE(stop_called.load(std::memory_order_acquire));
}

TEST(DisplayLifecycleCoordinator, PlatformStartRechecksLivenessImmediatelyBeforeHook) {
  const auto claim = display_helper_integration::claim_platform_streaming_lifecycle([]() {
    return true;
  });
  ASSERT_TRUE(claim);
  ASSERT_TRUE(claim->start_required);

  bool start_called = false;
  EXPECT_FALSE(display_helper_integration::run_platform_streaming_start(claim->generation, [&]() {
    start_called = true;
  },
                                                                        []() {
                                                                          return false;
                                                                        }));
  EXPECT_FALSE(start_called);

  bool callback_called = false;
  bool platform_started = true;
  EXPECT_TRUE(display_helper_integration::release_platform_streaming_lifecycle(claim->generation, [&](bool started) {
    callback_called = true;
    platform_started = started;
  }));
  EXPECT_TRUE(callback_called);
  EXPECT_FALSE(platform_started);
}

TEST(DisplayLifecycleCoordinator, PlatformStartRollbackUnwindsPartialHook) {
  const auto claim = display_helper_integration::claim_platform_streaming_lifecycle([]() {
    return true;
  });
  ASSERT_TRUE(claim);
  ASSERT_TRUE(claim->start_required);

  int start_calls = 0;
  int rollback_calls = 0;
  EXPECT_THROW(
    display_helper_integration::run_platform_streaming_start(
      claim->generation,
      [&]() {
        ++start_calls;
        throw std::runtime_error("intentional partial start failure");
      },
      []() {
        return true;
      },
      [&]() {
        ++rollback_calls;
      }
    ),
    std::runtime_error
  );
  EXPECT_EQ(start_calls, 1);
  EXPECT_EQ(rollback_calls, 1);

  bool callback_called = false;
  bool platform_started = true;
  EXPECT_TRUE(display_helper_integration::release_platform_streaming_lifecycle(claim->generation, [&](bool started) {
    callback_called = true;
    platform_started = started;
  }));
  EXPECT_TRUE(callback_called);
  EXPECT_FALSE(platform_started);
}

TEST(DisplayLifecycleCoordinator, PlatformLifecycleRejectsStaleGeneration) {
  std::atomic_int first_start_calls {0};
  std::atomic_int second_start_calls {0};

  const auto first = display_helper_integration::claim_platform_streaming_lifecycle([]() {
    return true;
  });
  ASSERT_TRUE(first);
  ASSERT_TRUE(first->start_required);
  EXPECT_TRUE(display_helper_integration::run_platform_streaming_start(first->generation, [&]() {
    ++first_start_calls;
  }));
  EXPECT_TRUE(display_helper_integration::release_platform_streaming_lifecycle(first->generation, [](bool) {
  }));

  const auto second = display_helper_integration::claim_platform_streaming_lifecycle([]() {
    return true;
  });
  ASSERT_TRUE(second);
  ASSERT_TRUE(second->start_required);
  EXPECT_NE(first->generation, second->generation);

  // A deferred callback from the prior lifecycle must not claim the new one.
  EXPECT_FALSE(display_helper_integration::run_platform_streaming_start(first->generation, [&]() {
    ++first_start_calls;
  }));
  EXPECT_EQ(first_start_calls.load(), 1);

  EXPECT_TRUE(display_helper_integration::run_platform_streaming_start(second->generation, [&]() {
    ++second_start_calls;
  }));
  EXPECT_EQ(second_start_calls.load(), 1);
  EXPECT_TRUE(display_helper_integration::release_platform_streaming_lifecycle(second->generation, [](bool) {
  }));
}

TEST(DisplayLifecycleCoordinator, PlatformReleaseRunsDeferredCleanupWithoutStart) {
  const auto claim = display_helper_integration::claim_platform_streaming_lifecycle([]() {
    return true;
  });
  ASSERT_TRUE(claim);
  ASSERT_TRUE(claim->start_required);

  bool callback_called = false;
  bool platform_started = true;
  EXPECT_TRUE(display_helper_integration::release_platform_streaming_lifecycle(claim->generation, [&](bool started) {
    callback_called = true;
    platform_started = started;
  }));
  EXPECT_TRUE(callback_called);
  EXPECT_FALSE(platform_started);
}

TEST(DisplayLifecycleCoordinator, PlatformReleaseSerializesAfterStartCallback) {
  const auto claim = display_helper_integration::claim_platform_streaming_lifecycle([]() {
    return true;
  });
  ASSERT_TRUE(claim);

  std::promise<void> start_entered;
  auto start_entered_future = start_entered.get_future();
  std::promise<void> allow_start_finish;
  auto allow_start_finish_future = allow_start_finish.get_future().share();
  std::promise<void> stop_entered;
  auto stop_entered_future = stop_entered.get_future();
  std::mutex order_mutex;
  std::vector<int> order;

  std::thread starter([&]() {
    EXPECT_TRUE(display_helper_integration::run_platform_streaming_start(claim->generation, [&]() {
      {
        std::lock_guard lock(order_mutex);
        order.push_back(1);
      }
      start_entered.set_value();
      allow_start_finish_future.wait();
      std::lock_guard lock(order_mutex);
      order.push_back(2);
    }));
  });

  if (start_entered_future.wait_for(2s) != std::future_status::ready) {
    allow_start_finish.set_value();
    starter.join();
    ADD_FAILURE() << "Platform start callback did not enter.";
    return;
  }
  std::thread releaser([&]() {
    EXPECT_TRUE(display_helper_integration::release_platform_streaming_lifecycle(claim->generation, [&](bool started) {
      EXPECT_TRUE(started);
      std::lock_guard lock(order_mutex);
      order.push_back(3);
      stop_entered.set_value();
    }));
  });

  EXPECT_EQ(stop_entered_future.wait_for(100ms), std::future_status::timeout);
  allow_start_finish.set_value();
  starter.join();
  EXPECT_EQ(stop_entered_future.wait_for(2s), std::future_status::ready);
  releaser.join();

  std::lock_guard lock(order_mutex);
  EXPECT_EQ(order, (std::vector<int> {1, 2, 3}));
}

#endif  // _WIN32

#include "../tests_common.h"
#include "src/video_capture_source.h"

#ifdef SUNSHINE_TEST_HAS_DISPLAY_QUERY_STOP
#include <display_device/windows/win_display_query_stop.h>
#endif

#include <future>
#include <memory>

namespace {
  struct capture_context_t {
    int generation = 0;
    safe::signal_t reinit;
#ifdef SUNSHINE_TEST_HAS_DISPLAY_QUERY_STOP
    std::stop_source display_stop {std::nostopstate};
#endif
  };

  using lease_t = video::capture_source_lease_t<capture_context_t>;
  using source_e = video::capture_source_e;

  // Like the production producer, this object has static storage. Refcounted
  // startup and teardown are exercised through the same lease used by video.cpp.
  std::atomic<int> starts {0};
  std::atomic<int> stops {0};
  std::function<void()> stop_hook;

  int start(capture_context_t &ctx) {
    ctx.generation = ++starts;
#ifdef SUNSHINE_TEST_HAS_DISPLAY_QUERY_STOP
    ctx.display_stop = std::stop_source {};
#endif
    return 0;
  }

  void stop([[maybe_unused]] capture_context_t &ctx) {
#ifdef SUNSHINE_TEST_HAS_DISPLAY_QUERY_STOP
    ctx.display_stop.request_stop();
#endif
    if (stop_hook) {
      stop_hook();
    }
    ++stops;
  }

  auto shared_source = safe::make_shared<capture_context_t>(start, stop);

  class CaptureSourceLease: public testing::Test {
  protected:
    void SetUp() override {
      starts = 0;
      stops = 0;
    }

    void TearDown() override {
      EXPECT_EQ(starts.load(), stops.load());
    }

    std::unique_ptr<lease_t> acquire(source_e source) {
      return std::make_unique<lease_t>(source, shared_source, start, stop);
    }
  };
}  // namespace

namespace {
  struct consumer_config_t {
    int framerate;
    int framerateX100;
    int encodingFramerate;
    int width;
    int videoFormat = 0;
    bool shared_capture_pyrowave = false;
  };

  struct consumer_t {
    std::shared_ptr<safe::signal_t> images = std::make_shared<safe::signal_t>();
    consumer_config_t config;
  };
}  // namespace

TEST(CaptureDemand, FastestLiveRateDoesNotReplaceEncoderOrDisplaySettings) {
  std::vector<consumer_t> consumers {
    {.config = {30, 0, 30000, 1920}},
    {.config = {60, 0, 60000, 3840}},
    {.config = {120, 0, 120000, 1280}},
  };
  consumers.back().images->stop();
  const auto selected = video::capture_config_for_consumers(consumers);
  ASSERT_TRUE(selected);
  EXPECT_EQ(selected->framerate, 60);
  EXPECT_EQ(selected->width, 1920);
  EXPECT_EQ(selected->encodingFramerate, 30000);
  EXPECT_EQ(consumers.front().config.framerate, 30);
  EXPECT_EQ(consumers[1].config.width, 3840);
  EXPECT_EQ(consumers[1].config.encodingFramerate, 60000);

  consumers.front().images->stop();
  const auto successor = video::capture_config_for_consumers(consumers);
  ASSERT_TRUE(successor);
  EXPECT_EQ(successor->framerate, 60);
  EXPECT_EQ(successor->width, 3840);
  consumers[1].images->stop();
  EXPECT_FALSE(video::capture_config_for_consumers(consumers));
  EXPECT_FALSE(video::capture_config_for_consumers(std::vector<consumer_t> {}));
}

TEST(CaptureDemand, ComparesEffectiveFractionalRateAndAcceptsEquivalentAliases) {
  std::vector<consumer_t> consumers {
    {.config = {120, 5994, 120000, 1920}},
    {.config = {90, 0, 90000, 1920}},
  };
  const auto selected = video::capture_config_for_consumers(consumers);
  ASSERT_TRUE(selected);
  EXPECT_EQ(selected->framerate, 90);
  EXPECT_EQ(selected->framerateX100, 0);
  EXPECT_TRUE(video::same_capture_pacing(consumer_config_t {24, 2397, 24000, 1920}, consumer_config_t {24, 2398, 24000, 1920}));
  EXPECT_FALSE(video::same_capture_pacing(consumer_config_t {60, 5994, 60000, 1920}, consumer_config_t {60, 0, 60000, 1920}));
  EXPECT_FALSE(video::same_capture_pacing(consumer_config_t {120, 11988, 120000, 1920}, consumer_config_t {120, 0, 120000, 1920}));
}

TEST(CaptureDemand, WindowsAutomaticAdjustmentCannotHideStrictDemand) {
  const video::capture_pacing_context_t desktop {{120000, 1001}, 120, true};
  const consumer_config_t automatic {60, 0, 60000, 1920};
  const consumer_config_t strict {60, 6000, 60000, 1920};
  EXPECT_FALSE(video::same_capture_pacing(automatic, strict, desktop));
  std::vector<consumer_t> consumers {{.config = automatic}, {.config = strict}};
  auto selected = video::capture_config_for_consumers(consumers, desktop);
  ASSERT_TRUE(selected);
  EXPECT_EQ(selected->framerateX100, 6000);
  consumers[1].config.framerateX100 = 5995;
  selected = video::capture_config_for_consumers(consumers, desktop);
  ASSERT_TRUE(selected);
  EXPECT_EQ(selected->framerateX100, 5995);
  EXPECT_EQ(consumers.front().config.framerateX100, 0);
  EXPECT_TRUE(video::same_capture_pacing(automatic, strict, {{120, 1}, 120, true}));
}

TEST(CaptureDemand, PyroWaveJoiningAnEncoderCaptureAsksForTheCopyPath) {
  constexpr int hevc = 1;
  constexpr int pyrowave = 3;
  std::vector<consumer_t> consumers {{.config = {60, 0, 60000, 1920, hevc}}};
  const auto running = video::capture_config_for_consumers(consumers);
  ASSERT_TRUE(running);
  EXPECT_FALSE(running->shared_capture_pyrowave);

  // The later PyroWave client does not change the pace or the first client's
  // codec, but the shared capture now has to give every image a stable copy.
  consumers.push_back(consumer_t {.config = {60, 0, 60000, 1920, pyrowave}});
  const auto joined = video::capture_config_for_consumers(consumers);
  ASSERT_TRUE(joined);
  EXPECT_EQ(joined->videoFormat, hevc);
  EXPECT_TRUE(joined->shared_capture_pyrowave);
  EXPECT_TRUE(video::same_capture_pacing(*running, *joined));
  EXPECT_FALSE(video::same_capture_demand(*running, *joined));
  // The capture restarted for the join is not restarted again.
  EXPECT_TRUE(video::same_capture_demand(*joined, *joined));
}

TEST(CaptureDemand, PyroWaveLeavingDoesNotRestartTheCopyPath) {
  constexpr int pyrowave = 3;
  std::vector<consumer_t> consumers {
    {.config = {60, 0, 60000, 1920, 1}},
    {.config = {60, 0, 60000, 1920, pyrowave}},
  };
  const auto running = video::capture_config_for_consumers(consumers);
  ASSERT_TRUE(running);
  ASSERT_TRUE(running->shared_capture_pyrowave);

  consumers.back().images->stop();
  const auto remaining = video::capture_config_for_consumers(consumers);
  ASSERT_TRUE(remaining);
  EXPECT_FALSE(remaining->shared_capture_pyrowave);
  // The copy path stays correct for the remaining encoder, so only a pace
  // change restarts the capture.
  EXPECT_TRUE(video::same_capture_demand(*running, *remaining));
  consumers.front().config.framerate = 120;
  EXPECT_FALSE(video::same_capture_demand(*running, *video::capture_config_for_consumers(consumers)));
}

TEST(CaptureDemand, PyroWaveAloneIsFlaggedAndStoppedPyroWaveIsIgnored) {
  constexpr int pyrowave = 3;
  std::vector<consumer_t> consumers {
    {.config = {60, 0, 60000, 1920, pyrowave}},
    {.config = {60, 0, 60000, 1920, 1}},
  };
  EXPECT_TRUE(video::capture_config_for_consumers(consumers)->shared_capture_pyrowave);
  consumers.front().images->stop();
  EXPECT_FALSE(video::capture_config_for_consumers(consumers)->shared_capture_pyrowave);
}

TEST(CaptureDemand, StoppedInitialConsumerAdmitsPendingLiveConsumer) {
  std::vector<consumer_t> consumers {{.config = {30, 0, 30000, 1920}}};
  consumers.front().images->stop();
  safe::queue_t<consumer_t> queue {30};
  consumer_t stopped {.config = {120, 0, 120000, 1280}};
  stopped.images->stop();
  ASSERT_TRUE(queue.try_raise(stopped));
  ASSERT_TRUE(queue.try_raise(consumer_t {.config = {60, 6000, 60000, 3840}}));
  const auto selected = video::await_capture_config(consumers, queue);
  ASSERT_TRUE(selected);
  EXPECT_EQ(selected->framerate, 60);
  EXPECT_EQ(selected->width, 3840);
  ASSERT_EQ(consumers.size(), 1);
  EXPECT_TRUE(consumers.front().images->running());
}

TEST(CaptureDemand, EmptyRetryWaitsForLiveAdmissionAndShutdownWakesWaiter) {
  using namespace std::chrono_literals;
  std::vector<consumer_t> consumers;
  safe::queue_t<consumer_t> queue {30};
  auto waiting = std::async(std::launch::async, [&] {
    return video::await_capture_config(consumers, queue);
  });
  EXPECT_EQ(waiting.wait_for(10ms), std::future_status::timeout);
  EXPECT_TRUE(queue.try_raise(consumer_t {.config = {60, 6000, 60000, 3840}}));
  const auto selected = waiting.get();
  ASSERT_TRUE(selected);
  EXPECT_EQ(selected->framerateX100, 6000);
  consumers.front().images->stop();
  auto cancelled = std::async(std::launch::async, [&] {
    return video::await_capture_config(consumers, queue);
  });
  EXPECT_EQ(cancelled.wait_for(10ms), std::future_status::timeout);
  queue.stop();
  EXPECT_FALSE(cancelled.get());
}

TEST_F(CaptureSourceLease, MirrorJoinReusesProducerAndItsFallbackGeneration) {
  auto first = acquire(source_e::active_output);
  auto second = acquire(source_e::active_output);
  ASSERT_NE(first->get(), nullptr);
  ASSERT_EQ(first->get(), second->get());
  EXPECT_EQ(starts.load(), 1);

  // Both encoders must observe one reinit, e.g. the WGC-to-DXGI handoff,
  // rather than creating competing output duplications.
  first->get()->reinit.raise(true);
  EXPECT_TRUE(second->get()->reinit.peek());
  first.reset();
  EXPECT_EQ(stops.load(), 0);
  EXPECT_TRUE(second->get()->reinit.peek());
  second.reset();
  EXPECT_EQ(stops.load(), 1);
}

TEST_F(CaptureSourceLease, RemoteMonitorAndInputRemainPrivate) {
  auto game = acquire(source_e::active_output);
  auto monitor_a = acquire(source_e::exact_output);
  auto monitor_b = acquire(source_e::exact_output);
  auto input = acquire(source_e::synthetic_black);
  EXPECT_NE(game->get(), monitor_a->get());
  EXPECT_NE(monitor_a->get(), monitor_b->get());
  EXPECT_NE(input->get(), game->get());
  EXPECT_NE(input->get(), monitor_a->get());
  EXPECT_EQ(starts.load(), 4);
  monitor_a->get()->reinit.raise(true);
  EXPECT_FALSE(game->get()->reinit.peek());
  EXPECT_FALSE(monitor_b->get()->reinit.peek());
  EXPECT_FALSE(input->get()->reinit.peek());
  monitor_a.reset();
  EXPECT_EQ(stops.load(), 1);
  EXPECT_EQ(game->get()->generation, 1);
}

TEST_F(CaptureSourceLease, FinalDisconnectStopsAndReconnectStartsFresh) {
#ifdef SUNSHINE_TEST_HAS_DISPLAY_QUERY_STOP
  std::stop_token old_stop;
#endif
  {
    auto first = acquire(source_e::active_output);
    auto second = acquire(source_e::active_output);
#ifdef SUNSHINE_TEST_HAS_DISPLAY_QUERY_STOP
    old_stop = first->get()->display_stop.get_token();
#endif
    second.reset();
    EXPECT_EQ(stops.load(), 0);
#ifdef SUNSHINE_TEST_HAS_DISPLAY_QUERY_STOP
    EXPECT_FALSE(old_stop.stop_requested());
#endif
    EXPECT_EQ(first->get()->generation, 1);
  }
  EXPECT_EQ(stops.load(), 1);
#ifdef SUNSHINE_TEST_HAS_DISPLAY_QUERY_STOP
  EXPECT_TRUE(old_stop.stop_requested());
#endif
  auto reconnect = acquire(source_e::active_output);
  EXPECT_EQ(reconnect->get()->generation, 2);
  EXPECT_FALSE(reconnect->get()->reinit.peek());
#ifdef SUNSHINE_TEST_HAS_DISPLAY_QUERY_STOP
  const auto new_stop = reconnect->get()->display_stop.get_token();
  EXPECT_TRUE(new_stop.stop_possible());
  EXPECT_FALSE(new_stop.stop_requested());
  EXPECT_NE(old_stop, new_stop);
#endif
}

#ifdef SUNSHINE_TEST_HAS_DISPLAY_QUERY_STOP
TEST_F(CaptureSourceLease, FinalReleaseCancelsDisplayRetryForSharedAndExactWorkers) {
  using namespace std::chrono_literals;
  for (const auto source : {source_e::active_output, source_e::exact_output}) {
    auto private_peer = acquire(source_e::exact_output);
    auto ending = acquire(source);
    const auto token = ending->get()->display_stop.get_token();
    std::promise<void> retry_entered;
    auto entered = retry_entered.get_future();
    int native_queries = 0;
    bool fallback = false;
    auto retry_worker = std::async(std::launch::async, [&] {
      try {
        display_device::DisplayQueryStopGuard guard {token};
        display_device::run_display_query([&] { return ++native_queries; });
        retry_entered.set_value();
        display_device::wait_for_display_query_retry(5s, [](auto delay) {
          std::this_thread::sleep_for(delay);
        });
        display_device::run_display_query([&] { return ++native_queries; });
        fallback = true;
      } catch (const display_device::DisplayQueryStopped &) {
        return true;
      }
      return false;
    });
    EXPECT_EQ(entered.wait_for(2s), std::future_status::ready);
    ending.reset();
    EXPECT_EQ(retry_worker.wait_for(2s), std::future_status::ready);
    EXPECT_TRUE(retry_worker.get());
    EXPECT_EQ(native_queries, 1);
    EXPECT_FALSE(fallback);
    EXPECT_FALSE(private_peer->get()->display_stop.stop_requested());
  }
}

TEST_F(CaptureSourceLease, StoppingOneMirrorLeavesDisplayRecoveryForPendingPeer) {
  auto first = acquire(source_e::active_output);
  auto pending = acquire(source_e::active_output);
  ASSERT_EQ(first->get(), pending->get());
  const auto token = pending->get()->display_stop.get_token();
  std::vector<consumer_t> consumers {{.config = {30, 0, 30000, 1920}}};
  safe::queue_t<consumer_t> queue {30};

  // Reinitialization is looking for a display as the active consumer leaves.
  display_device::DisplayQueryStopGuard guard {token};
  const bool initial_query = display_device::run_display_query([] { return true; });
  const bool reinit_query = display_device::run_display_query([] { return false; });
  EXPECT_TRUE(initial_query);
  EXPECT_FALSE(reinit_query);
  consumers.front().images->stop();
  first.reset();
  ASSERT_FALSE(token.stop_requested());
  ASSERT_TRUE(queue.try_raise(consumer_t {.config = {60, 0, 60000, 3840}}));

  const auto selected = video::await_capture_config(consumers, queue);
  ASSERT_TRUE(selected);
  EXPECT_EQ(selected->framerate, 60);
  EXPECT_TRUE(display_device::run_display_query([] { return true; }));
  EXPECT_EQ(starts.load(), 1);
  EXPECT_EQ(stops.load(), 0);
}

TEST(CaptureDisplayQueryStop, StopDiscardsNativeResultAndPreventsNextQuery) {
  std::stop_source stop;
  int native_queries = 0;
  {
    display_device::DisplayQueryStopGuard guard {stop.get_token()};
    EXPECT_THROW(display_device::run_display_query([&] {
      ++native_queries;
      stop.request_stop();
      return true;
    }), display_device::DisplayQueryStopped);
    EXPECT_THROW(display_device::run_display_query([&] {
      return ++native_queries;
    }), display_device::DisplayQueryStopped);
    EXPECT_EQ(native_queries, 1);
  }
  // Other calls on the same thread keep their normal policy after the scope.
  EXPECT_EQ(display_device::run_display_query([&] { return ++native_queries; }), 2);
  using namespace std::chrono_literals;
  display_device::wait_for_display_query_retry(250ms, [](auto delay) {
    EXPECT_EQ(delay, 250ms);
  });
}
#endif

TEST_F(CaptureSourceLease, ConcurrentJoinsStartOnlyOneProducer) {
  auto first = std::async(std::launch::async, [&] {
    return acquire(source_e::active_output);
  });
  auto second = std::async(std::launch::async, [&] {
    return acquire(source_e::active_output);
  });
  auto first_lease = first.get();
  auto second_lease = second.get();
  EXPECT_EQ(first_lease->get(), second_lease->get());
  EXPECT_EQ(starts.load(), 1);
  first_lease.reset();
  EXPECT_EQ(stops.load(), 0);
  second_lease.reset();
  EXPECT_EQ(stops.load(), 1);
}

TEST_F(CaptureSourceLease, FailedPrivateStartupDoesNotStopAnUnstartedWorker) {
  lease_t failed {
    source_e::exact_output,
    shared_source,
    [](capture_context_t &) {
      return -1;
    },
    stop
  };
  EXPECT_EQ(failed.get(), nullptr);
  EXPECT_EQ(starts.load(), 0);
  EXPECT_EQ(stops.load(), 0);
}

TEST_F(CaptureSourceLease, ReconnectWaitsForPreviousProducerTeardown) {
  using namespace std::chrono_literals;
  std::promise<void> entered_stop;
  std::promise<void> release_stop;
  auto entered = entered_stop.get_future();
  auto release = release_stop.get_future();
  stop_hook = [&] {
    entered_stop.set_value();
    release.wait();
  };
  auto first = acquire(source_e::active_output);
  auto disconnect = std::async(std::launch::async, [&] {
    first.reset();
  });
  EXPECT_EQ(entered.wait_for(2s), std::future_status::ready);
  auto reconnect = std::async(std::launch::async, [&] {
    return acquire(source_e::active_output);
  });
  EXPECT_EQ(reconnect.wait_for(10ms), std::future_status::timeout);
  EXPECT_EQ(starts.load(), 1);
  release_stop.set_value();
  disconnect.get();
  auto next = reconnect.get();
  stop_hook = {};
  EXPECT_EQ(stops.load(), 1);
  EXPECT_EQ(next->get()->generation, 2);
}

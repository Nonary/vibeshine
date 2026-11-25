#include "../tests_common.h"
#include "src/video_capture_source.h"
#include "src/video_policy.h"

#include <display_device/windows/win_api_recovery.h>

#include <future>
#include <memory>

void check_capture_display_from_peer();

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

  struct capture_context_t {
    int generation = 0;
    safe::signal_t reinit;
    std::shared_ptr<safe::queue_t<consumer_t>> queue;
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
    ctx.queue = std::make_shared<safe::queue_t<consumer_t>>(30);
    return 0;
  }

  void stop(capture_context_t &ctx) {
    ctx.queue->stop();
    if (stop_hook) {
      stop_hook();
    }
    ++stops;
  }

  display_device::DisplayQueryCancellation cancellation_for(safe::queue_t<consumer_t> &queue) {
    return {
      [&] { return !queue.running(); },
      [&](auto delay) { return queue.wait_for_stop(delay); },
    };
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
  auto first = acquire(source_e::active_output);
  auto second = acquire(source_e::active_output);
  const auto old_queue = first->get()->queue;
  second.reset();
  EXPECT_TRUE(old_queue->running());
  EXPECT_EQ(stops.load(), 0);
  first.reset();
  EXPECT_FALSE(old_queue->running());
  EXPECT_EQ(stops.load(), 1);

  auto reconnect = acquire(source_e::active_output);
  EXPECT_EQ(reconnect->get()->generation, 2);
  EXPECT_FALSE(reconnect->get()->reinit.peek());
  EXPECT_NE(old_queue, reconnect->get()->queue);
  EXPECT_TRUE(reconnect->get()->queue->running());
}

TEST_F(CaptureSourceLease, FinalReleaseWakesRetryWithoutAnotherQueryOrFallback) {
  using namespace std::chrono_literals;
  using namespace display_device;
  for (const auto source : {source_e::active_output, source_e::exact_output}) {
    auto independent = acquire(source_e::exact_output);
    auto ending = acquire(source);
    const auto queue = ending->get()->queue;
    auto cancellation = cancellation_for(*queue);
    const auto queue_wait = cancellation.wait_for_stop;
    std::promise<void> retry_started;
    cancellation.wait_for_stop = [&](auto delay) {
      retry_started.set_value();
      return queue_wait(delay);
    };

    int queries = 0;
    bool fallback = false;
    auto worker = std::async(std::launch::async, [&] {
      DisplayRecoveryBehaviorGuard recovery {DisplayRecoveryBehavior::Skip, cancellation};
      try {
        check_display_query_stop();
        ++queries;  // A native query reported transient topology churn.
        wait_for_display_query_retry(5s);
        ++queries;
        fallback = true;
      } catch (const DisplayQueryStopped &) {
        return true;
      }
      return false;
    });
    EXPECT_EQ(retry_started.get_future().wait_for(2s), std::future_status::ready);
    ending.reset();
    EXPECT_EQ(worker.wait_for(2s), std::future_status::ready);
    EXPECT_TRUE(worker.get());
    EXPECT_EQ(queries, 1);
    EXPECT_FALSE(fallback);
    EXPECT_TRUE(independent->get()->queue->running());
  }
}

TEST_F(CaptureSourceLease, RecoveringProducerKeepsPendingMirrorAndExactSelection) {
  using namespace std::chrono_literals;
  using namespace display_device;
  auto leaving = acquire(source_e::active_output);
  auto pending = acquire(source_e::active_output);
  ASSERT_EQ(leaving->get(), pending->get());
  auto &queue = *pending->get()->queue;
  DisplayRecoveryBehaviorGuard recovery {DisplayRecoveryBehavior::Skip, cancellation_for(queue)};

  std::vector<consumer_t> consumers {{.config = {30, 0, 30000, 1920}}};
  const std::vector<std::string> physical_only {"physical"};
  EXPECT_FALSE(video::policy::resolve_display_output("leased", physical_only));
  consumers.front().images->stop();
  ASSERT_TRUE(queue.try_raise(consumer_t {.config = {60, 0, 60000, 3840}}));
  leaving.reset();

  // The pending join may notify the queue, but the retry only waits for stop.
  EXPECT_FALSE(queue.wait_for_stop(1ms));
  wait_for_display_query_retry(0ms);
  const auto selected = video::await_capture_config(consumers, queue);
  ASSERT_TRUE(selected);
  EXPECT_EQ(selected->framerate, 60);

  const std::vector<std::string> settled {"physical", "leased"};
  EXPECT_EQ(video::policy::resolve_display_output("leased", settled), 1);
  const std::vector<std::string> virtual_outputs {"another", "leased"};
  EXPECT_EQ(video::policy::select_preferred_virtual_output("leased", virtual_outputs, virtual_outputs), "leased");
  EXPECT_EQ(starts.load(), 1);
  EXPECT_EQ(stops.load(), 0);
}

TEST(CaptureDisplayRecovery, NativeCallFinishesBeforeCancellationUnwindsAndRestoresContext) {
  using namespace std::chrono_literals;
  using namespace display_device;
  safe::queue_t<consumer_t> queue;
  std::promise<void> entered_native;
  std::promise<void> finish_native;
  auto release_native = finish_native.get_future();
  auto worker = std::async(std::launch::async, [&] {
    bool cleaned_up = false;
    try {
      DisplayRecoveryBehaviorGuard capture {DisplayRecoveryBehavior::Skip, cancellation_for(queue)};
      auto resource = util::fail_guard([&] { cleaned_up = true; });
      {
        DisplayRecoveryBehaviorGuard nested {DisplayRecoveryBehavior::Automatic};
        EXPECT_EQ(detail::current_display_recovery_behavior(), DisplayRecoveryBehavior::Automatic);
      }
      EXPECT_EQ(detail::current_display_recovery_behavior(), DisplayRecoveryBehavior::Skip);
      DisplayRecoveryBehaviorGuard nested {DisplayRecoveryBehavior::Automatic};
      check_capture_display_from_peer();
      entered_native.set_value();
      release_native.wait();
      check_capture_display_from_peer();
      ADD_FAILURE() << "A result returned after cancellation reached capture selection";
    } catch (const DisplayQueryStopped &) {
      EXPECT_TRUE(cleaned_up);
    }

    DisplayRecoveryBehaviorGuard restore {DisplayRecoveryBehavior::Automatic};
    EXPECT_EQ(detail::current_display_recovery_behavior(), DisplayRecoveryBehavior::Automatic);
    bool slept = false;
    wait_for_display_query_retry(1ms, [&](auto) { slept = true; });
    EXPECT_TRUE(slept);
  });

  EXPECT_EQ(entered_native.get_future().wait_for(2s), std::future_status::ready);
  queue.stop();
  EXPECT_NO_THROW(check_capture_display_from_peer());  // Independent thread and policy.
  EXPECT_EQ(worker.wait_for(0ms), std::future_status::timeout);
  finish_native.set_value();
  worker.get();
}

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

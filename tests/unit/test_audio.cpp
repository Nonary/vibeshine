/**
 * @file tests/unit/test_audio.cpp
 * @brief Deterministic tests for audio stream, sink, and capture policy.
 */
#include "../tests_common.h"

#include <array>
#include <atomic>
#include <chrono>
#include <deque>
#include <future>
#include <src/audio_lifecycle_policy.h>
#include <src/audio_lifecycle_state.h>
#include <src/audio_policy.h>
#include <src/audio_transport_policy.h>
#include <src/crypto.h>
#include <src/rtsp_audio_policy.h>
#include <thread>

using namespace audio::policy;

namespace {
  struct lifecycle_token_t {
    int id {};
  };
}  // namespace

TEST(AudioLifecycleState, RetainsAndReclaimsTheSameOwner) {
  audio::lifecycle::state_t<lifecycle_token_t> state;
  lifecycle_token_t retained {7};
  lifecycle_token_t reclaimed {};

  ASSERT_TRUE(state.retain(retained, true));
  ASSERT_TRUE(state.reclaim(reclaimed, true));
  EXPECT_EQ(reclaimed.id, 7);
  EXPECT_FALSE(state.terminal_pending());
}

TEST(AudioLifecycleState, BlocksReconnectUntilTerminalRestoreCompletes) {
  audio::lifecycle::state_t<lifecycle_token_t> state;
  lifecycle_token_t retained {11};
  ASSERT_TRUE(state.retain(retained, true));
  ASSERT_TRUE(state.begin_terminal());

  std::atomic_bool reconnect_finished {false};
  std::atomic_bool reclaimed {false};
  auto reconnect_waiting = std::promise<void> {};
  auto reconnect_waiting_future = reconnect_waiting.get_future();
  std::thread reconnect([&]() {
    lifecycle_token_t reconnect_token {};
    reclaimed.store(state.reclaim(reconnect_token, true, [&reconnect_waiting]() {
      reconnect_waiting.set_value();
    }),
                    std::memory_order_release);
    reconnect_finished.store(true, std::memory_order_release);
  });

  EXPECT_EQ(
    reconnect_waiting_future.wait_for(std::chrono::seconds {5}),
    std::future_status::ready
  );
  EXPECT_FALSE(reconnect_finished.load(std::memory_order_acquire));

  state.complete_terminal_restore();
  reconnect.join();
  EXPECT_TRUE(reconnect_finished.load(std::memory_order_acquire));
  EXPECT_FALSE(reclaimed.load(std::memory_order_acquire));
}

TEST(AudioLifecycleState, RepeatedTerminalReleaseRestoresAtMostOnce) {
  audio::lifecycle::state_t<lifecycle_token_t> state;
  lifecycle_token_t retained {13};
  ASSERT_TRUE(state.retain(retained, true));

  auto first = state.begin_terminal();
  ASSERT_TRUE(first);
  EXPECT_FALSE(state.begin_terminal());
  state.complete_terminal_restore();
  EXPECT_FALSE(state.begin_terminal());
}

TEST(AudioLifecycleState, ContinuousHandoffHasNoIntermediateRestore) {
  audio::lifecycle::state_t<lifecycle_token_t> state;
  state.owner_acquired();
  ASSERT_EQ(state.owner_count(), 1u);

  // The successor app is published while A still owns the shared context.
  ASSERT_FALSE(state.begin_terminal());
  state.app_started();
  state.owner_acquired();
  EXPECT_FALSE(state.terminal_pending());
  EXPECT_EQ(state.owner_count(), 2u);

  state.owner_released();
  EXPECT_EQ(state.owner_count(), 1u);
}

TEST(AudioLifecyclePolicy, RetainsFinalOwnerForResumableApp) {
  using audio::lifecycle::release_action_e;
  EXPECT_EQ(
    audio::lifecycle::final_owner_release_action(true, true, false),
    release_action_e::retain
  );
  EXPECT_EQ(
    audio::lifecycle::final_owner_release_action(true, true, true),
    release_action_e::restore
  );
  EXPECT_TRUE(audio::lifecycle::can_reclaim_retained_audio(true, true, false));
  EXPECT_FALSE(audio::lifecycle::can_reclaim_retained_audio(true, false, false));
}

TEST(AudioLifecyclePolicy, RestoresTerminalAndIgnoresUnchangedSink) {
  using audio::lifecycle::release_action_e;
  EXPECT_EQ(
    audio::lifecycle::final_owner_release_action(true, false, false),
    release_action_e::restore
  );
  EXPECT_EQ(
    audio::lifecycle::final_owner_release_action(false, true, false),
    release_action_e::ignore
  );
}

TEST(AudioStreamPolicy, SelectsChannelAndQualityVariant) {
  EXPECT_EQ(stream_index(2, false), 0);
  EXPECT_EQ(stream_index(2, true), 1);
  EXPECT_EQ(stream_index(6, false), 2);
  EXPECT_EQ(stream_index(6, true), 3);
  EXPECT_EQ(stream_index(8, false), 4);
  EXPECT_EQ(stream_index(8, true), 5);
  EXPECT_EQ(stream_index(3, true), 0);
}

TEST(AudioStreamPolicy, AppliesCustomSurroundLayoutWithoutMutatingDefault) {
  const stream_layout_t base {6, 4, 2, {0, 1, 4, 5, 2, 3, 0, 0}};
  const stream_layout_t custom {6, 6, 0, {0, 1, 4, 5, 2, 3, 0, 0}};

  EXPECT_EQ(apply_custom_layout(base, std::nullopt).streams, 4);
  const auto selected = apply_custom_layout(base, custom);
  EXPECT_EQ(selected.streams, 6);
  EXPECT_EQ(selected.coupled_streams, 0);
  EXPECT_EQ(base.streams, 4);
}

TEST(AudioSinkPolicy, PreservesPriorityAndEmptyFallbacks) {
  const sink_catalog_t sinks {
    "host",
    "virtual-stereo",
    "virtual-51",
    "virtual-71"
  };

  EXPECT_EQ(select_sink(sinks, "configured", 2, true), "configured");
  EXPECT_EQ(select_sink(sinks, "configured", 6, false), "virtual-51");
  EXPECT_EQ(select_sink(sinks, "", 8, true), "host");

  const sink_catalog_t no_virtual {"", std::nullopt, std::nullopt, std::nullopt};
  EXPECT_TRUE(select_sink(no_virtual, "", 2, false).empty());
}

TEST(AudioSinkPolicy, CaptureOnlyRequiresExplicitSelectedSinkAndPreservesVirtualRouting) {
  const sink_catalog_t sinks {"host", "virtual-stereo", "virtual-51", "virtual-71"};
  const auto physical = select_stream_sink(sinks, "second-device", "", 2, true);
  EXPECT_TRUE(capture_sink_without_routing(true, "second-device", "", physical));
  EXPECT_FALSE(capture_sink_without_routing(false, "second-device", "", physical));
  EXPECT_FALSE(capture_sink_without_routing(true, "", "", "host"));

  // Virtual sinks still take precedence when host audio is disabled or a
  // managed virtual sink is explicitly configured.
  const auto automatic_virtual = select_stream_sink(sinks, "second-device", "", 2, false);
  EXPECT_FALSE(capture_sink_without_routing(true, "second-device", "", automatic_virtual));
  const auto managed_virtual = select_stream_sink(sinks, "second-device", "second-device", 2, true);
  EXPECT_FALSE(capture_sink_without_routing(true, "second-device", "second-device", managed_virtual));

  const sink_catalog_t no_virtual {"host", std::nullopt, std::nullopt, std::nullopt};
  const auto selected = select_stream_sink(no_virtual, "second-device", "", 2, false);
  EXPECT_TRUE(capture_sink_without_routing(true, "second-device", "", selected));
  EXPECT_TRUE(capture_sink_without_routing(true, "host", "", "host"));
}

TEST(AudioSinkPolicy, ManagedVirtualSinkOverridesHostAudioRequest) {
  const sink_catalog_t sinks {
    "host",
    "sink-sunshine-stereo",
    "sink-sunshine-surround51",
    "sink-sunshine-surround71"
  };

  EXPECT_EQ(
    select_stream_sink(
      sinks,
      "host",
      "sink-sunshine-stereo",
      2,
      true
    ),
    "sink-sunshine-stereo"
  );
  EXPECT_EQ(
    select_stream_sink(
      sinks,
      "host",
      "sink-sunshine-stereo",
      6,
      true
    ),
    "sink-sunshine-surround51"
  );
}

namespace {
  class fake_source_t: public sample_source_t {
  public:
    std::deque<sample_status_e> statuses;
    std::deque<bool> reacquire_results;

    sample_status_e sample() override {
      if (statuses.empty()) {
        return sample_status_e::interrupted;
      }
      const auto status = statuses.front();
      statuses.pop_front();
      return status;
    }

    bool reacquire() override {
      if (reacquire_results.empty()) {
        return false;
      }
      const bool result = reacquire_results.front();
      reacquire_results.pop_front();
      return result;
    }
  };
}  // namespace

TEST(AudioCapturePolicy, RetainsSuccessTimeoutReinitializeAndStopLifecycle) {
  fake_source_t source;
  source.statuses = {
    sample_status_e::ok,
    sample_status_e::timeout,
    sample_status_e::reinitialize,
    sample_status_e::ok,
    sample_status_e::interrupted,
  };
  source.reacquire_results = {true};

  const auto summary = drive_capture(source, 10);
  EXPECT_EQ(summary.emitted, 2u);
  EXPECT_EQ(summary.timeouts, 1u);
  EXPECT_EQ(summary.reacquisitions, 1u);
  EXPECT_TRUE(summary.stopped);
}

TEST(AudioCapturePolicy, FailedReinitializeStopsWithoutEmitting) {
  fake_source_t source;
  source.statuses = {sample_status_e::reinitialize, sample_status_e::ok};
  source.reacquire_results = {false};

  const auto summary = drive_capture(source, 10);
  EXPECT_EQ(summary.emitted, 0u);
  EXPECT_EQ(summary.reacquisitions, 1u);
  EXPECT_TRUE(summary.stopped);
}

TEST(AudioPacketTransportPolicy, EnforcesPayloadAndPaddedCiphertextCapacity) {
  EXPECT_EQ(stream::MAX_AUDIO_PACKET_SIZE, 2560U);
  EXPECT_EQ(stream::MAX_AUDIO_PACKET_BUFFER_SIZE, 2576U);

  EXPECT_TRUE(stream::audio_packet_fits(2560, false, 2560));
  EXPECT_FALSE(stream::audio_packet_fits(2561, false, 2576));
  EXPECT_TRUE(stream::audio_packet_fits(2560, true, 2576));
  EXPECT_FALSE(stream::audio_packet_fits(2560, true, 2575));

  // Both sizes require 1408 bytes under PKCS#7; aligned plaintext requires
  // an additional complete padding block.
  EXPECT_EQ(stream::audio_cbc_ciphertext_size(1400), 1408U);
  EXPECT_EQ(stream::audio_cbc_ciphertext_size(1401), 1408U);
  EXPECT_EQ(stream::audio_cbc_ciphertext_size(2560), 2576U);
  EXPECT_TRUE(stream::audio_packet_fits(1400, true, 1408));
  EXPECT_TRUE(stream::audio_packet_fits(1401, true, 1408));
  EXPECT_FALSE(stream::audio_packet_fits(1408, true, 1408));
}

TEST(RtspAudioPolicy, AddsCodecResponseHeaderOnlyForDolby) {
  EXPECT_FALSE(rtsp_stream::should_send_audio_codec_header(audio::dolby_codec_e::opus));
  EXPECT_TRUE(rtsp_stream::should_send_audio_codec_header(audio::dolby_codec_e::ac3));
  EXPECT_TRUE(rtsp_stream::should_send_audio_codec_header(audio::dolby_codec_e::eac3));
}

TEST(AudioPacketTransport, EncryptsAndWritesLargestSupportedFrame) {
  const crypto::aes_t key(16, 0x5a);
  crypto::aes_t iv(16, 0x3c);
  crypto::cipher::cbc_t cipher {key, true};
  const audio::buffer_t plaintext(stream::MAX_AUDIO_PACKET_SIZE, 0x42);
  std::array<std::uint8_t, stream::MAX_AUDIO_PACKET_BUFFER_SIZE> destination {};
  int writes = 0;
  std::size_t written = 0;

  const auto bytes = stream::encode_and_write_audio(
    true,
    plaintext,
    destination.data(),
    destination.size(),
    [&cipher, &iv](std::string_view input, std::uint8_t *output) {
      return cipher.encrypt(input, output, &iv);
    },
    [&writes, &written](const std::uint8_t *, std::size_t size) {
      ++writes;
      written = size;
    }
  );

  EXPECT_EQ(bytes, static_cast<int>(stream::MAX_AUDIO_PACKET_BUFFER_SIZE));
  EXPECT_EQ(writes, 1);
  EXPECT_EQ(written, stream::MAX_AUDIO_PACKET_BUFFER_SIZE);
}

TEST(AudioPacketTransport, WritesLargestSupportedFrameWithoutEncryption) {
  const audio::buffer_t plaintext(stream::MAX_AUDIO_PACKET_SIZE, 0x42);
  std::array<std::uint8_t, stream::MAX_AUDIO_PACKET_SIZE> destination {};
  int writes = 0;
  std::size_t written = 0;

  const auto bytes = stream::encode_and_write_audio(
    false,
    plaintext,
    destination.data(),
    destination.size(),
    [](std::string_view, std::uint8_t *) { return -1; },
    [&writes, &written](const std::uint8_t *, std::size_t size) {
      ++writes;
      written = size;
    }
  );

  EXPECT_EQ(bytes, static_cast<int>(stream::MAX_AUDIO_PACKET_SIZE));
  EXPECT_EQ(writes, 1);
  EXPECT_EQ(written, stream::MAX_AUDIO_PACKET_SIZE);
}

TEST(AudioPacketTransport, RejectsInsufficientCapacityBeforeWriting) {
  const crypto::aes_t key(16, 0x5a);
  crypto::aes_t iv(16, 0x3c);
  crypto::cipher::cbc_t cipher {key, true};
  const audio::buffer_t plaintext(stream::MAX_AUDIO_PACKET_SIZE, 0x42);
  std::array<std::uint8_t, stream::MAX_AUDIO_PACKET_BUFFER_SIZE> destination {};
  int encryptions = 0;
  int writes = 0;

  const auto encrypted_bytes = stream::encode_and_write_audio(
    true,
    plaintext,
    destination.data(),
    destination.size() - 1,
    [&cipher, &iv, &encryptions](std::string_view input, std::uint8_t *output) {
      ++encryptions;
      return cipher.encrypt(input, output, &iv);
    },
    [&writes](const std::uint8_t *, std::size_t) { ++writes; }
  );
  EXPECT_EQ(encrypted_bytes, -1);
  EXPECT_EQ(encryptions, 0);
  EXPECT_EQ(writes, 0);

  const auto plaintext_bytes = stream::encode_and_write_audio(
    false,
    plaintext,
    destination.data(),
    stream::MAX_AUDIO_PACKET_SIZE - 1,
    [](std::string_view, std::uint8_t *) { return -1; },
    [&writes](const std::uint8_t *, std::size_t) { ++writes; }
  );
  EXPECT_EQ(plaintext_bytes, -1);
  EXPECT_EQ(writes, 0);
}

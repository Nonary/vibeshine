/**
 * @file src/audio.cpp
 * @brief Definitions for audio capture and encoding.
 */
#include <algorithm>
#include <mutex>
#include <thread>

// Use the build's existing FFmpeg linkage for optional Dolby audio encoding.
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/avutil.h>
#include <libavutil/channel_layout.h>
#include <libavutil/frame.h>
}

// lib includes
#include <opus/opus_multistream.h>

// local includes
#include "audio.h"
#include "dolby_session_worker.h"
#include "audio_lifecycle_policy.h"
#include "audio_lifecycle_state.h"
#include "audio_policy.h"
#include "audio_protocol.h"
#include "audio_source_lifecycle.h"
#include "audio_usb_mmcss.h"
#include "audio_usb_validation.h"
#include "audio_transport.h"
#include "config.h"
#include "globals.h"
#include "logging.h"
#include "audio_usb_lab_metrics.h"
#include "platform/common.h"
#include "process.h"
#include "thread_safe.h"
#include "utility.h"
#include "webrtc_stream.h"

namespace audio {
  using namespace std::literals;
  using sample_queue_t = std::shared_ptr<safe::queue_t<std::vector<float>>>;

  static void apply_surround_params(opus_stream_config_t &stream, const stream_params_t &params);

  namespace {
    context_manager_t &context_manager() {
      static context_manager_t manager {{
        [] { return platf::audio_control(); },
        [] { return proc::proc.current_app_id() > 0; },
        [] { return config::audio.sink; },
        [](std::string_view message) {
          if (message == "There will be no audio") BOOST_LOG(warning) << message;
          else BOOST_LOG(info) << message;
        },
#if defined(_WIN32) || defined(__linux__)
        true,
#else
        false,
#endif
      }};
      return manager;
    }
  }  // namespace

  int map_stream(int channels, bool quality);

  constexpr auto SAMPLE_RATE = 48000;

  // NOTE: If you adjust the bitrates listed here, make sure to update the
  // corresponding bitrate adjustment logic in rtsp_stream::cmd_announce()
  static opus_stream_config_t standard_stream(int channels,bool quality,const std::uint8_t *mapping) {
    session_profile_t profile;
    standard_opus_profile(channels,quality,profile);
    return {profile.sample_rate,profile.channels,profile.streams,profile.coupled,mapping,profile.bitrate};
  }
  opus_stream_config_t stream_configs[MAX_STREAM_CONFIG] {
    standard_stream(2,false,platf::speaker::map_stereo),standard_stream(2,true,platf::speaker::map_stereo),
    standard_stream(6,false,platf::speaker::map_surround51),standard_stream(6,true,platf::speaker::map_surround51),
    standard_stream(8,false,platf::speaker::map_surround71),standard_stream(8,true,platf::speaker::map_surround71),
    standard_stream(12,false,platf::speaker::map_surround714),
  };
  bool discrete_source_enabled() {
    return !startup::shared_context_allowed(config::audio_714_startup_settings());
  }
  packet_queue_policy_t configured_packet_queue_policy() {
    return packet_queue_policy(config::audio_714_startup_settings().source=="vibe_usb");
  }
  usb_launch_config_t usb_launch_settings(const startup::settings_t& settings) {
    return usb_session_t::bundled_configuration({settings.usb.bridge_executable,settings.usb.bridge_sha256,settings.usb.controller_instance,
            settings.usb_evidence_root,settings.usb_session_seconds,false,false});
  }

  bool discrete_714_available(std::string &reason) {
    reason.clear();
#ifndef _WIN32
    reason = "unavailable_12ch: this platform has not been validated";
    return false;
#else
    const auto &settings = config::audio_714_startup_settings();
    if (!config::audio.stream || !protocol::valid_714_bitrate(settings.bitrate)) {
      reason = "unavailable_12ch: audio disabled or invalid fixed 12ch bitrate";
      return false;
    }
    if(settings.source=="vibe_usb")return usb_session_t::configured(usb_launch_settings(settings),reason);
    reason = "unavailable_12ch: select the configured Vibe Speakers input";
    return false;
#endif
  }

  bool prepare_session(config_t &config, std::string &reason) {
    reason.clear();
    const auto usb_channels = static_cast<unsigned>(config.channels);
    const auto usb_mask = config.channels == 12 ? protocol::discrete_wave_mask : static_cast<std::uint32_t>(config.mask);
    if (config.packetDuration <= 0 || config.packetDuration > 120) {
      reason = "invalid audio packet duration";
      return false;
    }
    if (!config.use_ac3 && !config.use_eac3 && !config.bypass_opus &&
        config.packetDuration != 5 && config.packetDuration != 10 && config.packetDuration != 20 &&
        config.packetDuration != 40 && config.packetDuration != 60) {
      reason = "unsupported Opus packet duration";
      return false;
    }
    if (config.validated) {
      const auto &p=*config.validated;
      if(p.source_kind=="user_mode_usb"&&(p.usb.format.channels!=usb_channels||
         p.usb.format.channel_mask!=usb_mask||
         config.packetDuration!=5||config.use_ac3||config.use_eac3||config.bypass_opus)) {
        reason="USB session profile/codec changed after validation";return false;
      }
      if (config.validated->channels != config.channels || config.validated->frame_size != config.packetDuration * 48 ||
          config.validated->bitrate != config.session_bitrate ||
          (config.channels == 12 && (!config.delivery || !protocol::validate_714_request(config.channels, static_cast<std::uint32_t>(config.mask),
            config.packetDuration, config.flags[config_t::HIGH_QUALITY], std::nullopt)))) {
        reason = "audio session changed after validation";
        return false;
      }
      return true;
    }
    const auto index = map_stream(config.channels, config.flags[config_t::HIGH_QUALITY]);
    if (index < 0 || index >= MAX_STREAM_CONFIG) {
      reason = "unsupported audio channel/quality profile";
      return false;
    }
    if (config.channels != 12 && discrete_source_enabled() && config::audio_714_startup_settings().source!="vibe_usb") {
      reason = "isolated 12ch source mode accepts only negotiated 12ch sessions";
      return false;
    }
    auto stream = stream_configs[index];
    if (config.flags[config_t::CUSTOM_SURROUND_PARAMS]) {
      const auto &custom = config.customStreamParams;
      if (custom.channelCount != config.channels || custom.streams <= 0 || custom.streams > config.channels ||
          custom.coupledStreams < 0 || custom.coupledStreams > custom.streams ||
          custom.streams + custom.coupledStreams != config.channels ||
          std::any_of(std::begin(custom.mapping), std::begin(custom.mapping) + config.channels,
            [&](auto channel) { return channel >= config.channels; })) {
        reason = "invalid custom audio layout";
        return false;
      }
      apply_surround_params(stream, custom);
    }
    session_profile_t profile;
    profile.sample_rate = stream.sampleRate;
    profile.channels = stream.channelCount;
    profile.streams = stream.streams;
    profile.coupled = stream.coupledStreams;
    profile.mapping.fill(0);
    std::copy_n(stream.mapping, stream.channelCount, profile.mapping.begin());
    profile.bitrate = stream.bitrate;
    profile.frame_size = config.packetDuration * stream.sampleRate / 1000;
    if (config.channels == 12) {
      const policy::stream_layout_t layout {profile.channels, profile.streams, profile.coupled, profile.mapping};
      if (!protocol::validate_714_request(config.channels, static_cast<std::uint32_t>(config.mask), config.packetDuration,
            config.flags[config_t::HIGH_QUALITY], layout) || config.use_ac3 || config.use_eac3 || config.bypass_opus ||
          !discrete_714_available(reason)) {
        if (reason.empty()) reason = "unsupported 12ch session contract";
        return false;
      }
      const auto &settings = config::audio_714_startup_settings();
      profile.bitrate = settings.bitrate;
      config.delivery = std::make_shared<delivery_state_t>();
    }
    const auto &atmos_settings=config::audio_714_startup_settings();
    if(atmos_settings.source=="vibe_usb") {
      if(config.packetDuration!=5||config.use_ac3||config.use_eac3||config.bypass_opus||
         (atmos_settings.usb.sample_format!="pcm24"&&atmos_settings.usb.sample_format!="pcm16")) {
        reason="USB input requires exact 48kHz/240-frame Opus and pcm24 or pcm16; no fallback";return false;
      }
      profile.source_kind="user_mode_usb";profile.usb=atmos_settings.usb;profile.source_path=profile.usb.endpoint;
      profile.usb_launch=usb_launch_settings(atmos_settings);
      profile.usb.format.channels = usb_channels;
      profile.usb.format.channel_mask = usb_mask;
      profile.usb.format.container_bits=profile.usb.sample_format=="pcm24"?32:16;profile.usb.format.valid_bits=profile.usb.sample_format=="pcm24"?24:16;
      if(!profile.usb.format.valid()||!usb_session_t::configured(profile.usb_launch,reason))return false;
      if(!config.delivery)config.delivery=std::make_shared<delivery_state_t>();
    }
    config.session_bitrate = profile.bitrate;
    config.validated = std::make_shared<const session_profile_t>(std::move(profile));
    return true;
  }

  /**
   * @brief Encode one interleaved stereo or 5.1 float PCM frame as an AC-3-family access unit.
   */
  class ffmpeg_audio_encoder_t {
  public:
    ffmpeg_audio_encoder_t() = default;
    ffmpeg_audio_encoder_t(const ffmpeg_audio_encoder_t &) = delete;
    ffmpeg_audio_encoder_t &operator=(const ffmpeg_audio_encoder_t &) = delete;
    ffmpeg_audio_encoder_t(ffmpeg_audio_encoder_t &&) = delete;
    ffmpeg_audio_encoder_t &operator=(ffmpeg_audio_encoder_t &&) = delete;

    ~ffmpeg_audio_encoder_t() {
      if (packet_) {
        av_packet_free(&packet_);
      }
      if (frame_) {
        av_frame_free(&frame_);
      }
      if (context_) {
        avcodec_free_context(&context_);
      }
    }

    /**
     * @brief Initialize the selected FFmpeg encoder for stereo or 5.1 PCM.
     *
     * @param codec_id FFmpeg codec identifier for AC-3 or E-AC-3.
     * @param bit_rate Target encoded bitrate in bits per second.
     * @param channels Number of channels; only 2 and 6 are supported.
     * @return True if the encoder context and frame buffers initialize successfully.
     */
    bool initialize(AVCodecID codec_id, std::int64_t bit_rate, int channels) {
      if (channels != 2 && channels != 6) {
        return false;
      }

      const auto *codec = avcodec_find_encoder(codec_id);
      if (!codec) {
        return false;
      }

      context_ = avcodec_alloc_context3(codec);
      if (!context_) {
        return false;
      }

      context_->bit_rate = bit_rate;
      context_->sample_rate = SAMPLE_RATE;
      context_->sample_fmt = AV_SAMPLE_FMT_FLTP;
      context_->time_base = AVRational {1, SAMPLE_RATE};
      context_->thread_count = 1;
      channels_ = channels;

      if (const auto channel_layout = channels == 2 ? AV_CH_LAYOUT_STEREO : AV_CH_LAYOUT_5POINT1_BACK; av_channel_layout_from_mask(&context_->ch_layout, channel_layout) < 0 || avcodec_open2(context_, codec, nullptr) < 0) {
        return false;
      }

      // AC-3 and E-AC-3 use 1536 samples per frame here (32 ms at 48 kHz).
      if (context_->frame_size != 1536) {
        return false;
      }

      frame_ = av_frame_alloc();
      if (!frame_) {
        return false;
      }

      frame_->format = context_->sample_fmt;
      frame_->sample_rate = context_->sample_rate;
      frame_->nb_samples = context_->frame_size;
      if (av_channel_layout_copy(&frame_->ch_layout, &context_->ch_layout) < 0 || av_frame_get_buffer(frame_, 0) < 0) {
        return false;
      }

      packet_ = av_packet_alloc();
      return packet_ != nullptr;
    }

    /**
     * @brief Encode an interleaved floating-point PCM frame into a Dolby access unit.
     *
     * @param interleaved Interleaved float PCM samples for one complete frame.
     * @param output Destination buffer for the encoded access unit.
     * @return Encoded access-unit size in bytes, or -1 if input validation or encoding fails.
     */
    int encode(const std::vector<float> &interleaved, buffer_t &output) {
      if (!frame_ || interleaved.size() != static_cast<std::size_t>(frame_->nb_samples * channels_)) {
        return -1;
      }

      if (av_frame_make_writable(frame_) < 0) {
        return -1;
      }

      for (int channel = 0; channel < channels_; ++channel) {
        auto *destination = static_cast<float *>(static_cast<void *>(frame_->extended_data[channel]));
        for (int sample = 0; sample < frame_->nb_samples; ++sample) {
          destination[sample] = interleaved[static_cast<std::size_t>(sample * channels_ + channel)];
        }
      }

      frame_->pts = next_pts_;
      next_pts_ += frame_->nb_samples;

      if (avcodec_send_frame(context_, frame_) < 0 || avcodec_receive_packet(context_, packet_) < 0) {
        av_packet_unref(packet_);
        return -1;
      }

      const auto packet_size = packet_->size;
      if (packet_size <= 0 || static_cast<std::size_t>(packet_size) > output.size()) {
        av_packet_unref(packet_);
        return -1;
      }

      std::copy_n(packet_->data, packet_size, std::begin(output));
      av_packet_unref(packet_);
      return packet_size;
    }

  private:
    AVCodecContext *context_ {nullptr};
    AVFrame *frame_ {nullptr};
    AVPacket *packet_ {nullptr};
    std::int64_t next_pts_ {0};
    int channels_ {0};
  };

  void encodeThread(sample_queue_t samples, config_t config, void *channel_data) {
    auto packets = acquire_packet_queue<packet_t>(mail::man,mail::audio_packets,configured_packet_queue_policy());
    std::string error_message;
    if (!prepare_session(config, error_message)) {
      BOOST_LOG(error) << "Audio session rejected: " << error_message;
      samples->stop();
      return;
    }
    const auto &profile = *config.validated;

    // Encoding takes place on this thread
    platf::set_thread_name("audio::encode");
    platf::adjust_thread_priority(platf::thread_priority_e::high);

    if (config.use_ac3 || config.use_eac3) {
      const auto codec_id = config.use_eac3 ? AV_CODEC_ID_EAC3 : AV_CODEC_ID_AC3;
      const auto codec_name = config.use_eac3 ? "E-AC-3"sv : "AC-3"sv;
      const auto bitrate_kbps = dolby_bitrate_kbps(config.use_eac3, config.high_quality_dolby, config.channels);
      ffmpeg_audio_encoder_t encoder;

      // The worker's control flow is shared with the CPU-only regression test.
      // FFmpeg and packet delivery remain production callbacks.
      dolby::run_encoder_worker(
        *samples,
        encoder,
        [&](auto &active_encoder) {
          if (bitrate_kbps <= 0 || !active_encoder.initialize(codec_id, static_cast<std::int64_t>(bitrate_kbps) * 1000, config.channels)) {
            BOOST_LOG(error) << "Couldn't initialize the linked FFmpeg "sv << codec_name << " encoder"sv;
            return false;
          }
          BOOST_LOG(info) << codec_name << " initialized: "sv << config.channels
                          << " channels, 48 kHz, "sv << bitrate_kbps << " kbit/s"sv;
          return true;
        },
        [](auto &active_encoder, const auto &sample, buffer_t &packet) {
          return active_encoder.encode(sample, packet);
        },
        [] {
          // Reserve enough room for the largest supported 640 kbit/s AC-3 access unit.
          return buffer_t {4096};
        },
        [&](const auto &sample, buffer_t packet, int bytes) {
          if (webrtc_stream::has_active_sessions() && channel_data == nullptr) {
            // Dolby frame duration is fixed at 1536 samples at 48 kHz (32 ms).
            webrtc_stream::submit_audio_frame(sample, SAMPLE_RATE, config.channels, 1536);
          }
          packet.fake_resize(bytes);
          packets->raise(channel_data, std::move(packet));
        }
      );
      return;
    }

    opus_encoder_t opus;
    if (!opus.initialize(profile, error_message)) {
      BOOST_LOG(error) << "Couldn't initialize Opus: " << error_message;
      samples->stop();
      return;
    }
    BOOST_LOG(info) << "Opus initialized: " << profile.sample_rate / 1000 << " kHz, "
                    << profile.channels << " channels, " << profile.bitrate / 1000
                    << " kbps (total), LOWDELAY CBR, " << opus.version();

    auto frame_size = profile.frame_size;
    while (auto sample = samples->pop()) {
      buffer_t packet {1400};

      if (webrtc_stream::has_active_sessions() && channel_data == nullptr) {
        webrtc_stream::submit_audio_frame(*sample, profile.sample_rate, profile.channels, frame_size);
      }

      int bytes = opus.encode(*sample, {std::begin(packet), packet.size()}, error_message);
      if (bytes < 0) {
        BOOST_LOG(error) << "Couldn't encode audio: " << error_message;
        samples->stop();

        return;
      }

      packet.fake_resize(bytes);
      packets->raise(channel_data, std::move(packet));
    }
  }

  // Project Atmos owns a format lease and the existing routing context; the
  // isolated native/synthetic 12ch paths do not. There is no extra PCM queue:
  // source -> production encoder -> existing production packet queue.
  static void capture_discrete(safe::mail_t mail, const config_t &config, void *channel_data) {
    auto shutdown = mail->event<bool>(mail::shutdown);
    auto stop = util::fail_guard([&] { shutdown->raise(true); });
    auto packets = acquire_packet_queue<packet_t>(mail::man,mail::audio_packets,configured_packet_queue_policy());
    auto retire_delivery = util::fail_guard([&] {
      // First join any sender still using this session. A packet popped just
      // before this close keeps the shared gate and must skip its raw pointer.
      config.delivery->close();
      packets->discard_if([&](const packet_t &packet) { return packet.delivery == config.delivery; });
    });
    const auto &profile = *config.validated;
    static source_admission_t source_admission;
    auto source_lease = source_admission.try_acquire();
    if (!source_lease) {
      BOOST_LOG(error) << profile.channels << "ch source already in use; refusing a second source owner";
      return;
    }
    const bool fixed_ingress = profile.source_kind=="user_mode_usb";
    if (profile.source_kind == "user_mode_usb" && profile.usb.format.channels != static_cast<unsigned>(profile.channels)) {
      BOOST_LOG(error) << "USB capture channel count differs from negotiated encoder profile";
      return;
    }
    usb_lab_metrics lab_metrics(profile.usb.format);
    if (fixed_ingress) packets->preallocate();
    std::unique_ptr<usb_session_t> managed_usb;
    auto live_usb=profile.usb;
    const auto stop_owned_usb = [&] {
      if (managed_usb && !managed_usb->stop()) {
        BOOST_LOG(error) << "Vibe Speakers USB session cleanup incomplete: " << managed_usb->error();
      }
    };
    auto restore_usb = util::fail_guard(stop_owned_usb);
    if(profile.source_kind=="user_mode_usb") {
      managed_usb=std::make_unique<usb_session_t>(profile.usb_launch);
      if(!managed_usb->start(profile.usb.format,[&]{return shutdown->peek();})) {
        BOOST_LOG(error)<<"Vibe Speakers USB owned session could not start: "<<managed_usb->error();return;
      }
      live_usb=managed_usb->source();
    }
    opus_encoder_t encoder;
    std::string error_message;
    if (!encoder.initialize(profile, error_message)) {
      BOOST_LOG(error) << profile.channels << "ch encoder initialization failed: " << error_message;
      return;
    }
    auto usb=std::make_unique<usb_source_t>(live_usb);
    if(!usb->connect()){BOOST_LOG(error)<<"Vibe Speakers USB unavailable: "<<usb->error();return;}
    usb_mmcss_t usb_mmcss;
    auto revert_usb_mmcss=util::fail_guard([&] {
      if(usb_mmcss.status().registered){
        const bool reverted=usb_mmcss.close();const auto& m=usb_mmcss.status();
        BOOST_LOG(info)<<"USB encoder MMCSS thread="<<m.thread_id<<" task="<<m.task_index<<" registered="<<m.registered<<" reverted="<<reverted<<" revert_error="<<m.revert_error;
        if(!reverted)BOOST_LOG(error)<<"USB encoder MMCSS restoration failed; capture thread is ending";
      }
    });
    if(usb&&!usb_mmcss.start()){
      BOOST_LOG(error)<<"USB encoder MMCSS Audio registration failed: "<<usb_mmcss.status().register_error;return;
    }
    auto [initial_depth, capacity] = packets->occupancy();
    if(usb&&capacity!=4){BOOST_LOG(error)<<"USB production packet queue capacity is not the required four blocks";return;}
    BOOST_LOG(info) << "Audio source=" << profile.source_kind << " 48kHz Float32 channels=" << profile.channels << " streams=" << profile.streams << " coupled=" << profile.coupled << ", 5ms CBR "
                    << profile.bitrate << " bps; " << encoder.version()
                    << "; PCM queue=0, packet queue capacity=" << capacity * 240 << " frames/" << capacity * 5
                    << " ms (capacity, not latency), current=" << initial_depth * 240 << " frames";
    platf::set_thread_name("audio::discrete714");
    if(!usb)platf::adjust_thread_priority(platf::thread_priority_e::high);
    std::vector<float> pcm(static_cast<std::size_t>(profile.channels)*240);
    std::array<std::uint8_t, 1400> inline_packet {};
    std::uint64_t delivered = 0, timeouts = 0, overflow = 0, late = 0;
    std::uint64_t usb_as0_count=0, usb_prior_blocks=0, usb_prior_timeouts=0, usb_first_frame=0;
    usb_recovery_limit_t recovery_limit;
    std::uint64_t usb_overflows=0, usb_resyncs=0, usb_lost_frames=0;
    bool awaiting_overflow=false;
    std::chrono::steady_clock::time_point overflow_deadline{};
    double usb_prior_max_age=0;
    std::size_t high_water = initial_depth;
    auto metrics = util::fail_guard([&] {
      if(usb){const auto& s=usb->statistics();BOOST_LOG(info)<<"Vibe Speakers USB blocks="<<usb_prior_blocks+s.blocks<<" timeouts="<<usb_prior_timeouts+s.read_timeouts
        <<" rejected="<<s.protocol_errors<<" stale="<<s.stale_blocks<<" disconnects="<<s.disconnects<<" max_usb_receive_age_ms="<<std::max(usb_prior_max_age,s.maximum_age_ms)
        <<" clean_as0_boundaries="<<usb_as0_count<<" confirmed_queue_overflows="<<usb_overflows<<" recovered_input_boundaries="<<usb_resyncs
        <<" observed_lost_native_frames="<<usb_lost_frames<<" final_epoch="<<usb->metadata().epoch<<" first_native_frame_in_final_epoch="<<usb_first_frame;}
      const auto [depth, unused_capacity] = packets->occupancy();
      BOOST_LOG(info) << profile.channels << "ch source stopped: frames=" << delivered * 240 << " source_timeouts=" << timeouts
                      << " requested_opus_bitrate=" << profile.bitrate << " child_codec_bitrate=" << encoder.child_bitrate()
                      << " packet_budget_verified=" << encoder.packet_budget_verified() << " effective_packet_bitrate=" << encoder.effective_packet_bitrate()
                      << " queue_overflows=" << overflow << " rejected_packets=" << overflow << " late_blocks=" << late
                      << " queue_current_frames=" << depth * 240 << " queue_high_water_frames=" << high_water * 240
                      << " queue_capacity_frames=" << capacity * 240
                      << " queue_age_samples=" << config.delivery->queue_age_count()
                      << " queue_age_mean_ms=" << config.delivery->queue_age_mean_ms()
                      << " queue_age_max_ms=" << config.delivery->queue_age_max_ms();
    });
    using clock = std::chrono::steady_clock;
    source_timing_t timing {clock::now()};
    for (;;) {
      if (shutdown->peek()) {
        if (usb) {
          // The authenticated input pipe can be idle after a drained AS0.
          // Keep its handle alive until the owner has signaled its own stop,
          // restored the render roles and joined the bridge. Error exits
          // elsewhere keep the usual pipe-disconnect evidence.
          stop_owned_usb();
          restore_usb.disable();
        }
        break;
      }
      unsigned skipped_packets=0;
      if(awaiting_overflow && clock::now()>=overflow_deadline){
        BOOST_LOG(error)<<"USB overflow successor produced no qualified block; ending RTSP session";break;
      }
      {
        const auto status=usb->sample(pcm);
        const auto action=policy::sample_action(status);
        if (action==policy::sample_action_e::retry) {
          ++timeouts;
          const auto progress=timing.usb_timeout(usb->statistics().blocks!=0,clock::now());
          if(progress==source_timing_t::progress_e::retry)continue;
          BOOST_LOG(error) << profile.source_kind << " " << profile.channels << "ch input timed out for two seconds";
          break;
        }
        if (status==policy::sample_status_e::interrupted) {
          BOOST_LOG(info) << profile.source_kind << " " << profile.channels << "ch stream completed; draining " << delivered << " encoded packets";
          const auto result=config.delivery->wait_for(delivered,200ms,[&]{return shutdown->peek();});
          if(result!=drain_result_e::complete){
            BOOST_LOG(warning)<<profile.source_kind<<" packet drain incomplete, ending capture";
            break;
          }
          const bool lost=usb&&usb->end_reason()==usb_end_reason_e::confirmed_queue_overflow;
          if(usb && (usb->end_reason()==usb_end_reason_e::terminal_as0||lost) && !shutdown->peek()){
            const auto& old=usb->statistics(); const auto& last=usb->metadata();
            if(lost){
              ++usb_overflows;
              BOOST_LOG(error)<<"Vibe Speakers USB confirmed queue overflow: old_nonce="<<usb->session().header.nonce[0]<<":"<<usb->session().header.nonce[1]
                <<" epoch="<<last.epoch<<" old_last_sequence="<<last.blockSequence<<" old_last_native_end="<<last.firstFrame+240;
              if(!recovery_limit.admit(last.lastReceiveQpc,last.qpcFrequency)){
                BOOST_LOG(error)<<"USB repeated overflow; ending RTSP session";break;
              }
            }
            BOOST_LOG(info)<<"Vibe Speakers USB input boundary overflow="<<lost<<" epoch="<<last.epoch<<" nonce="<<usb->session().header.nonce[0]<<":"<<usb->session().header.nonce[1]
              <<" first_native_frame="<<usb_first_frame<<" last_native_frame_exclusive="<<last.firstFrame+240
              <<" blocks="<<old.blocks<<" encoder_lookahead_frames="<<encoder.lookahead()<<" network_packets_delivered="<<delivered;
            // One input boundary within a continuing RTSP session. The sender's
            // packet sequence, timebase, crypto and FEC remain continuous.
            try {
              if(!encoder.start_usb_epoch(error_message)){
                BOOST_LOG(error)<<"Could not initialize the next USB input encoder: "<<error_message;break;
              }
              auto next=std::make_unique<usb_source_t>(live_usb);
              const auto stopping=[&]{return shutdown->peek();};
              const bool connected=lost?next->connect_after_overflow(*usb,stopping):next->connect_after_as0(*usb,stopping);
              if(!connected){
                if(!shutdown->peek())BOOST_LOG(error)<<"USB input rearm failed: "<<next->error();
                break;
              }
              if(!lost)++usb_as0_count;
              awaiting_overflow=lost;overflow_deadline=clock::now()+2s;
              usb_prior_blocks+=old.blocks;usb_prior_timeouts+=old.read_timeouts;
              usb_prior_max_age=std::max(usb_prior_max_age,old.maximum_age_ms);
              usb=std::move(next);usb_first_frame=0;
              lab_metrics.next_section();
            }catch(const std::exception& e){
              BOOST_LOG(error)<<"USB input rearm failed: "<<e.what();break;
            }
            BOOST_LOG(info)<<"Vibe Speakers USB rearmed on the same endpoint, nonce="<<usb->session().header.nonce[0]<<":"<<usb->session().header.nonce[1];
            continue;
          }
          break;
        }
        if (action!=policy::sample_action_e::emit) {
          BOOST_LOG(error) << profile.source_kind << " " << profile.channels << "ch stopped: " << usb->error();
          break;
        }
        if(usb && usb->statistics().blocks==1){
          usb_first_frame=usb->metadata().firstFrame;
          if(awaiting_overflow){
            const auto gap=usb->lost_native_frames(), missing=gap/240;
            const auto next=static_cast<std::uint16_t>(delivered+usb_lost_frames/240);
            const bool can_continue=gap&&gap%240==0 && missing<=6 && delivered>=8 &&
                transport::representable_loss(next,static_cast<unsigned>(missing));
            usb_lost_frames+=gap;
            BOOST_LOG(warning)<<"Vibe Speakers USB overflow successor nonce="<<usb->session().header.nonce[0]<<":"<<usb->session().header.nonce[1]
              <<" epoch="<<usb->metadata().epoch<<" first_native_frame="<<usb_first_frame<<" gap_frames="<<gap
              <<" wire_continuation="<<can_continue<<" rtsp_restart_required="<<!can_continue;
            if(!can_continue)break;
            ++usb_resyncs;skipped_packets=static_cast<unsigned>(missing);awaiting_overflow=false;
          }
        }
      }
      timing.input_received(clock::now());
      buffer_t packet;
      if (!fixed_ingress) packet = buffer_t {1400};
      const auto destination = fixed_ingress ? std::span<std::uint8_t>(inline_packet) :
                                              std::span<std::uint8_t>(packet.begin(), packet.size());
      const auto lab_begin = lab_metrics.enabled() ? usb_lab_metrics::now() : 0;
      const auto bytes = encoder.encode(pcm, destination, error_message);
      const auto lab_end=lab_begin?usb_lab_metrics::now():0;
      if (bytes < 0) {
        BOOST_LOG(error) << profile.channels << "ch encode failed: " << error_message;
        break;
      }
      bool queued;
      if (fixed_ingress) queued = packets->try_raise(channel_data, std::span<const std::uint8_t>(inline_packet.data(), bytes), config.delivery,skipped_packets);
      else {
        packet.fake_resize(bytes);
        queued = packets->try_raise(channel_data, std::move(packet), config.delivery);
      }
      if (!queued) {
        ++overflow;
        BOOST_LOG(error) << profile.channels << "ch packet queue full/stopped; ending session without dropping a FEC shard";
        break;
      }
      ++delivered;
      if(usb&&lab_metrics.enabled())lab_metrics.record(usb->metadata(),usb->receive_qpc(),lab_begin,lab_end,usb_lab_metrics::now(),pcm);
      const auto depth = packets->occupancy().first;
      high_water = std::max(high_water, depth);
      if (!fixed_ingress && delivered % 200 == 0) BOOST_LOG(info) << "12ch queue current=" << depth * 240 << " frames/" << depth * 5 << " ms";
    }
  }

  void capture(safe::mail_t mail, config_t config, void *channel_data) {
    auto shutdown_event = mail->event<bool>(mail::shutdown);
    if (!config::audio.stream) {
      shutdown_event->view();
      return;
    }
    std::string error_message;
    if (!prepare_session(config, error_message)) {
      BOOST_LOG(error) << "Audio session rejected before capture: " << error_message;
      shutdown_event->raise(true);
      return;
    }
    if (config.validated->source_kind=="user_mode_usb") {
      capture_discrete(mail, config, channel_data);
      return;
    }
    const auto &profile = *config.validated;
    opus_stream_config_t stream {profile.sample_rate, profile.channels, profile.streams, profile.coupled, profile.mapping.data(), profile.bitrate};

    auto ref = get_audio_ctx_ref();
    if (!ref) {
      return;
    }

    audio_owner_acquired();
    auto owner_release = util::fail_guard([]() {
      audio_owner_released();
    });

    auto init_failure_fg = util::fail_guard([&shutdown_event]() {
      BOOST_LOG(error) << "Unable to initialize audio capture. The stream will not have audio."sv;

      // Wait for shutdown to be signalled if we fail init.
      // This allows streaming to continue without audio.
      shutdown_event->view();
    });

    auto &control = ref->control;
    if (!control) {
      return;
    }

    // Order of priority:
    // 1. Virtual sink
    // 2. Audio sink
    // 3. Host
    policy::sink_catalog_t sinks {ref->sink.host, std::nullopt, std::nullopt, std::nullopt};
    if (ref->sink.null) {
      sinks.stereo = ref->sink.null->stereo;
      sinks.surround51 = ref->sink.null->surround51;
      sinks.surround71 = ref->sink.null->surround71;
    }
    const auto sink = policy::select_stream_sink(
      sinks,
      config::audio.sink,
      config::audio.virtual_sink,
      stream.channelCount,
      config.flags[config_t::HOST_AUDIO]
    );

    bool capture_only = false;
#ifdef _WIN32
    capture_only = policy::capture_sink_without_routing(config::audio.sink_capture_only, config::audio.sink, config::audio.virtual_sink, sink);
#endif
    if (context_manager().assign(*ref.get(), sink, capture_only)) return;

    auto frame_size = config.packetDuration * stream.sampleRate / 1000;
    bool host_audio = config.flags[config_t::HOST_AUDIO];
    bool continuous_audio = config.flags[config_t::CONTINUOUS_AUDIO];
    auto mic = control->microphone(stream.mapping, stream.channelCount, stream.sampleRate, frame_size, continuous_audio, host_audio);
    if (!mic) {
      return;
    }

    // Audio is initialized, so we don't want to print the failure message
    init_failure_fg.disable();

    // Capture takes place on this thread
    platf::adjust_thread_priority(platf::thread_priority_e::critical);

    std::shared_ptr<sample_queue_t::element_type> samples;
    std::thread thread;
    if (!config.bypass_opus) {
      samples = std::make_shared<sample_queue_t::element_type>(30);
      thread = std::thread {encodeThread, samples, config, channel_data};
    }

    auto fg = util::fail_guard([&]() {
      dolby::finish_capture_session(
        samples,
        thread,
        [&]() { shutdown_event->view(); },
        [&]() { mic.reset(); }
      );
    });

    int samples_per_frame = frame_size * stream.channelCount;

    while (!shutdown_event->peek()) {
      std::vector<float> sample_buffer;
      sample_buffer.resize(samples_per_frame);

      auto status = mic->sample(sample_buffer);
      policy::sample_status_e policy_status;
      switch (status) {
        case platf::capture_e::ok:
          policy_status = policy::sample_status_e::ok;
          break;
        case platf::capture_e::timeout:
          policy_status = policy::sample_status_e::timeout;
          break;
        case platf::capture_e::reinit:
          policy_status = policy::sample_status_e::reinitialize;
          break;
        case platf::capture_e::interrupted:
          policy_status = policy::sample_status_e::interrupted;
          break;
        case platf::capture_e::error:
          policy_status = policy::sample_status_e::error;
          break;
      }
      const auto action = policy::sample_action(policy_status);
      switch (action) {
        case policy::sample_action_e::emit:
          break;
        case policy::sample_action_e::retry:
          continue;
        case policy::sample_action_e::reacquire:
          BOOST_LOG(info) << "Reinitializing audio capture"sv;
          mic.reset();
          do {
            mic = control->microphone(stream.mapping, stream.channelCount, stream.sampleRate, frame_size, continuous_audio, host_audio);
            if (!mic) {
              BOOST_LOG(warning) << "Couldn't re-initialize audio input"sv;
            }
          } while (!mic && !shutdown_event->view(5s));
          continue;
        case policy::sample_action_e::stop:
          return;
      }

      if (config.bypass_opus) {
        if (channel_data == nullptr) {
          webrtc_stream::submit_audio_frame(sample_buffer, stream.sampleRate, stream.channelCount, frame_size);
        }
      } else {
        samples->raise(std::move(sample_buffer));
      }
    }
  }

  audio_ctx_ref_t get_audio_ctx_ref() {
    if (discrete_source_enabled()) return {};
    return context_manager().acquire();
  }

  std::shared_ptr<void> capture_before_display_change() {
    if (discrete_source_enabled()) return {};
#ifdef _WIN32
    return context_manager().before_display_change(config::audio.stream);
#else
    return {};
#endif
  }

  void app_termination_requested() { if (!discrete_source_enabled()) context_manager().app_termination_requested(); }
  void app_started() { if (!discrete_source_enabled()) context_manager().app_started(); }
  void audio_owner_acquired() { context_manager().owner_acquired(); }
  void audio_owner_released() { context_manager().owner_released(); }

  bool ac3_available() {
    return avcodec_find_encoder(AV_CODEC_ID_AC3) != nullptr;
  }

  bool eac3_available() {
    return avcodec_find_encoder(AV_CODEC_ID_EAC3) != nullptr;
  }

  int dolby_bitrate_kbps(bool eac3, bool high_quality, int channels) {
    if (channels == 2) return high_quality ? 192 : 128;
    if (channels == 6) {
      if (eac3) return high_quality ? 384 : 192;
      return high_quality ? 640 : 384;
    }
    return 0;
  }

  dolby_codec_e configure_dolby_audio(
    config_t &config,
    std::string_view offered,
    std::string_view accepted,
    bool ac3_encoder_available,
    bool eac3_encoder_available,
    bool high_quality
  ) {
    using enum audio::dolby_codec_e;
    config.use_ac3 = false;
    config.use_eac3 = false;
    config.high_quality_dolby = false;
    const bool supported_channels = config.channels == 2 || config.channels == 6;
    auto selected = opus;
    if (supported_channels && offered == "AC3" && accepted == offered && ac3_encoder_available) {
      selected = ac3;
    } else if (supported_channels && offered == "EAC3" && accepted == offered && eac3_encoder_available) {
      selected = eac3;
    }
    if (selected != opus) {
      config.use_ac3 = selected == ac3;
      config.use_eac3 = selected == eac3;
      config.packetDuration = 32;
      config.flags[config_t::HIGH_QUALITY] = true;
      config.flags[config_t::CUSTOM_SURROUND_PARAMS] = false;
      config.high_quality_dolby = high_quality;
    }
    return selected;
  }

  std::string_view dolby_codec_name(dolby_codec_e codec) {
    using enum audio::dolby_codec_e;
    switch (codec) {
      case ac3: return "AC3";
      case eac3: return "EAC3";
      case opus:
      default: return "Opus";
    }
  }

  bool is_audio_ctx_sink_available(const audio_ctx_t &ctx) {
    if (!ctx.control) {
      return false;
    }

    const std::string &sink = ctx.sink.host.empty() ? config::audio.sink : ctx.sink.host;
    if (sink.empty()) {
      return false;
    }

    return ctx.control->is_sink_available(sink);
  }

  int map_stream(int channels, bool quality) {
    return policy::stream_index(channels, quality);
  }

  void apply_surround_params(opus_stream_config_t &stream, const stream_params_t &params) {
    policy::stream_layout_t base {
      stream.channelCount,
      stream.streams,
      stream.coupledStreams,
      {}
    };
    policy::stream_layout_t custom {
      params.channelCount,
      params.streams,
      params.coupledStreams,
      {}
    };
    std::copy_n(params.mapping, params.channelCount, custom.mapping.begin());
    const auto selected = policy::apply_custom_layout(base, custom);
    stream.channelCount = selected.channels;
    stream.streams = selected.streams;
    stream.coupledStreams = selected.coupled_streams;
    stream.mapping = params.mapping;
  }
}  // namespace audio

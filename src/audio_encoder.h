/** Production Opus boundary, shared with the offline host/client replay test. */
#pragma once
#include "audio_usb_source.h"
#include "audio_usb_session.h"
#include <array>
#include <cstdint>
#include <memory>
#include <span>
#include <string>

struct OpusMSEncoder;

namespace audio {
  struct session_profile_t {
    int sample_rate = 48000;
    int channels = 12;
    int streams = 8;
    int coupled = 4;
    std::array<std::uint8_t, 12> mapping {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11};
    int bitrate = 600000;
    int frame_size = 240;
    std::string source_kind;
    std::string source_path;
    unsigned test_seconds = 12;
    usb_source_config_t usb;
    usb_launch_config_t usb_launch;
  };
  // The host's advertised standard profiles and the real receiver harness use
  // this same table. Custom negotiated mappings are applied by prepare_session.
  bool standard_opus_profile(int channels, bool high_quality, session_profile_t &profile);

  class opus_encoder_t {
  public:
    opus_encoder_t() = default;
    ~opus_encoder_t();
    opus_encoder_t(const opus_encoder_t &) = delete;
    opus_encoder_t &operator=(const opus_encoder_t &) = delete;
    bool initialize(const session_profile_t &profile, std::string &error);
    bool start_usb_epoch(std::string &error);
    int encode(std::span<const float> pcm, std::span<std::uint8_t> output, std::string &error);
    int lookahead() const { return lookahead_; }
    int lsb_depth() const { return lsb_depth_; }
    // OPUS_GET_BITRATE sums child codec rates, excluding multistream framing.
    int child_bitrate() const { return child_bitrate_; }
    int effective_packet_bitrate() const { return effective_packet_bitrate_; }
    bool packet_budget_verified() const { return packet_budget_verified_; }
    static bool floating_point_build();
    static const char *version();
  private:
    OpusMSEncoder *encoder_ = nullptr;
    session_profile_t profile_;
    int lookahead_ = 0;
    int lsb_depth_ = 0;
    int child_bitrate_ = 0;
    int effective_packet_bitrate_ = 0;
    bool packet_budget_verified_ = false;
  };
}  // namespace audio

#include "audio_encoder.h"
#include "audio_protocol.h"
#include <opus/opus_multistream.h>
#include <algorithm>
#include <cmath>
#include <limits>
#include <cstring>

namespace audio {
  bool standard_opus_profile(int channels, bool high_quality, session_profile_t &profile) {
    const int index=policy::stream_index(channels,high_quality);
    if(index<0||index>6)return false;
    constexpr int rates[]={96000,512000,256000,1536000,450000,2048000,600000};
    constexpr int streams[]={1,1,4,6,5,8,8};
    constexpr int coupled[]={1,1,2,0,3,0,4};
    profile={};profile.channels=channels;profile.bitrate=rates[index];
    profile.streams=streams[index];profile.coupled=coupled[index];return true;
  }
  opus_encoder_t::~opus_encoder_t() {
    if (encoder_) opus_multistream_encoder_destroy(encoder_);
  }

  const char *opus_encoder_t::version() { return opus_get_version_string(); }

  bool opus_encoder_t::floating_point_build() { return std::strstr(version(), "-fixed") == nullptr; }

  bool opus_encoder_t::initialize(const session_profile_t &profile, std::string &error) {
    error.clear();
    if (encoder_) {
      error = "encoder already initialized; session format is immutable";
      return false;
    }
    if (profile.sample_rate != 48000 ||
        (profile.channels != 2 && profile.channels != 6 && profile.channels != 8 && profile.channels != 12) ||
        profile.streams <= 0 || profile.streams > profile.channels || profile.coupled < 0 ||
        profile.coupled > profile.channels || profile.coupled > profile.streams ||
        profile.streams + profile.coupled != profile.channels || profile.bitrate <= 0 ||
        (profile.frame_size != 120 && profile.frame_size != 240 && profile.frame_size != 480 &&
         profile.frame_size != 960 && profile.frame_size != 1920 && profile.frame_size != 2880)) {
      error = "unsupported Opus session profile";
      return false;
    }
    if (profile.channels == 12) {
      const policy::stream_layout_t layout {profile.channels, profile.streams, profile.coupled, profile.mapping};
      if (!protocol::is_discrete_layout(layout) || !protocol::valid_714_bitrate(profile.bitrate) || profile.frame_size != 240 ||
          (static_cast<std::int64_t>(profile.bitrate) * 5 + 7999) / 8000 > 1375) {
        error = "12ch requires 12/8/4 identity, 5ms CBR and a released packet-safe bitrate";
        return false;
      }
    }
    for (int channel = 0; channel < profile.channels; ++channel) {
      if (profile.mapping[channel] >= profile.channels) {
        error = "Opus mapping is outside the coded channel range";
        return false;
      }
    }
    if (profile.source_kind == "project_atmos_pcm714" || profile.source_kind == "user_mode_usb") {
      if (!floating_point_build()) {
        error = "Project Atmos requires a floating-point Opus build; fixed-point would reduce input precision";return false;
      }
      if (profile.frame_size != 240) {
        error = "Project Atmos requires actual 240-frame / 5ms packets";return false;
      }
    }
    int status = OPUS_OK;
    auto *candidate = opus_multistream_encoder_create(profile.sample_rate, profile.channels, profile.streams,
      profile.coupled, profile.mapping.data(), OPUS_APPLICATION_RESTRICTED_LOWDELAY, &status);
    if (!candidate || status != OPUS_OK) {
      if (candidate) opus_multistream_encoder_destroy(candidate);
      error = std::string("Opus encoder create: ") + opus_strerror(status);
      return false;
    }
    auto check = [&](int result, const char *operation) {
      if (result == OPUS_OK) return true;
      error = std::string(operation) + ": " + opus_strerror(result);
      return false;
    };
    if (!check(opus_multistream_encoder_ctl(candidate, OPUS_SET_BITRATE(profile.bitrate)), "OPUS_SET_BITRATE") ||
        !check(opus_multistream_encoder_ctl(candidate, OPUS_SET_VBR(0)), "OPUS_SET_VBR") ||
        !check(opus_multistream_encoder_ctl(candidate, OPUS_GET_BITRATE(&child_bitrate_)), "OPUS_GET_BITRATE child rates") ||
        !check(opus_multistream_encoder_ctl(candidate, OPUS_SET_LSB_DEPTH(24)), "OPUS_SET_LSB_DEPTH") ||
        !check(opus_multistream_encoder_ctl(candidate, OPUS_GET_LSB_DEPTH(&lsb_depth_)), "OPUS_GET_LSB_DEPTH") ||
        !check(opus_multistream_encoder_ctl(candidate, OPUS_GET_LOOKAHEAD(&lookahead_)), "OPUS_GET_LOOKAHEAD") || lsb_depth_ != 24) {
      if(error.empty())error="Opus did not retain the requested 24-bit input precision";
      opus_multistream_encoder_destroy(candidate);
      return false;
    }
    profile_ = profile;
    encoder_ = candidate;
    return true;
  }

  bool opus_encoder_t::start_usb_epoch(std::string &error) {
    if (!encoder_ || profile_.source_kind != "user_mode_usb") {
      error = "An initialized USB encoder is required for an input epoch change";
      return false;
    }
    opus_encoder_t fresh;
    if (!fresh.initialize(profile_, error)) return false;
    std::swap(encoder_, fresh.encoder_);
    lookahead_ = fresh.lookahead_;
    lsb_depth_ = fresh.lsb_depth_;
    child_bitrate_ = fresh.child_bitrate_;
    effective_packet_bitrate_ = 0;
    packet_budget_verified_ = false;
    return true;
  }

  int opus_encoder_t::encode(std::span<const float> pcm, std::span<std::uint8_t> output, std::string &error) {
    error.clear();
    const auto limit = profile_.channels == 12 ? 1375u : 1400u;
    if (!encoder_ || pcm.size() != static_cast<std::size_t>(profile_.channels * profile_.frame_size) ||
        output.empty() || std::any_of(pcm.begin(), pcm.end(), [](float value) { return !std::isfinite(value); })) {
      error = "invalid PCM block, output buffer, or uninitialized encoder";
      return -1;
    }
    const auto capacity = static_cast<opus_int32>(std::min<std::size_t>(output.size(), limit));
    if (profile_.channels == 12 && capacity < (static_cast<std::int64_t>(profile_.bitrate) * 5 + 7999) / 8000) {
      error = "packet buffer is too small for the fixed session bitrate";
      return -1;
    }
    const int bytes = opus_multistream_encode_float(encoder_, pcm.data(), profile_.frame_size, output.data(), capacity);
    if (bytes <= 0) {
      error = std::string("Opus encode: ") + opus_strerror(bytes);
      return -1;
    }
    const bool atmos = profile_.source_kind == "project_atmos_pcm714" || profile_.source_kind == "user_mode_usb";
    if ((profile_.channels == 12 || atmos) && bytes != profile_.bitrate / 1600) {
      error = "Opus payload did not match the fixed 5ms CBR session budget";
      return -1;
    }
    if (atmos) {
      // SET_BITRATE retains the aggregate request; GET_BITRATE reports child
      // rates and initially still sees their defaults. Observe it only as a
      // diagnostic. The real emitted packet, including multistream framing,
      // must meet the exact floor(requested / 1600) byte budget on EVERY call.
      const int result = opus_multistream_encoder_ctl(encoder_, OPUS_GET_BITRATE(&child_bitrate_));
      if (result != OPUS_OK) {
        error = std::string("OPUS_GET_BITRATE child rates after encode: ") + opus_strerror(result);
        return -1;
      }
      effective_packet_bitrate_ = bytes * 1600;
      packet_budget_verified_ = true;
    }
    return bytes;
  }
}  // namespace audio

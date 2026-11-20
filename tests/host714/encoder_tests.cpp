#include "src/audio_encoder.h"
#include "support/audio_source.h"
#include <opus/opus_multistream.h>
#include <array>
#include <cmath>
#include <cstdio>
#include <limits>
#include <stdexcept>
#include <vector>

static unsigned assertions = 0;
static void require(bool value, const char *message) {
  ++assertions;
  if (!value) throw std::runtime_error(message);
}

int main() {
  try {
    std::string error;
    for (int field = 0; field < 9; ++field) {
      audio::session_profile_t profile;
      if (field == 0) profile.bitrate = 3072000;
      if (field == 1) profile.streams = std::numeric_limits<int>::max();
      if (field == 2) profile.coupled = std::numeric_limits<int>::max();
      if (field == 3) profile.mapping[11] = 255;
      if (field == 4) profile.mapping[11] = 0;
      if (field == 5) profile.frame_size = 480;
      if (field == 6) profile.sample_rate = 44100;
      if (field == 7) profile.channels = -1;
      if (field == 8) profile.bitrate = -1;
      audio::opus_encoder_t encoder;
      require(!encoder.initialize(profile, error) && !error.empty(), "bad profile accepted");
    }
    for (unsigned seconds : {0u, 121u, std::numeric_limits<unsigned>::max()}) {
      bool rejected = false;
      try { audio::synthetic_source_t source(seconds); }
      catch (const std::invalid_argument &) { rejected = true; }
      require(rejected, "unbounded synthetic duration accepted");
    }
    for (int bitrate : {600000, 1536000, 2048000}) {
      audio::session_profile_t profile;
      profile.bitrate = bitrate;
      audio::opus_encoder_t encoder;
      require(encoder.initialize(profile, error), "encoder creation failed");
      require(!encoder.initialize(profile, error), "session encoder reinitialized");
      audio::synthetic_source_t source(1);
      int status = 0;
      auto *decoder = opus_multistream_decoder_create(48000, 12, 8, 4, profile.mapping.data(), &status);
      require(decoder && status == OPUS_OK, "decoder creation failed");
      std::vector<float> input, output, pcm(2880), decoded(2880);
      std::array<unsigned char, 1400> packet {};
      unsigned blocks = 0;
      while (source.read(pcm)) {
        input.insert(input.end(), pcm.begin(), pcm.end());
        const auto bytes = encoder.encode(pcm, packet, error);
        require(bytes == bitrate / 1600, "actual payload differs from fixed bitrate");
        require(opus_multistream_decode_float(decoder, packet.data(), bytes, decoded.data(), 240, 0) == 240, "decode failed");
        output.insert(output.end(), decoded.begin(), decoded.end());
        ++blocks;
      }
      require(blocks == 200 && source.exhausted() && source.frames_delivered() == 48000, "source EOF/timeline failure");
      require(!source.read(pcm), "source looped at EOF");
      const auto delay = encoder.lookahead();
      require(delay >= 0 && delay < 4800, "invalid delay");
      double minimum = 1;
      for (int channel = 0; channel < 12; ++channel) {
        double dot = 0, in_energy = 0, out_energy = 0;
        for (int frame = 4800; frame < 48000 - delay - 2400; ++frame) {
          const double x = input[static_cast<std::size_t>(frame * 12 + channel)];
          const double y = output[static_cast<std::size_t>((frame + delay) * 12 + channel)];
          dot += x * y; in_energy += x * x; out_energy += y * y;
        }
        require(in_energy > 0 && out_energy > 0, "silent channel");
        const double correlation = dot / std::sqrt(in_energy * out_energy);
        require(correlation > .95, "wrong channel/delay or poor reconstruction");
        minimum = std::min(minimum, correlation);
      }
      opus_multistream_decoder_destroy(decoder);
      require(encoder.encode(std::span<const float>(pcm.data(), pcm.size() - 1), packet, error) < 0, "short PCM accepted");
      pcm[0] = std::numeric_limits<float>::quiet_NaN();
      require(encoder.encode(pcm, packet, error) < 0, "NaN PCM accepted");
      pcm[0] = 0;
      require(encoder.encode(pcm, std::span<unsigned char>(packet.data(), 8), error) < 0, "clipped CBR accepted");
      std::printf("{\"bitrate\":%d,\"packets\":%u,\"payload_bytes\":%d,\"delay_samples\":%d,\"min_channel_correlation\":%.9f}\n",
        bitrate, blocks, bitrate / 1600, delay, minimum);
    }
    // Existing normal/HQ topologies remain usable through the same encoder.
    for (int channels : {2, 6, 8}) for (bool hq : {false, true}) {
      audio::session_profile_t profile;
      profile.channels = channels;
      profile.streams = channels == 2 ? 1 : hq ? channels : channels == 6 ? 4 : 5;
      profile.coupled = channels - profile.streams;
      profile.bitrate = channels == 2 ? hq ? 512000 : 96000 : channels == 6 ? hq ? 1536000 : 256000 : hq ? 2048000 : 450000;
      audio::opus_encoder_t encoder;
      require(encoder.initialize(profile, error), "legacy encoder rejected");
      std::vector<float> pcm(static_cast<std::size_t>(channels * 240), .01f);
      std::array<unsigned char, 1400> packet {};
      require(encoder.encode(pcm, packet, error) > 0, "legacy encode failed");
      require(!encoder.start_usb_epoch(error), "Legacy codec unexpectedly accepted a USB epoch reset");
    }
    for(int channels:{2,6,8,12}) {
      audio::session_profile_t profile;
      require(audio::standard_opus_profile(channels,false,profile),"USB production profile rejected");
      profile.source_kind="user_mode_usb";
      audio::opus_encoder_t encoder;
      require(encoder.initialize(profile,error),"USB Opus initialization failed");
      require(encoder.floating_point_build()&&encoder.lsb_depth()==24&&!encoder.packet_budget_verified(),"USB precision/pre-encode diagnostic mismatch");
      std::vector<float> pcm(static_cast<std::size_t>(channels)*240,1.0f/8388608.0f);
      std::array<std::uint8_t,1400> packet{};
      for(unsigned block=0;block<8;++block) {
        const auto bytes=encoder.encode(pcm,packet,error);
        require(bytes==profile.bitrate/1600&&encoder.packet_budget_verified()&&encoder.effective_packet_bitrate()==bytes*1600,"USB CBR packet precision/budget failed");
      }
      require(encoder.start_usb_epoch(error)&&!encoder.packet_budget_verified()&&encoder.lsb_depth()==24,"USB epoch encoder reset failed");
      audio::opus_encoder_t fresh;require(fresh.initialize(profile,error),"Cannot build independent fresh epoch encoder");
      std::array<std::uint8_t,1400> reference{};
      for(std::size_t i=0;i<pcm.size();++i)pcm[i]=static_cast<float>(0.08*std::sin(0.047*i));
      const auto first=encoder.encode(pcm,packet,error);const auto clean=fresh.encode(pcm,reference,error);
      require(first>0&&first==clean&&std::equal(packet.begin(),packet.begin()+first,reference.begin()),"New epoch retained prior encoder samples");
      profile.frame_size=480;audio::opus_encoder_t bad;
      require(!bad.initialize(profile,error),"USB accepted a non-240-frame encoder");
    }
    std::printf("{\"result\":\"PASS\",\"assertions\":%u,\"opus\":\"%s\",\"played\":false}\n", assertions, audio::opus_encoder_t::version());
    return 0;
  } catch (const std::exception &error) {
    std::fprintf(stderr, "FAIL after %u assertions: %s\n", assertions, error.what());
    return 1;
  }
}

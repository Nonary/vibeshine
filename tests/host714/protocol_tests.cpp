#include "src/audio_protocol.h"
#include "src/audio_display_transition.h"
#include "src/audio_startup_policy.h"

extern "C" {
#include "AudioConfig.h"
}

#include <cstdlib>
#include <iostream>
#include <limits>
#include <random>
#include <string>
#include <vector>

namespace {
  int assertions = 0;
  void check(bool result, const char *expression, int line) {
    ++assertions;
    if (!result) {
      std::cerr << "FAIL line " << line << ": " << expression << '\n';
      std::exit(1);
    }
  }
#define CHECK(expression) check(static_cast<bool>(expression), #expression, __LINE__)

  std::string offer(const audio::policy::stream_layout_t &layout, bool normal) {
    return "a=fmtp:97 surround-params=" + audio::protocol::format_surround_params(layout, normal) + "\r\n";
  }
}

int main() {
  using namespace audio::protocol;
  using audio::policy::stream_index;
  const std::string normal714 = "12,8,4,0,1,2,3,4,5,6,7,8,9,10,11";
  CHECK(parse_surround_params(normal714));
  CHECK(is_discrete_layout(*parse_surround_params(normal714)));
  CHECK(format_surround_params(discrete_layout(), false) == normal714);

  // The supplied parser-cases fixture, plus duplicate/silence and line-injection boundaries.
  const std::vector<std::string> invalid {
    "12,0,12,0,1,2,3,4,5,6,7,8,9,10,11",
    "12,1,11,0,1,2,3,4,5,6,7,8,9,10,11",
    "12,,8,4,0,1,2,3,4,5,6,7,8,9,10,11",
    "12,-1,13,0,1,2,3,4,5,6,7,8,9,10,11",
    "9999999999999999999999999999999999999999999999999999999999999999,8,4,0,1,2,3,4,5,6,7,8,9,10,11",
    normal714 + ",12", "12,8,4,0,1,2,3,4,5,6,7,8,9,10",
    "120,8,4,0,1,2,3,4,5,6,7,8,9,10,11",
    "12,8,4,0,1,2,3,4,5,6,7,8,9,10,12",
    "12,12,0,0,1,2,3,4,5,6,7,8,9,10,11",
    "12,8,4,0,1,2,3,4,5,6,7,8,9,10,10",
    "12,8,4,0,1,2,3,4,5,6,7,8,9,10,255",
    normal714 + ",", normal714 + " ", normal714 + "\r\na=x:1", " " + normal714,
    "", ",", std::string(129, '9'), "600012345", "617012345", "85301234568",
    "6,4,2,0,1,2,3,4,5", "22001", "21110"
  };
  for (const auto &value : invalid) CHECK(!parse_surround_params(value));
  CHECK(parse_surround_params("21101"));
  // A decoder can duplicate a lane. An encoder requires every coded channel.
  CHECK(!parse_surround_params("642012344"));
  CHECK(!parse_surround_params("85301234566"));
  CHECK(!valid_launch_audio((0x3fU << 16) | 6U, "642012344"));
  CHECK(!valid_launch_audio((0x63fU << 16) | 8U, "85301234566"));
  CHECK(valid_launch_audio((0x3fU << 16) | 6U, "642013245"));
  CHECK(valid_launch_audio((0x63fU << 16) | 8U, "85301324567"));

  CHECK(parse_uint32("0") == 0U);
  CHECK(parse_uint32("4294967295") == std::numeric_limits<std::uint32_t>::max());
  CHECK(parse_uint32("4131323916") == 0xF63F000CU);
  const auto client_packed = static_cast<std::uint32_t>(AUDIO_CONFIGURATION_714_SURROUND);
  const auto client_launch = SURROUNDAUDIOINFO_FROM_AUDIO_CONFIGURATION(client_packed);
  CHECK(client_packed == 4131327178U);
  CHECK(CHANNEL_COUNT_FROM_AUDIO_CONFIGURATION(client_packed) == 12U);
  CHECK(CHANNEL_MASK_FROM_AUDIO_CONFIGURATION(client_packed) == 0xF63FU);
  CHECK(client_launch == 4131323916U);
  CHECK(parse_uint32(std::to_string(client_launch)) == client_launch);
  CHECK(!parse_uint32("4294967296"));
  CHECK(!parse_uint32("999999999999999999999"));
  CHECK(!parse_uint32("-163643380"));
  CHECK(!parse_uint32("+12"));
  CHECK(!parse_uint32("12 "));
  CHECK(!parse_uint32(""));
  CHECK(valid_launch_audio(0xF63F000CU, normal714));
  CHECK(valid_launch_audio(0xF63F000CU, ""));
  CHECK(!valid_launch_audio(0x063F000CU, ""));
  CHECK(!valid_launch_audio(0xF63F0078U, ""));
  CHECK(!valid_launch_audio(0x003F0006U, normal714));

  for (const auto channels : {2, 6, 8, 12}) {
    CHECK(stream_index(channels, false) >= 0);
    CHECK(valid_launch_audio(channels == 12 ? 0xF63F000CU : static_cast<std::uint32_t>(channels), ""));
  }
  CHECK(stream_index(12, true) == -1);
  CHECK(stream_index(3, false) == -1);
  CHECK(validate_714_request(12, 0xF63F, 5, false, std::nullopt));
  CHECK(!validate_714_request(12, 0x2D63F, 5, false, std::nullopt));
  CHECK(!validate_714_request(12, 0xF63F, 10, false, std::nullopt));
  CHECK(!validate_714_request(12, 0xF63F, 5, true, std::nullopt));

  // Exercise the finished client's actual production parser against host output.
  OPUS_MULTISTREAM_CONFIGURATION normal {}, high {};
  bool hq = false;
  for (const auto channels : {6, 8}) {
    audio::policy::stream_layout_t low {channels, channels == 6 ? 4 : 5, channels == 6 ? 2 : 3, {}};
    auto full = low;
    full.streams = channels;
    full.coupled_streams = 0;
    for (int channel = 0; channel < channels; ++channel) low.mapping[channel] = full.mapping[channel] = static_cast<unsigned char>(channel);
    const auto sdp = offer(low, true) + offer(full, false) + offer(discrete_layout(), false);
    CHECK(LiParseAudioConfigurations(sdp.data(), sdp.size(), channels, &normal, &high, &hq) == 0);
    CHECK(hq);
    CHECK(normal.streams == low.streams && normal.coupledStreams == low.coupled_streams);
    CHECK(high.streams == channels && high.coupledStreams == 0);
    for (int channel = 0; channel < channels; ++channel) {
      CHECK(normal.mapping[channel] == channel);
      CHECK(high.mapping[channel] == channel);
    }
    std::swap(low.mapping[2], low.mapping[4]);
    const auto permuted_sdp = offer(low, true) + offer(low, false);
    CHECK(LiParseAudioConfigurations(permuted_sdp.data(), permuted_sdp.size(), channels, &normal, &high, &hq) == 0);
    for (int channel = 0; channel < channels; ++channel) {
      CHECK(normal.mapping[channel] == low.mapping[channel]);
      CHECK(high.mapping[channel] == low.mapping[channel]);
    }
    // Same custom topology in normal and HQ, with each offer's required mapping order.
    std::swap(low.mapping[2], low.mapping[4]);
    const auto custom_sdp = offer(low, true) + offer(low, false);
    CHECK(LiParseAudioConfigurations(custom_sdp.data(), custom_sdp.size(), channels, &normal, &high, &hq) == 0);
    for (int channel = 0; channel < channels; ++channel) {
      CHECK(normal.mapping[channel] == channel);
      CHECK(high.mapping[channel] == channel);
    }
  }
  const auto sdp714 = offer(discrete_layout(), false);
  CHECK(LiParseAudioConfigurations(sdp714.data(), sdp714.size(), 12, &normal, &high, &hq) == 0);
  CHECK(!hq);
  CHECK(normal.channelCount == 12 && normal.streams == 8 && normal.coupledStreams == 4);
  for (int channel = 0; channel < 12; ++channel) CHECK(normal.mapping[channel] == channel);

  attribute_map_t attributes;
  std::string_view client;
  const std::string announce = "v=0\r\ns=client\r\na=x-nv-audio.surround.numChannels:12 \r\na=x-nv-audio.surround.channelMask:63039\r\na=x-nv-audio.surround.AudioQuality:0\r\na=x-nv-aqos.packetDuration:5";
  CHECK(parse_announce_attributes(announce, attributes, client));
  CHECK(client == "client");
  CHECK(attributes.at("x-nv-aqos.packetDuration") == "5");
  CHECK(attributes.at("x-nv-audio.surround.numChannels") == "12");
  CHECK(!parse_announce_attributes(announce + "\na=x-nv-audio.surround.numChannels:12", attributes, client));
  CHECK(!parse_announce_attributes("a=x-nv-audio.surround.numChannels:", attributes, client));
  CHECK(!parse_announce_attributes("a=missing-colon", attributes, client));
  CHECK(!parse_announce_attributes(std::string("a=x:1\0x", 7), attributes, client));
  CHECK(!parse_announce_attributes(std::string(65537, 'a'), attributes, client));
  CHECK(!parse_announce_attributes("a=x:" + std::string(2049, 'a'), attributes, client));

  for (const auto bitrate : {600000, 1536000, 2048000}) {
    CHECK(valid_714_bitrate(bitrate));
    CHECK(reservation_714_kbps(bitrate, false) > bitrate * 3 / 2000);
    CHECK(reservation_714_kbps(bitrate, true) >= reservation_714_kbps(bitrate, false));
  }
  CHECK(!valid_714_bitrate(3072000));
  CHECK(reservation_714_kbps(3072000, true) == -1);

  // A 12ch request's deliberately empty display callback cannot create an
  // audio context. This invokes the real lazy hold implementation with spies.
  unsigned context_calls = 0;
  const auto capture = [&]() -> std::shared_ptr<void> { ++context_calls; return std::make_shared<int>(1); };
  audio::display_change_hold_t isolated {audio::display_change_hold_t::capture_f {}};
  std::shared_ptr<void> guard;
  isolated.before([] {});
  isolated.capture_if_empty_and_attach(guard);
  CHECK(context_calls == 0 && !guard);
  audio::display_change_hold_t legacy {capture};
  legacy.before([] {});
  legacy.capture_if_empty_and_attach(guard);
  CHECK(context_calls == 1 && guard);

  // Boot-only settings cannot be changed by config reset/hot apply. An early
  // read of defaults must not latch "off" before initial configuration parse.
  audio::startup::settings_store_t startup;
  {
    audio::startup::settings_store_t sessions;
    audio::startup::settings_t first;first.source="off";
    CHECK(sessions.publish(first));
    const auto retained=sessions.get();
    first.source="vibe_usb";first.usb.sample_format="pcm16";
    sessions.replace(first);
    CHECK(retained.source=="off");
    CHECK(sessions.get().source=="vibe_usb"&&sessions.get().usb.sample_format=="pcm16");
    CHECK(!sessions.publish(first));
  }
  CHECK(startup.get().source == "off");
  CHECK(startup.publish({"synthetic", 1536000}));
  CHECK(startup.get().source == "synthetic");
  CHECK(!startup.publish({"off", 600000}));
  CHECK(startup.get().bitrate == 1536000);
  std::unordered_map<std::string, std::string> reloaded {
    {"audio_714_source", "off"}, {"audio_714_bitrate", "3072000"}, {"video_unrelated", "preserve"}
  };
  CHECK(startup.preserve_on_reload(reloaded));
  CHECK(reloaded.at("audio_714_source") == "synthetic");
  CHECK(reloaded.at("audio_714_bitrate") == "1536000");
  CHECK(reloaded.at("video_unrelated") == "preserve");
  CHECK(!startup.preserve_on_reload(reloaded));
  CHECK(!audio::startup::snapshot_callback(startup.get(), 8, capture));
  CHECK(!audio::startup::snapshot_callback(startup.get(), 12, capture));
  CHECK(!audio::startup::snapshot_callback({}, 12, capture));
  CHECK(audio::startup::snapshot_callback({}, 8, capture));

  // Removed private/kernel source values never get a shared Default context.
  for(const auto *source:{"project_atmos","synthetic","endpoint","unknown"}) {
    audio::startup::settings_t removed;removed.source=source;
    for(int channels:{2,6,8,12})CHECK(!audio::startup::shared_context_allowed(removed,channels));
  }

  // Deterministic malformed input corpus through the production bounded parser.
  std::mt19937 random(714);
  for (int iteration = 0; iteration < 50000; ++iteration) {
    std::string fuzz(random() % 270, '\0');
    for (auto &character : fuzz) character = static_cast<char>(random() & 0x7F);
    const auto parsed = parse_surround_params(fuzz);
    if (parsed) {
      CHECK(parsed->channels == 2 || parsed->channels == 6 || parsed->channels == 8 || is_discrete_layout(*parsed));
    }
    (void) parse_uint32(fuzz);
    (void) parse_announce_attributes(fuzz, attributes, client);
  }
  std::cout << "PASS protocol: " << assertions << " assertions; 50000 malformed-input cases; actual Common SDP parser; no devices/sockets\n";
  return 0;
}

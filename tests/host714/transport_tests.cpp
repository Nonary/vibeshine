// Offline integration of the production host encoder/packetizer and the actual
// Common receiver replay adapter. No socket, audio endpoint or routing is opened.
#include "src/audio_encoder.h"
#include "support/audio_source.h"
#include "src/audio_transport.h"
#include <AudioTestAdapter.h>
extern "C" {
#include <AudioConfig.h>
#include "src/rswrapper.h"
}
#include <opus_multistream.h>
#include <openssl/evp.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
  reed_solomon_release_t original_release = nullptr;
  unsigned reed_releases = 0;
  void count_reed_release(reed_solomon *value) {
    ++reed_releases;
    original_release(value);
  }
  bool constructor_failure_cleanup() {
    // Isolated process. Make encrypted initialization unavailable after the
    // real RS allocation without altering Windows settings.
    if (EVP_set_default_properties(nullptr, "provider=vibelab_missing") != 1) return false;
    original_release = reed_solomon_release;
    reed_solomon_release = count_reed_release;
    bool rejected = false;
    try {
      audio::transport::config_t config;
      config.encrypted = true;
      audio::transport::packetizer_t packetizer(config);
    } catch (const std::runtime_error& e) {
      rejected = std::strcmp(e.what(), "Audio AES-CBC initialization failed") == 0;
    }
    reed_solomon_release = original_release;
    const bool reset = EVP_set_default_properties(nullptr, nullptr) == 1;
    std::printf("{\"aes_rejected\":%s,\"rs_releases\":%u,\"process_properties_restored\":%s}\n",
                rejected ? "true" : "false", reed_releases, reset ? "true" : "false");
    return rejected && reed_releases == 1 && reset;
  }
  using bytes_t = std::vector<std::uint8_t>;
  constexpr int channels = 12, frames = 240, packet_count = 200;
  constexpr std::uint32_t key_id = 0xF7142100U;
  int assertions = 0, failures = 0, cases = 0;

  bool check(bool condition, const char *message) {
    ++assertions;
    if (!condition) { ++failures; std::fprintf(stderr, "FAIL %s\n", message); }
    return condition;
  }
  unsigned be16(const std::uint8_t *p) { return (static_cast<unsigned>(p[0]) << 8) | p[1]; }
  std::uint32_t be32(const std::uint8_t *p) { return (be16(p) << 16) | be16(p + 2); }
  void put32(std::uint8_t *p, std::uint32_t value) {
    for (unsigned i = 0; i < 4; ++i) p[i] = static_cast<std::uint8_t>(value >> (24 - 8 * i));
  }
  audio::transport::config_t transport_config(bool encrypted, bool wrap = false) {
    audio::transport::config_t value;
    value.encrypted = encrypted;
    value.discrete_714 = true;
    value.key_id = key_id;
    for (unsigned i = 0; i < value.key.size(); ++i) value.key[i] = static_cast<std::uint8_t>(17 + i * 13);
    if (wrap) { value.sequence = 65528; value.timestamp = 0xFFFFFFE0U; }
    return value;
  }

  // Independent channel signatures with broadband content and low/mid tones.
  // All amplitudes are bounded; this fixture is decoded only to memory.
  float signal(int channel, int frame) {
    const auto noise = [](int c, int t) {
      std::uint32_t x = static_cast<std::uint32_t>(t) ^ (0x9e3779b9U * static_cast<std::uint32_t>(c + 1));
      x ^= x >> 16; x *= 0x7feb352dU; x ^= x >> 15; x *= 0x846ca68bU; x ^= x >> 16;
      return static_cast<double>(x & 65535U) / 32767.5 - 1.0;
    };
    constexpr double pi = 3.14159265358979323846;
    double value = 0;
    for (int tap = 0; tap < 5; ++tap) value += noise(channel, frame - tap) * .035;
    value += .065 * std::sin(2 * pi * (61 + channel * 97) * frame / 48000.0);
    value += .04 * std::sin(2 * pi * (941 + channel * 211) * frame / 48000.0);
    if ((frame + channel * 151) % (997 + channel * 20) == 0) value += .25;
    // Explicit 10 ms onset and end ramps.
    const double ramp = std::min({1.0, frame / 480.0, (packet_count * frames - 1 - frame) / 480.0});
    return static_cast<float>(value * std::max(0.0, ramp));
  }

  struct corpus_t {
    int bitrate = 0;
    int lookahead = 0;
    std::vector<float> pcm;
    std::vector<bytes_t> encoded;
    bool production_synthetic = false;
  };

  corpus_t make_corpus(int bitrate, bool production_synthetic = false) {
    corpus_t corpus;
    corpus.bitrate = bitrate;
    corpus.production_synthetic = production_synthetic;
    audio::session_profile_t profile;
    profile.bitrate = bitrate;
    audio::opus_encoder_t encoder;
    std::string error;
    if (!check(encoder.initialize(profile, error), "production encoder initializes agreed session")) {
      throw std::runtime_error(error);
    }
    corpus.lookahead = encoder.lookahead();
    check(corpus.lookahead >= 0 && corpus.lookahead < 4800, "reported production encoder delay bounded");
    std::array<float, channels * frames> pcm {};
    std::array<std::uint8_t, 1400> encoded {};
    audio::synthetic_source_t source(1);
    for (int packet = 0; packet < packet_count; ++packet) {
      if (production_synthetic) {
        if (!check(source.read(pcm), "bounded production synthetic source delivers expected block")) break;
      } else {
        for (int frame = 0; frame < frames; ++frame) for (int channel = 0; channel < channels; ++channel) {
          pcm[static_cast<std::size_t>(frame * channels + channel)] = signal(channel, packet * frames + frame);
        }
      }
      corpus.pcm.insert(corpus.pcm.end(), pcm.begin(), pcm.end());
      const auto bytes = encoder.encode(pcm, encoded, error);
      if (!check(bytes == bitrate / 1600, "actual production encoder fixed 5ms CBR payload")) {
        throw std::runtime_error(error.empty() ? "incorrect CBR length" : error);
      }
      corpus.encoded.emplace_back(encoded.begin(), encoded.begin() + bytes);
    }
    if (production_synthetic) {
      check(source.exhausted() && source.frames_delivered() == 48000 && !source.read(pcm),
            "production synthetic EOF is finite and never loops");
    }
    return corpus;
  }

  void encoder_errors() {
    ++cases;
    std::string error;
    for (int kind = 0; kind < 10; ++kind) {
      audio::session_profile_t profile;
      if (kind == 0) profile.bitrate = 3072000;
      if (kind == 1) profile.streams = std::numeric_limits<int>::max();
      if (kind == 2) profile.coupled = std::numeric_limits<int>::max();
      if (kind == 3) profile.channels = std::numeric_limits<int>::max();
      if (kind == 4) profile.mapping[11] = 0;
      if (kind == 5) profile.mapping[11] = 255;
      if (kind == 6) profile.frame_size = 120;
      if (kind == 7) profile.frame_size = 480;
      if (kind == 8) profile.bitrate = -1;
      if (kind == 9) profile.sample_rate = 44100;
      audio::opus_encoder_t encoder;
      check(!encoder.initialize(profile, error) && !error.empty(), "production encoder rejects incompatible twelve-channel profile");
    }
    audio::opus_encoder_t encoder;
    audio::session_profile_t profile;
    std::array<float, channels * frames> pcm {};
    std::array<std::uint8_t, 1400> encoded {};
    check(encoder.encode(pcm, encoded, error) < 0, "uninitialized production encoder safely rejects PCM");
    if (!check(encoder.initialize(profile, error), "production encoder initializes for negative input checks")) return;
    check(encoder.encode(std::span<const float>(pcm.data(), pcm.size() - 1), encoded, error) < 0,
          "production encoder rejects short input block");
    pcm[0] = std::numeric_limits<float>::infinity();
    check(encoder.encode(pcm, encoded, error) < 0, "production encoder rejects nonfinite Float32");
    pcm[0] = 0;
    check(encoder.encode(pcm, std::span<std::uint8_t>(encoded.data(), 374), error) < 0,
          "production encoder rejects undersized buffer instead of clipping bitrate");
  }

  struct receive_t {
    const corpus_t *corpus = nullptr;
    OpusMSDecoder *decoder = nullptr;
    std::vector<float> pcm;
    int cursor = 4;
    int plc = 0;
    int expected_missing = -1;
    int missing_count = 1;
  };
  receive_t *receiver = nullptr;

  void decode(char *data, int length) {
    auto &state = *receiver;
    if (!check(state.cursor < packet_count, "receiver callback count is bounded")) return;
    const auto &expected = state.corpus->encoded[static_cast<std::size_t>(state.cursor)];
    if (!data || length == 0) {
      ++state.plc;
      check(state.expected_missing>=0 && state.cursor>=state.expected_missing && state.cursor<state.expected_missing+state.missing_count,
            "PLC occurs only for injected unrecoverable packets");
    } else {
      check(state.expected_missing<0 || state.cursor<state.expected_missing || state.cursor>=state.expected_missing+state.missing_count,
            "receiver does not invent missing payload");
      check(length == static_cast<int>(expected.size()) && std::memcmp(data, expected.data(), expected.size()) == 0,
            "actual Common AES/FEC/reorder preserves host Opus bytes");
    }
    std::array<float, frames * channels> pcm {};
    const int decoded = opus_multistream_decode_float(state.decoder, reinterpret_cast<unsigned char *>(data), length,
                                                       pcm.data(), frames, 0);
    check(decoded == frames, "actual decoder produces 240 frames of 12 channels");
    check(std::all_of(pcm.begin(), pcm.end(), [](float x) { return std::isfinite(x); }), "decoded PCM is finite");
    state.pcm.insert(state.pcm.end(), pcm.begin(), pcm.end());
    ++state.cursor;
  }

  double correlate(const corpus_t &corpus, const std::vector<float> &decoded) {
    double minimum = 1;
    const int end = static_cast<int>(decoded.size() / channels) - corpus.lookahead;
    for (int channel = 0; channel < channels; ++channel) {
      double energy = 0;
      std::array<double, channels> cross {}, reference_energy {};
      for (int frame = 960; frame < end; ++frame) {
        const double output = decoded[static_cast<std::size_t>((frame + corpus.lookahead) * channels + channel)];
        energy += output * output;
        for (int reference = 0; reference < channels; ++reference) {
          const double input = corpus.pcm[static_cast<std::size_t>((frame + 4 * frames) * channels + reference)];
          cross[reference] += input * output;
          reference_energy[reference] += input * input;
        }
      }
      int dominant = -1;
      double strongest = -1, own = 0;
      for (int reference = 0; reference < channels; ++reference) {
        const auto denominator = std::sqrt(reference_energy[reference] * energy);
        const auto value = denominator > 0 ? cross[reference] / denominator : 0;
        if (reference == channel) own = value;
        if (value > strongest) { strongest = value; dominant = reference; }
      }
      check(dominant == channel, "dominant input/output channel identity including LFE and heights");
      check(own > (corpus.production_synthetic ? .95 : .35), "encoder-delay-corrected correlation meets corpus threshold");
      check(energy > .001, "each of twelve decoded channels contains a signal");
      minimum = std::min(minimum, own);
      std::printf("{\"event\":\"channel\",\"production_synthetic\":%s,\"bitrate\":%d,\"channel\":%d,\"dominant_input\":%d,\"correlation\":%.8f}\n",
                  corpus.production_synthetic ? "true" : "false", corpus.bitrate, channel, dominant, own);
    }
    return minimum;
  }

  // mode: 0 intact; 1 one loss per block; 2 two losses; 3 reorder+duplicate;
  // 4 unrecoverable loss -> actual PLC; 5 simultaneous sequence/timestamp wrap.
  void run(const corpus_t &corpus, bool encrypted, int mode) {
    ++cases;
    const int before = failures;
    const auto config = transport_config(encrypted, mode == 5);
    audio::transport::packetizer_t sender(config);
    std::vector<bytes_t> wire;
    wire.reserve(packet_count * 3 / 2);
    for (const auto &payload : corpus.encoded) {
      check(sender.push(payload, [&](auto header, auto data) {
        bytes_t datagram(header.begin(), header.end());
        datagram.insert(datagram.end(), data.begin(), data.end());
        wire.push_back(std::move(datagram));
      }), "production sender accepts fixed profile");
    }
    if (!check(wire.size() == packet_count * 3 / 2, "production sender emits four data and two parity datagrams per block")) return;
    for (int base = 0; base < packet_count; base += 4) {
      for (int shard = 0; shard < 6; ++shard) {
        const auto &packet = wire[static_cast<std::size_t>(base / 4 * 6 + shard)];
        const auto sequence = static_cast<std::uint16_t>(config.sequence + base + shard);
        check(packet[0] == 0x80 && packet[1] == (shard < 4 ? 97 : 127), "production wire RTP type");
        check(be16(packet.data() + 2) == sequence, "production RTP sequence including wrap");
        check(packet.size() <= 1400, "actual production UDP payload stays inside client limit");
        if (shard < 4) check(be32(packet.data() + 4) == config.timestamp + static_cast<std::uint32_t>((base + shard) * 5), "production RTP timestamp advances by five milliseconds");
        else {
          check(be32(packet.data() + 4) == 0, "historical parity RTP timestamp stays zero");
          check(packet[12] == shard - 4 && packet[13] == 97, "production parity shard identity");
          check(be16(packet.data() + 14) == static_cast<std::uint16_t>(config.sequence + base), "production FEC base sequence");
          check(be32(packet.data() + 16) == config.timestamp + static_cast<std::uint32_t>(base * 5), "production FEC base timestamp including wrap");
        }
      }
    }

    constexpr char sdp[] = "a=fmtp:97 surround-params=12,8,4,0,1,2,3,4,5,6,7,8,9,10,11\r\n";
    OPUS_MULTISTREAM_CONFIGURATION normal {}, high {};
    bool high_supported = false;
    check(LiParseAudioConfigurations(sdp, sizeof(sdp) - 1, channels, &normal, &high, &high_supported) == 0,
          "actual client SDP parser accepts sole normal 12/8/4 offer");
    check(normal.channelCount == 12 && normal.streams == 8 && normal.coupledStreams == 4 && !high_supported,
          "actual client normal topology and no automatic HQ");
    int status = OPUS_OK;
    receive_t state;
    state.corpus = &corpus;
    state.expected_missing = mode == 4 ? 14 : -1;
    state.decoder = opus_multistream_decoder_create(48000, normal.channelCount, normal.streams,
                                                    normal.coupledStreams, normal.mapping, &status);
    if (!check(state.decoder && status == OPUS_OK, "decoder created from actual parsed client topology")) return;
    receiver = &state;
    std::array<unsigned char, 16> iv {};
    put32(iv.data(), key_id);
    LiTestAudioBegin(encrypted, config.key.data(), iv.data(), decode);
    const auto push = [&](const bytes_t &packet) {
      check(LiTestAudioPush(packet.data(), static_cast<int>(packet.size())) == 0, "actual Common receiver accepts bounded production datagram");
    };
    // The production receiver intentionally synchronizes after the first block.
    for (int shard = 0; shard < 6; ++shard) push(wire[static_cast<std::size_t>(shard)]);
    for (int base = 4; base < packet_count; base += 4) {
      const auto first = static_cast<std::size_t>(base / 4 * 6);
      if (mode == 1) {
        for (int shard : {0, 2, 3, 4, 5, 5}) push(wire[first + shard]);
      } else if (mode == 2) {
        for (int shard : {2, 3, 4, 5, 4}) push(wire[first + shard]);
      } else if (mode == 3) {
        for (int shard : {1, 1, 0, 3, 2, 4, 5}) push(wire[first + shard]);
      } else {
        for (int shard = 0; shard < 6; ++shard) {
          // Both parity shards are withheld in this block, so the missing data
          // becomes actual Common PLC when subsequent blocks establish a gap.
          if (mode == 4 && base == 12 && (shard == 2 || shard >= 4)) continue;
          push(wire[first + shard]);
        }
      }
    }
    LiTestAudioEnd();
    receiver = nullptr;
    opus_multistream_decoder_destroy(state.decoder);
    check(state.cursor == packet_count, "all post-sync callbacks received in order");
    check(state.plc == (mode == 4 ? 1 : 0), "exact expected PLC callback count");
    double correlation = -1;
    if (mode == 0 && state.cursor == packet_count) correlation = correlate(corpus, state.pcm);
    const auto &stats = sender.statistics();
    check(stats.plaintext_min == static_cast<std::size_t>(corpus.bitrate / 1600) && stats.plaintext_max == stats.plaintext_min,
          "actual production CBR payload equals requested bitrate");
    const double seconds = packet_count * .005;
    std::printf("{\"event\":\"production_transport\",\"result\":\"%s\",\"production_synthetic\":%s,\"bitrate\":%d,\"encrypted\":%s,\"mode\":%d,"
                "\"callbacks\":%d,\"plc\":%d,\"opus_min\":%zu,\"opus_max\":%zu,\"opus_mean\":%.4f,"
                "\"udp_min\":%zu,\"udp_max\":%zu,\"udp_mean\":%.4f,\"opus_bps\":%.0f,\"udp_bps\":%.0f,"
                "\"minimum_correlation\":%.8f}\n",
                before == failures ? "PASS" : "FAIL", corpus.production_synthetic ? "true" : "false", corpus.bitrate, encrypted ? "true" : "false", mode,
                state.cursor - 4, state.plc, stats.plaintext_min, stats.plaintext_max,
                static_cast<double>(stats.plaintext_bytes) / stats.input_packets, stats.datagram_min, stats.datagram_max,
                static_cast<double>(stats.datagram_bytes) / stats.datagrams, stats.plaintext_bytes * 8 / seconds,
                stats.datagram_bytes * 8 / seconds, correlation);
  }

  int raw_count = 0;
  std::vector<bytes_t> raw_output;
  void raw_decode(char *data, int length) {
    ++raw_count;
    if (data && length > 0) raw_output.emplace_back(data, data + length);
  }

  void legacy_packetization() {
    // The same extracted production packetizer also services the old profiles.
    // Exercise their actual payload budgets through Common with one loss/block.
    // Encoder topology and legacy mapping roundtrips have separate real-code tests.
    for (const std::size_t size : {60u, 320u, 160u, 960u, 281u, 1280u}) {
      for (bool encrypted : {false, true}) {
        ++cases;
        const auto previous_failures = failures;
        auto config = transport_config(encrypted);
        config.discrete_714 = false;
        audio::transport::packetizer_t sender(config);
        std::vector<bytes_t> wire, expected;
        for (unsigned index = 0; index < 12; ++index) {
          bytes_t payload(size, static_cast<std::uint8_t>(index + 31));
          if (index >= 4) expected.push_back(payload);
          check(sender.push(payload, [&](auto header, auto data) {
            bytes_t value(header.begin(), header.end());
            value.insert(value.end(), data.begin(), data.end());
            wire.push_back(std::move(value));
          }), "legacy production packetizer accepts normal/HQ payload budget");
        }
        if (!check(wire.size() == 18, "legacy production packetizer emits all data/FEC")) continue;
        std::array<unsigned char, 16> iv {}; put32(iv.data(), key_id);
        raw_count = 0; raw_output.clear();
        LiTestAudioBegin(encrypted, config.key.data(), iv.data(), raw_decode);
        for (std::size_t index = 0; index < wire.size(); ++index) {
          if (index >= 6 && index % 6 == 1) continue;
          const auto &datagram = wire[index];
          check(LiTestAudioPush(datagram.data(), static_cast<int>(datagram.size())) == 0,
                "actual Common accepts legacy production data/FEC");
        }
        check(raw_count == 8 && raw_output == expected, "actual Common loss recovery preserves legacy payload bytes/order");
        LiTestAudioEnd();
        std::printf("{\"event\":\"legacy_packetizer\",\"result\":\"%s\",\"payload_bytes\":%zu,\"encrypted\":%s}\n",
                    failures == previous_failures ? "PASS" : "FAIL", size, encrypted ? "true" : "false");
      }
    }
  }

  void production_malformed() {
    ++cases;
    const auto config = transport_config(true);
    audio::transport::packetizer_t sender(config);
    std::vector<bytes_t> wire;
    bytes_t payload(375, 0x29);
    for (int index = 0; index < 8; ++index) {
      check(sender.push(payload, [&](auto header, auto data) {
        bytes_t value(header.begin(), header.end());
        value.insert(value.end(), data.begin(), data.end());
        wire.push_back(std::move(value));
      }), "malformed replay corpus generated by production sender");
    }
    if (!check(wire.size() == 12, "malformed replay has complete production FEC blocks")) return;
    std::array<unsigned char, 16> iv {}; put32(iv.data(), key_id);
    raw_count = 0; raw_output.clear();
    LiTestAudioBegin(true, config.key.data(), iv.data(), raw_decode);
    for (unsigned index = 0; index < 6; ++index) {
      check(LiTestAudioPush(wire[index].data(), static_cast<int>(wire[index].size())) == 0,
            "actual production first block synchronizes malformed replay");
    }
    check(raw_count == 0, "initial synchronization does not reach decoder");
    for (int size = 0; size < 12; ++size) {
      check(LiTestAudioPush(wire[6].data(), size) == -1, "actual production RTP header truncation rejected");
    }
    for (int size = 12; size <= 24; ++size) {
      check(LiTestAudioPush(wire[10].data(), size) == 0, "actual production FEC truncation safely dropped");
    }
    auto bad_parity = wire[10];
    bad_parity[12] = 2;
    check(LiTestAudioPush(bad_parity.data(), static_cast<int>(bad_parity.size())) == 0,
          "actual production FEC invalid shard index safely dropped");
    check(raw_count == 0, "malformed parity never reaches decode callback");
    check(LiTestAudioPush(wire[6].data(), static_cast<int>(wire[6].size() - 1)) == 0,
          "actual production CBC nonblock truncation safely dropped");
    check(raw_count == 0, "truncated production ciphertext does not reach decoder");
    // The length check leaves sequence 4 missing. A full-length CBC packet with
    // invalid PKCS#7 padding passes the queue, but must fail before decoding.
    // Flip the preceding cipher block's final byte to turn padding 9 into 0.
    auto bad_padding = wire[6];
    bad_padding[bad_padding.size() - 17] ^= 9;
    check(LiTestAudioPush(bad_padding.data(), static_cast<int>(bad_padding.size())) == 0,
          "actual production CBC corrupt padding safely dropped");
    check(raw_count == 0, "corrupt production CBC padding never reaches decoder");
    check(LiTestAudioPush(wire[7].data(), static_cast<int>(wire[7].size())) == 0,
          "actual production receiver recovers next independent CBC IV");
    check(raw_count == 1 && raw_output.front() == payload, "next production packet decrypts after corrupt CBC padding");
    LiTestAudioEnd();
    // A new source/session receives a fresh queue and crypto state, not the
    // previous session's partial block or duplicate history.
    raw_count = 0; raw_output.clear();
    LiTestAudioBegin(true, config.key.data(), iv.data(), raw_decode);
    for (const auto &datagram : wire) {
      check(LiTestAudioPush(datagram.data(), static_cast<int>(datagram.size())) == 0,
            "production replay after normal receiver stop/reconnect");
    }
    check(raw_count == 4 && std::all_of(raw_output.begin(), raw_output.end(), [&](const auto &value) { return value == payload; }),
          "stop/reconnect resets all actual receiver state");
    LiTestAudioEnd();
  }

  void boundaries() {
    ++cases;
    for (bool encrypted : {false, true}) {
      for (std::size_t length : {std::size_t(375), std::size_t(960), std::size_t(1280), std::size_t(1375)}) {
        auto config = transport_config(encrypted);
        audio::transport::packetizer_t sender(config);
        bytes_t payload(length, 0x55);
        std::vector<bytes_t> wire;
        const auto collect = [&](auto header, auto data) {
          bytes_t value(header.begin(), header.end()); value.insert(value.end(), data.begin(), data.end()); wire.push_back(value);
        };
        for (int i = 0; i < 12; ++i) check(sender.push(payload, collect), "production boundary-size push");
        if (!check(wire.size() == 18, "boundary sender produced all expected datagrams")) continue;
        check(wire[5].size() == audio::transport::datagram_size(length, encrypted, true), "actual FEC datagram includes exact PKCS#7 padding");
        check(wire[5].size() <= 1400, "actual boundary FEC datagram fits client");
        std::array<unsigned char, 16> iv {}; put32(iv.data(), key_id);
        raw_count = 0; raw_output.clear();
        LiTestAudioBegin(encrypted, config.key.data(), iv.data(), raw_decode);
        for (const auto &packet : wire) check(LiTestAudioPush(packet.data(), static_cast<int>(packet.size())) == 0, "boundary packet accepted by real receiver");
        check(raw_count == 8, "boundary payload callbacks after synchronization");
        check(std::all_of(raw_output.begin(), raw_output.end(), [&](const auto &decoded) { return decoded == payload; }), "boundary ciphertext decrypted identically");
        LiTestAudioEnd();

        // Sender refuses a session payload-size change, even at a block boundary.
        payload.push_back(0x44);
        const auto prior = wire.size();
        check(!sender.push(payload, collect) && wire.size() == prior, "CBR change rejected before emitting data");
      }
      for (std::size_t length : {std::size_t(1376), std::size_t(1400), std::size_t(1920)}) {
        audio::transport::packetizer_t sender(transport_config(encrypted));
        bytes_t payload(length, 0x44);
        int emitted = 0;
        check(!sender.push(payload, [&](auto, auto) { ++emitted; }) && emitted == 0, "oversize including 3072k rejected by actual sender before transmission");
      }
    }
    // These are additional real receiver negative/regression tests provided by
    // the completed client work, compiled without modifying its source tree.
    check(runAudioReceiveTests() == 0, "actual Common malformed/truncated/duplicate/PLC regression suite");
  }
}  // namespace

static void verify_usb_loss(const corpus_t& corpus,int start,int lost){
  auto config=transport_config(true);
  audio::transport::packetizer_t sender(config);
  receive_t state;state.corpus=&corpus;state.expected_missing=start;state.missing_count=lost;
  std::array<unsigned char,12> mapping{0,1,2,3,4,5,6,7,8,9,10,11};
  int status=0;state.decoder=opus_multistream_decoder_create(48000,12,8,4,mapping.data(),&status);
  check(status==OPUS_OK&&state.decoder,"Real libopus loss decoder initialized");
  receiver=&state;unsigned char iv[16]{};put32(iv,config.key_id);
  LiTestAudioBegin(true,config.key.data(),iv,decode);
  const bool valid=audio::transport::representable_loss(static_cast<std::uint16_t>(start),lost);
  constexpr int limits[4]={3,6,5,4};
  check(valid==(lost<=limits[start%4]),"Only the independently enumerated partial FEC groups may continue");
  for(int i=0;i<28;++i){
    if(i==start){
      check(sender.skip_confirmed_usb_loss(lost)==valid,"Packetizer stops when whole FEC block would disappear");
      if(!valid)break;
      i+=lost;
    }
    check(sender.push(corpus.encoded[i],[&](auto header,auto payload){
      bytes_t packet(header.begin(),header.end());packet.insert(packet.end(),payload.begin(),payload.end());
      if(header.size()==audio::transport::rtp_header_bytes){
        check(be16(header.data()+2)==unsigned(i)&&be32(header.data()+4)==unsigned(i*5),"Actual RTP preserves sequence and all elapsed milliseconds");
      }else{
        const int base=be16(header.data()+14);
        check(base+4<=start||base>=start+lost,"No parity fabricated for an incomplete source group");
      }
      check(LiTestAudioPush(packet.data(),static_cast<int>(packet.size()))==0,"Actual Common accepts packet after loss");
    }),"Production sender transmits only actual encoded samples");
  }
  LiTestAudioEnd();receiver=nullptr;opus_multistream_decoder_destroy(state.decoder);
  if(valid){
    check(state.cursor==28&&state.plc==lost,"Common delivers one real PLC frame for each lost 5ms quantum");
  }
}
int main(int argc, char **argv) {
  if (argc == 2 && std::strcmp(argv[1], "--constructor-failure") == 0) {
    reed_solomon_init();
    return constructor_failure_cleanup() ? 0 : 1;
  }
  if(argc==2&&std::strcmp(argv[1],"--usb-loss")==0){
    reed_solomon_init();const auto corpus=make_corpus(600000);
    for(int start=12;start<16;++start)for(int lost=1;lost<=7;++lost)verify_usb_loss(corpus,start,lost);
    std::printf("USB gap replay %d checks, %d failures, actual Common and Opus\n",assertions,failures);
    return failures?1:0;
  }
  if (argc == 2 && std::strcmp(argv[1], "--help") == 0) {
    std::puts("Host714TransportTests: offline production encoder/RTP/AES/FEC to actual Common receiver and Opus decoder. No audio output or sockets.");
    return 0;
  }
  if (argc != 1) return 2;
  try {
    reed_solomon_init();
    std::printf("{\"event\":\"libraries\",\"opus\":\"%s\",\"audio_output\":false}\n", opus_get_version_string());
    encoder_errors();
    boundaries();
    legacy_packetization();
    production_malformed();
    for (int bitrate : {600000, 1536000, 2048000}) {
      const auto corpus = make_corpus(bitrate);
      if (corpus.encoded.size() != packet_count) { ++failures; continue; }
      for (bool encrypted : {false, true}) for (int mode = 0; mode < 6; ++mode) run(corpus, encrypted, mode);
      const auto synthetic = make_corpus(bitrate, true);
      if (synthetic.encoded.size() != packet_count) { ++failures; continue; }
      for (bool encrypted : {false, true}) run(synthetic, encrypted, 0);
    }
  } catch (const std::exception &error) {
    ++failures;
    std::fprintf(stderr, "FAIL exception: %s\n", error.what());
  }
  std::printf("{\"event\":\"summary\",\"result\":\"%s\",\"cases\":%d,\"assertions\":%d,\"failures\":%d,\"skipped\":0,"
              "\"scope\":\"production host codec and packetizer plus actual Common receiver; no endpoint capture, sockets, Spatial objects or physical playback\"}\n",
              failures ? "FAIL" : "PASS", cases, assertions, failures);
  return failures ? 1 : 0;
}

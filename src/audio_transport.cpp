/**
 * @file src/audio_transport.cpp
 * @brief Production audio RTP, AES-CBC and 4+2 Reed-Solomon packetization.
 */
#include "audio_transport.h"

#include <algorithm>
#include <cstring>
#include <stdexcept>

#include "crypto.h"
extern "C" {
#include "../third-party/nanors/rs.h"
#include "rswrapper.h"
}

namespace audio::transport {
  namespace {
    constexpr std::size_t legacy_payload_limit = 4096;
    constexpr std::size_t shard_capacity = encrypted_size(legacy_payload_limit);

    void put16(std::uint8_t *dst, std::uint16_t value) {
      dst[0] = static_cast<std::uint8_t>(value >> 8);
      dst[1] = static_cast<std::uint8_t>(value);
    }
    void put32(std::uint8_t *dst, std::uint32_t value) {
      for (unsigned i = 0; i < 4; ++i) dst[i] = static_cast<std::uint8_t>(value >> (24 - 8 * i));
    }
    void observe(std::size_t value, std::size_t &minimum, std::size_t &maximum, std::uint64_t count) {
      minimum = count == 0 ? value : std::min(minimum, value);
      maximum = std::max(maximum, value);
    }
  }  // namespace

  struct packetizer_t::impl_t {
    config_t config;
    crypto::cipher::cbc_t cipher;
    crypto::aes_t iv = crypto::aes_t(16, 0);
    std::unique_ptr<reed_solomon, reed_solomon_release_t> rs;
    std::array<std::array<std::uint8_t, shard_capacity>, data_shards + parity_shards> shards {};
    std::array<std::uint8_t *, data_shards + parity_shards> shard_pointers {};
    std::array<std::uint8_t, rtp_header_bytes> data_header {};
    std::array<std::uint8_t, rtp_header_bytes + fec_header_bytes> parity_header {};
    std::size_t fixed_plaintext_size = 0;
    std::string failure;
    statistics_t stats;
    bool incomplete_group=false;

    explicit impl_t(const config_t &value):
        config(value), cipher(crypto::aes_t(value.key.begin(), value.key.end()), true), rs(nullptr, reed_solomon_release) {
      if (config.packet_duration_ms == 0 || (config.discrete_714 && config.packet_duration_ms != 5)) {
        throw std::invalid_argument("Invalid audio packet duration");
      }
      if (config.sequence % data_shards != 0) {
        throw std::invalid_argument("Audio packetizer must start at a complete FEC block");
      }
      // main.cpp initializes the same dispatch table before sessions are started.
      if (!reed_solomon_new || !reed_solomon_encode || !reed_solomon_release) {
        throw std::runtime_error("Audio Reed-Solomon dispatch is not initialized");
      }
      rs.reset(reed_solomon_new(data_shards, parity_shards));
      if (!rs) throw std::runtime_error("Cannot allocate audio Reed-Solomon state");
      // GameStream's fixed OpenFEC parity matrix, retained from audioBroadcastThread.
      constexpr unsigned char parity[] = {0x77, 0x40, 0x38, 0x0e, 0xc7, 0xa7, 0x0d, 0x6c};
      std::memcpy(rs->p, parity, sizeof(parity));
      for (std::size_t i = 0; i < shards.size(); ++i) shard_pointers[i] = shards[i].data();
      data_header[0] = parity_header[0] = 0x80;
      data_header[1] = 97;
      parity_header[1] = 127;
      parity_header[13] = 97;
      if(config.encrypted) {
        // Create the existing OpenSSL context once at session setup; no packet
        // is emitted and sequence/FEC state remains untouched.
        std::array<std::uint8_t,16> warm {};
        if(cipher.encrypt({},warm.data(),&iv)!=16) throw std::runtime_error("Audio AES-CBC initialization failed");
      }
    }

    void send(std::span<const std::uint8_t> header, std::span<const std::uint8_t> payload, const emit_t &emit) {
      emit(header, payload);
      const auto bytes = header.size() + payload.size();
      observe(bytes, stats.datagram_min, stats.datagram_max, stats.datagrams);
      ++stats.datagrams;
      stats.datagram_bytes += bytes;
    }
  };

  packetizer_t::packetizer_t(const config_t &config): impl(std::make_unique<impl_t>(config)) {}
  packetizer_t::~packetizer_t() = default;
  const std::string &packetizer_t::error() const { return impl->failure; }
  const statistics_t &packetizer_t::statistics() const { return impl->stats; }

  bool packetizer_t::skip_confirmed_usb_loss(unsigned blocks) {
    auto& s=*impl;
    if(!s.failure.empty())return false;
    if(s.stats.input_packets<2*data_shards || s.config.packet_duration_ms!=5 ||
       !representable_loss(s.config.sequence,blocks)){
      s.failure="USB loss cannot be represented by the synchronized Common receiver; new RTSP session required";return false;
    }
    s.config.sequence=static_cast<std::uint16_t>(s.config.sequence+blocks);
    s.config.timestamp+=s.config.packet_duration_ms*blocks;
    s.incomplete_group=s.config.sequence%data_shards!=0;
    return true;
  }

  bool packetizer_t::push(std::span<const std::uint8_t> plaintext, const emit_t &emit) {
    auto &s = *impl;
    if (!s.failure.empty()) return false;
    const auto fail = [&](const char *message) { s.failure = message; return false; };
    if (!emit || plaintext.empty() || plaintext.size() > legacy_payload_limit) return fail("Invalid audio payload size");
    if (s.config.discrete_714) {
      if (plaintext.size() > discrete_payload_limit ||
          datagram_size(plaintext.size(), s.config.encrypted, true) > client_datagram_limit) {
        return fail("12-channel audio exceeds the client datagram budget");
      }
      if (s.fixed_plaintext_size != 0 && s.fixed_plaintext_size != plaintext.size()) {
        return fail("12-channel CBR payload changed within the session");
      }
      s.fixed_plaintext_size = plaintext.size();
    }

    const auto sequence = s.config.sequence;
    const auto timestamp = s.config.timestamp;
    const auto index = sequence % data_shards;
    auto *destination = s.shard_pointers[index];
    int bytes = static_cast<int>(plaintext.size());
    if (s.config.encrypted) {
      put32(s.iv.data(), s.config.key_id + sequence);
      bytes = s.cipher.encrypt(std::string_view(reinterpret_cast<const char *>(plaintext.data()), plaintext.size()), destination, &s.iv);
      if (bytes < 0 || static_cast<std::size_t>(bytes) != encrypted_size(plaintext.size())) {
        return fail("Audio AES-CBC encryption failed or returned unexpected padding");
      }
    } else {
      std::copy(plaintext.begin(), plaintext.end(), destination);
    }

    put16(s.data_header.data() + 2, sequence);
    put32(s.data_header.data() + 4, timestamp);
    // GameStream's RTP audio timebase is milliseconds, independent of PCM rate.
    ++s.config.sequence;
    s.config.timestamp += s.config.packet_duration_ms;
    observe(plaintext.size(), s.stats.plaintext_min, s.stats.plaintext_max, s.stats.input_packets);
    ++s.stats.input_packets;
    s.stats.plaintext_bytes += plaintext.size();

    s.send(s.data_header, {destination, static_cast<std::size_t>(bytes)}, emit);
    if (index == 0) {
      s.incomplete_group=false;
      put16(s.parity_header.data() + 14, sequence);
      put32(s.parity_header.data() + 16, timestamp);
    }
    if (index == data_shards - 1 && !s.incomplete_group) {
      if (reed_solomon_encode(s.rs.get(), s.shard_pointers.data(), data_shards + parity_shards, bytes) != 0) {
        return fail("Audio Reed-Solomon encoding failed");
      }
      for (unsigned parity = 0; parity < parity_shards; ++parity) {
        put16(s.parity_header.data() + 2, static_cast<std::uint16_t>(sequence + parity + 1));
        s.parity_header[12] = static_cast<std::uint8_t>(parity);
        s.send(s.parity_header, {s.shard_pointers[data_shards + parity], static_cast<std::size_t>(bytes)}, emit);
      }
    }
    return true;
  }
}  // namespace audio::transport

/**
 * @file src/audio_transport.h
 * @brief The production GameStream audio packetizer, also used by offline replay.
 */
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <string>

namespace audio::transport {
  constexpr std::size_t rtp_header_bytes = 12;
  constexpr std::size_t fec_header_bytes = 12;
  constexpr std::size_t client_datagram_limit = 1400;
  constexpr std::size_t discrete_payload_limit = 1375;
  constexpr unsigned data_shards = 4;
  constexpr unsigned parity_shards = 2;
  // Common conceals missing positions in a partially received FEC group, but
  // skips wholly missing groups. It cannot represent the latter as elapsed PCM.
  constexpr bool representable_loss(std::uint16_t next, unsigned lost) {
    if(!lost || lost>2*(data_shards-1)) return false;
    for(unsigned i=0;i<lost;++i)
      if(((next+i)%data_shards)==0 && lost-i>=data_shards) return false;
    return true;
  }

  // PKCS#7 always appends padding, including a complete block at exact multiples.
  constexpr std::size_t encrypted_size(std::size_t plaintext) {
    return (plaintext / 16 + 1) * 16;
  }

  constexpr std::size_t datagram_size(std::size_t plaintext, bool encrypted, bool parity) {
    return rtp_header_bytes + (parity ? fec_header_bytes : 0) +
           (encrypted ? encrypted_size(plaintext) : plaintext);
  }

  struct config_t {
    bool encrypted = false;
    bool discrete_714 = false;
    unsigned packet_duration_ms = 5;
    std::array<std::uint8_t, 16> key {};
    std::uint32_t key_id = 0;
    // Initial values permit boundary replay; normal sessions start at zero.
    std::uint16_t sequence = 0;
    std::uint32_t timestamp = 0;
  };

  struct statistics_t {
    std::uint64_t input_packets = 0;
    std::uint64_t plaintext_bytes = 0;
    std::size_t plaintext_min = 0;
    std::size_t plaintext_max = 0;
    std::uint64_t datagrams = 0;
    std::uint64_t datagram_bytes = 0;
    std::size_t datagram_min = 0;
    std::size_t datagram_max = 0;
  };

  class packetizer_t {
  public:
    // The callback consumes the two spans synchronously. It is the socket send in
    // production and a byte-for-byte replay collector in the integration test.
    using emit_t = std::function<void(std::span<const std::uint8_t>, std::span<const std::uint8_t>)>;
    explicit packetizer_t(const config_t &config);
    ~packetizer_t();
    packetizer_t(const packetizer_t &) = delete;
    packetizer_t &operator=(const packetizer_t &) = delete;

    bool push(std::span<const std::uint8_t> plaintext, const emit_t &emit);
    bool skip_confirmed_usb_loss(unsigned blocks);
    const std::string &error() const;
    const statistics_t &statistics() const;

  private:
    struct impl_t;
    std::unique_ptr<impl_t> impl;
  };
}  // namespace audio::transport

/**
 * @file src/audio_transport_policy.h
 * @brief Bounds checks for encoded audio packets and their CBC transport buffers.
 */
#pragma once

#include "audio_codec.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string_view>

namespace stream {
  constexpr std::size_t MAX_AUDIO_PACKET_SIZE = audio::MAX_DOLBY_PACKET_SIZE;

  /**
   * @brief Return the output size of AES-CBC with PKCS#7 padding enabled.
   *
   * PKCS#7 always adds padding, including a complete block when the input is
   * already block-aligned. OpenSSL's EVP encryption path uses that behavior.
   */
  constexpr std::size_t audio_cbc_ciphertext_size(std::size_t plaintext_size) {
    return ((plaintext_size / 16) + 1) * 16;
  }

  constexpr std::size_t MAX_AUDIO_PACKET_BUFFER_SIZE = audio_cbc_ciphertext_size(MAX_AUDIO_PACKET_SIZE);

  /**
   * @brief Check that an encoded audio packet fits its destination buffer.
   */
  constexpr bool audio_packet_fits(std::size_t plaintext_size, bool encrypted, std::size_t destination_capacity) {
    if (plaintext_size > MAX_AUDIO_PACKET_SIZE) {
      return false;
    }

    const auto required_size = encrypted ? audio_cbc_ciphertext_size(plaintext_size) : plaintext_size;
    return required_size <= destination_capacity;
  }

  /**
   * @brief Encode an audio packet and invoke the writer only after successful encoding.
   *
   * The capacity check and the write share the same production path, allowing
   * callers to guarantee that an undersized destination is never submitted.
   */
  template<typename Plaintext, typename Encrypt, typename Write>
  int encode_and_write_audio(
    bool encrypted,
    const Plaintext &plaintext,
    std::uint8_t *destination,
    std::size_t destination_capacity,
    Encrypt &&encrypt,
    Write &&write
  ) {
    if (!audio_packet_fits(plaintext.size(), encrypted, destination_capacity)) {
      return -1;
    }

    int bytes {};
    if (encrypted) {
      bytes = encrypt(
        std::string_view {reinterpret_cast<const char *>(std::to_address(plaintext.begin())), plaintext.size()},
        destination
      );
    } else {
      std::copy(plaintext.begin(), plaintext.end(), destination);
      bytes = static_cast<int>(plaintext.size());
    }

    if (bytes < 0) {
      return -1;
    }

    write(destination, static_cast<std::size_t>(bytes));
    return bytes;
  }
}  // namespace stream

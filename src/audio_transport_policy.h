/**
 * @file src/audio_transport_policy.h
 * @brief Bounds checks for encoded audio packets and their CBC transport buffers.
 */
#pragma once

#include "audio.h"

#include <cstddef>

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
}  // namespace stream

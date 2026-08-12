/**
 * @file src/amf/amf_encoded_frame.h
 * @brief Declarations for AMF encoded frame.
 */
#pragma once

#include "amf_lifecycle.h"

#include <chrono>
#include <cstdint>
#include <optional>
#include <vector>

namespace amf {

  /**
   * @brief Encoded frame from AMF encoder.
   */
  struct amf_encoded_frame {
    std::vector<uint8_t> data;
    uint64_t frame_index = 0;
    bool idr = false;
    lifecycle::encoded_picture_type_e picture_type = lifecycle::encoded_picture_type_e::unknown;
    std::optional<int64_t> intra_pixels;
    bool requested_full_refresh = false;
    bool after_ref_frame_invalidation = false;
    bool fatal = false;  // Set when encoder is in unrecoverable state (device lost, repeated failures)
  };

  /**
   * @brief Result of one native AMF submission attempt.
   *
   * Output can contain frames from earlier submissions because AMF is asynchronous.
   * input_accepted is kept separate so recovery state (IDR/RFI/LTR) is committed only
   * after the current surface was actually accepted by the encoder.
   */
  struct amf_encode_result {
    std::vector<amf_encoded_frame> frames;
    std::optional<std::chrono::steady_clock::time_point> input_accepted_at;
    bool input_accepted = false;
    bool fatal = false;
  };

}  // namespace amf

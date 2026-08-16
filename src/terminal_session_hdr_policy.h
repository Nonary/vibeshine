/**
 * @file src/terminal_session_hdr_policy.h
 * @brief Fail-closed policy for the seat-local native HDR activator.
 */
#pragma once

#include <cstdint>

namespace terminal_session::hdr {
  inline constexpr std::uint32_t activation_capability_magic = 0x52444856;  // VHDR
  inline constexpr std::uint32_t activation_capability_version = 2;

  struct target_binding_t {
    std::uint32_t session_id {};
    std::uint32_t source_adapter_low {};
    std::uint32_t source_adapter_high {};
    std::uint32_t source_id {};
    std::uint32_t target_adapter_low {};
    std::uint32_t target_adapter_high {};
    std::uint32_t target_id {};

    friend constexpr bool operator==(const target_binding_t &, const target_binding_t &) = default;
  };

  /**
   * One-shot activation contract transferred over a random first-instance,
   * local-only named pipe created by the SYSTEM broker. Keeping the display
   * identity out of argv prevents another process from retargeting an admitted
   * helper after CreateProcessAsUserW.
   */
  struct activation_capability_t {
    std::uint32_t magic {activation_capability_magic};
    std::uint32_t size {48};
    std::uint32_t version {activation_capability_version};
    std::uint32_t parent_pid {};
    std::uint32_t session_id {};
    std::uint32_t source_adapter_low {};
    std::uint32_t source_adapter_high {};
    std::uint32_t source_id {};
    std::uint32_t target_adapter_low {};
    std::uint32_t target_adapter_high {};
    std::uint32_t target_id {};
    std::uint32_t reserved {};
  };

  static_assert(sizeof(activation_capability_t) == 48);

  [[nodiscard]] constexpr target_binding_t binding_from_capability(const activation_capability_t &capability) noexcept {
    return {
      capability.session_id,
      capability.source_adapter_low,
      capability.source_adapter_high,
      capability.source_id,
      capability.target_adapter_low,
      capability.target_adapter_high,
      capability.target_id,
    };
  }

  enum class action_e {
    skip,
    accept_active,
    activate,
    reject,
  };

  struct display_state_t {
    bool found {};
    bool supported {};
    bool user_enabled {};
    bool active {};
    bool hdr_color_mode {};
    unsigned int bits_per_color_channel {};
  };

  [[nodiscard]] constexpr action_e decide(const bool requested, const display_state_t &state) noexcept {
    if (!requested) return action_e::skip;
    if (!state.found || !state.supported || !state.user_enabled || state.bits_per_color_channel < 10) return action_e::reject;
    if (state.active && state.hdr_color_mode) return action_e::accept_active;
    return action_e::activate;
  }
}

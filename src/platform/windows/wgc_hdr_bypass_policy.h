#pragma once

namespace platf::dxgi::wgc_hdr_bypass {
  struct inputs {
    bool opt_in_enabled {};
    bool client_requested_hdr {};
    bool force_sdr {};
    bool prefer_sdr_10bit {};
    bool non_console_interactive_session {};
    bool wgc_backend {};
    bool fp16_capture {};
  };

  constexpr bool authorized(const inputs &value) noexcept {
    return value.opt_in_enabled &&
           value.client_requested_hdr &&
           !value.force_sdr &&
           !value.prefer_sdr_10bit &&
           value.non_console_interactive_session &&
           value.wgc_backend &&
           value.fp16_capture;
  }
}  // namespace platf::dxgi::wgc_hdr_bypass

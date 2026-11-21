/**
 * @file src/session_display_policy.h
 * @brief Resolve per-session display preparation without modifying persistent settings.
 */
#pragma once

#include "config.h"
#include "platform/windows/virtual_display_policy.h"

namespace proc::display_policy {
  using preparation_e = config::video_t::dd_t::config_option_e;

  /**
   * @brief Read the generic representation of a session preparation override.
   * @param overrides Application or authenticated client runtime overrides.
   * @return The explicit preparation, or no value when it is inherited.
   * @note Invalid values follow config.cpp's disabled fallback.
   */
  inline std::optional<preparation_e> preparation_override(const std::unordered_map<std::string, std::string> &overrides) {
    const auto it = overrides.find("dd_configuration_option");
    if (it == overrides.end()) {
      return std::nullopt;
    }
    if (it->second == "verify_only") return preparation_e::verify_only;
    if (it->second == "ensure_active") return preparation_e::ensure_active;
    if (it->second == "ensure_primary") return preparation_e::ensure_primary;
    if (it->second == "ensure_only_display") return preparation_e::ensure_only_display;
    return preparation_e::disabled;
  }

  // Capture this after merging app and client settings, before output
  // fallback or recovery populates the virtual-display fields.
  template <class Session>
  bool has_virtual_display_override(const Session &session) {
    if (session.client_virtual_display_override) {
      return *session.client_virtual_display_override;
    }
    if (session.client_requests_virtual_display) {
      return true;
    }
    if (session.output_name_override) {
      std::string_view output {*session.output_name_override};
      const auto first = output.find_first_not_of(" \t\r\n");
      if (first == std::string_view::npos) return false;
      output = output.substr(first, output.find_last_not_of(" \t\r\n") - first + 1);
      return VDISPLAY::policy::is_virtual_display_selection(output, true);
    }
    if (session.virtual_display_mode_override == config::video_t::virtual_display_mode_e::disabled) {
      return false;
    }
    return session.virtual_display || session.virtual_display_mode_override.has_value() ||
           session.virtual_display_layout_override.has_value();
  }

  /**
   * @brief Use layout preparation for explicit virtual overrides and enabled automation.
   * @param inherited Effective global preparation.
   * @param session Resolved session policy and current target state.
   * @param virtual_display_intended Whether this is the initial virtual creation request.
   * @return Effective preparation for the actual or intended display target.
   */
  template <class Session>
  preparation_e effective_preparation(const preparation_e inherited, const Session &session, const bool virtual_display_intended = false) {
    const auto option = session.dd_config_option_override.value_or(inherited);
    if ((session.virtual_display || virtual_display_intended) &&
        (session.explicit_virtual_display_override || option != preparation_e::disabled)) {
      // The virtual layout chooses EnsureActive/Primary/OnlyDisplay downstream.
      return preparation_e::ensure_active;
    }
    return option;
  }
}  // namespace proc::display_policy

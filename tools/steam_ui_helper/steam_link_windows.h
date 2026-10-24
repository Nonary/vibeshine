/**
 * @file tools/steam_ui_helper/steam_link_windows.h
 * @brief Steam's CEF debugger over WinHTTP, with checks that the port belongs to Steam.
 */
#pragma once

#include "controller.h"

#include <filesystem>

namespace steam_ui_helper {
  class windows_steam_link_t final: public steam_link_t {
  public:
    platf::steam::big_picture::policy::endpoint_facts_t endpoint_facts() override;
    json targets() override;
    std::unique_ptr<cdp_session_t> connect(const std::string &ws_url, std::function<void(cdp_event_t)> on_event) override;

    /** Steam's install folder for the current user, or empty when Steam isn't installed. */
    static std::filesystem::path steam_root();
  };
}  // namespace steam_ui_helper

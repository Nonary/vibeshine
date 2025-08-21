/**
 * @file src/confighttp_playnite.cpp
 * @brief Playnite-specific HTTP endpoints and helpers (Windows-only).
 */

#ifdef _WIN32

// standard includes
#include <filesystem>
#include <string>
#include <string_view>

// third-party includes
#include <nlohmann/json.hpp>
#include <Simple-Web-Server/server_https.hpp>

// local includes
#include "config_playnite.h"
#include "src/platform/windows/playnite_integration.h"
#include "src/platform/windows/ipc/misc_utils.h"
#include "confighttp.h"
#include "logging.h"

// Windows headers
#include <ShlObj.h>

namespace confighttp {

  // Bring request/response types into scope to match confighttp.cpp usage
  using resp_https_t = std::shared_ptr<typename SimpleWeb::ServerBase<SimpleWeb::HTTPS>::Response>;
  using req_https_t = std::shared_ptr<typename SimpleWeb::ServerBase<SimpleWeb::HTTPS>::Request>;

  // Forward declarations for helpers defined in confighttp.cpp
  bool authenticate(resp_https_t response, req_https_t request);
  void print_req(const req_https_t &request);
  void send_response(resp_https_t response, const nlohmann::json &output_tree);
  void bad_request(resp_https_t response, req_https_t request, const std::string &error_message = "Bad Request");
  bool check_content_type(resp_https_t response, req_https_t request, const std::string_view &contentType);

  // Enhance app JSON with a Playnite-derived cover path when applicable.
  void enhance_app_with_playnite_cover(nlohmann::json &input_tree) {
    try {
      if ((!input_tree.contains("image-path") || (input_tree["image-path"].is_string() && input_tree["image-path"].get<std::string>().empty())) &&
          input_tree.contains("playnite-id") && input_tree["playnite-id"].is_string()) {
        std::string cover;
        if (platf::playnite::get_cover_png_for_playnite_game(input_tree["playnite-id"].get<std::string>(), cover)) {
          input_tree["image-path"] = cover;
        }
      }
    } catch (...) {
      // Best-effort only
    }
  }

  // No longer needed: old fallback path resolver removed with AssocQueryString-based detection

  void getPlayniteStatus(resp_https_t response, req_https_t request) {
    if (!authenticate(response, request)) {
      return;
    }
    print_req(request);
    nlohmann::json out;
    out["enabled"] = config::playnite.enabled;
    out["active"] = platf::playnite::is_active();
    bool session_required = false;
    // Deprecated fields removed: playnite_running, installed_unknown
    std::string destPath;
    std::filesystem::path dest;
    // If there is no active console desktop session, mark as session required.
    try {
      HANDLE tok = platf::dxgi::retrieve_users_token(false);
      if (!tok) {
        session_required = true;
      } else {
        CloseHandle(tok);
      }
    } catch (...) {
      // If we cannot determine, leave as-is
    }
    // Resolve the user's Playnite extensions directory via URL association.
    // Requires user impersonation when running as SYSTEM.
    if (platf::playnite::get_extension_target_dir(destPath)) {
      dest = destPath;
      bool installed = std::filesystem::exists(dest / "extension.yaml") && std::filesystem::exists(dest / "SunshinePlaynite.psm1");
      out["installed"] = installed;
      out["extensions_dir"] = dest.string();
    } else {
      out["installed"] = false;
      out["extensions_dir"] = std::string();
    }
    out["session_required"] = session_required;
    // For UI backwards-compatibility: expose user_session_active derived from session_required
    out["user_session_active"] = !session_required;
    BOOST_LOG(info) << "Playnite status: enabled=" << out["enabled"] << ", active=" << out["active"]
                    << ", session_required=" << (session_required ? "true" : "false")
                    << ", dir=" << (dest.empty() ? std::string("(unknown)") : dest.string());
    send_response(response, out);
  }

  void getPlayniteGames(resp_https_t response, req_https_t request) {
    if (!authenticate(response, request)) {
      return;
    }
    print_req(request);
    try {
      std::string json;
      if (!platf::playnite::get_games_list_json(json)) {
        // return empty array if not available
        json = "[]";
      }
      BOOST_LOG(info) << "Playnite games: json length=" << json.size();
      SimpleWeb::CaseInsensitiveMultimap headers;
      headers.emplace("Content-Type", "application/json");
      headers.emplace("X-Frame-Options", "DENY");
      headers.emplace("Content-Security-Policy", "frame-ancestors 'none';");
      response->write(SimpleWeb::StatusCode::success_ok, json, headers);
    } catch (std::exception &e) {
      bad_request(response, request, e.what());
    }
  }

  void getPlayniteCategories(resp_https_t response, req_https_t request) {
    if (!authenticate(response, request)) {
      return;
    }
    print_req(request);
    try {
      std::string json;
      if (!platf::playnite::get_categories_list_json(json)) {
        // return empty array if not available
        json = "[]";
      }
      BOOST_LOG(info) << "Playnite categories: json length=" << json.size();
      SimpleWeb::CaseInsensitiveMultimap headers;
      headers.emplace("Content-Type", "application/json");
      headers.emplace("X-Frame-Options", "DENY");
      headers.emplace("Content-Security-Policy", "frame-ancestors 'none';");
      response->write(SimpleWeb::StatusCode::success_ok, json, headers);
    } catch (std::exception &e) {
      bad_request(response, request, e.what());
    }
  }

  void installPlaynite(resp_https_t response, req_https_t request) {
    if (!check_content_type(response, request, "application/json")) {
      return;
    }
    if (!authenticate(response, request)) {
      return;
    }
    print_req(request);
    std::string err;
    nlohmann::json out;
    bool request_restart = false;
    try {
      std::stringstream ss; ss << request->content.rdbuf();
      if (ss.rdbuf()->in_avail() > 0) {
        auto in = nlohmann::json::parse(ss);
        request_restart = in.value("restart", false);
      }
    } catch (...) {
      // ignore body parse errors; treat as no-restart
    }
    // Prefer same resolved dir as status
    std::string target;
    bool have_target = platf::playnite::get_extension_target_dir(target);
    bool ok = false;
    if (have_target) {
      ok = platf::playnite::install_plugin_to(target, err);
    } else {
      ok = platf::playnite::install_plugin(err);
    }
    BOOST_LOG(info) << "Playnite install: status=" << (ok ? "true" : "false") << (ok ? "" : std::string(", error=") + err);
    out["status"] = ok;
    if (!ok) out["error"] = err;
    // Optionally close and restart Playnite to pick up the new plugin
    if (ok && request_restart) {
      bool restarted = platf::playnite::restart_playnite();
      out["restarted"] = restarted;
    }
    send_response(response, out);
  }

  void postPlayniteForceSync(resp_https_t response, req_https_t request) {
    if (!authenticate(response, request)) return;
    print_req(request);
    nlohmann::json out;
    bool ok = platf::playnite::force_sync();
    out["status"] = ok;
    send_response(response, out);
  }

  void postPlayniteLaunch(resp_https_t response, req_https_t request) {
    if (!authenticate(response, request)) return;
    print_req(request);
    nlohmann::json out;
    // Use unified restart path: will start Playnite if not running
    bool ok = platf::playnite::restart_playnite();
    out["status"] = ok;
    send_response(response, out);
  }

}  // namespace confighttp

#endif // _WIN32

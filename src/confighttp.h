/**
 * @file src/confighttp.h
 * @brief Declarations for the Web UI Config HTTP server.
 */
#pragma once

// standard includes
#include <string>
// third-party includes
#include <nlohmann/json.hpp>

// local includes
#include "thread_safe.h"

#define WEB_DIR SUNSHINE_ASSETS_DIR "/web/"

namespace confighttp {
  constexpr auto PORT_HTTPS = 1;
  void start();
  // Writes the apps file and refreshes the client-visible app cache/list
  // Sorts entries by name for a stable UI.
  bool refresh_client_apps_cache(nlohmann::json &file_tree);
}  // namespace confighttp

// mime types map (defined in confighttp.cpp)
namespace confighttp { extern const std::map<std::string, std::string> mime_types; }

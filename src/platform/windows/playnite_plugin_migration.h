/** Ownership-checked, recoverable retirement of an alternate Playnite connector. */
#pragma once

#include <filesystem>
#include <fstream>
#include <map>
#include <optional>
#include <string>
#ifdef _WIN32
  #include <Windows.h>
#endif

namespace platf::playnite::migration {
  namespace fs = std::filesystem;

  // Do not follow junctions/symlinks to another installation or user's files.
  inline bool plain_path(const fs::path &path) {
    if (!path.is_absolute()) return false;
    fs::path current = path.root_path();
    for (const auto &part : path.relative_path()) {
      if (part == "..") return false;
      current /= part;
      std::error_code ec;
      const auto status = fs::symlink_status(current, ec);
      if (ec || fs::is_symlink(status)) return false;
#ifdef _WIN32
      const auto attributes = GetFileAttributesW(current.c_str());
      if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_REPARSE_POINT)) return false;
#endif
    }
    return true;
  }

  inline std::string scalar(std::string value) {
    const auto first = value.find_first_not_of(" \t\r");
    if (first == std::string::npos) return {};
    value = value.substr(first, value.find_last_not_of(" \t\r") - first + 1);
    if (value.size() >= 2 && ((value.front() == '\'' && value.back() == '\'') ||
                             (value.front() == '"' && value.back() == '"'))) {
      value = value.substr(1, value.size() - 2);
    }
    return value;
  }

  // These are the manifests shipped by this integration, not a directory-name
  // heuristic. Unknown/ambiguous YAML is deliberately left alone.
  inline bool owned_manifest(const fs::path &manifest) {
    std::error_code ec;
    if (!plain_path(manifest) || !fs::is_regular_file(manifest, ec) || ec || fs::file_size(manifest, ec) > 65536 || ec) return false;
    std::ifstream stream(manifest);
    std::map<std::string, std::string> fields;
    std::string line;
    while (std::getline(stream, line)) {
      if (line.compare(0, 3, "\xef\xbb\xbf") == 0) line.erase(0, 3);
      const auto colon = line.find(':');
      if (colon == std::string::npos) continue;
      const auto key = scalar(line.substr(0, colon));
      if (key != "Id" && key != "Module" && key != "Type") continue;
      // Nested keys must never masquerade as the top-level identity.
      if (line.empty() || line.front() == ' ' || line.front() == '\t' || fields.count(key)) return false;
      fields[key] = scalar(line.substr(colon + 1));
    }
    if (stream.bad()) return false;
    const bool script = fields["Id"] == "Sunshine.Playnite.PowerShell" && fields["Type"] == "Script" &&
                        fields["Module"] == "SunshinePlaynite.psm1";
    const bool compiled = fields["Id"] == "Sunshine.Playnite.Integration" && fields["Type"] == "GenericPlugin" &&
                          (fields["Module"] == "SunshinePlaynite.dll" || fields["Module"] == "VibeshinePlaynite.dll" ||
                           fields["Module"] == "VibepolloPlaynite.dll");
    return script || compiled;
  }

  // The caller supplies only roots tied to the selected registry-confirmed
  // installation. An explicit destination elsewhere must not retire either.
  inline std::optional<fs::path> alternate_root(const fs::path &destination, const fs::path &profile, const fs::path &install) {
    std::error_code ec;
    if (fs::equivalent(destination, profile, ec) && !ec) return install;
    ec.clear();
    if (fs::equivalent(destination, install, ec) && !ec) return profile;
    return std::nullopt;
  }

  // Called only after the replacement has been verified and Playnite stopped.
  // A missing manifest is an idempotent success; unowned/linked entries are not
  // touched. A failed rename leaves the original intact and is retryable.
  inline bool disable_duplicate(const fs::path &active, const fs::path &alternate, std::string &error) {
    std::error_code ec;
    if (!fs::exists(alternate, ec)) {
      if (!ec) return true;
      error = "Could not inspect alternate Playnite extension: " + ec.message();
      return false;
    }
    if (fs::equivalent(active, alternate, ec) && !ec) return true;
    if (!plain_path(active) || !plain_path(alternate)) return true;
    const auto manifest = alternate / "extension.yaml";
    if (!owned_manifest(manifest)) return true;
    // Never disable the old connector if the new activation manifest is absent
    // or unrecognized (including interrupted deployment).
    if (!owned_manifest(active / "extension.yaml")) {
      error = "Replacement Playnite connector manifest could not be verified";
      return false;
    }
    for (unsigned int suffix = 0; suffix < 1000; ++suffix) {
      const auto backup = alternate / ("extension.yaml.vibeshine-disabled" + (suffix ? "." + std::to_string(suffix) : ""));
      ec.clear();
      const auto status = fs::symlink_status(backup, ec);
      if (ec && ec != std::errc::no_such_file_or_directory) break;
      if (fs::exists(status)) continue;
      ec.clear();
      fs::rename(manifest, backup, ec);
      if (!ec) return true;
      break;
    }
    error = "Could not disable duplicate Playnite connector in " + alternate.string() +
            (ec ? ": " + ec.message() : ": backup slots exhausted");
    return false;
  }
}

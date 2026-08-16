#include "steam_offline_policy.h"

#include <algorithm>
#include <cctype>
#include <vector>

namespace steam_offline {
  namespace {
    std::string basename(std::string_view value) {
      const auto slash = value.find_last_of("/\\");
      value = slash == std::string_view::npos ? value : value.substr(slash + 1);
      return std::string {value};
    }

    std::string lowercase(std::string value) {
      std::ranges::transform(value, value.begin(), [](const unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
      return value;
    }

    std::vector<std::string> arguments(std::string_view command_line) {
      std::vector<std::string> result;
      std::string argument;
      bool quoted = false;
      while (!command_line.empty()) {
        while (!quoted && !command_line.empty() && std::isspace(static_cast<unsigned char>(command_line.front()))) command_line.remove_prefix(1);
        if (command_line.empty()) break;
        argument.clear();
        while (!command_line.empty()) {
          const char ch = command_line.front();
          command_line.remove_prefix(1);
          if (ch == '"') {
            quoted = !quoted;
          } else if (std::isspace(static_cast<unsigned char>(ch)) && !quoted) {
            break;
          } else {
            argument.push_back(ch);
          }
        }
        result.push_back(argument);
      }
      return result;
    }

    std::string first_argument(std::string_view command_line) {
      const auto parsed = arguments(command_line);
      return parsed.empty() ? std::string {} : parsed.front();
    }

    bool has_ipc_override(std::string_view command_line) {
      const auto parsed = arguments(command_line);
      return std::ranges::any_of(parsed, [](const std::string &argument) {
        const auto normalized = lowercase(argument);
        constexpr std::string_view flag = "-master_ipc_name_override";
        return normalized == flag || normalized.starts_with(std::string {flag} + "=");
      });
    }

    bool valid_seat_id(std::string_view value) {
      // Keep the launch token bounded and filesystem-safe.  It is used only
      // for deterministic seat-owned roots and filter keys.
      return !value.empty() && value.size() < max_seat_id_size &&
        std::ranges::all_of(value, [](const unsigned char ch) {
          return std::isalnum(ch) || ch == '-' || ch == '_' || ch == '.';
        });
    }
  }

  bool game_library_outside_mirror(const std::string_view game_path,
                                   const std::string_view mirror_root) noexcept {
    if (game_path.empty() || mirror_root.empty()) return false;
    return !path_is_same_or_descendant(game_path, mirror_root);
  }

  bool path_is_same_or_descendant(const std::string_view path, const std::string_view root) noexcept {
    if (path.empty() || root.empty()) return false;
    auto normalize = [](std::string value) {
      std::ranges::transform(value, value.begin(), [](char ch) {
        if (ch == '/') return '\\';
        return static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
      });
      while (value.size() > 3 && value.ends_with("\\")) value.pop_back();
      return value;
    };
    const auto game = normalize(std::string {path});
    const auto normalized_root = normalize(std::string {root});
    return game == normalized_root || (game.size() > normalized_root.size() && game.compare(0, normalized_root.size(), normalized_root) == 0 && game[normalized_root.size()] == '\\');
  }

  bool is_configured_steam_client(const std::string_view command_line) noexcept {
    return lowercase(basename(first_argument(command_line))) == "steam.exe";
  }

  std::string ipc_name_for_seat(const std::string_view opaque_seat_id) {
    std::string id;
    if (valid_seat_id(opaque_seat_id)) {
      id.assign(opaque_seat_id);
    } else {
      std::uint64_t hash = 1469598103934665603ULL;
      for (const auto ch : opaque_seat_id) {
        hash ^= static_cast<unsigned char>(ch);
        hash *= 1099511628211ULL;
      }
      id = "opaque-" + std::to_string(hash);
    }
    auto result = "vibeshine-seat-" + id;
    if (result.size() > max_ipc_name_size) result.resize(max_ipc_name_size);
    return result;
  }

  std::string append_ipc_override(std::string command_line, const std::string_view opaque_seat_id) {
    if (!is_configured_steam_client(command_line) || has_ipc_override(command_line)) return command_line;
    if (command_line.size() > max_command_line_size) return {};
    const auto ipc_name = ipc_name_for_seat(opaque_seat_id);
    constexpr std::size_t suffix_bound = sizeof(" -master_ipc_name_override ") - 1;
    if (command_line.size() > max_command_line_size - suffix_bound - ipc_name.size()) return {};
    command_line += " -master_ipc_name_override " + ipc_name;
    return command_line;
  }

  std::string rewrite_client_command(std::string command_line, const std::string_view mirror_root,
                                     const std::string_view cache_root, const std::string_view opaque_seat_id) {
    if (!is_configured_steam_client(command_line) || mirror_root.empty() || cache_root.empty()) return {};
    const auto parsed = arguments(command_line);
    if (parsed.empty()) return {};
    std::string rewritten = "\"" + std::string {mirror_root} + "\\steam.exe\"";
    std::size_t first_end = 0;
    bool quoted = false;
    for (; first_end < command_line.size(); ++first_end) {
      const char ch = command_line[first_end];
      if (ch == '"') quoted = !quoted;
      else if (!quoted && std::isspace(static_cast<unsigned char>(ch))) break;
    }
    if (first_end < command_line.size()) rewritten += command_line.substr(first_end);
    rewritten = append_ipc_override(std::move(rewritten), opaque_seat_id);
    if (rewritten.empty()) return {};
    // The proxy consumes both Chromium spellings.  These are appended rather
    // than inherited from the original command so a stale profile cannot be
    // shared with the console Steam instance.
    rewritten += " -cachedir \"" + std::string {cache_root} + "\\htmlcache\" -userdatadir \"" +
      std::string {cache_root} + "\\userdata\"";
    return rewritten.size() <= max_command_line_size ? rewritten : std::string {};
  }

  std::string deterministic_filter_key(const std::string_view seat_id, const std::uint64_t generation,
                                       const std::string_view canonical_path, const bool ipv6) {
    return std::string {seat_id} + ":" + std::to_string(generation) + ":" + std::string {canonical_path} + (ipv6 ? ":v6" : ":v4");
  }

}

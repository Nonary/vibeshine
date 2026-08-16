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
      // The driver ABI reserves one byte for the terminating NUL in its
      // 64-byte field; keep the launch token bound identical so a command
      // cannot advertise an ID that registration would reject.
      return !value.empty() && value.size() < max_seat_id_size &&
        std::ranges::all_of(value, [](const unsigned char ch) {
          return std::isalnum(ch) || ch == '-' || ch == '_' || ch == '.';
        });
    }
  }

  bool is_recognized_client_image(const std::string_view image_name) noexcept {
    const auto image = lowercase(basename(image_name));
    return image == "steam.exe" || image == "steamwebhelper.exe" || image == "gameoverlayui.exe" ||
      image == "steamerrorreporter.exe" || image == "steamerrorreporter64.exe";
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

  lineage_registry_t::lineage_registry_t(const std::size_t capacity): entries_(capacity) {}

  bool lineage_registry_t::register_root(const process_identity_t root, const std::uint64_t generation, const std::string_view seat_id) {
    if (!root.pid || !root.object_key || !generation || !valid_seat_id(seat_id) || generation_ != 0) return false;
    const auto found = std::ranges::find_if(entries_, [](const entry_t &entry) { return entry.state == lineage_state_e::empty; });
    if (found == entries_.end()) return false;
    *found = {.identity = root, .state = lineage_state_e::root, .generation = generation};
    root_ = root;
    generation_ = generation;
    seat_id_ = seat_id;
    return true;
  }

  bool lineage_registry_t::observe_child(const process_identity_t child, const process_identity_t parent, const std::string_view image_name) {
    if (!child.pid || !child.object_key || !parent.object_key) return false;
    const auto parent_entry = std::ranges::find_if(entries_, [&](const entry_t &entry) { return entry.identity == parent && entry.state != lineage_state_e::empty; });
    if (parent_entry == entries_.end()) return false;
    const auto free_entry = std::ranges::find_if(entries_, [](const entry_t &entry) { return entry.state == lineage_state_e::empty; });
    if (free_entry == entries_.end()) return false;
    *free_entry = {.identity = child, .state = is_recognized_client_image(image_name) ? lineage_state_e::blocked_client : lineage_state_e::descendant,
                   .generation = parent_entry->generation};
    return true;
  }

  lineage_state_e lineage_registry_t::state(const process_identity_t identity) const noexcept {
    const auto found = std::ranges::find_if(entries_, [&](const entry_t &entry) { return entry.identity == identity; });
    return found == entries_.end() ? lineage_state_e::empty : found->state;
  }

  bool lineage_registry_t::remove(const process_identity_t identity) noexcept {
    if (identity != root_) return false;
    const auto found = std::ranges::find_if(entries_, [&](const entry_t &entry) { return entry.identity == identity; });
    if (found == entries_.end()) return false;
    const auto generation = found->generation;
    for (auto &entry : entries_) {
      if (entry.generation == generation) entry = {};
    }
    if (generation_ == generation) {
      generation_ = 0;
      root_ = {};
      seat_id_.clear();
    }
    return true;
  }

  bool lineage_registry_t::generation_matches(const std::uint64_t generation) const noexcept {
    return generation != 0 && generation == generation_;
  }

  bool lineage_registry_t::registration_matches(const process_identity_t root, const std::uint64_t generation,
                                                 const std::string_view seat_id) const noexcept {
    return root == root_ && generation_matches(generation) && seat_id == seat_id_;
  }
}

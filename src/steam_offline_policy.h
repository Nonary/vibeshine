#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <span>
#include <vector>

namespace steam_offline {
  enum class protected_generation_state { absent, present, unknown };
  [[nodiscard]] constexpr bool stale_filters_removal_allowed(protected_generation_state state) noexcept {
    return state == protected_generation_state::absent;
  }
  [[nodiscard]] bool exact_original_image_match(std::string_view process_path,
                                                 std::string_view recorded_path) noexcept;
  [[nodiscard]] std::wstring normalize_windows_image_path(std::wstring value) noexcept;
  [[nodiscard]] bool exact_original_image_match(std::wstring_view process_path,
                                                 std::wstring_view mirror_root,
                                                 std::span<const std::wstring> normalized_recorded_paths) noexcept;
  [[nodiscard]] constexpr bool quarantine_complete_for_retry(bool complete) noexcept { return complete; }

  constexpr std::size_t max_seat_id_size = 64;
  constexpr std::size_t max_ipc_name_size = 96;
  constexpr std::size_t max_command_line_size = 32768;

  // Games stay in the configured Steam library, outside the client mirror.
  // Every executable copied into the mirror is filtered; no basename allow-list
  // is used because it would silently miss future Steam helpers.
  [[nodiscard]] bool game_library_outside_mirror(std::string_view game_path,
                                                 std::string_view mirror_root) noexcept;
  [[nodiscard]] bool path_is_same_or_descendant(std::string_view path, std::string_view root) noexcept;
  // JobObjectBasicProcessIdList is complete only when the returned count
  // equals the assigned count; a successful partial result must be retried.
  [[nodiscard]] constexpr bool complete_job_process_list(std::size_t assigned, std::size_t returned) noexcept {
    return assigned == returned;
  }
  [[nodiscard]] bool is_configured_steam_client(std::string_view command_line) noexcept;
  [[nodiscard]] constexpr bool enabled_for_terminal(bool terminal_session, bool requested) noexcept { return terminal_session && requested; }
  [[nodiscard]] std::string ipc_name_for_seat(std::string_view opaque_seat_id);
  [[nodiscard]] std::string append_ipc_override(std::string command_line, std::string_view opaque_seat_id);
  [[nodiscard]] std::string rewrite_client_command(std::string command_line, std::string_view mirror_root,
                                                   std::string_view cache_root, std::string_view opaque_seat_id);
  [[nodiscard]] std::string deterministic_filter_key(std::string_view seat_id, std::uint64_t generation,
                                                     std::string_view canonical_path, bool ipv6);

}

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace steam_offline {
  constexpr std::size_t max_seat_id_size = 64;
  constexpr std::size_t max_ipc_name_size = 96;
  constexpr std::size_t max_command_line_size = 32768;

  // Games stay in the configured Steam library, outside the client mirror.
  // Every executable copied into the mirror is filtered; no basename allow-list
  // is used because it would silently miss future Steam helpers.
  [[nodiscard]] bool game_library_outside_mirror(std::string_view game_path,
                                                 std::string_view mirror_root) noexcept;
  [[nodiscard]] bool is_configured_steam_client(std::string_view command_line) noexcept;
  [[nodiscard]] constexpr bool enabled_for_terminal(bool terminal_session, bool requested) noexcept { return terminal_session && requested; }
  [[nodiscard]] std::string ipc_name_for_seat(std::string_view opaque_seat_id);
  [[nodiscard]] std::string append_ipc_override(std::string command_line, std::string_view opaque_seat_id);
  [[nodiscard]] std::string rewrite_client_command(std::string command_line, std::string_view mirror_root,
                                                   std::string_view cache_root, std::string_view opaque_seat_id);
  [[nodiscard]] std::string deterministic_filter_key(std::string_view seat_id, std::uint64_t generation,
                                                     std::string_view canonical_path, bool ipv6);

}

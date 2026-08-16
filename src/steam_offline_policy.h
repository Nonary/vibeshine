#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace steam_offline {
  constexpr std::size_t max_seat_id_size = 64;
  constexpr std::size_t max_ipc_name_size = 96;
  constexpr std::size_t max_command_line_size = 32768;

  [[nodiscard]] bool is_recognized_client_image(std::string_view image_name) noexcept;
  [[nodiscard]] bool is_configured_steam_client(std::string_view command_line) noexcept;
  [[nodiscard]] constexpr bool enabled_for_terminal(bool terminal_session, bool requested) noexcept { return terminal_session && requested; }
  [[nodiscard]] std::string ipc_name_for_seat(std::string_view opaque_seat_id);
  [[nodiscard]] std::string append_ipc_override(std::string command_line, std::string_view opaque_seat_id);

  struct process_identity_t {
    std::uint32_t pid {};
    std::uintptr_t object_key {};
    friend bool operator==(const process_identity_t &, const process_identity_t &) = default;
  };

  enum class lineage_state_e : std::uint8_t { empty, root, descendant, blocked_client };

  // A bounded, transport-neutral model of the kernel lineage contract. The
  // driver uses the process object pointer as object_key, so a PID reuse can
  // never inherit the previous registration.
  class lineage_registry_t {
  public:
    explicit lineage_registry_t(std::size_t capacity = 256);
    [[nodiscard]] bool register_root(process_identity_t root, std::uint64_t generation, std::string_view seat_id);
    [[nodiscard]] bool observe_child(process_identity_t child, process_identity_t parent, std::string_view image_name);
    [[nodiscard]] lineage_state_e state(process_identity_t identity) const noexcept;
    [[nodiscard]] bool remove(process_identity_t identity) noexcept;
    [[nodiscard]] bool generation_matches(std::uint64_t generation) const noexcept;
    [[nodiscard]] bool registration_matches(process_identity_t root, std::uint64_t generation, std::string_view seat_id) const noexcept;

  private:
    struct entry_t {
      process_identity_t identity {};
      lineage_state_e state {lineage_state_e::empty};
      std::uint64_t generation {};
    };
    std::vector<entry_t> entries_;
    process_identity_t root_ {};
    std::uint64_t generation_ {};
    std::string seat_id_;
  };
}

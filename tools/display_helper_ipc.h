#pragma once

#include <charconv>
#include <cstdint>
#include <limits>
#include <optional>
#include <string_view>
#include <system_error>

namespace display_helper_ipc {
  inline std::optional<std::uintptr_t> inherited_server_handle_from_command_line(int argc, char *argv[]) {
    constexpr std::string_view prefix = "--ipc-handle=";
    std::optional<std::uintptr_t> result;
    for (int index = 1; index < argc; ++index) {
      const std::string_view argument = argv[index] ? std::string_view(argv[index]) : std::string_view {};
      if (!argument.starts_with(prefix)) {
        continue;
      }
      if (result || argument.size() == prefix.size()) {
        return std::nullopt;
      }
      std::uintptr_t value = 0;
      const auto text = argument.substr(prefix.size());
      const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value, 10);
      if (parsed.ec != std::errc {} || parsed.ptr != text.data() + text.size() || value == 0 ||
          value == std::numeric_limits<std::uintptr_t>::max()) {
        return std::nullopt;
      }
      result = value;
    }
    return result;
  }
}  // namespace display_helper_ipc

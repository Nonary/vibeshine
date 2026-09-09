#pragma once

#include <charconv>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace platf::steam {
  // Only recognize the built-in Steam handoffs. Arbitrary commands still go
  // through the administrator's command manifest and supervised app scope.
  inline std::optional<std::vector<std::string>> session_handoff_arguments(std::string_view command) {
    for (const auto action : {std::string_view {"open"}, std::string_view {"close"}}) {
      const auto uri = "steam://" + std::string(action) + "/bigpicture";
      if (command == "setsid steam " + uri || command == "steam " + uri ||
          command == "/usr/bin/steam " + uri ||
          command == "/usr/libexec/vibeshine/vibeshine-session-exec steam-big-picture " + std::string(action)) {
        return std::vector<std::string> {"steam-big-picture", std::string(action)};
      }
    }

    constexpr std::string_view prefix = "/usr/libexec/vibeshine/vibeshine-session-exec steam ";
    if (!command.starts_with(prefix)) {
      return std::nullopt;
    }
    const auto id = command.substr(prefix.size());
    std::uint32_t app_id = 0;
    const auto parsed = std::from_chars(id.data(), id.data() + id.size(), app_id);
    if (parsed.ec != std::errc {} || parsed.ptr != id.data() + id.size() ||
        app_id == 0 || std::to_string(app_id) != id) {
      return std::nullopt;
    }
    return std::vector<std::string> {"steam", std::string(id)};
  }
}  // namespace platf::steam

#pragma once

#ifdef _WIN32

  #include "src/platform/windows/virtual_display_policy.h"

  #ifndef WIN32_LEAN_AND_MEAN
    #define WIN32_LEAN_AND_MEAN
  #endif
  #include <windows.h>
  #include <wtsapi32.h>

  #include <cstdint>
  #include <filesystem>
  #include <optional>
  #include <string>

namespace display_helper_session {
  inline std::optional<std::uint32_t> current_process_session_id() noexcept {
    DWORD session_id = 0;
    if (!ProcessIdToSessionId(GetCurrentProcessId(), &session_id)) {
      return std::nullopt;
    }
    return session_id;
  }

  inline bool is_non_console_interactive() noexcept {
    const auto session_id = current_process_session_id();
    return VDISPLAY::policy::is_non_console_interactive_session(
      session_id.has_value(),
      session_id.value_or(0),
      WTSGetActiveConsoleSessionId());
  }

  inline std::string pipe_name() {
    const auto session_id = current_process_session_id();
    return session_id && VDISPLAY::policy::is_non_console_interactive_session(
                           true,
                           *session_id,
                           WTSGetActiveConsoleSessionId()) ?
             "sunshine_display_helper_session_" + std::to_string(*session_id) :
             "sunshine_display_helper";
  }

  inline std::wstring singleton_mutex_name() {
    // The Local namespace is scoped by the Windows session. A console helper
    // and a managed-seat helper therefore cannot suppress one another.
    return L"Local\\SunshineDisplayHelper";
  }

  inline std::filesystem::path scope_runtime_path(std::filesystem::path root) {
    const auto session_id = current_process_session_id();
    if (session_id && VDISPLAY::policy::is_non_console_interactive_session(
                        true,
                        *session_id,
                        WTSGetActiveConsoleSessionId())) {
      root /= L"terminal_sessions";
      root /= std::to_wstring(*session_id);
    }
    return root;
  }
}  // namespace display_helper_session

#endif  // _WIN32

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
  #include <cwctype>
  #include <limits>
  #include <optional>
  #include <string>
  #include <string_view>

namespace display_helper_session {
  struct managed_context_t {
    bool advertised {};
    bool valid {};
    std::uint32_t session_id {};
    std::uint64_t generation {};
    std::string capability;
  };

  inline std::optional<std::wstring> environment(const wchar_t *name) noexcept {
    const DWORD size = GetEnvironmentVariableW(name, nullptr, 0);
    if (!size || size > 512) return std::nullopt;
    std::wstring value(size, L'\0');
    const DWORD written = GetEnvironmentVariableW(name, value.data(), size);
    if (!written || written >= size) return std::nullopt;
    value.resize(written);
    return value;
  }

  inline bool decimal(std::wstring_view value, std::uint64_t &out) noexcept {
    if (value.empty() || value.size() > 20) return false;
    std::uint64_t parsed = 0;
    for (const wchar_t ch: value) {
      if (ch < L'0' || ch > L'9' || parsed > (std::numeric_limits<std::uint64_t>::max() - static_cast<std::uint64_t>(ch - L'0')) / 10) return false;
      parsed = parsed * 10 + static_cast<std::uint64_t>(ch - L'0');
    }
    out = parsed;
    return true;
  }

  inline bool capability_text_valid(std::wstring_view value) noexcept {
    if (value.size() < 32 || value.size() > 64) return false;
    for (const wchar_t ch: value) {
      if (!((ch >= L'0' && ch <= L'9') || (ch >= L'a' && ch <= L'f') ||
            (ch >= L'A' && ch <= L'F') || ch == L'-' || ch == L'{' || ch == L'}')) return false;
    }
    return true;
  }

  inline const managed_context_t &managed_context() noexcept {
    static const managed_context_t context = [] {
      managed_context_t result {};
      const auto session = environment(L"VIBESHINE_TERMINAL_SESSION_ID");
      const auto generation = environment(L"VIBESHINE_TERMINAL_GENERATION");
      const auto capability = environment(L"VIBESHINE_TERMINAL_HELPER_CAPABILITY");
      result.advertised = session.has_value() || generation.has_value() || capability.has_value();
      std::uint64_t session_value = 0;
      std::uint64_t generation_value = 0;
      if (!session || !generation || !capability || !decimal(*session, session_value) ||
          !decimal(*generation, generation_value) || session_value == 0 || generation_value == 0 ||
          session_value > (std::numeric_limits<std::uint32_t>::max)() || !capability_text_valid(*capability)) return result;
      result.valid = true;
      result.session_id = static_cast<std::uint32_t>(session_value);
      result.generation = generation_value;
      result.capability.reserve(capability->size());
      for (const wchar_t ch: *capability) result.capability.push_back(static_cast<char>(ch));
      return result;
    }();
    return context;
  }

  inline bool has_managed_context() noexcept { return managed_context().advertised; }
  inline bool managed_context_is_valid() noexcept { return managed_context().valid; }
  inline std::uint64_t managed_generation() noexcept { return managed_context().generation; }

  inline std::optional<std::uint32_t> current_process_session_id() noexcept {
    DWORD session_id = 0;
    if (!ProcessIdToSessionId(GetCurrentProcessId(), &session_id)) {
      return std::nullopt;
    }
    return session_id;
  }

  inline bool is_non_console_interactive() noexcept {
    const auto session_id = current_process_session_id();
    return managed_context_is_valid() && session_id && *session_id == managed_context().session_id;
  }

  inline std::string pipe_name() {
    if (has_managed_context()) {
      return "sunshine_display_helper_" + managed_context().capability;
    }
    return "sunshine_display_helper";
  }

  inline std::wstring singleton_mutex_name() {
    if (has_managed_context()) {
      return L"Local\\SunshineDisplayHelper-" +
        std::wstring {managed_context().capability.begin(), managed_context().capability.end()};
    }
    return L"Local\\SunshineDisplayHelper";
  }

  inline std::filesystem::path scope_runtime_path(std::filesystem::path root) {
    if (has_managed_context()) {
      root /= L"terminal_sessions";
      if (managed_context_is_valid()) {
        root /= std::to_wstring(managed_context().session_id);
        root /= std::to_wstring(managed_context().generation);
        root /= std::filesystem::path {managed_context().capability};
      } else {
        root /= L"invalid";
      }
    }
    return root;
  }
}  // namespace display_helper_session

#endif  // _WIN32

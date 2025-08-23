/**
 * @file src/platform/windows/misc.h
 * @brief Miscellaneous declarations for Windows.
 */
#pragma once

// standard includes
#include <chrono>
#include <functional>
#include <string_view>
#include <system_error>

// platform includes
#include <Windows.h>
#include <winnt.h>

namespace platf {
  void print_status(const std::string_view &prefix, HRESULT status);
  HDESK syncThreadDesktop();

  int64_t qpc_counter();

  std::chrono::nanoseconds qpc_time_difference(int64_t performance_counter1, int64_t performance_counter2);

  /**
   * @brief Convert a UTF-8 string into a UTF-16 wide string.
   * @param string The UTF-8 string.
   * @return The converted UTF-16 wide string.
   */
  std::wstring from_utf8(const std::string &string);

  /**
   * @brief Convert a UTF-16 wide string into a UTF-8 string.
   * @param string The UTF-16 wide string.
   * @return The converted UTF-8 string.
   */
  std::string to_utf8(const std::wstring &string);

  // Additional helpers used by configuration HTTP to safely access user resources
  bool is_running_as_system();
  HANDLE retrieve_users_token(bool elevated);

  // Impersonate the given user token and invoke the callback while impersonating.
  // Returns an std::error_code describing any failure (empty on success).
  std::error_code impersonate_current_user(HANDLE user_token, std::function<void()> callback);

  /**
   * @brief Override per-user predefined registry keys (HKCU, HKCR) for the given token.
   * @param token Primary user token to use for HKCU/HKCR views, or nullptr to restore defaults.
   * @return true on success.
   */
  bool override_per_user_predefined_keys(HANDLE token);
}  // namespace platf

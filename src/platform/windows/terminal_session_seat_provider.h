/**
 * @file src/platform/windows/terminal_session_seat_provider.h
 * @brief Privileged managed-account/Remote-IDD provider for terminal seats.
 */
#pragma once

#include "src/terminal_session_runtime.h"

#include <memory>

namespace terminal_session::windows {
  [[nodiscard]] std::unique_ptr<seat_provider_t> make_seat_provider(std::size_t maximum_seats = 8);

  /** Disable every exact broker-owned account, optionally logging off and deleting it. */
  [[nodiscard]] bool secure_managed_accounts(bool remove);

  /** Child mode run by sunshinesvc in the target WTS session. */
  int run_seat_acl_helper(int argc, wchar_t **argv);
}

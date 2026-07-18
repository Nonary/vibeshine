#pragma once

#include <chrono>

namespace display_helper::v2::timing {
  // A full staged APPLY may include the v1-compatible 0/750/2500/5500ms
  // settling staircase, then policy retries. Each individual APPLY can spend
  // two five-second topology windows and a staged rollback/positioning phase.
  // Keep IPC acknowledgement and the capture gate alive for one deliberately
  // conservative whole-operation envelope so capture never starts mid-repair.
  inline constexpr auto kApplyOperationEnvelope = std::chrono::seconds(180);

  // The producer owns the operation envelope. Consumers waiting on the
  // producer's future get a small scheduling margin so they cannot proceed
  // just before it publishes the final verification result.
  inline constexpr auto kApplyGateConsumerSlack = std::chrono::seconds(5);
}  // namespace display_helper::v2::timing

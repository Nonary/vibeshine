/**
 * @file src/platform/windows/ipc/display_settings_protocol.h
 * @brief Shared display helper recovery acknowledgement contract.
 */
#pragma once

#include <cstdint>

namespace platf::display_helper_protocol {
  // SnapshotResult (type 12) has the exact payload:
  // [u8 success][u64 little-endian request ID][u8 recovery version].
  // Version 1 success confirms a usable durable baseline and armed recovery
  // ownership, including the restore task when enabled, or positively proven
  // headlessness without retained physical recovery evidence. A snapshot write
  // alone does not satisfy this contract. The APPLY protocol is independent.
  // Older helpers omit the version and cannot authorize a new host's mutation;
  // older hosts accept the appended byte after their existing correlated ID.
  inline constexpr std::uint8_t kSnapshotRecoveryVersion = 1;
}

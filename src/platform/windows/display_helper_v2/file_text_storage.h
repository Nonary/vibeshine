#pragma once

#include "src/platform/windows/display_helper_v2/text_storage.h"

#include <array>
#include <cstdint>
#include <filesystem>

namespace display_helper::v2 {
  /// Windows/runtime adapter: preserves the legacy durable atomic-write path.
  class AtomicFileTextStorage final : public ITextStorage {
  public:
    explicit AtomicFileTextStorage(bool snapshot_envelope = false) : snapshot_envelope_(snapshot_envelope) {}
    std::optional<std::string> read(const std::string &key) override;
    bool write_atomically(const std::string &key, const std::string &text) override;
    std::optional<std::string> read(const std::string &key, SnapshotTier tier) override;
    bool write_atomically(const std::string &key, const std::string &text, SnapshotTier tier) override;
    bool remove(const std::string &key) override;
    bool exists(const std::string &key) override;

  private:
    bool snapshot_envelope_ {};
  };

}  // namespace display_helper::v2

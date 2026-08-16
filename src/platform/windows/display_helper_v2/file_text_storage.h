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
    bool remove(const std::string &key) override;
    bool exists(const std::string &key) override;

  private:
    bool snapshot_envelope_ {};
  };

  void set_managed_snapshot_mac_key(const std::array<std::uint8_t, 32> &key);
  void clear_managed_snapshot_mac_key();
}  // namespace display_helper::v2

#pragma once

#include <cstdint>
#include <array>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace steam_offline {

  struct preparation_t {
    std::filesystem::path mirror_root;
    std::filesystem::path cache_root;
    std::filesystem::path steam_executable;
    std::filesystem::path proxy_executable;
    std::string manifest_digest;
    std::size_t filtered_executable_count {};
  };

  // The broker owns this object.  It uses only the documented user-mode BFE
  // API and is intentionally unusable from a non-LocalSystem process.
  class manager_t {
  public:
    manager_t() = default;
    ~manager_t();
    manager_t(const manager_t &) = delete;
    manager_t &operator=(const manager_t &) = delete;
    manager_t(manager_t &&) noexcept;
    manager_t &operator=(manager_t &&) noexcept;

    [[nodiscard]] static bool available(std::string &error) noexcept;
    [[nodiscard]] bool prepare(const std::filesystem::path &steam_executable,
                               const std::filesystem::path &proxy_executable,
                               void *source_impersonation_token,
                               std::string_view seat_id, std::string_view user_sid, std::uint64_t generation,
                               preparation_t &result, std::string &error) noexcept;
    [[nodiscard]] bool healthy(std::string &error) const noexcept;
    [[nodiscard]] bool release(std::string &error) noexcept;
    // Keep persistent filters installed when termination cannot be proven.
    // A quarantined manager is intentionally non-releasable until a later
    // reconciliation observes that the isolated tree is gone.
    void quarantine() noexcept { quarantined_ = true; }
    void clear_quarantine() noexcept { quarantined_ = false; }
    [[nodiscard]] bool active() const noexcept { return engine_ != nullptr && !filter_keys_.empty(); }
    [[nodiscard]] const preparation_t &preparation() const noexcept { return preparation_; }

  private:
    void *engine_ {};
    std::vector<std::array<std::uint8_t, 16>> filter_keys_;
    preparation_t preparation_;
    std::string seat_id_;
    std::uint64_t generation_ {};
    bool quarantined_ {};
  };

  // The command and app manifest are read by the SYSTEM broker, never from a
  // worker-provided path.  A missing or ambiguous command is a hard failure.
  [[nodiscard]] std::filesystem::path trusted_steam_executable(
    const std::filesystem::path &apps_manifest,
    std::span<const std::uint8_t> launch_payload,
    std::string &error) noexcept;
}

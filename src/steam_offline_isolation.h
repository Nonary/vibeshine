#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace steam_offline {
  class registration_t {
  public:
    registration_t() = default;
    ~registration_t();
    registration_t(const registration_t &) = delete;
    registration_t &operator=(const registration_t &) = delete;
    registration_t(registration_t &&other) noexcept;
    registration_t &operator=(registration_t &&other) noexcept;

    [[nodiscard]] static bool available(std::string &error) noexcept;
    [[nodiscard]] bool register_root(std::uint32_t pid, std::uint64_t process_creation_time, std::uint64_t generation,
                                     std::string_view seat_id, std::string &error) noexcept;
    [[nodiscard]] bool healthy(std::string &error) const noexcept;
    [[nodiscard]] bool release(std::string &error) noexcept;
    [[nodiscard]] bool active() const noexcept { return device_ != nullptr && registration_id_ != 0; }

  private:
    void *device_ {};
    std::uint64_t registration_id_ {};
    std::uint32_t root_pid_ {};
    std::uint64_t process_creation_time_ {};
    std::uint64_t generation_ {};
    std::uint64_t readiness_generation_ {};
    std::string seat_id_;
  };
}

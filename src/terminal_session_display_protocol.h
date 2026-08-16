#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>

namespace terminal_session::display {
  inline constexpr std::uint32_t magic = 0x31534454; // "TSD1"
  inline constexpr std::uint16_t version = 1;
  inline constexpr std::size_t max_message_size = 64;

  enum class operation : std::uint8_t {
    query = 1,
    set_mode = 2,
    set_hdr = 3,
  };

  enum class result : std::uint8_t {
    success = 0,
    malformed = 1,
    stale = 2,
    unavailable = 3,
    invalid = 4,
  };

#pragma pack(push, 1)
  struct request_t {
    std::uint32_t magic {terminal_session::display::magic};
    std::uint16_t version {terminal_session::display::version};
    std::uint8_t operation {static_cast<std::uint8_t>(operation::query)};
    std::uint8_t flags {};
    std::uint64_t generation {};
    std::uint64_t request_id {};
    std::uint32_t width {};
    std::uint32_t height {};
    std::uint32_t refresh_rate_millihz {};
    std::uint32_t hdr_enabled {};
    std::uint32_t reserved {};
  };

  struct response_t {
    std::uint32_t magic {terminal_session::display::magic};
    std::uint16_t version {terminal_session::display::version};
    std::uint8_t operation {static_cast<std::uint8_t>(operation::query)};
    std::uint8_t result {static_cast<std::uint8_t>(display::result::malformed)};
    std::uint64_t generation {};
    std::uint64_t request_id {};
    std::uint32_t width {};
    std::uint32_t height {};
    std::uint32_t refresh_rate_millihz {};
    std::uint32_t hdr_enabled {};
    std::uint32_t native_error {};
    std::uint32_t reserved {};
  };
#pragma pack(pop)

  static_assert(sizeof(request_t) == 48);
  static_assert(sizeof(response_t) == 48);

  inline bool valid_operation(const std::uint8_t value) {
    return value >= static_cast<std::uint8_t>(operation::query) &&
           value <= static_cast<std::uint8_t>(operation::set_hdr);
  }

  inline bool valid_request(const request_t &request) {
    return request.magic == magic && request.version == version && valid_operation(request.operation) &&
           request.flags == 0 && request.generation != 0 && request.request_id != 0 && request.reserved == 0 &&
           (request.operation == static_cast<std::uint8_t>(operation::query) &&
            request.width == 0 && request.height == 0 && request.refresh_rate_millihz == 0 && request.hdr_enabled == 0) ||
            (request.operation == static_cast<std::uint8_t>(operation::set_mode) &&
             request.width >= 1 && request.width <= 16384 && request.height >= 1 && request.height <= 16384 &&
             request.refresh_rate_millihz >= 1000 && request.refresh_rate_millihz <= 1'000'000 && request.hdr_enabled == 0) ||
            (request.operation == static_cast<std::uint8_t>(operation::set_hdr) &&
             request.width == 0 && request.height == 0 && request.refresh_rate_millihz == 0 &&
             request.hdr_enabled <= 1));
  }

  inline bool valid_response(const response_t &response) {
    return response.magic == magic && response.version == version && valid_operation(response.operation) &&
           response.result <= static_cast<std::uint8_t>(result::invalid) && response.generation != 0 &&
           response.request_id != 0 && response.reserved == 0;
  }

  [[nodiscard]] std::optional<response_t> transact(operation operation_code,
                                                    std::uint64_t generation,
                                                    std::uint32_t width = 0,
                                                    std::uint32_t height = 0,
                                                    std::uint32_t refresh_rate_millihz = 0,
                                                    std::uint32_t hdr_enabled = 0);

  template<class T>
  inline bool decode(const std::uint8_t *bytes, const std::size_t size, T &value) {
    if (!bytes || size != sizeof(T) || size > max_message_size) return false;
    std::memcpy(&value, bytes, sizeof(T));
    return true;
  }
}

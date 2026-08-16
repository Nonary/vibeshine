#pragma once

#include <cstdint>
#include <cstddef>

namespace steam_offline::driver {
  constexpr std::uint32_t protocol_version = 1;
  constexpr std::uint32_t device_type = 0x00000012; // FILE_DEVICE_NETWORK
  constexpr std::uint32_t method_buffered = 0;
  constexpr std::uint32_t file_any_access = 0;
  constexpr std::uint32_t file_write_data = 2;
  constexpr std::uint32_t ctl_code(std::uint32_t function, std::uint32_t access) {
    return (device_type << 16) | (access << 14) | (function << 2) | method_buffered;
  }
  constexpr std::uint32_t register_root_ioctl = ctl_code(0x801, file_write_data);
  constexpr std::uint32_t unregister_root_ioctl = ctl_code(0x802, file_write_data);
  constexpr std::uint32_t status_ioctl = ctl_code(0x803, file_write_data);
  constexpr std::size_t max_seat_id_size = 64;

#pragma pack(push, 1)
  struct register_root_t {
    std::uint32_t version;
    std::uint32_t root_pid;
    // FILETIME creation timestamp, paired with root_pid to defeat PID reuse.
    std::uint64_t process_creation_time;
    std::uint64_t generation;
    // NUL-terminated provider seat token; the service validates its charset
    // and length before issuing this IOCTL.
    char seat_id[max_seat_id_size];
  };
  struct registration_t {
    std::uint32_t version;
    std::uint32_t reserved;
    std::uint64_t registration_id;
    // Generation of the BFE/WFP readiness state at admission.
    std::uint64_t readiness_generation;
  };
  struct unregister_root_t {
    std::uint32_t version;
    std::uint32_t reserved;
    std::uint64_t registration_id;
    std::uint64_t generation;
    char seat_id[max_seat_id_size];
  };
  struct status_t {
    std::uint32_t version;
    std::uint32_t reserved;
    std::uint32_t bfe_ready;
    std::uint32_t wfp_ready;
    std::uint64_t bfe_generation;
  };
#pragma pack(pop)

  static_assert(offsetof(register_root_t, process_creation_time) == 8);
  static_assert(offsetof(register_root_t, generation) == 16);
  static_assert(offsetof(register_root_t, seat_id) == 24);
  static_assert(offsetof(unregister_root_t, registration_id) == 8);
  static_assert(offsetof(unregister_root_t, generation) == 16);
  static_assert(offsetof(unregister_root_t, seat_id) == 24);
  static_assert(sizeof(registration_t) == 24);
  static_assert(sizeof(status_t) == 24);
}

#include "steam_offline_isolation.h"

#include "steam_offline_filter_ioctl.h"

#include <algorithm>
#include <cctype>

#ifdef _WIN32
  #include <Windows.h>
#endif

namespace steam_offline {
#ifdef _WIN32
  namespace {
    constexpr wchar_t device_name[] = L"\\\\.\\VibeshineSteamOfflineFilter";
    HANDLE open_device() noexcept {
      return CreateFileW(device_name,
                         GENERIC_READ | GENERIC_WRITE,
                         FILE_SHARE_READ | FILE_SHARE_WRITE,
                         nullptr,
                         OPEN_EXISTING,
                         FILE_ATTRIBUTE_NORMAL,
                         nullptr);
    }
    std::string win32_error(const char *prefix) {
      return std::string {prefix} + " (Win32 error " + std::to_string(GetLastError()) + ")";
    }
  }
#endif

  namespace {
    bool safe_seat_id(const std::string_view value) noexcept {
      return !value.empty() && value.size() < driver::max_seat_id_size &&
        std::all_of(value.begin(), value.end(), [](const unsigned char ch) {
          return std::isalnum(ch) || ch == '-' || ch == '_' || ch == '.';
        });
    }
  }

  registration_t::~registration_t() {
    std::string ignored;
    (void) release(ignored);
  }

  registration_t::registration_t(registration_t &&other) noexcept:
      device_(other.device_), registration_id_(other.registration_id_), root_pid_(other.root_pid_),
      process_creation_time_(other.process_creation_time_), generation_(other.generation_),
      readiness_generation_(other.readiness_generation_), seat_id_(std::move(other.seat_id_)) {
    other.device_ = nullptr;
    other.registration_id_ = 0;
    other.root_pid_ = 0;
    other.process_creation_time_ = 0;
    other.generation_ = 0;
    other.readiness_generation_ = 0;
    other.seat_id_.clear();
  }

  registration_t &registration_t::operator=(registration_t &&other) noexcept {
    if (this == &other) return *this;
    std::string ignored;
    (void) release(ignored);
    device_ = other.device_;
    registration_id_ = other.registration_id_;
    root_pid_ = other.root_pid_;
    process_creation_time_ = other.process_creation_time_;
    generation_ = other.generation_;
    readiness_generation_ = other.readiness_generation_;
    seat_id_ = std::move(other.seat_id_);
    other.device_ = nullptr;
    other.registration_id_ = 0;
    other.root_pid_ = 0;
    other.process_creation_time_ = 0;
    other.generation_ = 0;
    other.readiness_generation_ = 0;
    other.seat_id_.clear();
    return *this;
  }

  bool registration_t::available(std::string &error) noexcept {
#ifdef _WIN32
    HANDLE device = open_device();
    if (!device || device == INVALID_HANDLE_VALUE) {
      error = win32_error("Steam offline isolation driver is unavailable");
      return false;
    }
    driver::status_t status {.version = driver::protocol_version};
    DWORD returned = 0;
    const bool ready = DeviceIoControl(device, driver::status_ioctl, nullptr, 0,
                                       &status, sizeof(status), &returned, nullptr) != FALSE &&
      returned == sizeof(status) && status.version == driver::protocol_version &&
      status.bfe_ready != 0 && status.wfp_ready != 0 && status.bfe_generation != 0;
    CloseHandle(device);
    if (!ready) {
      error = "Steam offline isolation driver is installed but BFE/WFP is not ready.";
    }
    return ready;
#else
    error = "Steam offline isolation is Windows-only.";
    return false;
#endif
  }

  bool registration_t::register_root(const std::uint32_t pid, const std::uint64_t process_creation_time,
                                     const std::uint64_t generation, const std::string_view seat_id, std::string &error) noexcept {
    if (active() || !pid || !process_creation_time || !generation || !safe_seat_id(seat_id)) {
      error = "Steam offline isolation registration identity is invalid or already active.";
      return false;
    }
#ifdef _WIN32
    HANDLE device = open_device();
    if (!device || device == INVALID_HANDLE_VALUE) {
      error = win32_error("Steam offline isolation driver is unavailable");
      return false;
    }
    driver::register_root_t input {.version = driver::protocol_version, .root_pid = pid,
                                   .process_creation_time = process_creation_time, .generation = generation};
    std::copy(seat_id.begin(), seat_id.end(), input.seat_id);
    driver::registration_t output {.version = driver::protocol_version};
    DWORD returned = 0;
    if (!DeviceIoControl(device, driver::register_root_ioctl, &input, sizeof(input), &output, sizeof(output), &returned, nullptr) ||
        returned != sizeof(output) || output.version != driver::protocol_version || output.registration_id == 0 ||
        output.readiness_generation == 0) {
      error = win32_error("Steam offline isolation root registration was rejected");
      CloseHandle(device);
      return false;
    }
    device_ = device;
    registration_id_ = output.registration_id;
    root_pid_ = pid;
    process_creation_time_ = process_creation_time;
    generation_ = generation;
    readiness_generation_ = output.readiness_generation;
    seat_id_ = std::string {seat_id};
    return true;
#else
    error = "Steam offline isolation is Windows-only.";
    return false;
#endif
  }

  bool registration_t::release(std::string &error) noexcept {
    if (!device_ && !registration_id_) return true;
#ifdef _WIN32
    HANDLE device = static_cast<HANDLE>(device_);
    driver::unregister_root_t input {.version = driver::protocol_version, .registration_id = registration_id_,
                                      .generation = generation_};
    std::copy(seat_id_.begin(), seat_id_.end(), input.seat_id);
    DWORD returned = 0;
    const bool released = DeviceIoControl(device, driver::unregister_root_ioctl, &input, sizeof(input), nullptr, 0, &returned, nullptr) != FALSE;
    if (!released) {
      error = win32_error("Steam offline isolation registration could not be removed");
      return false;
    }
    CloseHandle(device);
    device_ = nullptr;
    registration_id_ = 0;
    root_pid_ = 0;
    process_creation_time_ = 0;
    generation_ = 0;
    readiness_generation_ = 0;
    seat_id_.clear();
    return true;
#else
    error = "Steam offline isolation is Windows-only.";
    return false;
#endif
  }

  bool registration_t::healthy(std::string &error) const noexcept {
    if (!active()) {
      error = "Steam offline isolation registration is not active.";
      return false;
    }
#ifdef _WIN32
    driver::status_t output {.version = driver::protocol_version};
    DWORD returned = 0;
    if (!DeviceIoControl(static_cast<HANDLE>(device_), driver::status_ioctl, nullptr, 0,
                         &output, sizeof(output), &returned, nullptr) ||
        returned != sizeof(output) || output.version != driver::protocol_version ||
        output.bfe_ready == 0 || output.wfp_ready == 0 ||
        output.bfe_generation == 0 || output.bfe_generation != readiness_generation_) {
      error = win32_error("Steam offline isolation driver lost WFP/BFE readiness");
      return false;
    }
    return true;
#else
    error = "Steam offline isolation is Windows-only.";
    return false;
#endif
  }
}

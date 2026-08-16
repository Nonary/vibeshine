/**
 * @file src/terminal_session_seat_pool.h
 * @brief Reusable managed-account seat allocation for terminal sessions.
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace terminal_session::seat_pool {
  enum class connection_state_e : std::uint8_t {
    disconnected,
    connected,
    faulted,
  };

  enum class release_disposition_e : std::uint8_t {
    /** Stream ended. Keep the client/generation attached to the warm seat. */
    retain,
    /** Client can no longer resume. Recycle only after retained applications exit. */
    abandon,
    /** Service shutdown. Disconnect and recycle only after applications exit. */
    shutdown,
  };

  struct capability_t {
    bool supported {};
    bool termwrap_ready {};
    bool session_controller {};
    bool remote_display {};
    bool audio_endpoint {};
    bool token_launch {};
    std::string error;
  };

  struct seat_t {
    std::string seat_id;
    std::string account_name;
    std::string account_sid;
    std::uint32_t windows_session_id {};
    connection_state_e connection {connection_state_e::disconnected};
    bool managed {};
    // False when discovery found an ownerless retained application. Such a
    // seat survives service restart but may not be assigned to another client.
    bool reusable {true};
  };

  struct request_t {
    std::string client_uuid;
    std::uint64_t generation {};
    std::uint32_t launch_id {};
    std::uint16_t width {1920};
    std::uint16_t height {1080};
  };

  struct lease_t {
    seat_t seat;
    request_t owner;
    bool created {};
    bool resumed {};
  };

  /** Privileged Windows operations remain behind this injectable boundary. */
  class backend_t {
  public:
    virtual ~backend_t() = default;
    [[nodiscard]] virtual capability_t preflight() = 0;
    [[nodiscard]] virtual std::vector<seat_t> discover(std::string &error) = 0;
    [[nodiscard]] virtual std::optional<seat_t> create(std::string &error) = 0;
    virtual bool connect(seat_t &seat, const request_t &request, std::string &error) = 0;
    virtual bool disconnect(seat_t &seat, std::string &error) = 0;
    [[nodiscard]] virtual bool has_retained_applications(const seat_t &seat) noexcept = 0;
  };

  /**
   * A seat pool assigns unowned disconnected seats in round-robin order.
   * Disconnect retains client affinity; it never logs off or deletes accounts.
   */
  class pool_t {
  public:
    explicit pool_t(std::unique_ptr<backend_t> backend, std::size_t maximum_seats = 8);

    [[nodiscard]] capability_t preflight();
    [[nodiscard]] std::optional<lease_t> acquire(const request_t &request, std::string &error);
    [[nodiscard]] bool release(const request_t &request, release_disposition_e disposition, std::string &error);
    [[nodiscard]] std::optional<lease_t> snapshot(std::string_view client_uuid) const;
    [[nodiscard]] std::size_t size() const;

  private:
    struct record_t {
      seat_t seat;
      std::optional<request_t> owner;
      bool draining {};
    };

    bool refresh_locked(std::string &error);
    void reap_locked();
    [[nodiscard]] std::optional<std::string> select_available_locked();
    [[nodiscard]] static bool same_generation(const request_t &left, const request_t &right);

    std::unique_ptr<backend_t> backend_;
    std::size_t maximum_seats_;
    std::size_t next_index_ {};
    bool discovered_ {};
    mutable std::mutex mutex_;
    std::unordered_map<std::string, record_t> seats_;
  };
} // namespace terminal_session::seat_pool

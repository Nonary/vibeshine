/**
 * @file src/platform/windows/playnite_ipc.h
 * @brief Playnite plugin IPC server using Windows named pipes with anonymous handshake.
 */
#pragma once

#include "src/logging.h"
#include "src/platform/windows/ipc/pipes.h"

#include <atomic>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <thread>

namespace platf::playnite {

  /**
   * @brief Minimal IPC server to handshake with the Playnite plugin and receive messages.
   *
   * Server uses AnonymousPipeFactory to establish a control pipe at a well-known name, then
   * performs an anonymous handshake to switch to a per-session data pipe. It then polls for
   * incoming messages from the plugin.
   */
  class IpcServer {
  public:
    // Optional dynamic control pipe name; if empty, defaults to the well-known connector name
    IpcServer();
    explicit IpcServer(const std::string &control_name);
    ~IpcServer();

    /**
     * @brief Start server thread if not already running.
     */
    void start();

    /**
     * @brief Stop server thread.
     */
    void stop();

    /**
     * @brief Set optional handler for raw plugin messages.
     */
    void set_message_handler(std::function<void(std::span<const uint8_t>)> handler) {
      handler_ = std::move(handler);
    }

    // Returns true if actively connected to the plugin
    bool is_active() const {
      return active_.load();
    }

    // Returns true if the server thread is running/listening (may not be connected yet)
    bool is_started() const {
      return running_.load();
    }

    // Send a JSON line (UTF-8 + trailing \n) to the plugin if connected
    bool send_json_line(const std::string &json);

  private:
    void run();
    void accumulate_and_dispatch_lines(std::span<const uint8_t> bytes);
    std::unique_ptr<platf::dxgi::INamedPipe> wait_for_handshake_and_get_data_pipe();
    bool validate_client_and_get_expected_sid(platf::dxgi::WinPipe *wp);
    bool get_client_pid(platf::dxgi::WinPipe *wp, DWORD &pid);
    bool is_playnite_process(DWORD pid);
    bool get_client_sid(platf::dxgi::WinPipe *wp, std::wstring &sid);
    bool get_expected_user_sid(std::wstring &sid);
    void serve_connected_loop();
    // Check if any Playnite process is running (Desktop or Fullscreen)
    bool is_playnite_running();
    /**
     * @brief Check if an interactive user session is currently available.
     *
     * We only attempt to create / listen on the Playnite control pipe when an interactive
     * user session (desktop token) exists. This prevents repeated failing attempts and
     * associated log spam while Sunshine is running as a service before any user logs in.
     *
     * @return true if a user session token could be acquired (session available); false otherwise.
     */
    bool is_user_session_available();

    std::atomic<bool> running_ {false};
    std::thread worker_;
    std::unique_ptr<platf::dxgi::AsyncNamedPipe> pipe_;
    std::function<void(std::span<const uint8_t>)> handler_;
    std::atomic<bool> broken_ {false};
    std::atomic<bool> active_ {false};
    std::string recv_buffer_;
    std::string control_name_;
    bool no_session_logged_ = false;  ///< Ensures we only log missing session once until it appears.
    bool no_playnite_logged_ = false;  ///< Ensures we only log missing Playnite once until it appears.
  };
}  // namespace platf::playnite

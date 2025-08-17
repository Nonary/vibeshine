/**
 * @file src/platform/windows/playnite_ipc.h
 * @brief Playnite plugin IPC server using Windows named pipes with anonymous handshake.
 */
#pragma once

#include <atomic>
#include <span>
#include <functional>
#include <memory>
#include <string>
#include <thread>

#include "src/logging.h"
#include "src/platform/windows/ipc/pipes.h"

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
    IpcServer();
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
    void set_message_handler(std::function<void(std::span<const uint8_t>)> handler) { handler_ = std::move(handler); }

    // Returns true if actively connected to the plugin
    bool is_active() const { return active_.load(); }

    // Send a JSON line (UTF-8 + trailing \n) to the plugin if connected
    bool send_json_line(const std::string &json);

  private:
    void run();

    std::atomic<bool> running_ {false};
    std::thread worker_;
    std::unique_ptr<platf::dxgi::AsyncNamedPipe> pipe_;
    std::function<void(std::span<const uint8_t>)> handler_;
    std::atomic<bool> broken_ {false};
    std::atomic<bool> active_ {false};
  };
}  // namespace platf::playnite

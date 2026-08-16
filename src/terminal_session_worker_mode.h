/**
 * @file src/terminal_session_worker_mode.h
 * @brief Private RTSP/media worker bootstrap over service-owned local IPC.
 */
#pragma once

#include <memory>
#include <string>

namespace terminal_session::worker_mode {
  class context_t;
  struct bootstrap_t {
    bool requested {};
    std::unique_ptr<context_t> context;
    std::string error;
  };

  [[nodiscard]] bootstrap_t connect_from_environment();
  /** True only inside the service-admitted private Sunshine worker process. */
  [[nodiscard]] bool active() noexcept;

  class context_t {
  public:
    ~context_t();
    context_t(context_t &&) noexcept;
    context_t &operator=(context_t &&) noexcept;
    context_t(const context_t &) = delete;
    context_t &operator=(const context_t &) = delete;

    [[nodiscard]] bool publish_ready(std::string &error);
    void stop();

  private:
    class impl_t;
    explicit context_t(std::unique_ptr<impl_t> impl);
    std::unique_ptr<impl_t> impl_;
    friend bootstrap_t connect_from_environment();
  };
}

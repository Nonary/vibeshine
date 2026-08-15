#pragma once

#include "terminal_session_runtime.h"

#include <memory>

namespace terminal_session::worker {
  /** Real process boundary used by the service; provider resources remain injectable. */
  class process_t final: public seat_worker_t {
  public:
    process_t() = default;
    ~process_t() override;
    std::optional<route_t> start(const worker_request_t &, std::string &error) override;
    bool stop(const route_t &) noexcept override;
    bool cleanup_needed() const noexcept override;
  private:
#ifdef _WIN32
    void *process_ {};
    void *job_ {};
    std::string pipe_name_;
#endif
    std::uint32_t pid_ {};
  };
}

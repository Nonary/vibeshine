/** Admission and monotonic timing decisions shared by discrete capture and tests. */
#pragma once

#include <atomic>
#include <chrono>
#include <utility>

namespace audio {
  class source_admission_t {
  public:
    class lease_t {
    public:
      lease_t(const lease_t &) = delete;
      lease_t &operator=(const lease_t &) = delete;
      lease_t(lease_t &&other) noexcept: owner_(std::exchange(other.owner_, nullptr)) {}
      ~lease_t() { if (owner_) owner_->occupied_.clear(); }
      explicit operator bool() const { return owner_ != nullptr; }

    private:
      friend class source_admission_t;
      explicit lease_t(source_admission_t *owner): owner_(owner) {}
      source_admission_t *owner_;
    };

    lease_t try_acquire() { return lease_t {occupied_.test_and_set() ? nullptr : this}; }

  private:
    std::atomic_flag occupied_ = ATOMIC_FLAG_INIT;
  };

  class source_timing_t {
  public:
    using clock = std::chrono::steady_clock;
    using time_point = clock::time_point;
    enum class cadence_e { ready, late, stalled };
    enum class input_e { ready, timeout, failed };
    enum class progress_e { emit, retry, stalled, stopped };

    explicit source_timing_t(time_point now): deadline_(now), last_input_(now) {}
    time_point next_deadline() { return deadline_ += std::chrono::milliseconds {5}; }

    cadence_e synthetic_cadence(time_point now) {
      if (now - deadline_ > std::chrono::milliseconds {100}) return cadence_e::stalled;
      if (now - deadline_ > std::chrono::milliseconds {5}) {
        deadline_ = now;
        return cadence_e::late;
      }
      return cadence_e::ready;
    }

    progress_e native_progress(input_e status, time_point now) const {
      if (status == input_e::timeout) {
        return now - last_input_ <= std::chrono::seconds {2} ? progress_e::retry : progress_e::stalled;
      }
      return status == input_e::ready ? progress_e::emit : progress_e::stopped;
    }
    void input_received(time_point now) { last_input_ = now; }

    // Before the first real block this authenticated USB input is idle. A
    // started epoch must continue producing blocks or terminate explicitly.
    progress_e usb_timeout(bool received_in_epoch, time_point now) const {
      return received_in_epoch ? native_progress(input_e::timeout, now) : progress_e::retry;
    }

  private:
    time_point deadline_;
    time_point last_input_;
  };
}  // namespace audio

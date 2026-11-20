/** Per-session sender completion and lifetime gate for discrete test sources. */
#pragma once

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>

namespace audio {
  enum class drain_result_e { complete, stopped, timed_out };

  class delivery_state_t {
  public:
    // A queued packet owns this state independently of the raw session pointer.
    // The sender must acquire the lease before dereferencing that pointer.
    // close() joins any active lease and prevents all subsequent dereferences.
    std::unique_lock<std::mutex> acquire_send() {
      std::unique_lock lock(send_mutex_);
      if (closed_) lock.unlock();
      return lock;
    }

    void close() {
      std::lock_guard lock(send_mutex_);
      closed_ = true;
    }

    // Call only after the synchronous packetizer push completed, including all
    // parity emits. Dequeuing alone is deliberately not an acknowledgment.
    void acknowledge() {
      completed_.fetch_add(1, std::memory_order_release);
      changed_.notify_all();
    }

    std::uint64_t completed() const { return completed_.load(std::memory_order_acquire); }

    void observe_queue_age(std::chrono::steady_clock::duration age) {
      const auto ns = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(age).count());
      queue_age_count_.fetch_add(1, std::memory_order_relaxed);
      queue_age_sum_ns_.fetch_add(ns, std::memory_order_relaxed);
      auto maximum = queue_age_max_ns_.load(std::memory_order_relaxed);
      while (maximum < ns && !queue_age_max_ns_.compare_exchange_weak(maximum, ns, std::memory_order_relaxed)) {}
    }
    std::uint64_t queue_age_count() const { return queue_age_count_.load(std::memory_order_relaxed); }
    double queue_age_max_ms() const { return queue_age_max_ns_.load(std::memory_order_relaxed) / 1000000.0; }
    double queue_age_mean_ms() const {
      const auto count = queue_age_count();
      return count ? queue_age_sum_ns_.load(std::memory_order_relaxed) / (1000000.0 * count) : 0;
    }

    template<class Stopped>
    drain_result_e wait_for(std::uint64_t expected, std::chrono::milliseconds timeout, Stopped stopped) {
      using clock = std::chrono::steady_clock;
      const auto deadline = clock::now() + timeout;
      std::unique_lock lock(wait_mutex_);
      while (completed() < expected) {
        if (stopped()) return drain_result_e::stopped;
        const auto now = clock::now();
        if (now >= deadline) return drain_result_e::timed_out;
        // The external session shutdown event does not own this condition
        // variable, so bound cancellation observation to at most five ms.
        changed_.wait_until(lock, std::min(deadline, now + std::chrono::milliseconds {5}),
          [&] { return completed() >= expected; });
      }
      return drain_result_e::complete;
    }

  private:
    std::mutex send_mutex_;
    bool closed_ = false;
    std::atomic<std::uint64_t> completed_ {0};
    std::atomic<std::uint64_t> queue_age_count_ {0}, queue_age_sum_ns_ {0}, queue_age_max_ns_ {0};
    std::mutex wait_mutex_;
    std::condition_variable changed_;
  };
}  // namespace audio

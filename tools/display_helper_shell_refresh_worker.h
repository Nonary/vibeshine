#pragma once

#include <condition_variable>
#include <functional>
#include <mutex>
#include <stop_token>
#include <thread>
#include <utility>

namespace display_helper {
  // Serializes cosmetic shell notifications without holding up display admission.
  // New requests coalesce while a notification is in flight. Cancellation drops
  // queued work; an in-flight Windows broadcast cannot itself be interrupted.
  class shell_refresh_worker {
  public:
    explicit shell_refresh_worker(std::function<void(std::stop_token)> refresh):
        refresh_(std::move(refresh)), thread_([this](std::stop_token stop) { run(stop); }) {}

    void request() {
      {
        std::lock_guard lock(mutex_);
        pending_ = true;
      }
      cv_.notify_one();
    }

    void cancel_pending() {
      std::lock_guard lock(mutex_);
      pending_ = false;
    }

  private:
    void run(std::stop_token stop) {
      std::unique_lock lock(mutex_);
      while (cv_.wait(lock, stop, [this] { return pending_; })) {
        if (stop.stop_requested()) break;
        pending_ = false;
        lock.unlock();
        refresh_(stop);
        lock.lock();
      }
    }

    std::function<void(std::stop_token)> refresh_;
    std::mutex mutex_;
    std::condition_variable_any cv_;
    bool pending_ {false};
    // Destroyed first: jthread requests stop and joins before state goes away.
    std::jthread thread_;
  };
}  // namespace display_helper

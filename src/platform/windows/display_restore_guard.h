#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <utility>

namespace display_helper {
  /**
   * @brief Generation-safe host view of the helper restore currently pending.
   */
  class PendingRestoreTracker {
  public:
    std::uint64_t begin_restore() {
      auto generation = next_generation_.fetch_add(1, std::memory_order_acq_rel) + 1;
      if (generation == 0) {
        generation = next_generation_.fetch_add(1, std::memory_order_acq_rel) + 1;
      }
      auto observed = pending_generation_.load(std::memory_order_acquire);
      while (observed < generation &&
             !pending_generation_.compare_exchange_weak(
               observed,
               generation,
               std::memory_order_acq_rel,
               std::memory_order_acquire
             )) {
      }
      return generation;
    }

    std::uint64_t discover_restore() {
      auto existing = pending_generation_.load(std::memory_order_acquire);
      if (existing != 0) {
        return existing;
      }

      auto generation = next_generation_.fetch_add(1, std::memory_order_acq_rel) + 1;
      if (generation == 0) {
        generation = next_generation_.fetch_add(1, std::memory_order_acq_rel) + 1;
      }
      std::uint64_t expected_none = 0;
      if (pending_generation_.compare_exchange_strong(
            expected_none,
            generation,
            std::memory_order_acq_rel,
            std::memory_order_acquire
          )) {
        return generation;
      }
      return expected_none;
    }

    std::uint64_t current() const {
      return pending_generation_.load(std::memory_order_acquire);
    }

    bool clear_if(std::uint64_t generation) {
      if (generation == 0) {
        return false;
      }
      return pending_generation_.compare_exchange_strong(
        generation,
        0,
        std::memory_order_acq_rel,
        std::memory_order_acquire
      );
    }

    void clear() {
      pending_generation_.store(0, std::memory_order_release);
    }

  private:
    std::atomic<std::uint64_t> next_generation_ {0};
    std::atomic<std::uint64_t> pending_generation_ {0};
  };

  /**
   * @brief Linearizes restore cancellation against entry into a blocking display
   *        mutation such as SetDisplayConfig.
   *
   * Windows display mutations cannot be interrupted once entered. Callers must
   * therefore make the cancel-vs-mutate decision while holding the same lock.
   * A failed/unvalidated mutation leaves topology_unconfirmed latched until a
   * later Apply or restore is validated. Baseline capture remains separately
   * blocked until a strict restore validation proves the original topology.
   */
  class RestoreMutationGuard {
  public:
    class CaptureLease {
    public:
      CaptureLease(CaptureLease &&) noexcept = default;
      CaptureLease &operator=(CaptureLease &&) noexcept = default;
      CaptureLease(const CaptureLease &) = delete;
      CaptureLease &operator=(const CaptureLease &) = delete;

    private:
      friend class RestoreMutationGuard;

      explicit CaptureLease(std::unique_lock<std::mutex> lock):
          lock_(std::move(lock)) {}

      std::unique_lock<std::mutex> lock_;
    };

    class WorkerLease {
    public:
      WorkerLease() = default;
      WorkerLease(const WorkerLease &) = delete;
      WorkerLease &operator=(const WorkerLease &) = delete;

      WorkerLease(WorkerLease &&other) noexcept
          :
          owner_(std::exchange(other.owner_, nullptr)) {}

      WorkerLease &operator=(WorkerLease &&other) noexcept {
        if (this != &other) {
          release();
          owner_ = std::exchange(other.owner_, nullptr);
        }
        return *this;
      }

      ~WorkerLease() {
        release();
      }

      template<typename Cancelled>
      bool try_begin_mutation(Cancelled &&cancelled) {
        if (!owner_) {
          return false;
        }
        return owner_->try_begin_mutation_locked(std::forward<Cancelled>(cancelled));
      }

    private:
      friend class RestoreMutationGuard;

      explicit WorkerLease(RestoreMutationGuard &owner):
          owner_(&owner) {}

      void release() {
        if (owner_) {
          owner_->worker_finished();
          owner_ = nullptr;
        }
      }

      RestoreMutationGuard *owner_ = nullptr;
    };

    WorkerLease begin_worker(bool initially_fenced = false) {
      std::lock_guard<std::mutex> lock(mutex_);
      if (active_workers_ == 0) {
        cancellation_fenced_ = initially_fenced;
      }
      ++active_workers_;
      return WorkerLease(*this);
    }

    template<typename Cancelled>
    bool try_begin_mutation_for_active_worker(Cancelled &&cancelled) {
      return try_begin_mutation_locked(std::forward<Cancelled>(cancelled));
    }

    /**
     * @return true when cancellation was accepted before any unconfirmed
     *         topology mutation; false means the restore must be allowed to run.
     */
    template<typename Cancel>
    bool try_disarm(Cancel &&cancel) {
      std::lock_guard<std::mutex> lock(mutex_);
      if (topology_unconfirmed_) {
        return false;
      }
      cancellation_fenced_ = true;
      std::invoke(std::forward<Cancel>(cancel));
      return true;
    }

    /**
     * Force-cancel for APPLY and report whether fallback baseline capture is unsafe.
     */
    template<typename Cancel>
    bool supersede_for_apply(Cancel &&cancel) {
      std::lock_guard<std::mutex> lock(mutex_);
      const bool unsafe = baseline_capture_blocked_;
      cancellation_fenced_ = true;
      std::invoke(std::forward<Cancel>(cancel));
      return unsafe;
    }

    bool capture_allowed() const {
      std::lock_guard<std::mutex> lock(mutex_);
      return !baseline_capture_blocked_ && (active_workers_ == 0 || cancellation_fenced_);
    }

    /**
     * Hold this lease through capture and persistence. A restore worker cannot
     * cross its mutation gate until the baseline transaction is complete.
     */
    std::optional<CaptureLease> try_begin_capture() {
      std::unique_lock<std::mutex> lock(mutex_);
      if (baseline_capture_blocked_ || (active_workers_ != 0 && !cancellation_fenced_)) {
        return std::nullopt;
      }
      return CaptureLease(std::move(lock));
    }

    bool topology_unconfirmed() const {
      std::lock_guard<std::mutex> lock(mutex_);
      return topology_unconfirmed_;
    }

    void mark_topology_confirmed() {
      std::lock_guard<std::mutex> lock(mutex_);
      // A restore worker that was superseded after its final cancellation check
      // must not publish a late baseline-safe confirmation.
      if (cancellation_fenced_) {
        return;
      }
      topology_unconfirmed_ = false;
      baseline_capture_blocked_ = false;
    }

    /**
     * A superseding session APPLY can prove the live topology safe for further
     * mutation, but it must not authorize a new restore baseline captured from
     * the session layout. Only strict restore validation clears that block.
     */
    void mark_superseding_apply_confirmed() {
      std::lock_guard<std::mutex> lock(mutex_);
      topology_unconfirmed_ = false;
    }

    // Once a session Apply mutates topology, the pre-session baseline must not
    // be replaced by that session/partial layout. A strict restore is the only
    // operation that clears this capture block.
    void mark_apply_started() {
      std::lock_guard<std::mutex> lock(mutex_);
      baseline_capture_blocked_ = true;
    }

  private:
    template<typename Cancelled>
    bool try_begin_mutation_locked(Cancelled &&cancelled) {
      std::lock_guard<std::mutex> lock(mutex_);
      if (active_workers_ == 0 || cancellation_fenced_ || std::invoke(std::forward<Cancelled>(cancelled))) {
        return false;
      }
      topology_unconfirmed_ = true;
      baseline_capture_blocked_ = true;
      return true;
    }

    void worker_finished() {
      std::lock_guard<std::mutex> lock(mutex_);
      if (active_workers_ > 0) {
        --active_workers_;
      }
      if (active_workers_ == 0) {
        cancellation_fenced_ = false;
      }
    }

    mutable std::mutex mutex_;
    std::size_t active_workers_ = 0;
    bool cancellation_fenced_ = false;
    bool topology_unconfirmed_ = false;
    bool baseline_capture_blocked_ = false;
  };
}  // namespace display_helper

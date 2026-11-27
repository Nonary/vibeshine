#pragma once
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>

namespace audio {
  // Single producer/single consumer; one startup allocation may contain this
  // object. No allocation, lock, wait, overwrite or silent drop in these calls.
  template<class T, std::size_t Capacity> class bounded_trace_t {
    static_assert(Capacity > 1);
    static_assert(std::atomic<std::uint64_t>::is_always_lock_free);
    std::array<T, Capacity> slots_ {};
    alignas(64) std::atomic<std::uint64_t> head_ {0};
    alignas(64) std::atomic<std::uint64_t> tail_ {0};
  public:
    static constexpr std::size_t capacity = Capacity;
    T *reserve() {
      const auto tail=tail_.load(std::memory_order_relaxed);
      return tail-head_.load(std::memory_order_acquire)<Capacity ? &slots_[tail%Capacity] : nullptr;
    }
    std::uint64_t publish() {return tail_.fetch_add(1,std::memory_order_release)+1;}
    const T *front() const {
      const auto head=head_.load(std::memory_order_relaxed);
      return head<tail_.load(std::memory_order_acquire) ? &slots_[head%Capacity] : nullptr;
    }
    void pop() {head_.fetch_add(1,std::memory_order_release);}
    std::uint64_t completed() const {return head_.load(std::memory_order_acquire);}
    std::uint64_t published() const {return tail_.load(std::memory_order_acquire);}
    std::size_t producer_depth() const {
      return static_cast<std::size_t>(tail_.load(std::memory_order_relaxed)-head_.load(std::memory_order_acquire));
    }
  };
}

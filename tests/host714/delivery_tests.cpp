#include "src/audio_delivery.h"
#include "src/thread_safe.h"

#include <future>
#include <iostream>
#include <latch>
#include <memory>
#include <stdexcept>
#include <thread>

namespace {
  unsigned assertions = 0;
  void check(bool result, const char *description) {
    ++assertions;
    if (!result) throw std::runtime_error(description);
  }
  struct owner_t { unsigned sends = 0; };
  struct queued_t {
    owner_t *owner;
    std::shared_ptr<audio::delivery_state_t> delivery;
  };
}

int main() {
  using namespace std::chrono_literals;
  using audio::drain_result_e;
  try {
    auto delivery = std::make_shared<audio::delivery_state_t>();
    auto owner = std::make_unique<owner_t>();
    safe::queue_t<queued_t> queue(4);
    queue.raise(queued_t {owner.get(), delivery});

    // The sender already removed the final packet, but has not yet completed
    // its data/parity callback. The old empty-queue EOF test failed here.
    std::latch in_flight(1), permit_send(1);
    std::atomic_bool parity_finished {false};
    std::thread sender([&] {
      auto packet = queue.pop();
      auto lease = packet->delivery->acquire_send();
      in_flight.count_down();
      permit_send.wait();
      ++packet->owner->sends;
      parity_finished.store(true);
      packet->delivery->acknowledge();
    });
    in_flight.wait();
    const auto empty_but_unsent = queue.occupancy().first == 0 && delivery->completed() == 0;
    const auto before_send = delivery->wait_for(1, 0ms, [] { return false; });
    const auto cancelled = delivery->wait_for(1, 200ms, [] { return true; });

    std::promise<void> closing_entered;
    auto closing_started = closing_entered.get_future();
    auto closing = std::async(std::launch::async, [&] {
      closing_entered.set_value();
      delivery->close();
    });
    closing_started.wait();
    const bool close_waited_for_sender = closing.wait_for(10ms) == std::future_status::timeout;
    permit_send.count_down();
    sender.join();
    closing.get();
    check(empty_but_unsent, "fixture did not reproduce queue-empty/in-flight boundary");
    check(before_send == drain_result_e::timed_out, "EOF mistook dequeued packet for sent packet");
    check(cancelled == drain_result_e::stopped, "drain ignored session stop");
    check(close_waited_for_sender, "close released owner while sender still held its lease");
    check(parity_finished.load() && owner->sends == 1 && delivery->completed() == 1,
      "send acknowledgment did not follow final parity completion");
    check(delivery->wait_for(1, 0ms, [] { return false; }) == drain_result_e::complete,
      "EOF did not accept completed send");
    check(delivery->wait_for(2, 0ms, [] { return false; }) == drain_result_e::timed_out,
      "acknowledgment counted unsent data");
    auto *stale_owner = owner.get();
    owner.reset();

    // A packet popped before cleanup can outlive the source/session. Its shared
    // state must block the raw owner access, even with an empty global queue.
    queued_t stale {stale_owner, delivery};
    bool touched_stale_owner = false;
    {
      auto lease = stale.delivery->acquire_send();
      if (lease.owns_lock()) {
        touched_stale_owner = true;
        ++stale.owner->sends;
      }
    }
    check(!touched_stale_owner, "closed packet dereferenced a retired owner");

    // Stop/discard one producer while another keeps the shared queue alive.
    auto other_delivery = std::make_shared<audio::delivery_state_t>();
    owner_t other_owner;
    queue.raise(queued_t {stale_owner, delivery});
    queue.raise(queued_t {&other_owner, other_delivery});
    queue.raise(queued_t {stale_owner, delivery});
    queue.raise(queued_t {&other_owner, other_delivery});
    queue.discard_if([&](const queued_t &packet) { return packet.delivery == delivery; });
    check(queue.occupancy().first == 2, "stop discarded another producer's packets");
    for (unsigned packet_index = 0; packet_index < 2; ++packet_index) {
      auto packet = queue.pop();
      auto lease = packet->delivery->acquire_send();
      check(lease.owns_lock() && packet->owner == &other_owner, "peer sender was retired with stopped source");
      ++packet->owner->sends;
      packet->delivery->acknowledge();
    }
    check(other_owner.sends == 2 && other_delivery->completed() == 2, "peer did not complete its sends");
    check(other_delivery->wait_for(2, 0ms, [] { return false; }) == drain_result_e::complete,
      "peer acknowledgment did not survive source cleanup");
    check(delivery->completed() == 1, "peer send changed retired source acknowledgment");
    other_delivery->close();
    other_delivery->close();
    check(!other_delivery->acquire_send().owns_lock(), "repeated close reopened sender admission");

    // Failed send callbacks release the lease but never count as completed.
    audio::delivery_state_t failed;
    try {
      auto lease = failed.acquire_send();
      throw std::runtime_error("injected send failure");
    } catch (const std::runtime_error &) {}
    failed.close();
    check(failed.completed() == 0, "failed callback was acknowledged");
    check(!failed.acquire_send().owns_lock(), "failure cleanup retained a dangling sender lease");
    check(failed.wait_for(0, 0ms, [] { return false; }) == drain_result_e::complete,
      "empty source cannot finish");

    std::cout << "PASS delivery: " << assertions
              << " assertions; real sender gate, acknowledgment wait, queue discard; no devices/sockets\n";
    return 0;
  } catch (const std::exception &error) {
    std::cerr << "FAIL delivery: " << error.what() << '\n';
    return 1;
  }
}

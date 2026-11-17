#include "src/audio_source_lifecycle.h"
#include "src/audio_display_transition.h"

#include <atomic>
#include <barrier>
#include <iostream>
#include <stdexcept>
#include <thread>

namespace {
  unsigned assertions = 0;
  void check(bool value, const char *message) {
    ++assertions;
    if (!value) throw std::runtime_error(message);
  }
}

int main() {
  using namespace std::chrono_literals;
  using timing_t = audio::source_timing_t;
  using enum timing_t::cadence_e;
  using input = timing_t::input_e;
  using progress = timing_t::progress_e;
  try {
    {
      auto snapshot=std::make_shared<int>(1);std::weak_ptr<int> snapshot_lifetime=snapshot;
      auto prepared=std::make_shared<int>(2);std::weak_ptr<int> source_lifetime=prepared;
      std::shared_ptr<void> hold=snapshot;unsigned calls=0;
      check(audio::retain_required_source([&]{++calls;return prepared;},hold),"effective source preparation failed");
      check(calls==1,"existing snapshot suppressed required source preparation");
      snapshot.reset();prepared.reset();
      check(!snapshot_lifetime.expired()&&!source_lifetime.expired(),"combined source/snapshot lifetime lost");
      const auto saved=hold;
      check(!audio::retain_required_source([&]()->std::shared_ptr<void>{++calls;return {};},hold),"failed required source admitted");
      check(calls==2&&hold==saved,"source failure consumed the existing snapshot");
      auto next=std::make_shared<int>(3);std::shared_ptr<void> next_hold=hold;
      check(audio::retain_required_source([&]{++calls;return next;},next_hold)&&calls==3,"reconnect skipped effective profile preparation");
    }
    audio::source_admission_t admission;
    {
      auto first = admission.try_acquire();
      check(static_cast<bool>(first), "initial source owner refused");
      { auto second = admission.try_acquire(); check(!second, "second simultaneous source owner admitted"); }
      auto third = admission.try_acquire();
      check(!third, "failed contender released the active owner's admission");
      auto moved = std::move(first);
      check(!first && static_cast<bool>(moved), "source lease ownership not transferred exactly once");
    }
    {
      auto restarted = admission.try_acquire();
      check(static_cast<bool>(restarted), "normal source end prevents reconnect");
    }
    bool acquired_before_failure = false;
    try {
      auto failed = admission.try_acquire();
      acquired_before_failure = static_cast<bool>(failed);
      throw std::runtime_error("injected source construction/format failure");
    } catch (const std::runtime_error &) {}
    check(acquired_before_failure, "failure fixture cannot acquire source");
    {
      auto restarted = admission.try_acquire();
      check(static_cast<bool>(restarted), "source startup failure leaked admission");
    }

    // Both real threads race for the production gate and retain their leases
    // until both attempts are complete. Exactly one producer can own capture.
    std::barrier start(3), acquired(3), release(3);
    std::atomic<unsigned> winners {0};
    const auto contend = [&] {
      start.arrive_and_wait();
      auto lease = admission.try_acquire();
      if (lease) ++winners;
      acquired.arrive_and_wait();
      release.arrive_and_wait();
    };
    std::thread one(contend), two(contend);
    start.arrive_and_wait();
    acquired.arrive_and_wait();
    const auto concurrent_winners = winners.load();
    release.arrive_and_wait();
    one.join(); two.join();
    check(concurrent_winners == 1, "concurrent source admission allowed zero or two owners");
    { auto after_race = admission.try_acquire(); check(static_cast<bool>(after_race), "concurrent source exit leaked admission"); }

    const timing_t::time_point origin {};
    timing_t usb(origin);
    check(usb.usb_timeout(false,origin+10min)==progress::retry,"Initial quiet USB endpoint was stopped");
    usb.input_received(origin+10min);
    check(usb.usb_timeout(true,origin+10min+2s)==progress::retry,"Existing USB two-second boundary changed");
    check(usb.usb_timeout(true,origin+10min+2100ms)==progress::stalled,"A started USB producer may not stall indefinitely");
    check(usb.usb_timeout(false,origin+12min)==progress::retry,"Fresh authenticated USB epoch did not return to idle");
    timing_t cadence(origin);
    check(cadence.next_deadline() == origin + 5ms, "first synthetic block not scheduled for 5ms");
    check(cadence.synthetic_cadence(origin + 5ms) == ready, "on-time block marked late");
    check(cadence.next_deadline() == origin + 10ms, "synthetic deadline not monotonic 5ms");
    check(cadence.synthetic_cadence(origin + 15ms) == ready, "existing exact 5ms lateness boundary changed");
    check(cadence.next_deadline() == origin + 15ms, "on-time schedule drifted");
    check(cadence.synthetic_cadence(origin + 21ms) == late, "missed cadence not reported");
    check(cadence.next_deadline() == origin + 26ms, "missed cadence triggered an immediate catch-up burst");
    check(cadence.synthetic_cadence(origin + 126ms) == late, "exact 100ms stall boundary changed");
    check(cadence.next_deadline() == origin + 131ms, "late schedule did not restart from current time");
    check(cadence.synthetic_cadence(origin + 232ms) == stalled, "synthetic source stall did not terminate");

    timing_t endpoint(origin);
    check(endpoint.native_progress(input::timeout, origin + 1s) == progress::retry, "short endpoint silence stopped source");
    check(endpoint.native_progress(input::timeout, origin + 2s) == progress::retry, "exact native timeout boundary changed");
    check(endpoint.native_progress(input::timeout, origin + 2001ms) == progress::stalled, "native source stall not detected");
    check(endpoint.native_progress(input::ready, origin + 1500ms) == progress::emit, "valid endpoint input not delivered");
    endpoint.input_received(origin + 1500ms);
    check(endpoint.native_progress(input::timeout, origin + 3s) == progress::retry, "valid input did not reset stall deadline");
    check(endpoint.native_progress(input::timeout, origin + 3501ms) == progress::stalled, "stall after resumed input not detected");
    check(endpoint.native_progress(input::failed, origin + 1501ms) == progress::stopped,
      "device invalidation/error/interruption did not stop without fallback");
    timing_t reconnected(origin + 10s);
    check(reconnected.native_progress(input::timeout, origin + 10s) == progress::retry, "reconnect retained previous stall state");
    check(reconnected.next_deadline() == origin + 10005ms, "reconnect retained previous synthetic deadline");

    std::cout << "PASS source lifecycle: " << assertions
              << " assertions; production admission, concurrent owners, monotonic schedule, timeout/error decisions; no devices/sockets\n";
    return 0;
  } catch (const std::exception &error) {
    std::cerr << "FAIL source lifecycle: " << error.what() << '\n';
    return 1;
  }
}

#pragma once
#include "thread_safe.h"
#include <thread>

namespace audio {
  struct packet_queue_policy_t {
    std::uint32_t capacity;
    bool clear_on_overflow;
  };
  constexpr packet_queue_policy_t packet_queue_policy(bool usb) {
    return usb?packet_queue_policy_t{4,false}:packet_queue_policy_t{32,true};
  }
  template<class Packet> auto acquire_packet_queue(const safe::mail_t& mail,std::string_view id,packet_queue_policy_t policy) {
    return mail->queue<Packet>(id,policy.capacity,policy.clear_on_overflow);
  }
  // Materialize a strong reference before launching. The mail registry itself
  // owns only a weak reference; a delayed scheduler must receive this generation.
  template<class Packet,class Worker> std::thread start_packet_worker(const safe::mail_t& mail,std::string_view id,
                                                                      packet_queue_policy_t policy,Worker worker) {
    return std::thread(std::move(worker),acquire_packet_queue<Packet>(mail,id,policy));
  }
}

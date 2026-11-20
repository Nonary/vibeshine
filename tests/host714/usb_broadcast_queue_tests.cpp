#include "src/audio_packet_queue.h"
#include <chrono>
#include <future>
#include <iostream>
#include <stdexcept>

static void verify(bool usb){
  const auto policy=audio::packet_queue_policy(usb);
  auto mail=std::make_shared<safe::mail_raw_t>();
  auto owner=audio::acquire_packet_queue<unsigned>(mail,"audio",policy);
  owner->reset();owner->preallocate();
  const auto original=owner.get();
  std::weak_ptr<typename decltype(owner)::element_type> weak=owner;
  std::promise<void> release;
  auto ready=release.get_future();
  unsigned capacity=0,size=0,head=0;bool same=false;
  auto worker=audio::start_packet_worker<unsigned>(mail,"audio",policy,[&](auto queue){
    ready.wait();
    auto producer=audio::acquire_packet_queue<unsigned>(mail,"audio",policy);
    same=queue==producer && queue.get()==original;
    capacity=queue->occupancy().second;
    for(unsigned i=0;i<capacity;++i)producer->raise(i);
    producer->raise(100);
    size=queue->occupancy().first;head=queue->pop(std::chrono::milliseconds(10)).value_or(999);
  });
  owner.reset();const bool retained=!weak.expired();
  release.set_value();worker.join();
  if(!same||!retained||capacity!=(usb?4u:32u)||size!=(usb?4u:1u)||head!=(usb?0u:100u))
    throw std::runtime_error("Broadcast worker lost startup queue identity/capacity/overflow policy");
}
int main(){
  try{verify(true);verify(false);std::cout<<"Delayed audio broadcast worker PASS, exact USB 4/legacy 32 and overflow behavior\n";return 0;}
  catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}

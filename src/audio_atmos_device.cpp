#include "audio_atmos_device.h"
#include <stdexcept>
#include <utility>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <exception>
namespace audio {
  namespace {
    // Format/role lifecycle work only, never called from the audio block loop.
    // The backend is created, used and destroyed on one worker so COM and the
    // named ownership mutex cannot migrate when a display-held context ends.
    class affine_backend final:public atmos_device_backend_t {
      std::mutex caller_,mutex_;
      std::condition_variable changed_;
      std::thread worker_;
      std::unique_ptr<atmos_device_backend_t> inner_;
      std::function<void()> command_;
      std::exception_ptr error_;
      bool ready_=false,pending_=false,done_=false,stopping_=false;
      void invoke(std::function<void()> operation) {
        std::lock_guard serial(caller_);std::unique_lock lock(mutex_);
        command_=std::move(operation);error_=nullptr;done_=false;pending_=true;changed_.notify_all();
        changed_.wait(lock,[this]{return done_;});
        if(error_)std::rethrow_exception(error_);
      }
    public:
      explicit affine_backend(std::function<std::unique_ptr<atmos_device_backend_t>()> factory) {
        worker_=std::thread([this,factory=std::move(factory)] {
          std::unique_lock lock(mutex_);lock.unlock();
          try{inner_=factory();}catch(...){lock.lock();error_=std::current_exception();ready_=true;changed_.notify_all();return;}
          lock.lock();ready_=true;changed_.notify_all();
          for(;;){
            changed_.wait(lock,[this]{return stopping_||pending_;});if(stopping_)break;
            auto operation=std::move(command_);pending_=false;lock.unlock();
            std::exception_ptr error;try{operation();}catch(...){error=std::current_exception();}
            lock.lock();error_=error;done_=true;changed_.notify_all();
          }
          lock.unlock();inner_.reset();
        });
        std::unique_lock lock(mutex_);changed_.wait(lock,[this]{return ready_;});
        if(error_){auto error=error_;lock.unlock();worker_.join();std::rethrow_exception(error);}
      }
      ~affine_backend()override {
        {std::lock_guard lock(mutex_);stopping_=true;changed_.notify_all();}
        if(worker_.joinable())worker_.join();
      }
      void verify()override{invoke([this]{inner_->verify();});}
      atmos_device_state_t read()override{atmos_device_state_t result;invoke([&]{result=inner_->read();});return result;}
      std::vector<unsigned char> read_public_mix()override{std::vector<unsigned char> result;invoke([&]{result=inner_->read_public_mix();});return result;}
      atmos_format_settle_t settle(const atmos_device_state_t &target,const atmos_device_state_t &initial,const std::vector<unsigned char> &mix)override {
        atmos_format_settle_t result;invoke([&]{result=inner_->settle(target,initial,mix);});return result;
      }
      atmos_setter_diagnostic_t last_setter_diagnostic()override{atmos_setter_diagnostic_t result;invoke([&]{result=inner_->last_setter_diagnostic();});return result;}
      atmos_device_state_t target(const atmos_format_t &format)override{atmos_device_state_t result;invoke([&]{result=inner_->target(format);});return result;}
      void set(const atmos_device_state_t &value)override{invoke([&]{inner_->set(value);});}
    };
  }
  std::unique_ptr<atmos_device_backend_t> make_affine_atmos_backend(std::function<std::unique_ptr<atmos_device_backend_t>()> factory) {
    return std::make_unique<affine_backend>(std::move(factory));
  }
  atmos_device_lease_t::atmos_device_lease_t(std::unique_ptr<atmos_device_backend_t> backend):backend_(std::move(backend)) {}
  atmos_device_lease_t::~atmos_device_lease_t() { restore(); }
  void atmos_device_lease_t::set_recorded(const atmos_device_state_t &value,atmos_setter_diagnostic_t &diagnostic) {
    try { backend_->set(value); }
    catch(...) { diagnostic=backend_->last_setter_diagnostic();throw; }
    diagnostic=backend_->last_setter_diagnostic();
  }
  bool atmos_device_lease_t::inspect() {
    try { backend_->verify(); report_.before=backend_->read(); report_.after=report_.before; return true; }
    catch(const std::exception &e) { report_.error=e.what(); return false; }
  }
  bool atmos_device_lease_t::apply(const atmos_format_t &format) {
    if(report_.active || report_.applied) { report_.error="Atmos format lease cannot be reused";return false; }
    if(!format.valid()) { report_.error="Unsupported Atmos format; no fallback";return false; }
    try {
      backend_->verify(); report_.before=backend_->read(); report_.installed=backend_->target(format);
      // Own the intended value before the mutation: even a failed setter may
      // have applied it. Rollback only if a readback proves our exact value.
      report_.active=true;
      set_recorded(report_.installed,report_.apply_setter); report_.applied=true;
      backend_->verify(); report_.apply_readback=backend_->read(); report_.apply_readback_valid=true;
      report_.after=report_.apply_readback;
      try { report_.apply_public_mix_readback=backend_->read_public_mix(); }
      catch(const std::exception &e) { report_.apply_public_mix_readback_error=e.what(); }
      report_.apply_settle=backend_->settle(report_.installed,report_.apply_readback,report_.apply_public_mix_readback);
      if(report_.apply_settle.qpc_frequency)report_.after=report_.apply_settle.final_readback;
      if(report_.after!=report_.installed ||
         (report_.apply_settle.qpc_frequency&&!report_.apply_settle.target_matched))
        throw std::runtime_error("Atmos format readback differs from requested format");
      return true;
    } catch(const std::exception &e) { report_.error=e.what(); restore(); return false; }
  }
  bool atmos_device_lease_t::restore() noexcept {
    if(!report_.active) return report_.restore!=atmos_restore_e::failed;
    report_.active=false;
    try {
      backend_->verify(); const auto current=backend_->read(); report_.after=current;
      if(current==report_.before) {
        // Policy can return the original before the independently activated
        // public client observes it. A no-write restore needs the same proof.
        report_.restore_settle=backend_->settle(report_.before,current,{});
        if(report_.restore_settle.qpc_frequency){
          report_.after=report_.restore_settle.final_readback;
          if(report_.after!=report_.before||!report_.restore_settle.target_matched)
            throw std::runtime_error("Atmos original format restore readback failed");
        }
        report_.restore=atmos_restore_e::restored; return true;
      }
      const bool owned_partial=(current.device_format==report_.before.device_format || current.device_format==report_.installed.device_format) &&
        (current.mix_format==report_.before.mix_format || current.mix_format==report_.installed.mix_format);
      if(current!=report_.installed && !owned_partial) { report_.restore=atmos_restore_e::conflict; return true; }
      set_recorded(report_.before,report_.restore_setter); backend_->verify(); report_.after=backend_->read();
      report_.restore_settle=backend_->settle(report_.before,report_.after,{});
      if(report_.restore_settle.qpc_frequency)report_.after=report_.restore_settle.final_readback;
      if(report_.after!=report_.before ||
         (report_.restore_settle.qpc_frequency&&!report_.restore_settle.target_matched))
        throw std::runtime_error("Atmos original format restore readback failed");
      report_.restore=atmos_restore_e::restored; return true;
    } catch(const std::exception &e) { report_.restore=atmos_restore_e::failed;report_.error+=(report_.error.empty()?"":"; ")+std::string(e.what());return false; }
  }
#ifndef _WIN32
  std::unique_ptr<atmos_device_backend_t> make_owned_usb_format_backend(const usb_source_config_t &) {
    throw std::runtime_error("Owned Vibe Speakers format lease requires Windows");
  }
#endif
}

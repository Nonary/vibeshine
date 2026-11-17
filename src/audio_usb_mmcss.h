#pragma once
#include <cstdint>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <avrt.h>
#endif

namespace audio {
struct usb_mmcss_status_t {
  std::uint32_t thread_id=0,task_index=0,register_error=0,revert_error=0;
  bool registered=false,reverted=false;
};
#ifdef _WIN32
struct usb_mmcss_api_t {
  decltype(&AvSetMmThreadCharacteristicsW) set=&AvSetMmThreadCharacteristicsW;
  decltype(&AvRevertMmThreadCharacteristics) revert=&AvRevertMmThreadCharacteristics;
};
#endif
// One registration on the actual USB consume/encode thread. No process
// priority, task configuration, timer resolution or queue limits are changed.
class usb_mmcss_t {
  usb_mmcss_status_t status_;
#ifdef _WIN32
  usb_mmcss_api_t api_;
  HANDLE handle_=nullptr;
#endif
public:
#ifdef _WIN32
  explicit usb_mmcss_t(usb_mmcss_api_t api={})noexcept:api_(api){}
#else
  usb_mmcss_t()=default;
#endif
  usb_mmcss_t(const usb_mmcss_t&)=delete;
  usb_mmcss_t& operator=(const usb_mmcss_t&)=delete;
  ~usb_mmcss_t(){close();}
  const usb_mmcss_status_t& status()const noexcept{return status_;}
  bool start()noexcept {
#ifdef _WIN32
    if(status_.thread_id){status_.register_error=ERROR_INVALID_STATE;return false;}
    status_.thread_id=GetCurrentThreadId();DWORD index=0;
    handle_=api_.set(L"Audio",&index);status_.task_index=index;
    if(!handle_){status_.register_error=GetLastError();return false;}
    status_.registered=true;return true;
#else
    return false;
#endif
  }
  bool close()noexcept {
#ifdef _WIN32
    if(!handle_)return !status_.registered||status_.reverted;
    if(GetCurrentThreadId()!=status_.thread_id){status_.revert_error=ERROR_INVALID_THREAD_ID;return false;}
    const auto handle=handle_;handle_=nullptr;
    status_.reverted=api_.revert(handle)!=FALSE;
    if(!status_.reverted)status_.revert_error=GetLastError();
    return status_.reverted;
#else
    return true;
#endif
  }
};
}

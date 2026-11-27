// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <windows.h>
#include <avrt.h>

namespace vibe::usb_audio {
#ifdef VIBE_USB_EXPERIMENT_PRO_AUDIO_WORKER
inline constexpr bool usb_worker_pro_audio=true;
#else
inline constexpr bool usb_worker_pro_audio=false;
#endif
struct mmcss_status {
  DWORD thread_id=0, task_index=0, register_error=0, revert_error=0;
  bool registered=false, reverted=false;
};
struct mmcss_api {
  decltype(&AvSetMmThreadCharacteristicsW) set=&AvSetMmThreadCharacteristicsW;
  decltype(&AvRevertMmThreadCharacteristics) revert=&AvRevertMmThreadCharacteristics;
};
// One registration on the USB worker. No process priority/task/registry edits.
class mmcss_audio {
  mmcss_status &status_;
  mmcss_api api_;
  HANDLE handle_=nullptr;
public:
  explicit mmcss_audio(mmcss_status &status,mmcss_api api={}):status_(status),api_(api){}
  mmcss_audio(const mmcss_audio&)=delete;
  mmcss_audio& operator=(const mmcss_audio&)=delete;
  ~mmcss_audio(){close();}
  bool start() noexcept {
    if(status_.thread_id)return false;
    status_.thread_id=GetCurrentThreadId();
    handle_=api_.set(usb_worker_pro_audio?L"Pro Audio":L"Audio",&status_.task_index);
    status_.registered=handle_!=nullptr;
    if(!handle_)status_.register_error=GetLastError();
    return status_.registered;
  }
  bool close() noexcept {
    if(!handle_)return !status_.registered||status_.reverted;
    if(GetCurrentThreadId()!=status_.thread_id){status_.revert_error=ERROR_INVALID_THREAD_ID;return false;}
    const auto value=handle_;handle_=nullptr;
    status_.reverted=api_.revert(value)!=FALSE;
    if(!status_.reverted)status_.revert_error=GetLastError();
    return status_.reverted;
  }
};
}

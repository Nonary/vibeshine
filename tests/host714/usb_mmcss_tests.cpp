#include "src/audio_usb_mmcss.h"
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>

namespace {
unsigned sets=0,reverts=0;bool fail_set=false,fail_revert=false;
void check(bool value){if(!value)throw std::runtime_error("USB MMCSS lifecycle regression");}
HANDLE WINAPI fake_set(LPCWSTR task,LPDWORD index){
  ++sets;check(std::wstring(task)==L"Audio");*index=7;
  if(fail_set){SetLastError(ERROR_ACCESS_DENIED);return nullptr;}
  return reinterpret_cast<HANDLE>(static_cast<std::uintptr_t>(0x1234));
}
BOOL WINAPI fake_revert(HANDLE handle){
  ++reverts;check(handle==reinterpret_cast<HANDLE>(static_cast<std::uintptr_t>(0x1234)));
  if(fail_revert){SetLastError(ERROR_GEN_FAILURE);return FALSE;}return TRUE;
}
}
int main(){
  try{
    const audio::usb_mmcss_api_t api{fake_set,fake_revert};
    {audio::usb_mmcss_t registration(api);check(registration.start());check(registration.status().registered&&registration.status().thread_id==GetCurrentThreadId()&&registration.status().task_index==7);
      bool wrong_thread_result=true;std::thread other([&]{wrong_thread_result=registration.close();});other.join();
      check(!wrong_thread_result&&reverts==0&&registration.status().revert_error==ERROR_INVALID_THREAD_ID);
      check(registration.close()&&registration.close());check(reverts==1);check(!registration.start());}
    check(sets==1&&reverts==1);
    fail_set=true;{audio::usb_mmcss_t registration(api);check(!registration.start()&&registration.status().register_error==ERROR_ACCESS_DENIED);check(registration.close());}
    check(sets==2&&reverts==1);fail_set=false;
    try{audio::usb_mmcss_t registration(api);check(registration.start());throw 42;}catch(int){}
    check(sets==3&&reverts==2);
    fail_revert=true;{audio::usb_mmcss_t registration(api);check(registration.start());check(!registration.close()&&registration.status().revert_error==ERROR_GEN_FAILURE);check(!registration.close());}
    check(sets==4&&reverts==3);
    std::cout<<"USB MMCSS lifecycle PASS; fake API only, no audio or scheduling registration\n";
    return 0;
  }catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}
}

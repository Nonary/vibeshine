#include "src/vibe_profile_ids.h"
#include <optional>
#include "src/audio_usb_source.h"
#include "src/usb_audio_identity.h"
#ifndef NOMINMAX
#define NOMINMAX
#endif
#define INITGUID
#include "../../audio_atmos_device.h"
#include "../../audio_role_control.h"
#include "../audio.h"
#include <windows.h>
#include <audioclient.h>
#include <mmdeviceapi.h>
#include <mmreg.h>
#include <ks.h>
#include <ksmedia.h>
#include <cfgmgr32.h>
#include <devpkey.h>
#include "PolicyConfig.h"
#include <algorithm>
#include <cstring>
#include <cwctype>
#include <stdexcept>
#include <atomic>

namespace audio { namespace {
  template<class T> struct com_ptr {
    T *p=nullptr;
    ~com_ptr(){if(p)p->Release();}
    T *operator->()const{return p;}
  };
  void hr(HRESULT value,const char *operation) {
    if(FAILED(value))throw std::runtime_error(std::string(operation)+" HRESULT="+std::to_string(static_cast<unsigned long>(value)));
  }
  void need(bool value,const char *operation){if(!value)throw std::runtime_error(operation);}
  std::wstring wide(const std::string &text) {
    need(!text.empty()&&text.size()<32700,"Empty/oversized Atmos binding");
    int size=MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,text.data(),static_cast<int>(text.size()),nullptr,0);
    need(size>0,"Invalid UTF8 Atmos binding");std::wstring result(size,L'\0');
    MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,text.data(),static_cast<int>(text.size()),result.data(),size);return result;
  }
  std::vector<unsigned char> bytes(const WAVEFORMATEX *format) {
    need(format&&format->cbSize<=128,"Invalid Windows format snapshot");
    const auto *p=reinterpret_cast<const unsigned char*>(format);
    return {p,p+sizeof(WAVEFORMATEX)+format->cbSize};
  }
  std::uint64_t clock_qpc(){LARGE_INTEGER v{};QueryPerformanceCounter(&v);return static_cast<std::uint64_t>(v.QuadPart);}
  class format_notifications final:public IMMNotificationClient {
    struct entry {std::atomic<bool> ready{false};atmos_format_notification_t value;};
    std::atomic<ULONG> refs_{1};
    std::wstring endpoint_;
    std::array<entry,128> entries_{};
    std::atomic<std::uint64_t> count_{0};
    HRESULT notify(LPCWSTR endpoint,std::uint32_t kind,const PROPERTYKEY *key=nullptr)noexcept {
      if(!endpoint||_wcsicmp(endpoint,endpoint_.c_str()))return S_OK;
      const auto index=count_.fetch_add(1,std::memory_order_acq_rel);
      if(index<entries_.size()){
        auto &entry=entries_[index];entry.value.qpc=clock_qpc();entry.value.kind=kind;
        if(key){entry.value.property_id=key->pid;std::memcpy(entry.value.property_set.data(),&key->fmtid,16);}
        entry.ready.store(true,std::memory_order_release);
      }
      SetEvent(changed);return S_OK;
    }
  public:
    HANDLE changed=nullptr;
    explicit format_notifications(std::wstring endpoint):endpoint_(std::move(endpoint)) {
      changed=CreateEventW(nullptr,TRUE,FALSE,nullptr);need(!!changed,"Cannot create format notification event");
    }
    ~format_notifications(){CloseHandle(changed);}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid,void **out)override {
      if(!out)return E_POINTER;*out=nullptr;
      if(iid!=__uuidof(IUnknown)&&iid!=__uuidof(IMMNotificationClient))return E_NOINTERFACE;
      *out=static_cast<IMMNotificationClient*>(this);AddRef();return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef()override{return refs_.fetch_add(1,std::memory_order_relaxed)+1;}
    ULONG STDMETHODCALLTYPE Release()override{auto left=refs_.fetch_sub(1,std::memory_order_acq_rel)-1;if(!left)delete this;return left;}
    HRESULT STDMETHODCALLTYPE OnDeviceStateChanged(LPCWSTR id,DWORD)override{return notify(id,2);}
    HRESULT STDMETHODCALLTYPE OnDeviceAdded(LPCWSTR id)override{return notify(id,3);}
    HRESULT STDMETHODCALLTYPE OnDeviceRemoved(LPCWSTR id)override{return notify(id,4);}
    HRESULT STDMETHODCALLTYPE OnDefaultDeviceChanged(EDataFlow flow,ERole,LPCWSTR id)override{return flow==eRender?notify(id,5):S_OK;}
    HRESULT STDMETHODCALLTYPE OnPropertyValueChanged(LPCWSTR id,const PROPERTYKEY key)override{return notify(id,1,&key);}
    std::uint64_t count()const{return count_.load(std::memory_order_acquire);}
    void snapshot(std::uint64_t baseline,atmos_format_settle_t &report)const {
      const auto end=count();report.notification_count=end-baseline;
      for(auto i=baseline;i<end&&i<entries_.size();++i){
        if(entries_[i].ready.load(std::memory_order_acquire))report.notifications.push_back(entries_[i].value);
        else ++report.notifications_pending;
      }
      if(end>entries_.size())report.observation_limit=true;
    }
  };
  class windows_backend final:public atmos_device_backend_t {
    atmos_device_request_t request_;
    std::optional<usb_lab::Identity> owned_usb_;
    std::wstring endpoint_,instance_;
    com_ptr<IMMDeviceEnumerator> enumerator_;
    com_ptr<IPolicyConfig> policy_;
    bool com_=false, owns_=false;
    HANDLE mutex_=nullptr;
    format_notifications *notifications_=nullptr;
    bool notifications_registered_=false;
    std::uint64_t notification_baseline_=0,set_begin_=0,set_end_=0;
    atmos_setter_diagnostic_t last_setter_;
  public:
    explicit windows_backend(const atmos_device_request_t &request, std::optional<usb_lab::Identity> usb=std::nullopt):request_(request),owned_usb_(std::move(usb)),endpoint_(wide(request.endpoint)),instance_(wide(request.device_instance)) {
      auto status=CoInitializeEx(nullptr,COINIT_MULTITHREADED);
      if(status!=RPC_E_CHANGED_MODE){hr(status,"Initialize endpoint COM");com_=true;}
      try {
        hr(CoCreateInstance(__uuidof(MMDeviceEnumerator),nullptr,CLSCTX_ALL,__uuidof(IMMDeviceEnumerator),reinterpret_cast<void**>(&enumerator_.p)),"Create endpoint enumerator");
#ifdef __MINGW32__
        hr(CoCreateInstance(CLSID_CPolicyConfigClient,nullptr,CLSCTX_ALL,IID_IPolicyConfig,reinterpret_cast<void**>(&policy_.p)),"Create existing host device policy");
#else
        hr(CoCreateInstance(__uuidof(CPolicyConfigClient),nullptr,CLSCTX_ALL,__uuidof(IPolicyConfig),reinterpret_cast<void**>(&policy_.p)),"Create existing host device policy");
#endif
        std::wstring name=L"Local\\ProjectAtmosFormatLease-";
        for(auto c:endpoint_) name+=(c==L'\\'||c==L'/'?L'_':static_cast<wchar_t>(towupper(c)));
        mutex_=CreateMutexW(nullptr,FALSE,name.c_str());need(!!mutex_,"Cannot create Atmos format ownership mutex");
        const auto acquired=WaitForSingleObject(mutex_,0);
        owns_=acquired==WAIT_OBJECT_0||acquired==WAIT_ABANDONED;
        need(acquired==WAIT_OBJECT_0,"Atmos format is already owned or previous owner abandoned it; no mutation");
        verify();
        notifications_=new format_notifications(endpoint_);
        hr(enumerator_->RegisterEndpointNotificationCallback(notifications_),"Register exact endpoint format notifications");
        notifications_registered_=true;
      } catch(...) { release();throw; }
    }
    void release() noexcept {
      if(notifications_registered_)enumerator_->UnregisterEndpointNotificationCallback(notifications_);
      notifications_registered_=false;
      if(notifications_){notifications_->Release();notifications_=nullptr;}
      if(owns_&&mutex_)ReleaseMutex(mutex_);owns_=false;
      if(mutex_)CloseHandle(mutex_);mutex_=nullptr;
      if(policy_.p){policy_.p->Release();policy_.p=nullptr;}
      if(enumerator_.p){enumerator_.p->Release();enumerator_.p=nullptr;}
      if(com_)CoUninitialize();com_=false;
    }
    ~windows_backend()override{release();}
    void verify()override {
      need(owned_usb_.has_value(),"An exact owned USB identity is required");
      const auto resolved=usb_lab::Resolve(endpoint_,*owned_usb_);
      need(usb_lab::Same(resolved.usbInstance,instance_),"Owned USB format endpoint changed");
    }
    atmos_device_state_t read()override {
      WAVEFORMATEX *device=nullptr,*mix=nullptr;
      struct cleanup {WAVEFORMATEX *&a,*&b;~cleanup(){CoTaskMemFree(a);CoTaskMemFree(b);}} guard{device,mix};
      hr(policy_->GetDeviceFormat(endpoint_.c_str(),0,&device),"Snapshot device format");
      const auto mix_status=policy_->GetMixFormat(endpoint_.c_str(),&mix);
      // On our ephemeral USB endpoint the old per-port mix may not be usable
      // by the newly attached descriptor. Keep that absence explicit so the
      // owner can install the requested, separately supported format.
      if(owned_usb_&&mix_status==HRESULT_FROM_WIN32(ERROR_NOT_FOUND))return {bytes(device),{}};
      hr(mix_status,"Snapshot mix format");
      return {bytes(device),bytes(mix)};
    }
    std::vector<unsigned char> read_public_mix()override {
      com_ptr<IMMDevice> endpoint;hr(enumerator_->GetDevice(endpoint_.c_str(),&endpoint.p),"Resolve endpoint for independent mix readback");
      com_ptr<IAudioClient> client;hr(endpoint->Activate(__uuidof(IAudioClient),CLSCTX_ALL,nullptr,reinterpret_cast<void**>(&client.p)),"Activate independent mix readback");
      WAVEFORMATEX *mix=nullptr;
      struct cleanup {WAVEFORMATEX *&value;~cleanup(){CoTaskMemFree(value);}} guard{mix};
      const auto mix_status=client->GetMixFormat(&mix);
      if(owned_usb_&&(mix_status==AUDCLNT_E_UNSUPPORTED_FORMAT||mix_status==HRESULT_FROM_WIN32(ERROR_NOT_FOUND)))return {};
      hr(mix_status,"Public IAudioClient mix readback");
      return bytes(mix); // No Initialize, stream, render buffer or audio start.
    }
    atmos_format_settle_t settle(const atmos_device_state_t &target,const atmos_device_state_t &initial,const std::vector<unsigned char> &public_mix)override {
      atmos_format_settle_t report;LARGE_INTEGER frequency{};QueryPerformanceFrequency(&frequency);
      report.qpc_frequency=frequency.QuadPart;report.maximum_wait_ms=2000;
      report.set_begin_qpc=set_begin_;report.set_end_qpc=set_end_;
      report.final_readback=initial;report.final_public_mix=public_mix.empty()?read_public_mix():public_mix;
      report.initial_read_qpc=report.final_read_qpc=clock_qpc();
      const auto matches=[&]{return report.final_readback==target&&report.final_public_mix==target.mix_format;};
      report.attempted=!matches();
      const auto deadline=GetTickCount64()+report.maximum_wait_ms;
      auto consumed=notification_baseline_;
      const auto fresh_read=[&]{verify();report.final_readback=read();report.final_public_mix=read_public_mix();report.final_read_qpc=clock_qpc();++report.reads;};
      while(!matches()){
        if(report.reads>=64){report.observation_limit=true;break;}
        const auto now=GetTickCount64();
        if(now>=deadline){report.timed_out=true;fresh_read();break;}
        // Reset before rechecking the monotonic counter: a callback between
        // either operation cannot be lost. No polling timer or fixed delay.
        ResetEvent(notifications_->changed);
        const auto latest=notifications_->count();
        if(latest!=consumed){consumed=latest;fresh_read();continue;}
        const auto wait=WaitForSingleObject(notifications_->changed,static_cast<DWORD>(deadline-now));
        need(wait==WAIT_OBJECT_0||wait==WAIT_TIMEOUT,"Format notification wait failed");
        if(wait==WAIT_TIMEOUT){report.timed_out=true;fresh_read();break;}
        ++report.wakes;
      }
      report.target_matched=matches();notifications_->snapshot(notification_baseline_,report);
      return report;
    }
    atmos_device_state_t target(const atmos_format_t &f)override {
      need(f.valid(),"Unsupported Atmos native format");
      WAVEFORMATEXTENSIBLE device{};
      device.Format={WAVE_FORMAT_EXTENSIBLE,static_cast<WORD>(f.channels),48000,48000u*f.block_align(),f.block_align(),f.container_bits,22};
      device.Samples.wValidBitsPerSample=f.valid_bits;device.dwChannelMask=f.channel_mask;device.SubFormat=KSDATAFORMAT_SUBTYPE_PCM;
      com_ptr<IMMDevice> endpoint;hr(enumerator_->GetDevice(endpoint_.c_str(),&endpoint.p),"Resolve endpoint for support query");
      com_ptr<IAudioClient> client;hr(endpoint->Activate(__uuidof(IAudioClient),CLSCTX_ALL,nullptr,reinterpret_cast<void**>(&client.p)),"Activate format query");
      const auto supported=client->IsFormatSupported(AUDCLNT_SHAREMODE_EXCLUSIVE,&device.Format,nullptr);
      hr(supported,"Exact native Atmos format support");need(supported==S_OK,"Atmos requires exact S_OK native format support");
      auto mix=device;mix.Format.wBitsPerSample=32;mix.Format.nBlockAlign=static_cast<WORD>(f.channels*4);
      mix.Format.nAvgBytesPerSec=48000*mix.Format.nBlockAlign;mix.Samples.wValidBitsPerSample=32;mix.SubFormat=KSDATAFORMAT_SUBTYPE_IEEE_FLOAT;
      return {bytes(&device.Format),bytes(&mix.Format)};
    }
    void set(const atmos_device_state_t &state)override {
      verify();need(state.device_format.size()>=sizeof(WAVEFORMATEX)&&state.mix_format.size()>=sizeof(WAVEFORMATEX),"Missing original format snapshot");
      auto device=state.device_format,mix=state.mix_format;
      if(request_.diagnostic_mix_argument==atmos_mix_argument_e::zero)mix.assign(sizeof(WAVEFORMATEXTENSIBLE),0);
      if(request_.diagnostic_mix_argument==atmos_mix_argument_e::null_pointer)mix.clear();
      last_setter_={};last_setter_.mix_argument=request_.diagnostic_mix_argument;
      last_setter_.device_before=device;last_setter_.mix_before=mix;
      notification_baseline_=notifications_->count();ResetEvent(notifications_->changed);
      set_begin_=clock_qpc();
      const auto result=policy_->SetDeviceFormat(endpoint_.c_str(),reinterpret_cast<WAVEFORMATEX*>(device.data()),mix.empty()?nullptr:reinterpret_cast<WAVEFORMATEX*>(mix.data()));
      set_end_=clock_qpc();
      last_setter_.called=true;last_setter_.begin_qpc=set_begin_;last_setter_.end_qpc=set_end_;
      last_setter_.hresult=static_cast<std::uint32_t>(result);last_setter_.device_after=device;last_setter_.mix_after=mix;
      hr(result,"Set owned Atmos device/mix format");
    }
    atmos_setter_diagnostic_t last_setter_diagnostic()override{return last_setter_;}
  };
  // Windows-only adapter for the existing context_manager/role_control. The
  // full host and audio harness use this exact class; no independent routing
  // algorithm or visibility/Steam recovery path is introduced.

}
std::unique_ptr<atmos_device_backend_t> make_owned_usb_format_backend(const usb_source_config_t& source) {
  const usb_lab::Identity identity{0x1209,VibeLabUsbProduct(source.format.channels,source.format.channel_mask,
    source.format.container_bits,source.format.valid_bits),wide(source.serial),wide(source.controller_instance),
    wide(source.service),int(source.port)};
  usb_lab::Validate(identity);
  const atmos_device_request_t request{source.endpoint,source.usb_instance,source.format};
  return make_affine_atmos_backend([request,identity]{return std::make_unique<windows_backend>(request,identity);});
}
void test_atmos_format_notifications() {
  auto *client=new format_notifications(L"Atmos-Notification-Test-Endpoint");
  struct release {format_notifications *value;~release(){value->Release();}} guard{client};
  need(client->count()==0&&WaitForSingleObject(client->changed,0)==WAIT_TIMEOUT,"Notification starts signalless");
  PROPERTYKEY key{};key.pid=42;key.fmtid.Data1=0x12345678;
  client->OnPropertyValueChanged(L"Different-Endpoint",key);
  client->OnDefaultDeviceChanged(eCapture,eConsole,L"Atmos-Notification-Test-Endpoint");
  need(client->count()==0&&WaitForSingleObject(client->changed,0)==WAIT_TIMEOUT,"Foreign endpoint/capture notification leaked");
  client->OnPropertyValueChanged(L"ATMOS-NOTIFICATION-TEST-ENDPOINT",key);
  need(client->count()==1&&WaitForSingleObject(client->changed,0)==WAIT_OBJECT_0,"Exact endpoint notification did not signal");
  atmos_format_settle_t report;client->snapshot(0,report);
  need(report.notification_count==1&&report.notifications.size()==1&&report.notifications[0].qpc>0&&
    report.notifications[0].kind==1&&report.notifications[0].property_id==42&&report.notifications[0].property_set[0]==0x78,"Notification raw evidence differs");
  const auto consumed=client->count();client->OnDeviceStateChanged(L"Atmos-Notification-Test-Endpoint",DEVICE_STATE_ACTIVE);
  ResetEvent(client->changed);
  need(client->count()!=consumed,"Monotonic count lost a notification before reset/recheck");
  void *unknown=nullptr;need(SUCCEEDED(client->QueryInterface(__uuidof(IUnknown),&unknown))&&unknown,"Notification IUnknown unavailable");
  static_cast<IUnknown*>(unknown)->Release();
  for(unsigned i=0;i<130;++i)client->OnPropertyValueChanged(L"Atmos-Notification-Test-Endpoint",key);
  report={};client->snapshot(0,report);
  need(report.notification_count==132&&report.notifications.size()==128&&report.observation_limit,"Notification bounded diagnostic capacity failed");
}
}

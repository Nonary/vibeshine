// SPDX-License-Identifier: GPL-3.0-or-later
// Adapted from Vibeshine dualsense_usbip_transport.cpp; see THIRD-PARTY.md.

// winsock2 must precede every header that includes windows.h.
// clang-format off
#include <winsock2.h>
// clang-format on
#include "vibe_usb_audio.h"

#include "usbip_win2_abi.h"
#include "mmcss_audio.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <audioclient.h>
#include <cfgmgr32.h>
#include <cstring>
#include <cwchar>
#include <limits>
#include <mmdeviceapi.h>
#include <mmreg.h>
#include <mutex>
#include <propidl.h>
#include <bcrypt.h>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace vibe::usb_audio {
  namespace {
    using namespace std::chrono_literals;

    // The published detach API addresses a reusable port number. Serialize
    // our attach/removal and socket-close paths to prevent our controllers
    // from reusing a port between its identity query and its detach.
    std::mutex driver_lifecycle_mutex;

    struct handle {
      HANDLE value = INVALID_HANDLE_VALUE;

      explicit handle(HANDLE v = INVALID_HANDLE_VALUE):
          value(v) {}

      ~handle() {
        if (value != INVALID_HANDLE_VALUE && value) {
          CloseHandle(value);
        }
      }

      handle(const handle &) = delete;
      handle &operator=(const handle &) = delete;

      explicit operator bool() const {
        return value && value != INVALID_HANDLE_VALUE;
      }
    };
    struct socket_event {
      SOCKET socket;
      WSAEVENT value=WSACreateEvent();
      explicit socket_event(SOCKET s):socket(s){}
      ~socket_event(){if(value!=WSA_INVALID_EVENT){WSAEventSelect(socket,nullptr,0);WSACloseEvent(value);}}
      socket_event(const socket_event&)=delete;
      socket_event& operator=(const socket_event&)=delete;
    };

    HANDLE reserve_session_instance() {
      const auto slot=CreateMutexW(nullptr,FALSE,L"Local\\VibeSpeakersPrivateUsbInstance-v1");
      if(!slot)return nullptr;
      if(GetLastError()==ERROR_ALREADY_EXISTS){CloseHandle(slot);return nullptr;}
      return slot; // presence, rather than thread ownership, reserves this Windows session
    }

    template<class T>
    struct release_com {
      void operator()(T *p) const {
        if (p) {
          p->Release();
        }
      }
    };

    template<class T>
    using com_ptr = std::unique_ptr<T, release_com<T>>;

    struct com_scope {
      HRESULT result = CoInitializeEx(nullptr, COINIT_MULTITHREADED);

      ~com_scope() {
        if (SUCCEEDED(result)) {
          CoUninitialize();
        }
      }
    };

    // CM's list can change while it is being sized. Retry only a bounded
    // number of times, and open just the public released VHCI interface.
    constexpr DEVPROPKEY instance_devkey {{0x78c34fc8, 0x104a, 0x4aca, {0x9e, 0xa4, 0x52, 0x4d, 0x52, 0x99, 0x6e, 0x57}}, 256};

    bool controller_identity(DEVINST node,const std::wstring& expected) {
      std::array<wchar_t,MAX_DEVICE_ID_LEN> id{};
      if(expected.empty()||CM_Get_Device_IDW(node,id.data(),ULONG(id.size()),0)!=CR_SUCCESS||_wcsicmp(id.data(),expected.c_str()))return false;
      std::array<wchar_t,4096> value{};ULONG type=0,size=sizeof(value);
      if(CM_Get_DevNode_Registry_PropertyW(node,CM_DRP_SERVICE,&type,value.data(),&size,0)!=CR_SUCCESS||type!=REG_SZ||size<sizeof(wchar_t)||size>sizeof(value)||size%sizeof(wchar_t)||value[size/sizeof(wchar_t)-1]!=0||_wcsicmp(value.data(),L"usbip2_ude"))return false;
      value.fill(0);size=sizeof(value);
      if(CM_Get_DevNode_Registry_PropertyW(node,CM_DRP_HARDWAREID,&type,value.data(),&size,0)!=CR_SUCCESS||type!=REG_MULTI_SZ||size<2*sizeof(wchar_t)||size>sizeof(value)||size%sizeof(wchar_t)||value[size/sizeof(wchar_t)-1]!=0||value[size/sizeof(wchar_t)-2]!=0)return false;
      const std::size_t chars=size/sizeof(wchar_t);
      for(std::size_t at=0;at<chars&&value[at];){const auto end=std::find(value.begin()+at,value.begin()+chars,L'\0');if(end==value.begin()+chars)return false;if(!_wcsicmp(value.data()+at,L"ROOT\\USBIP_WIN2\\UDE"))return true;at=std::size_t(end-value.begin())+1;}
      return false;
    }

    HANDLE open_driver(const std::wstring& exact_instance,DEVINST *node = nullptr) {
      // The caller supplies its journal-owned instance. No first-GUID fallback,
      // hardware-ID-wide operation, alternate instance search or implicit root.
      if(exact_instance.empty()||exact_instance.size()>=MAX_DEVICE_ID_LEN||exact_instance.find(L'\0')!=std::wstring::npos)return INVALID_HANDLE_VALUE;
      auto instance_filter=exact_instance; // legacy CM signature uses mutable DEVINSTID_W
      auto guid = driver_abi::interface_guid;
      for (unsigned attempt = 0; attempt < 4; ++attempt) {
        ULONG count = 0;
        if (CM_Get_Device_Interface_List_SizeW(&count, &guid, instance_filter.data(), CM_GET_DEVICE_INTERFACE_LIST_PRESENT) != CR_SUCCESS || count < 2 || count > 65536) {
          return INVALID_HANDLE_VALUE;
        }
        std::vector<wchar_t> paths(count);
        const auto result = CM_Get_Device_Interface_ListW(&guid, instance_filter.data(), paths.data(), count, CM_GET_DEVICE_INTERFACE_LIST_PRESENT);
        if (result == CR_BUFFER_SMALL) {
          continue;
        }
        if (result != CR_SUCCESS) {
          return INVALID_HANDLE_VALUE;
        }
        const auto end=std::find(paths.begin(),paths.end(),L'\0');
        if(end==paths.begin()||end==paths.end()||end+1==paths.end()||*(end+1)!=0)return INVALID_HANDLE_VALUE;
        std::array<wchar_t,MAX_DEVICE_ID_LEN> instance{};DEVPROPTYPE type=0;ULONG size=sizeof(instance);DEVINST selected=0;
        if(CM_Get_Device_Interface_PropertyW(paths.data(),&instance_devkey,&type,reinterpret_cast<BYTE*>(instance.data()),&size,0)!=CR_SUCCESS||type!=DEVPROP_TYPE_STRING||size<sizeof(wchar_t)||size>sizeof(instance)||size%sizeof(wchar_t)||instance[size/sizeof(wchar_t)-1]!=0||_wcsicmp(instance.data(),exact_instance.c_str())||CM_Locate_DevNodeW(&selected,instance.data(),CM_LOCATE_DEVNODE_NORMAL)!=CR_SUCCESS||!controller_identity(selected,exact_instance))return INVALID_HANDLE_VALUE;
        const auto device=CreateFileW(paths.data(),GENERIC_READ|GENERIC_WRITE,FILE_SHARE_READ|FILE_SHARE_WRITE,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL|FILE_FLAG_OVERLAPPED,nullptr);
        if(device!=INVALID_HANDLE_VALUE&&node)*node=selected;
        return device;
      }
      return INVALID_HANDLE_VALUE;
    }

    struct ioctl_request {
      handle device;
      handle event {CreateEventW(nullptr, TRUE, FALSE, nullptr)};
      OVERLAPPED overlap {};
      std::vector<std::uint8_t> input;
      std::vector<std::uint8_t> output;
      std::function<void(bool, const std::vector<std::uint8_t> &, DWORD, DWORD)> late_completion;

      ioctl_request() {
        overlap.hEvent = event.value;
      }
    };

    // An overlapped IOCTL lets attach time out without blocking stream
    // control indefinitely. Cancellation buffers must stay alive until the
    // kernel has completed the request, even if cancellation is delayed.
    bool device_io(HANDLE device, DWORD code, std::span<const std::uint8_t> input, std::span<std::uint8_t> output, DWORD &returned, std::chrono::milliseconds timeout, std::function<void(bool, const std::vector<std::uint8_t> &, DWORD, DWORD)> late_completion = {}, bool cancel_on_timeout = true) {
      auto request = std::make_shared<ioctl_request>();
      request->late_completion = std::move(late_completion);
      if (!request->event || !DuplicateHandle(GetCurrentProcess(), device, GetCurrentProcess(), &request->device.value, 0, FALSE, DUPLICATE_SAME_ACCESS)) {
        return false;
      }
      request->input.assign(input.begin(), input.end());
      request->output.resize(output.size());
      returned = 0;
      const auto immediate = DeviceIoControl(request->device.value, code, request->input.data(), static_cast<DWORD>(request->input.size()), request->output.data(), static_cast<DWORD>(request->output.size()), &returned, &request->overlap);
      if (!immediate) {
        if (GetLastError() != ERROR_IO_PENDING) {
          return false;
        }
        if (WaitForSingleObject(request->event.value, static_cast<DWORD>(timeout.count())) != WAIT_OBJECT_0) {
          if (cancel_on_timeout) {
            CancelIoEx(request->device.value, &request->overlap);
          }
          if (!cancel_on_timeout || WaitForSingleObject(request->event.value, 2000) != WAIT_OBJECT_0) {
            auto retire = [request]() noexcept {
              DWORD completed = 0;
              // TRUE retains the OVERLAPPED and buffers through actual kernel
              // completion, including the rare thread-allocation failure path.
              const auto succeeded = GetOverlappedResult(request->device.value, &request->overlap, &completed, TRUE);
              const auto error = succeeded ? ERROR_SUCCESS : GetLastError();
              if (request->late_completion) {
                try { request->late_completion(succeeded, request->output, completed, error); } catch (...) {}
              }
            };
            try { std::thread(retire).detach(); } catch (...) { retire(); }
          } else if (request->late_completion) {
            DWORD completed = 0;
            const auto succeeded = GetOverlappedResult(request->device.value, &request->overlap, &completed, FALSE);
            const auto error = succeeded ? ERROR_SUCCESS : GetLastError();
            try { request->late_completion(succeeded, request->output, completed, error); } catch (...) {}
          }
          SetLastError(ERROR_TIMEOUT);
          return false;
        }
        if (!GetOverlappedResult(request->device.value, &request->overlap, &returned, FALSE)) {
          return false;
        }
      }
      if (returned > output.size()) {
        SetLastError(ERROR_INVALID_DATA);
        return false;
      }
      std::copy_n(request->output.begin(), returned, output.begin());
      return true;
    }

    template<class T>
    auto read_bytes(const T &v) {
      return std::span<const std::uint8_t> {reinterpret_cast<const std::uint8_t *>(&v), sizeof(v)};
    }

    template<class T>
    auto write_bytes(T &v) {
      return std::span<std::uint8_t> {reinterpret_cast<std::uint8_t *>(&v), sizeof(v)};
    }

    void stop_exact_attach_attempts(HANDLE device, const std::string &service, const std::string &busid) {
      driver_abi::stop_attach_attempts request {};
      request.size = sizeof(request);
      constexpr std::string_view host = "127.0.0.1";
      std::copy(host.begin(), host.end(), request.host);
      std::copy(service.begin(), service.end(), request.service);
      std::copy(busid.begin(), busid.end(), request.busid);
      DWORD returned = 0;
      device_io(device, driver_abi::stop_attach_attempts_ioctl, read_bytes(request), write_bytes(request), returned, 1s);
    }

    bool compatible_driver(HANDLE device) {
      driver_abi::get_imported_devices query {};
      query.size = sizeof(query);
      // The released driver rejects the size header before interpreting its
      // fields. GET_IMPORTED_DEVICES is a read-only probe of this exact ABI.
      std::vector<std::uint8_t> devices(driver_abi::imported_devices_offset + 255 * sizeof(driver_abi::imported_device));
      DWORD returned = 0;
      return device_io(device, driver_abi::get_imported_devices_ioctl, read_bytes(query.size), devices, returned, 1s) &&
             returned >= driver_abi::imported_devices_offset && (returned - driver_abi::imported_devices_offset) % sizeof(driver_abi::imported_device) == 0;
    }

    template<std::size_t N>
    void copy_string(char (&target)[N], const std::string &source) {
      // All callers supply short generated ASCII strings, never user input.
      std::copy_n(source.data(), std::min(source.size(), N - 1), target);
    }

    template<std::size_t N>
    bool equals(const char (&value)[N], const std::string &expected) {
      return expected.size() < N && std::equal(expected.begin(), expected.end(), value) && value[expected.size()] == '\0';
    }

    identity make_identity(profile format, bool) {
      constexpr char digits[] = "0123456789ABCDEF";
      std::array<unsigned char,21> random{};
      if(BCryptGenRandom(nullptr,random.data(),ULONG(random.size()),BCRYPT_USE_SYSTEM_PREFERRED_RNG)!=0)throw std::runtime_error("BCryptGenRandom");
      identity result;result.format=format;result.serial="VA";
      for(unsigned i=0;i<15;++i){result.busid+=digits[random[i]>>4];result.busid+=digits[random[i]&15];}
      for(unsigned i=15;i<21;++i){result.serial+=digits[random[i]>>4];result.serial+=digits[random[i]&15];}
      result.serial+=digits[format.channels];
      // Every owned device session receives an independent random serial.
      return result;
    }

    // The released UDE driver can expose a port-generated instance ID even
    // when its attach request includes a serial. Find our port under the exact
    // host controller we opened; matching VID/PID alone could select a physical
    // device or another session's endpoint. USB/IP's returned port is the
    // Usb20PortNumber/Usb30PortNumber supplied to UdecxUsbDevicePlugIn.
    bool controller_root(DEVINST host, int port, std::uint16_t product, DEVINST &root) {
      if (port <= 0 || !product) {
        return false;
      }
      std::array<wchar_t,32> prefix{};
      const auto characters=swprintf_s(prefix.data(),prefix.size(),L"USB\\VID_%04X&PID_%04X\\",unsigned(lab_vendor),unsigned(product));
      if(characters<=0)return false;
      DEVINST hub = 0;
      if (CM_Get_Child(&hub, host, 0) != CR_SUCCESS) {
        return false;
      }
      for (unsigned hubs = 0; hubs < 32; ++hubs) {
        DEVINST device = 0;
        if (CM_Get_Child(&device, hub, 0) == CR_SUCCESS) {
          for (unsigned devices = 0; devices < 256; ++devices) {
            std::array<wchar_t, MAX_DEVICE_ID_LEN> instance {};
            ULONG address = 0;
            ULONG type = 0;
            ULONG size = sizeof(address);
            if (CM_Get_Device_IDW(device, instance.data(), ULONG(instance.size()), 0) == CR_SUCCESS && _wcsnicmp(instance.data(), prefix.data(), static_cast<std::size_t>(characters)) == 0 && CM_Get_DevNode_Registry_PropertyW(device, CM_DRP_ADDRESS, &type, &address, &size, 0) == CR_SUCCESS && type == REG_DWORD && size == sizeof(address) && address == static_cast<ULONG>(port)) {
              root = device;
              return true;
            }
            if (CM_Get_Sibling(&device, device, 0) != CR_SUCCESS) {
              break;
            }
          }
        }
        if (CM_Get_Sibling(&hub, hub, 0) != CR_SUCCESS) {
          break;
        }
      }
      return false;
    }

    // Audio endpoints use software devnodes, so match their physical device
    // container GUID rather than relying on an endpoint's parent devnode.
    constexpr GUID container_key_guid {0x8c7ed206, 0x3f8a, 0x4827, {0xb3, 0xab, 0xae, 0x9e, 0x1f, 0xae, 0xfc, 0x6c}};
    constexpr DEVPROPKEY container_devkey {container_key_guid, 2};
    constexpr PROPERTYKEY container_property {container_key_guid, 2};

    bool render_endpoint_ready(IMMDeviceEnumerator *enumerator, const GUID &container, binding &result) {
      IMMDeviceCollection *raw_collection = nullptr;
      if (FAILED(enumerator->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &raw_collection))) {
        return false;
      }
      com_ptr<IMMDeviceCollection> collection(raw_collection);
      UINT count = 0;
      if (FAILED(collection->GetCount(&count))) {
        return false;
      }
      for (UINT index = 0; index < std::min(count, UINT {1024}); ++index) {
        IMMDevice *raw_device = nullptr;
        if (FAILED(collection->Item(index, &raw_device))) {
          continue;
        }
        com_ptr<IMMDevice> device(raw_device);
        IPropertyStore *raw_properties = nullptr;
        if (FAILED(device->OpenPropertyStore(STGM_READ, &raw_properties))) {
          continue;
        }
        com_ptr<IPropertyStore> properties(raw_properties);
        PROPVARIANT value {};
        const auto property_ok = properties->GetValue(container_property, &value);
        const auto matches = SUCCEEDED(property_ok) && value.vt == VT_CLSID && value.puuid && IsEqualGUID(*value.puuid, container);
        PropVariantClear(&value);
        if (!matches) {
          continue;
        }
        // A valid active owned endpoint can exist before its cached mix can
        // activate after a profile change. Publish identity independently;
        // the managed host must set and verify the actual format before PCM.
        LPWSTR endpoint=nullptr;
        if(SUCCEEDED(device->GetId(&endpoint))&&endpoint){result.endpoint_id=endpoint;CoTaskMemFree(endpoint);}
        if(result.endpoint_id.empty())continue;
        IAudioClient *raw_client = nullptr;
        const auto activated=device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, reinterpret_cast<void **>(&raw_client));
        result.mix_hresult=activated;
        if (FAILED(activated)) {
          return true;
        }
        com_ptr<IAudioClient> client(raw_client);
        WAVEFORMATEX *format = nullptr;
        const auto format_ok = client->GetMixFormat(&format);
        result.mix_hresult=format_ok;
        const auto ready = SUCCEEDED(format_ok) && format;
        if(ready) {
          result.mix_channels=format->nChannels;result.mix_rate=format->nSamplesPerSec;result.mix_bits=format->wBitsPerSample;
          if(format->wFormatTag==WAVE_FORMAT_EXTENSIBLE&&format->cbSize>=22)result.mix_mask=reinterpret_cast<WAVEFORMATEXTENSIBLE*>(format)->dwChannelMask;
        }
        CoTaskMemFree(format);
        return true;
      }
      return false;
    }
  }  // namespace

  struct controller::impl: std::enable_shared_from_this<controller::impl> {
    handle session_instance{reserve_session_instance()};
    identity owned;
    callbacks handlers;
    session protocol;
    const std::string busid;
    const std::string serial;
    std::string service;
    DEVINST host_node = 0;
    handle driver;
    SOCKET listener = INVALID_SOCKET;
    std::thread worker;
    std::atomic_bool stop {false};
    handle stop_event {CreateEventW(nullptr,TRUE,FALSE,nullptr)};
    std::atomic_bool imported {false};
    handle worker_ready {CreateEventW(nullptr,TRUE,FALSE,nullptr)};
    mmcss_status worker_mmcss;
    std::atomic_bool worker_failed {false};
    counters final_counters;
    bool winsock = false;
    std::atomic_int imported_port {0};

    impl(profile format, callbacks cb, const std::wstring& exact_instance, bool managed):
        owned(make_identity(format,managed)),handlers(std::move(cb)),protocol(owned,handlers),
        busid(owned.busid),serial(owned.serial),
        driver(open_driver(exact_instance,&host_node)) {}

    bool stopped=false;
    counters stop_and_join() {
      if(stopped)return final_counters;
      stopped=true;
      {
        const std::lock_guard lifecycle(driver_lifecycle_mutex);
        stop_attach_attempts();
        detach_owned();
      }
      stop = true;
      if(stop_event)SetEvent(stop_event.value);
      if (worker.joinable()) {
        worker.join();
      }
      protocol.close();
      // Socket loss schedules retries even for PLUGIN_HARDWARE_ONCE.
      // Cancel just this controller's exact location after socket closure.
      {
        const std::lock_guard lifecycle(driver_lifecycle_mutex);
        detach_owned();
        detach_barrier();
        stop_attach_attempts();
      }
      final_counters=protocol.stats();
      final_counters.worker_thread_id=worker_mmcss.thread_id;
      final_counters.mmcss_task_index=worker_mmcss.task_index;
      final_counters.mmcss_register_error=worker_mmcss.register_error;
      final_counters.mmcss_revert_error=worker_mmcss.revert_error;
      final_counters.mmcss_registered=worker_mmcss.registered;
      final_counters.mmcss_reverted=worker_mmcss.reverted;
      final_counters.worker_failed=worker_failed;
      return final_counters;
    }
    ~impl() {
      stop_and_join();
      if(listener!=INVALID_SOCKET)closesocket(listener);
      if(winsock)WSACleanup();
    }

    void detach_barrier() {
      if (!driver || service.empty()) {
        return;
      }
      // Detach uses the sequential queue; retry cancellation uses a parallel
      // queue. An impossible positive port is rejected without touching any
      // device after earlier queued socket-loss detaches have finished. Never
      // use zero or a negative port: those request removal of all devices.
      driver_abi::plugout_hardware request {};
      request.size = sizeof(request);
      request.port = std::numeric_limits<int>::max();
      auto cleanup_device = std::make_shared<handle>();
      if (!DuplicateHandle(GetCurrentProcess(), driver.value, GetCurrentProcess(), &cleanup_device->value, 0, FALSE, DUPLICATE_SAME_ACCESS)) {
        return;
      }
      auto stop_after_barrier = [cleanup_device, service = service, busid = busid](bool, const std::vector<std::uint8_t> &, DWORD, DWORD error) {
        if (error == ERROR_INVALID_PARAMETER) {
          stop_exact_attach_attempts(cleanup_device->value, service, busid);
        }
      };
      DWORD returned = 0;
      // Keep this harmless request pending if foreground teardown times out.
      // Cancellation while queued would not establish a barrier. Its retained
      // completion context runs exact-location STOP after actual dispatch.
      device_io(driver.value, driver_abi::plugout_hardware_ioctl, read_bytes(request), {}, returned, 2s, std::move(stop_after_barrier), false);
    }

    void stop_attach_attempts() {
      if (!driver || service.empty()) {
        return;
      }
      stop_exact_attach_attempts(driver.value, service, busid);
    }

    bool owned_import_present() {
      const auto port=imported_port.load();
      if(!driver||service.empty()||port<=0||port>255)return false;
      driver_abi::get_imported_devices query{};query.size=sizeof(query);
      for(unsigned capacity=4;capacity<=256;capacity*=2){
        std::vector<std::uint8_t> devices(driver_abi::imported_devices_offset+capacity*sizeof(driver_abi::imported_device));DWORD returned=0;
        if(!device_io(driver.value,driver_abi::get_imported_devices_ioctl,read_bytes(query.size),devices,returned,1s)){
          if(GetLastError()==ERROR_INSUFFICIENT_BUFFER)continue;return false;
        }
        if(returned<driver_abi::imported_devices_offset||returned>devices.size()||(returned-driver_abi::imported_devices_offset)%sizeof(driver_abi::imported_device))return false;
        unsigned matches=0;
        for(unsigned offset=driver_abi::imported_devices_offset;offset+sizeof(driver_abi::imported_device)<=returned;offset+=sizeof(driver_abi::imported_device)){
          driver_abi::imported_device device{};std::memcpy(&device,devices.data()+offset,sizeof(device));
          if(device.port==port&&device.vendor==lab_vendor&&device.product==owned.format.product_id()&&device.speed==3&&device.devid==0x10001&&
             equals(device.host,"127.0.0.1")&&equals(device.service,service)&&equals(device.busid,busid)&&equals(device.serial,serial))++matches;
        }
        return matches==1;
      }
      return false;
    }
    void detach_owned() {
      if (!driver || service.empty()) {
        return;
      }
      // A port number is reused after removal. Confirm this exact loopback
      // location still belongs to us before sending a port-based detach.
      driver_abi::get_imported_devices query {};
      query.size = sizeof(query);
      for (unsigned capacity = 4; capacity <= 1024; capacity *= 2) {
        std::vector<std::uint8_t> devices(driver_abi::imported_devices_offset + capacity * sizeof(driver_abi::imported_device));
        DWORD returned = 0;
        if (!device_io(driver.value, driver_abi::get_imported_devices_ioctl, read_bytes(query.size), devices, returned, 1s)) {
          if (GetLastError() == ERROR_INSUFFICIENT_BUFFER) {
            continue;
          }
          return;
        }
        if (returned < driver_abi::imported_devices_offset || (returned - driver_abi::imported_devices_offset) % sizeof(driver_abi::imported_device)) {
          return;
        }
        for (unsigned offset = driver_abi::imported_devices_offset; offset + sizeof(driver_abi::imported_device) <= returned; offset += sizeof(driver_abi::imported_device)) {
          driver_abi::imported_device device {};
          std::memcpy(&device, devices.data() + offset, sizeof(device));
          const auto known_port=imported_port.load();
          if (device.port > 0 && (!known_port || device.port==known_port) && device.vendor==lab_vendor && device.product==owned.format.product_id() && device.speed==3 && device.devid==0x10001 && equals(device.host, "127.0.0.1") && equals(device.service, service) && equals(device.busid, busid) && equals(device.serial, serial)) {
            driver_abi::plugout_hardware request {};
            request.size = sizeof(request);
            request.port = device.port;
            device_io(driver.value, driver_abi::plugout_hardware_ioctl, read_bytes(request), {}, returned, 2s);
            return;
          }
        }
        return;
      }
    }

    bool start() {
      const auto failed=[this](unsigned stage) {
        if(handlers.diagnostic)handlers.diagnostic("attach_stage="+std::to_string(stage)+" win32="+std::to_string(GetLastError())+" mutex="+std::to_string(bool(session_instance))+" driver="+std::to_string(bool(driver)));
        return false;
      };
      if (!session_instance || !driver || !stop_event || !worker_ready || !compatible_driver(driver.value)) {
        return failed(1);
      }
      WSADATA data {};
      if (WSAStartup(MAKEWORD(2, 2), &data)) {
        return failed(2);
      }
      winsock = true;
      listener = WSASocketW(AF_INET, SOCK_STREAM, IPPROTO_TCP, nullptr, 0, WSA_FLAG_NO_HANDLE_INHERIT);
      if (listener == INVALID_SOCKET) {
        return failed(3);
      }
      const BOOL exclusive = TRUE;
      if (setsockopt(listener, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, reinterpret_cast<const char *>(&exclusive), sizeof(exclusive))) {
        return failed(4);
      }
      sockaddr_in address {};
      address.sin_family = AF_INET;
      address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
      address.sin_port = 0;
      if (bind(listener, reinterpret_cast<const sockaddr *>(&address), sizeof(address)) || listen(listener, 1)) {
        return failed(5);
      }
      int size = sizeof(address);
      if (getsockname(listener, reinterpret_cast<sockaddr *>(&address), &size)) {
        return failed(6);
      }
      service = std::to_string(ntohs(address.sin_port));
      worker = std::thread([this] {
        mmcss_audio audio(worker_mmcss);
        const auto registered=audio.start();
        if(!registered)worker_failed=true;
        // All startup fields are published before readiness; no attach is
        // attempted when registration fails. Close always runs on this worker.
        if(!SetEvent(worker_ready.value))worker_failed=true;
        try {
          if(handlers.diagnostic)handlers.diagnostic("usb_worker_mmcss task="+std::string(usb_worker_pro_audio?"Pro Audio":"Audio")+" thread="+std::to_string(worker_mmcss.thread_id)+" registered="+std::to_string(registered)+" error="+std::to_string(worker_mmcss.register_error));
          if(registered&&!worker_failed)serve();
        } catch (...) { worker_failed=true; }
        if(!audio.close())worker_failed=true;
        try {
          if(handlers.diagnostic)handlers.diagnostic("usb_worker_mmcss_end reverted="+std::to_string(worker_mmcss.reverted)+" error="+std::to_string(worker_mmcss.revert_error));
        }catch(...){worker_failed=true;}
        // A failed revert ends this worker; it is never reused for a peer.
        imported = false;
      });
            const HANDLE startup_events[]{worker_ready.value,stop_event.value};
      if(WaitForMultipleObjects(2,startup_events,FALSE,10000)!=WAIT_OBJECT_0||
         worker_failed||!worker_mmcss.registered)return failed(7);
      driver_abi::plugin_hardware request {};
      request.size = sizeof(request);
      copy_string(request.host, "127.0.0.1");
      copy_string(request.service, service);
      copy_string(request.busid, busid);
      copy_string(request.serial, serial);
      request.wsk_events = true;
      DWORD returned = 0;
      const std::lock_guard lifecycle(driver_lifecycle_mutex);
      // Keep the server and its exact identity alive if cancellation races a
      // successful attach. Its completion owner records the late port before
      // the last shared reference runs normal, owned-device teardown.
      auto late_attach = [owned = shared_from_this()](bool succeeded, const std::vector<std::uint8_t> &response, DWORD actual, DWORD) {
        if (succeeded && actual == driver_abi::attach_reply_size && response.size() >= actual) {
          int port = 0;
          std::memcpy(&port, response.data() + sizeof(ULONG), sizeof(port));
          if (port > 0) {
            owned->imported_port = port;
          }
        }
      };
      if (!device_io(driver.value, driver_abi::plugin_hardware_once, read_bytes(request), write_bytes(request).first(driver_abi::attach_reply_size), returned, 10s, std::move(late_attach)) || returned != driver_abi::attach_reply_size || request.port <= 0) {
        return failed(8);
      }
      imported_port = request.port;
      return imported;
    }

    void serve() {
      // Wrong/idle loopback clients cannot consume the one legitimate import.
      // Never disclose the 120-bit bus-id capability through DEVLIST.
      for(unsigned attempt=0;!stop&&attempt<32;++attempt) {
        serve_connection();
        if(protocol.imported())return;
        protocol=session(owned,handlers);
      }
    }
    void serve_connection() {
      SOCKET connection = INVALID_SOCKET;
      socket_event accept_event(listener);
      if(accept_event.value==WSA_INVALID_EVENT||WSAEventSelect(listener,accept_event.value,FD_ACCEPT|FD_CLOSE))return;
      while (!stop && connection == INVALID_SOCKET) {
        const HANDLE waiting[]{stop_event.value,accept_event.value};
        const auto ready=WaitForMultipleObjects(2,waiting,FALSE,INFINITE);
        if(ready!=WAIT_OBJECT_0+1)return;
        WSANETWORKEVENTS events{};
        if(WSAEnumNetworkEvents(listener,accept_event.value,&events)||events.iErrorCode[FD_ACCEPT_BIT]||(events.lNetworkEvents&FD_CLOSE))return;
        if(!(events.lNetworkEvents&FD_ACCEPT))continue;
        sockaddr_in peer {};
        int size = sizeof(peer);
        connection = accept(listener, reinterpret_cast<sockaddr *>(&peer), &size);
        if (connection != INVALID_SOCKET && (peer.sin_family != AF_INET || peer.sin_addr.s_addr != htonl(INADDR_LOOPBACK))) {
          closesocket(connection);
          connection = INVALID_SOCKET;
        }
      }
      if (connection == INVALID_SOCKET) {
        return;
      }

      // Keep socket closure guaranteed if a callback throws.
      struct close_socket {
        SOCKET value;
        std::atomic_bool &imported;

        ~close_socket() {
          const std::lock_guard lifecycle(driver_lifecycle_mutex);
          imported = false;
          shutdown(value, SD_BOTH);
          closesocket(value);
        }
      } close {connection, imported};

      socket_event network(connection);
      handle timer(CreateWaitableTimerExW(nullptr,nullptr,0x00000002,TIMER_MODIFY_STATE|SYNCHRONIZE)); // HIGH_RESOLUTION, auto reset
      const BOOL nodelay = TRUE;
      if(network.value==WSA_INVALID_EVENT||!timer||WSAEventSelect(connection,network.value,FD_READ|FD_WRITE|FD_CLOSE)||setsockopt(connection,IPPROTO_TCP,TCP_NODELAY,reinterpret_cast<const char*>(&nodelay),sizeof(nodelay))) {
        return;
      }
      const auto handshake_deadline = std::chrono::steady_clock::now() + 1s;
      std::array<std::uint8_t, 8192> incoming {};
      bool readable=true,writable=true;
      while (!stop) {
        protocol.poll();
        if ((protocol.finished() || protocol.failed()) && protocol.output().empty()) {
          return;
        }
        imported = protocol.imported() && !protocol.failed();
        if (!imported && std::chrono::steady_clock::now() >= handshake_deadline) {
          return;
        }
        if(writable&&!protocol.output().empty()) {
          const auto outgoing=protocol.output();
          const auto written = send(connection, reinterpret_cast<const char *>(outgoing.data()), static_cast<int>(outgoing.size()), 0);
          if (written == SOCKET_ERROR) {
            if (WSAGetLastError() != WSAEWOULDBLOCK) {
              return;
            }
            writable=false;
          } else if (!written) {
            return;
          } else {
            protocol.consume_output(static_cast<std::size_t>(written));
          }
        }
        if(readable) {
          // Bound work per iteration so one socket cannot prevent shutdown
          // or starve the poll that completes scheduled interrupt/iso URBs.
          for (unsigned reads = 0; reads < 8; ++reads) {
            const auto received = recv(connection, reinterpret_cast<char *>(incoming.data()), static_cast<int>(incoming.size()), 0);
            if (received == SOCKET_ERROR) {
              if (WSAGetLastError() != WSAEWOULDBLOCK) {
                return;
              }
              readable=false;
              break;
            }
            if (!received) {
              return;
            }
            if (!protocol.feed(std::span(incoming).first(received))) {
              readable=false;
              break;
            }
          }
        }
        if((protocol.finished()||protocol.failed())&&protocol.output().empty())return;
        // Continue only while actual socket work remains. Otherwise wait on
        // the socket notification, exact next USB deadline, or explicit stop.
        // No select(1ms), timer-resolution mutation, Sleep, or idle polling.
        if(readable||(writable&&!protocol.output().empty()))continue;
        auto deadline=protocol.next_deadline();
        if(!protocol.imported())deadline=std::min(deadline,handshake_deadline);
        const HANDLE waiting[]{stop_event.value,network.value,timer.value};DWORD count=2;
        if(deadline!=std::chrono::steady_clock::time_point::max()){
          const auto remaining=deadline-std::chrono::steady_clock::now();
          if(remaining<=decltype(remaining)::zero())continue;
          const auto ns=std::chrono::duration_cast<std::chrono::nanoseconds>(remaining).count();
          LARGE_INTEGER due{};due.QuadPart=-std::max<std::int64_t>(1,(ns+99)/100);
          if(!SetWaitableTimerEx(timer.value,&due,0,nullptr,nullptr,nullptr,0))return;
          count=3;
        }else CancelWaitableTimer(timer.value);
        const auto wake=WaitForMultipleObjects(count,waiting,FALSE,INFINITE);
        if(wake==WAIT_OBJECT_0)return;
        if(wake==WAIT_OBJECT_0+1){
          WSANETWORKEVENTS events{};
          if(WSAEnumNetworkEvents(connection,network.value,&events)||events.iErrorCode[FD_READ_BIT]||events.iErrorCode[FD_WRITE_BIT])return;
          if(events.lNetworkEvents&(FD_READ|FD_CLOSE))readable=true;
          if(events.lNetworkEvents&FD_WRITE)writable=true;
        }else if(wake!=WAIT_OBJECT_0+2)return;
      }
    }
  };

  bool available(const std::wstring& controller_instance) {
    const handle driver(open_driver(controller_instance));
    return driver && compatible_driver(driver.value);
  }

  controller::controller(std::shared_ptr<impl> state):
      state_(std::move(state)) {}

  controller::~controller() = default;

  bool controller::connected() const noexcept {
    return state_->imported&&!state_->worker_failed;
  }

  binding controller::inspect() {
    binding result;
    result.worker_thread_id=state_->worker_mmcss.thread_id;result.mmcss_task_index=state_->worker_mmcss.task_index;result.mmcss_registered=state_->worker_mmcss.registered;result.worker_pro_audio=usb_worker_pro_audio;
    result.busid=state_->busid;result.serial=state_->serial;result.service=state_->service;result.port=state_->imported_port;
    std::array<wchar_t,MAX_DEVICE_ID_LEN> id{};
    if(CM_Get_Device_IDW(state_->host_node,id.data(),ULONG(id.size()),0)==CR_SUCCESS)result.host_instance=id.data();
    if(!state_->owned_import_present())return result;
    result.vendor=lab_vendor;result.product=state_->owned.format.product_id();
    DEVINST root=0;
    if(!controller_root(state_->host_node,result.port,result.product,root))return result;
    if(CM_Get_Device_IDW(root,id.data(),ULONG(id.size()),0)==CR_SUCCESS)result.usb_instance=id.data();
    const com_scope com;if(FAILED(com.result)&&com.result!=RPC_E_CHANGED_MODE)return result;
    GUID container{};DEVPROPTYPE type=0;ULONG size=sizeof(container);
    if(CM_Get_DevNode_PropertyW(root,&container_devkey,&type,reinterpret_cast<BYTE*>(&container),&size,0)!=CR_SUCCESS||type!=DEVPROP_TYPE_GUID||size!=sizeof(container))return result;
    IMMDeviceEnumerator* raw=nullptr;
    if(FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator),nullptr,CLSCTX_ALL,__uuidof(IMMDeviceEnumerator),reinterpret_cast<void**>(&raw))))return result;
    com_ptr<IMMDeviceEnumerator> enumerator(raw);render_endpoint_ready(enumerator.get(),container,result);
    return result;
  }
  counters controller::stop(){return state_->stop_and_join();}
  const clock_flight* controller::clock_after_stop()const noexcept{return state_&&state_->stopped?state_->protocol.clock_trace():nullptr;}
  std::unique_ptr<controller> create(profile format,callbacks handlers,const std::wstring& controller_instance,bool verified_managed_owner) {
    if(!format.valid()||!verified_managed_owner)return {};
    auto state=std::make_shared<controller::impl>(format,std::move(handlers),controller_instance,verified_managed_owner);
    if(!state->start())return {};
    return std::unique_ptr<controller>(new controller(std::move(state)));
  }
} // namespace vibe::usb_audio

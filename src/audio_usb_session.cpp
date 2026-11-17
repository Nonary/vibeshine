#include "audio_usb_session.h"
#include "audio_atmos_device.h"
#include <algorithm>
#include <array>
#include <filesystem>
#include <fstream>
#include <cwctype>
#include <new>
#include <stdexcept>
#include <utility>
#include <mutex>
#include <nlohmann/json.hpp>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <bcrypt.h>
#include <sddl.h>
#include <aclapi.h>
#include <shlobj.h>
#include <wtsapi32.h>
#include <userenv.h>
#include <mmdeviceapi.h>
#include <thread>
#include "audio_context.h"
#include "audio_role_control.h"
#include "vibe_profile_ids.h"
#include "platform/windows/PolicyConfig.h"
#include "usb_audio_identity.h"
#endif

namespace audio {
namespace {
using json=nlohmann::json;
void need(bool value,const char* message){if(!value)throw std::runtime_error(message);}
bool hash_text(const std::string& text){return text.size()==64&&text.find_first_not_of("0123456789abcdefABCDEF")==std::string::npos;}
std::string lower(std::string value){for(auto& c:value)if(c>='A'&&c<='Z')c=char(c-'A'+'a');return value;}
bool controller_text(const std::string& text){
  const auto value=lower(text);
  return value.starts_with("root\\usb\\")&&value.size()>9&&value.size()<200&&
    value.substr(9).find_first_not_of("0123456789abcdef")==std::string::npos;
}
std::uint64_t integer(const json& value,const char* key,std::uint64_t maximum=UINT64_MAX){
  const auto& n=value.at(key);
  need(n.is_number_unsigned()||(n.is_number_integer()&&n.get<std::int64_t>()>=0),"Binding requires unsigned integer");
  const auto result=n.get<std::uint64_t>();need(result<=maximum,"Binding integer out of range");return result;
}
std::string text(const json& value,const char* key,std::size_t maximum=32700){
  const auto& field=value.at(key);need(field.is_string(),"Binding requires string");auto result=field.get<std::string>();
  need(!result.empty()&&result.size()<=maximum&&result.find('\0')==std::string::npos&&result.find_first_of("\r\n")==std::string::npos,"Binding string is empty or unsafe");return result;
}
std::wstring quote(const std::wstring& argument){
  std::wstring result=L"\"";std::size_t backslashes=0;
  for(auto c:argument){if(c==L'\\'){++backslashes;continue;}if(c==L'\"'){result.append(backslashes*2+1,L'\\');result+=c;}else{result.append(backslashes,L'\\');result+=c;}backslashes=0;}
  result.append(backslashes*2,L'\\');return result+L'\"';
}
#ifdef _WIN32
struct handle_t {
  HANDLE value=INVALID_HANDLE_VALUE;
  handle_t()=default;explicit handle_t(HANDLE v):value(v){}
  handle_t(const handle_t&)=delete;handle_t& operator=(const handle_t&)=delete;
  handle_t(handle_t&& other)noexcept:value(std::exchange(other.value,INVALID_HANDLE_VALUE)){}
  handle_t& operator=(handle_t&& other)noexcept{if(this!=&other){reset();value=std::exchange(other.value,INVALID_HANDLE_VALUE);}return *this;}
  ~handle_t(){reset();}void reset(){if(value&&value!=INVALID_HANDLE_VALUE)CloseHandle(value);value=INVALID_HANDLE_VALUE;}
  explicit operator bool()const{return value&&value!=INVALID_HANDLE_VALUE;}
};
std::wstring wide(const std::string& text){
  need(!text.empty()&&text.size()<32700&&text.find('\0')==std::string::npos,"Invalid USB managed text");
  const int count=MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,text.data(),int(text.size()),nullptr,0);need(count>0,"Invalid UTF8");
  std::wstring value(count,L'\0');need(MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,text.data(),int(text.size()),value.data(),count)==count,"UTF8 conversion failed");return value;
}
std::string narrow(const std::wstring& text){
  if(text.empty())return {};const int count=WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,text.data(),int(text.size()),nullptr,0,nullptr,nullptr);need(count>0,"Invalid UTF16");
  std::string value(count,'\0');need(WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,text.data(),int(text.size()),value.data(),count,nullptr,nullptr)==count,"UTF16 conversion failed");return value;
}
std::uint64_t qpc(){LARGE_INTEGER value{};need(QueryPerformanceCounter(&value)!=FALSE,"QPC unavailable");return std::uint64_t(value.QuadPart);}
std::uint64_t created(HANDLE process){FILETIME start{},end{},kernel{},user{};need(GetProcessTimes(process,&start,&end,&kernel,&user)!=FALSE,"Child creation time unavailable");return (std::uint64_t(start.dwHighDateTime)<<32)|start.dwLowDateTime;}
void append_owner_arguments(std::vector<std::wstring>& arguments,DWORD pid,std::uint64_t creation_time){
  need(pid!=0&&creation_time!=0,"USB bridge requires a complete owner PID/creation tuple");
  need(std::find(arguments.begin(),arguments.end(),L"--owner-pid")==arguments.end()&&
       std::find(arguments.begin(),arguments.end(),L"--owner-creation-time")==arguments.end(),"Duplicate USB owner tuple");
  arguments.insert(arguments.end(),{L"--owner-pid",std::to_wstring(pid),L"--owner-creation-time",std::to_wstring(creation_time)});
}
void safe_existing_path(const std::filesystem::path& input,bool directory){
  need(input.is_absolute()&&input.has_root_name()&&input.root_name().wstring().size()==2,"Absolute local drive path required");
  need(input==input.lexically_normal(),"Noncanonical path refused");
  for(auto path=input;!path.empty();path=path.parent_path()){
    const auto attributes=GetFileAttributesW(path.c_str());need(attributes!=INVALID_FILE_ATTRIBUTES&&!(attributes&FILE_ATTRIBUTE_REPARSE_POINT),"Missing or reparse path refused");
    if(path==input)need(bool(attributes&FILE_ATTRIBUTE_DIRECTORY)==directory,"Wrong file/directory kind");
    if(path==path.root_path())break;
  }
}
std::string hash_handle(HANDLE file){
  LARGE_INTEGER zero{};need(SetFilePointerEx(file,zero,nullptr,FILE_BEGIN)!=FALSE,"Cannot rewind pinned image");
  BCRYPT_ALG_HANDLE algorithm=nullptr;need(BCryptOpenAlgorithmProvider(&algorithm,BCRYPT_SHA256_ALGORITHM,nullptr,0)>=0,"SHA256 unavailable");
  struct algorithm_guard{BCRYPT_ALG_HANDLE p;~algorithm_guard(){BCryptCloseAlgorithmProvider(p,0);}} ag{algorithm};
  BCRYPT_HASH_HANDLE hash=nullptr;need(BCryptCreateHash(algorithm,&hash,nullptr,0,nullptr,0,0)>=0,"SHA256 creation failed");
  struct hash_guard{BCRYPT_HASH_HANDLE p;~hash_guard(){BCryptDestroyHash(p);}} hg{hash};
  std::array<UCHAR,65536> bytes{};DWORD count=0;
  do{need(ReadFile(file,bytes.data(),DWORD(bytes.size()),&count,nullptr)!=FALSE,"Pinned image read failed");if(count)need(BCryptHashData(hash,bytes.data(),count,0)>=0,"SHA256 update failed");}while(count);
  std::array<UCHAR,32> digest{};need(BCryptFinishHash(hash,digest.data(),ULONG(digest.size()),0)>=0,"SHA256 finish failed");
  std::string result;constexpr char hex[]="0123456789abcdef";for(auto b:digest){result+=hex[b>>4];result+=hex[b&15];}return result;
}
handle_t pinned_image(const std::filesystem::path& path,const std::string& expected){
  safe_existing_path(path,false);handle_t file(CreateFileW(path.c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr));
  need(bool(file),"Cannot lock pinned executable");need(hash_handle(file.value)==lower(expected),"Pinned executable SHA256 differs");return file;
}
std::wstring current_image(){std::array<wchar_t,32768> path{};const auto length=GetModuleFileNameW(nullptr,path.data(),DWORD(path.size()));need(length&&length<path.size(),"Current executable path unavailable");return {path.data(),length};}
std::wstring random_name(){std::array<UCHAR,16> random{};need(BCryptGenRandom(nullptr,random.data(),ULONG(random.size()),BCRYPT_USE_SYSTEM_PREFERRED_RNG)>=0,"Cryptographic nonce unavailable");constexpr wchar_t hex[]=L"0123456789abcdef";std::wstring value;for(auto b:random){value+=hex[b>>4];value+=hex[b&15];}return value;}
std::wstring token_sid(HANDLE queried_token=nullptr){
  handle_t token;if(queried_token){need(DuplicateHandle(GetCurrentProcess(),queried_token,GetCurrentProcess(),&token.value,TOKEN_QUERY,FALSE,0),"Duplicate user token");}else{need(OpenProcessToken(GetCurrentProcess(),TOKEN_QUERY,&token.value)!=FALSE,"Current user token unavailable");}DWORD bytes=0;GetTokenInformation(token.value,TokenUser,nullptr,0,&bytes);need(bytes>=sizeof(TOKEN_USER)&&bytes<65536,"Token size invalid");
  std::vector<BYTE> data(bytes);need(GetTokenInformation(token.value,TokenUser,data.data(),bytes,&bytes)!=FALSE,"Token user read failed");LPWSTR raw=nullptr;need(ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(data.data())->User.Sid,&raw)!=FALSE,"User SID conversion failed");std::wstring result(raw);LocalFree(raw);return result;
}
std::wstring user_sid(){return token_sid();}
std::wstring setup_user_sid(){
  const auto process_sid=token_sid();
  if(process_sid!=L"S-1-5-18")return process_sid;
  DWORD session=0;need(ProcessIdToSessionId(GetCurrentProcessId(),&session)&&session!=0,"Service audio requires an interactive session");
  handle_t user;need(WTSQueryUserToken(session,&user.value),"No logged-on user in this service session");
  DWORD actual=0,bytes=0;need(GetTokenInformation(user.value,TokenSessionId,&actual,sizeof(actual),&bytes)&&actual==session,"Setup user session differs");
  const auto sid=token_sid(user.value);need(sid!=L"S-1-5-18","Setup must belong to the logged-on account");return sid;
}
struct private_security {
  PSECURITY_DESCRIPTOR descriptor=nullptr;SECURITY_ATTRIBUTES attributes{sizeof(SECURITY_ATTRIBUTES),nullptr,FALSE};
  explicit private_security(const std::wstring& sid,bool inherit=false){const auto flags=inherit?L"OICI":L"";const auto sddl=std::wstring(L"D:P(A;")+flags+L";GA;;;SY)(A;"+flags+L";GA;;;BA)(A;"+flags+L";GA;;;"+sid+L")";need(ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(),SDDL_REVISION_1,&descriptor,nullptr)!=FALSE,"Private DACL creation failed");attributes.lpSecurityDescriptor=descriptor;}
  ~private_security(){if(descriptor)LocalFree(descriptor);}
};
handle_t unique_event(const std::wstring& name,private_security& security){SetLastError(ERROR_SUCCESS);handle_t result(CreateEventW(&security.attributes,TRUE,FALSE,name.c_str()));const auto error=GetLastError();need(bool(result)&&error!=ERROR_ALREADY_EXISTS,"Private event collision or creation failure");return result;}
void write_new(const std::filesystem::path& path,const json& value){
  const auto data=value.dump(2);handle_t file(CreateFileW(path.c_str(),GENERIC_WRITE,0,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr));need(bool(file),"Cannot create immutable managed evidence");DWORD done=0;need(data.size()<1024*1024&&WriteFile(file.value,data.data(),DWORD(data.size()),&done,nullptr)&&done==data.size()&&FlushFileBuffers(file.value),"Managed evidence write failed");
}
json read_binding(const std::filesystem::path& path,std::string& sha){
  safe_existing_path(path,false);handle_t file(CreateFileW(path.c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr));need(bool(file),"Cannot lock binding evidence");LARGE_INTEGER size{};need(GetFileSizeEx(file.value,&size)&&size.QuadPart>0&&size.QuadPart<=65536,"Binding evidence size invalid");sha=hash_handle(file.value);LARGE_INTEGER zero{};need(SetFilePointerEx(file.value,zero,nullptr,FILE_BEGIN)!=FALSE,"Binding rewind failed");std::string data(std::size_t(size.QuadPart),'\0');DWORD read=0;need(ReadFile(file.value,data.data(),DWORD(data.size()),&read,nullptr)&&read==data.size(),"Binding evidence read failed");return json::parse(data);
}

// Setup state is a machine-protected ownership record, not a user-supplied
// controller selector. No device is adopted from a portable JSON configuration.
void protected_setup_handle(HANDLE file) {
  PSID owner=nullptr;PACL acl=nullptr;PSECURITY_DESCRIPTOR descriptor=nullptr;
  need(GetSecurityInfo(file,SE_FILE_OBJECT,OWNER_SECURITY_INFORMATION|DACL_SECURITY_INFORMATION,
      &owner,nullptr,&acl,nullptr,&descriptor)==ERROR_SUCCESS,"Cannot read setup ACL");
  struct release {PSECURITY_DESCRIPTOR value;~release(){LocalFree(value);}} guard{descriptor};
  std::array<BYTE,SECURITY_MAX_SID_SIZE> system{},admins{};DWORD a=DWORD(system.size()),b=DWORD(admins.size());
  need(CreateWellKnownSid(WinLocalSystemSid,nullptr,system.data(),&a)&&
       CreateWellKnownSid(WinBuiltinAdministratorsSid,nullptr,admins.data(),&b),"Cannot create ownership SIDs");
  const auto trusted=[&](PSID sid){return EqualSid(sid,system.data())||EqualSid(sid,admins.data());};
  SECURITY_DESCRIPTOR_CONTROL control{};DWORD revision=0;
  need(owner&&trusted(owner)&&acl&&GetSecurityDescriptorControl(descriptor,&control,&revision)&&
       (control&SE_DACL_PROTECTED),"Setup must be administrator-owned with a protected ACL");
  for(DWORD i=0;i<acl->AceCount;++i){
    void* raw=nullptr;need(GetAce(acl,i,&raw)!=FALSE,"Malformed setup ACE");
    const auto* header=static_cast<ACE_HEADER*>(raw);
    need(header->AceType==ACCESS_ALLOWED_ACE_TYPE||header->AceType==ACCESS_DENIED_ACE_TYPE,"Unsupported setup ACE");
    if(header->AceType!=ACCESS_ALLOWED_ACE_TYPE)continue;
    const auto* ace=static_cast<ACCESS_ALLOWED_ACE*>(raw);
    constexpr DWORD writes=GENERIC_ALL|GENERIC_WRITE|FILE_WRITE_DATA|FILE_APPEND_DATA|FILE_WRITE_EA|
        FILE_WRITE_ATTRIBUTES|DELETE|WRITE_DAC|WRITE_OWNER|FILE_DELETE_CHILD;
    need(trusted(const_cast<DWORD*>(&ace->SidStart))||!(ace->Mask&writes),"Untrusted setup write permission");
  }
}
[[maybe_unused]] std::string owned_setup_controller() {
  struct ci_info {ULONG size=sizeof(ci_info),flags=0;} ci;
  const auto query=reinterpret_cast<LONG(WINAPI*)(ULONG,PVOID,ULONG,PULONG)>(
      GetProcAddress(GetModuleHandleW(L"ntdll.dll"),"NtQuerySystemInformation"));
  ULONG returned=0;
  need(query&&query(103,&ci,sizeof(ci),&returned)>=0&&returned==sizeof(ci)&&
       (ci.flags&1)&&!(ci.flags&2),"Code Integrity required with TESTSIGNING off");
  std::array<wchar_t,MAX_PATH> common{};
  need(SUCCEEDED(SHGetFolderPathW(nullptr,CSIDL_COMMON_APPDATA,nullptr,SHGFP_TYPE_CURRENT,common.data())),"Resolve machine setup directory");
  const auto base=std::filesystem::path(common.data())/L"VibeSpeakers";
  const auto state=base/L"users"/setup_user_sid();
  std::vector<handle_t> directories;
  for(const auto& path:{base,base/L"users",state}) {
    safe_existing_path(path,true);
    handle_t dir(CreateFileW(path.c_str(),READ_CONTROL,FILE_SHARE_READ|FILE_SHARE_WRITE,nullptr,
        OPEN_EXISTING,FILE_FLAG_BACKUP_SEMANTICS|FILE_FLAG_OPEN_REPARSE_POINT,nullptr));
    need(bool(dir),"Cannot lock protected setup directory");protected_setup_handle(dir.value);
    directories.push_back(std::move(dir));
  }
  const auto path=state/L"setup.json";safe_existing_path(path,false);
  handle_t file(CreateFileW(path.c_str(),GENERIC_READ|READ_CONTROL,FILE_SHARE_READ,nullptr,
      OPEN_EXISTING,FILE_FLAG_OPEN_REPARSE_POINT,nullptr));
  need(bool(file),"Vibe Speakers is not set up for this Windows user. Open Audio settings and choose Set up.");
  protected_setup_handle(file.value);
  LARGE_INTEGER size{};need(GetFileSizeEx(file.value,&size)&&size.QuadPart>0&&size.QuadPart<=65536,"Invalid setup record size");
  std::string data(std::size_t(size.QuadPart),'\0');DWORD read=0;
  need(ReadFile(file.value,data.data(),DWORD(data.size()),&read,nullptr)&&read==data.size(),"Cannot read setup record");
  const auto value=json::parse(data);
  need(text(value,"Schema")=="vibe-speakers-setup/1"&&text(value,"UserSid")==narrow(setup_user_sid())&&
       text(value,"Phase")=="Ready"&&value.at("CreatedByVibeshine")==true,"Setup owner/schema/state differs");
  need(_wcsicmp(wide(text(value,"InstallRoot")).c_str(),std::filesystem::path(current_image()).parent_path().c_str())==0,
       "Vibe Speakers setup belongs to another installation");
  const auto id=text(value,"ControllerInstance",200),token=text(value,"Token",32);
  need(controller_text(id)&&token.size()==32&&token.find_first_not_of("0123456789abcdef")==std::string::npos,"Invalid setup binding");
  auto instance=wide(id);DEVINST dev=0;need(CM_Locate_DevNodeW(&dev,instance.data(),CM_LOCATE_DEVNODE_NORMAL)==CR_SUCCESS,"Owned controller is absent");
  const auto property=[&](const DEVPROPKEY& key){
    std::array<wchar_t,512> data{};DEVPROPTYPE type=0;ULONG bytes=sizeof(data);
    need(CM_Get_DevNode_PropertyW(dev,&key,&type,reinterpret_cast<BYTE*>(data.data()),&bytes,0)==CR_SUCCESS&&
         type==DEVPROP_TYPE_STRING&&bytes>=2&&bytes<=sizeof(data)&&bytes%2==0&&data[bytes/2-1]==0,"Cannot verify controller property");
    return std::wstring(data.data());
  };
  need(property(DEVPKEY_Device_FriendlyName)==L"Vibe Speakers "+wide(token)&&
       property(DEVPKEY_Device_Service)==L"usbip2_ude","Controller ownership marker/service differs");
  return id;
}
template<class T>struct com_ptr{T* p=nullptr;~com_ptr(){if(p)p->Release();}T* operator->()const{return p;}};
void hr(HRESULT result,const char* operation){if(FAILED(result))throw std::runtime_error(std::string(operation)+" HRESULT="+std::to_string(static_cast<unsigned long>(result)));}
struct role_com {
  com_ptr<IMMDeviceEnumerator> enumerator;com_ptr<IPolicyConfig> policy;bool initialized=false;
  role_com(){const auto status=CoInitializeEx(nullptr,COINIT_MULTITHREADED);if(status!=RPC_E_CHANGED_MODE){hr(status,"Role COM initialize");initialized=true;}try{hr(CoCreateInstance(__uuidof(MMDeviceEnumerator),nullptr,CLSCTX_ALL,__uuidof(IMMDeviceEnumerator),reinterpret_cast<void**>(&enumerator.p)),"Role enumerator");
#ifdef __MINGW32__
    hr(CoCreateInstance(CLSID_CPolicyConfigClient,nullptr,CLSCTX_ALL,IID_IPolicyConfig,reinterpret_cast<void**>(&policy.p)),"Role policy");
#else
    hr(CoCreateInstance(__uuidof(CPolicyConfigClient),nullptr,CLSCTX_ALL,__uuidof(IPolicyConfig),reinterpret_cast<void**>(&policy.p)),"Role policy");
#endif
  }catch(...){release();throw;}}
  void release()noexcept{if(policy.p){policy.p->Release();policy.p=nullptr;}if(enumerator.p){enumerator.p->Release();enumerator.p=nullptr;}if(initialized){CoUninitialize();initialized=false;}}
  ~role_com(){release();}
};

// The PnP import table and MMDevice default roles converge independently.
// Subscribe before detach so an early removal cannot be missed between reads.
class role_notice final : public IMMNotificationClient {
  std::atomic<ULONG> references_{1};
  std::wstring owned_;
  void signal() noexcept { ++count; SetEvent(changed.value); }
  void endpoint(LPCWSTR id) noexcept { if(id && _wcsicmp(id,owned_.c_str())==0)signal(); }
public:
  handle_t changed{CreateEventW(nullptr,FALSE,FALSE,nullptr)};
  std::atomic<std::uint64_t> count{0};
  explicit role_notice(std::wstring owned):owned_(std::move(owned)){need(bool(changed),"Cannot create role notification event");}
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id,void** out) override {
    if(!out)return E_POINTER;
    *out=nullptr;
    if(id!=__uuidof(IUnknown)&&id!=__uuidof(IMMNotificationClient))return E_NOINTERFACE;
    *out=static_cast<IMMNotificationClient*>(this);AddRef();return S_OK;
  }
  ULONG STDMETHODCALLTYPE AddRef() override {return ++references_;}
  ULONG STDMETHODCALLTYPE Release() override {const auto count_left=--references_;if(!count_left)delete this;return count_left;}
  HRESULT STDMETHODCALLTYPE OnDefaultDeviceChanged(EDataFlow flow,ERole role,LPCWSTR) override {
    if(flow==eRender&&role>=eConsole&&role<=eCommunications)signal();
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE OnDeviceStateChanged(LPCWSTR id,DWORD) override {endpoint(id);return S_OK;}
  HRESULT STDMETHODCALLTYPE OnDeviceAdded(LPCWSTR id) override {endpoint(id);return S_OK;}
  HRESULT STDMETHODCALLTYPE OnDeviceRemoved(LPCWSTR id) override {endpoint(id);return S_OK;}
  HRESULT STDMETHODCALLTYPE OnPropertyValueChanged(LPCWSTR id,const PROPERTYKEY) override {endpoint(id);return S_OK;}
};

class role_watch {
  struct apartment {
    HRESULT status=CoInitializeEx(nullptr,COINIT_MULTITHREADED);
    apartment(){if(status!=RPC_E_CHANGED_MODE)hr(status,"Role notification COM initialize");}
    ~apartment(){if(SUCCEEDED(status))CoUninitialize();}
  } apartment_;
  // Keep the callback alive through unregister and enumerator release.
  com_ptr<role_notice> notice_;
  com_ptr<IMMDeviceEnumerator> enumerator_;
  bool registered_=false;
public:
  explicit role_watch(const std::string& endpoint) {
    hr(CoCreateInstance(__uuidof(MMDeviceEnumerator),nullptr,CLSCTX_ALL,__uuidof(IMMDeviceEnumerator),reinterpret_cast<void**>(&enumerator_.p)),"Role notification enumerator");
    notice_.p=new role_notice(wide(endpoint));
    hr(enumerator_->RegisterEndpointNotificationCallback(notice_.p),"Register role notifications");
    registered_=true;
  }
  ~role_watch(){if(registered_)enumerator_->UnregisterEndpointNotificationCallback(notice_.p);}
  HRESULT close() noexcept {
    if(!registered_)return S_OK;
    const auto status=enumerator_->UnregisterEndpointNotificationCallback(notice_.p);
    if(SUCCEEDED(status)||status==E_NOTFOUND)registered_=false;
    return status;
  }
  DWORD wait(DWORD timeout)const{return WaitForSingleObject(notice_.p->changed.value,timeout);}
  std::uint64_t notifications()const{return notice_.p->count.load();}
};
class usb_control final:public platf::audio_control_t {
  using roles_t=role_control_t<ERole,3>;
  std::shared_ptr<std::string> target_;
  std::string& failure_;
  std::vector<std::string> captured_roles_;
  std::uint64_t captured_qpc_=0;
  static roles_t::registry_t& registry(){static roles_t::registry_t value;return value;}
  roles_t roles_;
  roles_t::ids_t defaults(){role_com api;roles_t::ids_t ids;for(unsigned n=0;n<3;++n){com_ptr<IMMDevice> device;const auto status=api.enumerator->GetDefaultAudioEndpoint(eRender,static_cast<ERole>(n),&device.p);if(status==E_NOTFOUND)continue;hr(status,"Read render role");LPWSTR id=nullptr;hr(device->GetId(&id),"Read role ID");if(id)ids[n]=id;CoTaskMemFree(id);}return ids;}
public:
  explicit usb_control(std::shared_ptr<std::string> target,std::string& failure):target_(std::move(target)),failure_(failure),roles_(registry(),{
    [this]{return defaults();},[](ERole role,const std::wstring& id){role_com api;return static_cast<long>(api.policy->SetDefaultEndpoint(id.c_str(),role));},
    [](const std::wstring& id)->std::optional<std::wstring>{role_com api;com_ptr<IMMDevice> device;DWORD state=0;if(FAILED(api.enumerator->GetDevice(id.c_str(),&device.p))||FAILED(device->GetState(&state))||state!=DEVICE_STATE_ACTIVE)return {};return id;},
    [](std::jthread thread){if(thread.joinable()){thread.request_stop();thread.join();}},{},{},{}
  }){}
  std::optional<platf::sink_t> sink_info()override{
    try {
      const auto ids=defaults(); // E_NOTFOUND is a real absent role. Other HRESULTs still fail.
      roles_.capture(ids);
      captured_roles_={narrow(ids[0]),narrow(ids[1]),narrow(ids[2])};
      captured_qpc_=qpc();
      return platf::sink_t{narrow(ids[0]),std::nullopt};
    }
    catch(const std::exception& exception){failure_=exception.what();return std::nullopt;}
  }
  const std::vector<std::string>& captured_roles()const{return captured_roles_;}
  std::uint64_t captured_qpc()const{return captured_qpc_;}
  std::vector<std::string> render_roles()override{const auto ids=defaults();return {narrow(ids[0]),narrow(ids[1]),narrow(ids[2])};}
  int set_sink(const std::string& sink)override{if(target_->empty()||sink!=*target_)return -1;return roles_.assign(wide(sink));}
  bool adopt_for_restore(const std::string& endpoint){
    if(endpoint.empty()||endpoint!=*target_)return false;
    const auto id=wide(endpoint);
    // An assignment may throw after its first write; do not lose that owner.
    return roles_.assigned()==id||roles_.adopt_for_restore(id);
  }
  int restore_sink(const std::string&)override{return roles_.restore();}
  bool is_sink_available(const std::string& sink)override{if(target_->empty()||sink!=*target_)return false;try{role_com api;com_ptr<IMMDevice> device;DWORD state=0;return SUCCEEDED(api.enumerator->GetDevice(wide(sink).c_str(),&device.p))&&SUCCEEDED(device->GetState(&state))&&state==DEVICE_STATE_ACTIVE;}catch(...){return false;}}
  std::unique_ptr<platf::mic_t> microphone(const std::uint8_t*,int,std::uint32_t,std::uint32_t,bool,bool)override{return {};}
};
// Read-only exact import-table absence, independent of endpoint disappearance.
std::uint16_t profile_product(const atmos_format_t& format){
  const auto id=VibeUsbProduct(format.channels,format.channel_mask,format.container_bits,format.valid_bits);
  need(id!=0,"Unknown USB profile product ID");
  return id;
}
bool owned_import_absent(const usb_source_config_t& source){
  using namespace vibe::usb_audio;auto guid=driver_abi::interface_guid;auto instance=wide(source.controller_instance);ULONG count=0;
  need(CM_Get_Device_Interface_List_SizeW(&count,&guid,instance.data(),CM_GET_DEVICE_INTERFACE_LIST_PRESENT)==CR_SUCCESS&&count>=2&&count<=65536,"Detach query interface unavailable");std::vector<wchar_t> paths(count);
  need(CM_Get_Device_Interface_ListW(&guid,instance.data(),paths.data(),count,CM_GET_DEVICE_INTERFACE_LIST_PRESENT)==CR_SUCCESS,"Detach query enumeration changed");const auto end=std::find(paths.begin(),paths.end(),L'\0');need(end!=paths.begin()&&end!=paths.end()&&end+1!=paths.end()&&*(end+1)==0,"Detach query requires exactly one interface");
  DEVPROPTYPE type=0;std::array<wchar_t,MAX_DEVICE_ID_LEN> actual{};ULONG bytes=sizeof(actual);need(CM_Get_Device_Interface_PropertyW(paths.data(),&DEVPKEY_Device_InstanceId,&type,reinterpret_cast<BYTE*>(actual.data()),&bytes,0)==CR_SUCCESS&&type==DEVPROP_TYPE_STRING&&bytes>=2&&bytes<=sizeof(actual)&&bytes%2==0&&actual[bytes/2-1]==0&&_wcsicmp(actual.data(),instance.c_str())==0,"Detach query controller differs");
  handle_t file(CreateFileW(paths.data(),GENERIC_READ|GENERIC_WRITE,FILE_SHARE_READ|FILE_SHARE_WRITE,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr));need(bool(file),"Detach query open failed");ULONG query=sizeof(driver_abi::get_imported_devices);DWORD returned=0;std::vector<BYTE> rows(driver_abi::imported_devices_offset+255*sizeof(driver_abi::imported_device));
  need(DeviceIoControl(file.value,driver_abi::get_imported_devices_ioctl,&query,sizeof(query),rows.data(),DWORD(rows.size()),&returned,nullptr)&&returned>=driver_abi::imported_devices_offset&&returned<=rows.size()&&(returned-driver_abi::imported_devices_offset)%sizeof(driver_abi::imported_device)==0,"Detach query malformed");
  usb_lab::Identity id{0x1209,profile_product(source.format),wide(source.serial),instance,wide(source.service),int(source.port)};
  for(std::size_t at=driver_abi::imported_devices_offset;at<returned;at+=sizeof(driver_abi::imported_device)){driver_abi::imported_device row{};std::memcpy(&row,rows.data()+at,sizeof(row));if(usb_lab::ImportedMatches(row,id))return false;}return true;
}
void validate_binding(const json& b,const usb_source_config_t& expected,DWORD pid,std::uint64_t creation,DWORD session){
  need(text(b,"Schema")=="vibe-usb-bridge-binding/1"&&text(b,"event")=="binding"&&text(b,"source_kind")=="user_mode_usb"&&integer(b,"qpc_frequency")>0,"Unexpected binding schema/event/source/clock");
  need(integer(b,"pid",UINT32_MAX)==pid&&integer(b,"creation_time")==creation&&integer(b,"windows_session",UINT32_MAX)==session,"Binding process identity differs");
  need(integer(b,"vendor",65535)==0x1209&&integer(b,"product",65535)==profile_product(expected.format)&&integer(b,"channels",12)==expected.format.channels&&integer(b,"channel_mask",UINT32_MAX)==expected.format.channel_mask&&integer(b,"sample_rate",UINT32_MAX)==48000&&integer(b,"container_bits",32)==expected.format.container_bits&&integer(b,"valid_bits",32)==expected.format.valid_bits,"Binding profile differs");
  need(lower(text(b,"controller_instance"))==lower(expected.controller_instance)&&text(b,"pipe_name")==expected.pipe.substr(9),"Binding controller/pipe differs");
  need(b.at("owner_verified").is_boolean()&&b.at("owner_verified").get<bool>()&&
       integer(b,"owner_pid",UINT32_MAX)==GetCurrentProcessId()&&
       integer(b,"owner_creation_time")==created(GetCurrentProcess()),"Verified USB bridge owner differs");
  need(b.at("worker_mmcss_registered").is_boolean()&&b.at("worker_mmcss_registered").get<bool>()&&integer(b,"worker_thread_id",UINT32_MAX)>0,"USB worker MMCSS not registered");
}
#endif
}

#ifdef _WIN32
#ifndef VIBE_SESSION_TESTING
struct usb_session_t::impl_t {
  usb_launch_config_t config;usb_source_config_t bound;std::string failure;
  json evidence={{"Schema","vibe-usb-managed-session/1"},{"started",false},{"ready",false},{"roles_restored",false},{"own_detach_verified",false},{"process_exited",false}};
  std::filesystem::path directory,prefix;
  std::shared_ptr<std::string> target=std::make_shared<std::string>();
  std::unique_ptr<context_manager_t> manager;audio_ctx_ref_t context;
  handle_t bridge_image,self_image,child,ready,stop_event,ownership;
  DWORD child_pid=0,child_session=0;std::uint64_t child_created=0;bool attempted=false,finished=false,assigned=false;unsigned stop_attempts=0;
  explicit impl_t(usb_launch_config_t value):config(std::move(value)){}
  void error(const std::string& message){if(!failure.empty())failure+="; ";failure+=message;evidence["error"]=failure;}
  void receipt(const std::string& suffix) noexcept {try{if(!directory.empty())write_new(directory/("managed-"+suffix+".json"),evidence);}catch(...){/* optional bounded session summary */}}
  void recover_attached_role_target(){
    if(assigned||!child||!context||!context->control||prefix.empty())return;
    const auto path=prefix.wstring()+L".binding.json";
    if(GetFileAttributesW(path.c_str())==INVALID_FILE_ATTRIBUTES){evidence["failed_start_role_target_known"]=false;return;}
    std::string binding_sha;const auto binding=read_binding(path,binding_sha);
    validate_binding(binding,bound,child_pid,child_created,child_session);
    auto candidate=bound;candidate.endpoint=text(binding,"endpoint_id");candidate.usb_instance=text(binding,"usb_instance");candidate.port=unsigned(integer(binding,"port",255));candidate.serial=text(binding,"serial",64);candidate.service=text(binding,"service",5);
    // Cleanup requires real ownership, even if the requested mix format was
    // the reason readiness failed. Never accept an arbitrary JSON endpoint.
    const auto status=CoInitializeEx(nullptr,COINIT_MULTITHREADED);if(status!=RPC_E_CHANGED_MODE)hr(status,"Recovery COM initialize");
    struct apartment{bool owned;~apartment(){if(owned)CoUninitialize();}} apartment_guard{SUCCEEDED(status)};
    const usb_lab::Identity identity{0x1209,profile_product(candidate.format),wide(candidate.serial),wide(candidate.controller_instance),wide(candidate.service),int(candidate.port)};
    const auto actual=usb_lab::Resolve(wide(candidate.endpoint),identity);need(usb_lab::Same(actual.usbInstance,wide(candidate.usb_instance)),"Failed-start recovery USB instance differs");
    auto* control=dynamic_cast<usb_control*>(context->control.get());need(control!=nullptr,"Failed-start control owner differs");
    *target=candidate.endpoint;need(control->adopt_for_restore(candidate.endpoint),"Failed-start role ownership could not be adopted");
    bound=std::move(candidate);assigned=true;evidence["failed_start_role_target_known"]=true;evidence["failed_start_role_adopted_without_writes"]=true;evidence["binding_sha256"]=binding_sha;evidence["binding"]=binding;
  }
  bool start(atmos_format_t format,const std::function<bool()>& cancelled){
    try{
      need(!attempted,"Managed USB sessions are single-use");attempted=true;need(usb_session_t::configured(config,failure),"Managed USB configuration rejected");need(format.valid(),"Unsupported exact native USB format");
      need(!cancelled||!cancelled(),"Managed USB start cancelled");
      bridge_image=pinned_image(wide(config.bridge_executable),config.bridge_sha256);
      const auto self=current_image();safe_existing_path(self,false);self_image=handle_t(CreateFileW(self.c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr));need(bool(self_image),"Cannot lock host executable");const auto self_sha=hash_handle(self_image.value);
      const auto sid=user_sid(),nonce=random_name();private_security event_security(sid),file_security(sid,true);
      auto ownership_name=L"Global\\VibeSpeakersController_"+wide(config.controller_instance);std::replace(ownership_name.begin()+7,ownership_name.end(),L'\\',L'_');std::transform(ownership_name.begin()+7,ownership_name.end(),ownership_name.begin()+7,[](wchar_t c){return wchar_t(towupper(c));});ownership=unique_event(ownership_name,event_security);
      directory=std::filesystem::path(wide(config.evidence_root))/(L"usb-managed-"+nonce);need(CreateDirectoryW(directory.c_str(),&file_security.attributes)!=FALSE,"Cannot create private new evidence directory");prefix=directory/L"bridge";
      const auto ready_name=L"Local\\VibeUsbManaged_"+nonce+L"_ready",stop_name=L"Local\\VibeUsbManaged_"+nonce+L"_stop";ready=unique_event(ready_name,event_security);stop_event=unique_event(stop_name,event_security);
      bound.bridge_executable=config.bridge_executable;bound.bridge_sha256=lower(config.bridge_sha256);bound.controller_instance=config.controller_instance;bound.format=format;bound.sample_format=format.container_bits==16?"pcm16":"pcm24";bound.pipe=R"(\\.\pipe\VibeSpeakersUsb_)"+narrow(nonce);
      evidence["evidence_directory"]=narrow(directory.wstring());evidence["bridge_executable"]=config.bridge_executable;evidence["bridge_sha256"]=bound.bridge_sha256;evidence["host_executable"]=narrow(self);evidence["host_sha256"]=self_sha;evidence["controller_instance"]=config.controller_instance;evidence["pipe"]=bound.pipe;evidence["channels"]=format.channels;evidence["channel_mask"]=format.channel_mask;evidence["container_bits"]=format.container_bits;evidence["valid_bits"]=format.valid_bits;
      manager=std::make_unique<context_manager_t>(context_environment_t{[this]{return std::make_unique<usb_control>(target,failure);},[]{return false;},[]{return std::string{};},{},false});
      context=manager->acquire();need(context&&context->control,"Cannot acquire USB routing snapshot");
      const auto* captured=dynamic_cast<const usb_control*>(context->control.get());need(captured!=nullptr,"Unexpected USB role-control owner");
      evidence["roles_before"]=captured->captured_roles();evidence["roles_snapshot_qpc"]=captured->captured_qpc();
      need(!cancelled||!cancelled(),"Managed USB start cancelled before attach");
      std::vector<std::wstring> args={wide(config.bridge_executable),L"--managed-runtime",L"--controller-instance",wide(config.controller_instance),L"--channels",std::to_wstring(format.channels),L"--channel-mask",std::to_wstring(format.channel_mask),L"--native-format",wide(bound.sample_format),L"--pipe-name",wide(bound.pipe.substr(9)),L"--allowed-client",self,L"--allowed-client-sha256",wide(self_sha),L"--out",prefix.wstring(),L"--ready-event",ready_name,L"--stop-event",stop_name};
      const DWORD owner_pid=GetCurrentProcessId();
      const auto owner_created=created(GetCurrentProcess());
      DWORD owner_session=0;
      need(ProcessIdToSessionId(owner_pid,&owner_session)!=FALSE,"Owner Windows session unavailable");
      append_owner_arguments(args,owner_pid,owner_created);
#ifdef VIBE_AUDIO_TEST_INSTRUMENTATION
      args.insert(args.end(),{L"--seconds",std::to_wstring(config.seconds)});
#endif
      evidence["owner_binding"]={{"pid",owner_pid},{"creation_time",owner_created},{"windows_session",owner_session},{"image",narrow(self)},{"sha256",self_sha}};
      if(config.raw)args.push_back(L"--raw");if(!config.detailed)args.push_back(L"--summary-only");std::wstring command;for(const auto& arg:args){if(!command.empty())command+=L' ';command+=quote(arg);}need(command.size()<32700,"Managed command exceeds limit");
      STARTUPINFOW startup{};startup.cb=sizeof(startup);PROCESS_INFORMATION process{};evidence["launch_qpc"]=qpc();
      need(CreateProcessW(wide(config.bridge_executable).c_str(),command.data(),nullptr,nullptr,FALSE,CREATE_NO_WINDOW,nullptr,std::filesystem::path(wide(config.bridge_executable)).parent_path().c_str(),&startup,&process)!=FALSE,"Pinned USB bridge launch failed");
      child=handle_t(process.hProcess);handle_t child_thread(process.hThread);child_pid=process.dwProcessId;child_created=created(child.value);DWORD windows_session=0;need(ProcessIdToSessionId(child_pid,&windows_session)!=FALSE,"Child Windows session unavailable");DWORD current_session=0;need(ProcessIdToSessionId(GetCurrentProcessId(),&current_session)&&current_session==windows_session,"Child session differs");
      child_session=windows_session;
      evidence["started"]=true;evidence["pid"]=child_pid;evidence["creation_time"]=child_created;evidence["windows_session"]=windows_session;
      const auto deadline=GetTickCount64()+45000;const HANDLE waits[]{child.value,ready.value};
      for(;;){need(!cancelled||!cancelled(),"Managed USB readiness cancelled");const auto now=GetTickCount64();need(now<deadline,"Managed USB readiness timed out");const auto result=WaitForMultipleObjects(2,waits,FALSE,DWORD(std::min<ULONGLONG>(100,deadline-now)));if(result==WAIT_OBJECT_0+1)break;need(result==WAIT_TIMEOUT,"USB bridge exited before readiness");}
      need(WaitForSingleObject(child.value,0)==WAIT_TIMEOUT&&created(child.value)==child_created,"Ready child identity lost");std::string binding_sha;const auto binding=read_binding(prefix.wstring()+L".binding.json",binding_sha);validate_binding(binding,bound,child_pid,child_created,windows_session);
      bound.endpoint=text(binding,"endpoint_id");bound.usb_instance=text(binding,"usb_instance");bound.port=unsigned(integer(binding,"port",255));bound.serial=text(binding,"serial",64);bound.service=text(binding,"service",5);
      // Cached per-port Windows mix state can survive a changed USB profile.
      // Mutate only the endpoint proven by the current owned import tuple.
      auto format_backend=make_owned_usb_format_backend(bound);
      const auto previous_format=format_backend->read();
      const auto desired_format=format_backend->target(format);
      evidence["format_change_required"]=previous_format!=desired_format;
      if(previous_format!=desired_format){
        format_backend->set(desired_format);
        const auto observed=format_backend->read();
        const auto settled=format_backend->settle(desired_format,observed,format_backend->read_public_mix());
        need(settled.target_matched,"Owned USB format did not settle");
        evidence["format_settle_reads"]=settled.reads;
        evidence["format_settle_qpc"]=settled.final_read_qpc;
      }
      std::string binding_error;const auto binding_ok=usb_source_t::inspect_binding(bound,binding_error);need(binding_ok,binding_error.empty()?"Read-only USB binding check failed":binding_error.c_str());evidence["binding_sha256"]=binding_sha;evidence["binding"]=binding;evidence["ready"]=true;evidence["ready_qpc"]=qpc();
      need(!cancelled||!cancelled(),"Managed USB start cancelled before role assignment");
      *target=bound.endpoint;const auto assignment=manager->assign(*context.get(),bound.endpoint,false);assigned=true;need(assignment==0,"USB render-role assignment failed");evidence["roles_assigned"]=context->control->render_roles();need(std::all_of(evidence["roles_assigned"].begin(),evidence["roles_assigned"].end(),[this](const json& id){return id==bound.endpoint;}),"USB render-role assignment readback differs");evidence["assignment_qpc"]=qpc();receipt("ready");return true;
    }catch(const std::exception& exception){error(exception.what());stop();return false;}
  }
  bool stop() noexcept {
    if(finished)return failure.empty();bool ok=true;
    try{recover_attached_role_target();}catch(const std::exception& exception){ok=false;error(std::string("Failed-start ownership recovery: ")+exception.what());}
    std::unique_ptr<role_watch> watch;
    if(child&&context&&context->control&&!bound.endpoint.empty()) {
      try {
        watch=std::make_unique<role_watch>(bound.endpoint);
        evidence["role_watch_registered_qpc"]=qpc();
        evidence["roles_at_watch_registration"]=context->control->render_roles();
      }catch(const std::exception& exception){ok=false;error(exception.what());}
    }
    try{
      if(context&&context->control&&!evidence.contains("roles_before_detach")){
        const auto before=context->control->render_roles();evidence["roles_before_restore"]=before;
        bool conflict=false;if(assigned)for(std::size_t i=0;i<before.size();++i)if(before[i]!=bound.endpoint&&before[i]!=evidence["roles_before"].at(i).get<std::string>())conflict=true;
        evidence["role_conflict_preserved"]=conflict;
        const auto result=assigned?context->control->restore_sink(context->sink.host):0;context->restore_sink=false;
        const auto after=context->control->render_roles();
        bool restored=result==0&&after.size()==before.size();
        for(std::size_t i=0;restored&&i<after.size();++i){
          const auto original=evidence["roles_before"].at(i).get<std::string>();
          // Only our assigned roles may be changed. Preserve later user choices.
          const bool originally_absent=original.empty();
          const auto expected=assigned&&before[i]==bound.endpoint&&!originally_absent?original:before[i];
          // Windows removes an initially absent default when its USB device
          // disappears. role_control never attempts to set an empty device ID.
          if(after[i]!=expected&&!(originally_absent&&after[i].empty()))restored=false;
          if(!assigned&&after[i]!=original)restored=false; // no claim after incomplete attach
        }
        evidence["role_restore_result"]=result;evidence["roles_before_detach"]=after;evidence["restore_qpc"]=qpc();evidence["existing_roles_restored_before_detach"]=restored;
        if(!restored){ok=false;error("Render-role restore readback incomplete or topology changed before assignment");}
        if(result!=0){ok=false;error("Render-role restoration returned failure");}
      }
    }catch(const std::exception& exception){ok=false;error(exception.what());if(context)context->restore_sink=false;}
    try{
      // Restoration precedes stop/detach. No child PID search and no kill.
      if(child){need(SetEvent(stop_event.value)!=FALSE,"Owned stop event signal failed");evidence["stop_signaled"]=true;evidence["stop_qpc"]=qpc();const auto waited=WaitForSingleObject(child.value,15000);evidence["process_exited"]=waited==WAIT_OBJECT_0;
        if(waited!=WAIT_OBJECT_0){ok=false;error("Owned bridge did not exit within 15 seconds; never force-killed");}
        else{DWORD code=STILL_ACTIVE;need(GetExitCodeProcess(child.value,&code)!=FALSE,"Child exit code unavailable");need(created(child.value)==child_created,"Retained process creation time differs");evidence["exit_code"]=code;if(code){ok=false;error("Owned bridge returned nonzero exit");}
          if(!bound.serial.empty()){const auto absent=owned_import_absent(bound);evidence["own_detach_verified"]=absent;if(!absent){ok=false;error("Exact owned USB import remains after bridge exit");}}
          child.reset();bridge_image.reset();self_image.reset();finished=true;
        }
      }else{finished=true;}
    }catch(const std::exception& exception){ok=false;error(exception.what());}
    try {
      if(finished&&context&&context->control&&evidence.contains("roles_before_detach")) {
        auto after=context->control->render_roles();
        const auto original=evidence.at("roles_before").get<std::vector<std::string>>();
        const auto before_detach=evidence.at("roles_before_detach").get<std::vector<std::string>>();
        constexpr DWORD wait_budget_ms=2000;
        const auto begin_qpc=qpc();
        const auto deadline=GetTickCount64()+wait_budget_ms;
        json observations=json::array();
        unsigned read_count=1;
        observations.push_back({{"qpc",begin_qpc},{"roles",after}});
        bool timed_out=false;
        // Only a lingering ID of our removed device is a topology transition.
        // A different endpoint is a real observation and is never overwritten.
        while(watch&&std::find(after.begin(),after.end(),bound.endpoint)!=after.end()) {
          const auto now=GetTickCount64();
          const DWORD result=now>=deadline?WAIT_TIMEOUT:watch->wait(DWORD(deadline-now));
          need(result==WAIT_OBJECT_0||result==WAIT_TIMEOUT,"Role notification wait failed");
          timed_out=result==WAIT_TIMEOUT;
          after=context->control->render_roles(); // includes the final timeout read
          ++read_count;
          if(observations.size()<32)observations.push_back({{"qpc",qpc()},{"roles",after}});
          if(timed_out)break;
        }
        evidence["role_settle"]={{"begin_qpc",begin_qpc},{"end_qpc",qpc()},{"budget_ms",wait_budget_ms},{"timed_out",timed_out},{"reads",read_count},{"notifications",watch?watch->notifications():0},{"observations",observations},{"final_roles",after}};
        const bool complete=after.size()==3&&original.size()==3&&before_detach.size()==3;
        bool exact=complete;
        json absence=json::array(),conflicts=json::array();
        for(std::size_t i=0;complete&&i<after.size();++i) {
          const bool foreign=assigned&&!before_detach[i].empty()&&before_detach[i]!=bound.endpoint&&before_detach[i]!=original[i];
          const auto& expected=foreign?before_detach[i]:original[i];
          if(original[i].empty())absence.push_back({{"role",i},{"absent_after_detach",after[i].empty()}});
          if(foreign)conflicts.push_back({{"role",i},{"endpoint",before_detach[i]},{"preserved",after[i]==before_detach[i]}});
          if(after[i]!=expected)exact=false;
        }
        evidence["roles_after"]=after;evidence["originally_absent_roles"]=absence;evidence["role_conflicts"]=conflicts;
        evidence["role_conflict_preserved"]=!conflicts.empty()&&exact;
        evidence["roles_restored"]=exact&&evidence.value("existing_roles_restored_before_detach",false);
        evidence["roles_after_detach_qpc"]=qpc();
        if(!evidence["roles_restored"].get<bool>()){ok=false;error("Render roles differ after owned USB detach");}
      }
      if(watch){const auto status=watch->close();evidence["role_watch_unregister_hresult"]=static_cast<long>(status);hr(status,"Unregister role notifications");}
      if(finished){if(context)context={};manager.reset();ownership.reset();}
      evidence["stop_complete"]=ok&&finished;evidence["end_qpc"]=qpc();receipt("stop-"+std::to_string(++stop_attempts));
    }catch(const std::exception& exception){ok=false;error(exception.what());}
    return ok&&finished&&failure.empty();
  }
  ~impl_t(){
    stop();
    if(child&&WaitForSingleObject(child.value,0)==WAIT_TIMEOUT){
      // Retain the exact process/image/controller ownership until real exit,
      // even when a caller releases a failed session. No forced termination.
      struct retired {handle_t process,image,self,lease,stop;};
      auto* state=new(std::nothrow) retired{std::move(child),std::move(bridge_image),std::move(self_image),std::move(ownership),std::move(stop_event)};
      if(state){try{std::thread([state]{WaitForSingleObject(state->process.value,INFINITE);delete state;}).detach();}catch(...){/* Keep ownership resources until process shutdown. */}}
    }
  }
};
usb_launch_config_t usb_session_t::bundled_configuration(usb_launch_config_t config){
#if !SUNSHINE_ENABLE_VIBE_SPEAKERS
  config.bridge_executable.clear();config.bridge_sha256.clear();config.controller_instance.clear();
  config.setup_error="Vibe Speakers is not included in this build";
  return config;
#else
  try {
#ifndef VIBE_AUDIO_TEST_INSTRUMENTATION
    config.seconds=0;config.detailed=false;config.raw=false;
    config.evidence_root.clear();
#endif
    const auto directory=std::filesystem::path(current_image()).parent_path();
    const auto bridge=directory/L"tools"/L"VibeUsbBridge.exe";safe_existing_path(bridge,false);
    handle_t image(CreateFileW(bridge.c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr));
    need(bool(image),"Cannot pin bundled VibeUsbBridge");
    config.bridge_executable=narrow(bridge.wstring());config.bridge_sha256=hash_handle(image.value);
    config.controller_instance=owned_setup_controller();
    std::array<wchar_t,32768> local{};
    const auto length=GetEnvironmentVariableW(L"LOCALAPPDATA",local.data(),DWORD(local.size()));
    need(length&&length<local.size(),"Local application data path unavailable");
    const auto sessions=config.evidence_root.empty()?std::filesystem::path(local.data())/L"Vibeshine"/L"VibeSpeakers"/L"sessions":std::filesystem::path(wide(config.evidence_root));
    std::filesystem::create_directories(sessions);safe_existing_path(sessions,true);
    try{
      std::vector<std::filesystem::path> completed;
      for(const auto& entry:std::filesystem::directory_iterator(sessions)){
        if(!entry.is_directory()||!entry.path().filename().wstring().starts_with(L"usb-managed-"))continue;
        safe_existing_path(entry.path(),true);bool done=false;
        for(const auto& file:std::filesystem::directory_iterator(entry.path()))
          if(file.path().filename().wstring().starts_with(L"managed-stop-")&&std::filesystem::file_size(file.path())<65536){
            safe_existing_path(file.path(),false);std::ifstream stream(file.path());json receipt;
            if(stream>>receipt)done=done||receipt.value("stop_complete",false);
          }
        if(done)completed.push_back(entry.path());
      }
      std::sort(completed.begin(),completed.end(),[](const auto& a,const auto& b){return std::filesystem::last_write_time(a)>std::filesystem::last_write_time(b);});
      for(std::size_t i=31;i<completed.size();++i){
        bool ordinary=true;for(const auto& file:std::filesystem::directory_iterator(completed[i])){
          const auto attributes=GetFileAttributesW(file.path().c_str());
          if(attributes==INVALID_FILE_ATTRIBUTES||(attributes&(FILE_ATTRIBUTE_REPARSE_POINT|FILE_ATTRIBUTE_DIRECTORY)))ordinary=false;
        }
        if(ordinary){for(const auto& file:std::filesystem::directory_iterator(completed[i]))std::filesystem::remove(file.path());std::filesystem::remove(completed[i]);}
      }
    }catch(...){/* optional diagnostics retention cannot veto playback */}
    config.evidence_root=narrow(sessions.wstring());
  } catch(const std::exception& error) {
    // configured() reports missing or invalid package data before a stream is accepted.
    config.bridge_executable.clear();config.bridge_sha256.clear();
    config.setup_error=std::string("Vibe Speakers setup/package validation failed: ")+error.what();
  }
  return config;
#endif
}
bool usb_session_t::configured(const usb_launch_config_t& config,std::string& error){
  if(!config.setup_error.empty()){error=config.setup_error;return false;}
  try{error.clear();need(hash_text(config.bridge_sha256)&&controller_text(config.controller_instance)&&config.seconds<=86400&&(!config.raw||config.detailed),"USB launch hash/controller/duration/diagnostics invalid");safe_existing_path(wide(config.bridge_executable),false);need(lower(std::filesystem::path(wide(config.bridge_executable)).extension().string())==".exe","USB bridge must be an executable");safe_existing_path(wide(config.evidence_root),true);return true;}catch(const std::exception& exception){error=exception.what();return false;}
}
#endif // production lifecycle; guard builds compile the same pure helpers

#else
usb_launch_config_t usb_session_t::bundled_configuration(usb_launch_config_t config){return config;}
struct usb_session_t::impl_t {usb_source_config_t bound;std::string failure="Managed USB requires Windows";explicit impl_t(usb_launch_config_t){}bool start(atmos_format_t,const std::function<bool()>&){return false;}bool stop()noexcept{return true;}};
bool usb_session_t::configured(const usb_launch_config_t&,std::string& error){error="Managed USB requires Windows";return false;}
#endif
#ifndef VIBE_SESSION_TESTING
namespace {
std::mutex managed_session_mutex;
std::weak_ptr<usb_session_t> prepared_session;
}
std::shared_ptr<usb_session_t> usb_session_t::acquire(usb_launch_config_t config,atmos_format_t format,
    std::string& error,const std::function<bool()>& cancelled){
  std::lock_guard lock(managed_session_mutex);
  if(auto existing=prepared_session.lock()){
#ifdef _WIN32
    if(!existing->impl_->finished){
      if(existing->source().format!=format){error="Vibe Speakers is already in use with another audio profile";return {};}
      return existing;
    }
#endif
  }
  auto session=std::make_shared<usb_session_t>(std::move(config));
  if(!session->start(format,cancelled)){error=session->error();return {};}
  prepared_session=session;return session;
}
usb_session_t::usb_session_t(usb_launch_config_t config):impl_(std::make_unique<impl_t>(std::move(config))){}
usb_session_t::~usb_session_t()=default;
bool usb_session_t::start(atmos_format_t format,const std::function<bool()>& cancelled){return impl_->start(format,cancelled);}
bool usb_session_t::stop()noexcept{std::lock_guard lock(managed_session_mutex);return impl_->stop();}
const usb_source_config_t& usb_session_t::source()const{return impl_->bound;}
const std::string& usb_session_t::error()const{return impl_->failure;}
std::string usb_session_t::evidence_json()const{
#ifdef _WIN32
  return impl_->evidence.dump();
#else
  return "{}";
#endif
}

std::string usb_session_t::availability_json(){
  json status={{"available",false},{"ready",false},{"setup_required",false},{"message","Vibe Speakers is not included in this build"}};
#if defined(_WIN32) && SUNSHINE_ENABLE_VIBE_SPEAKERS
  status["available"]=true;
  try{const auto controller=owned_setup_controller();status["ready"]=true;status["message"]="Ready";}
  catch(const std::exception& error){status["setup_required"]=true;status["message"]=error.what();}
#endif
  return status.dump();
}
bool usb_session_t::request_setup(std::string& error){
#if defined(_WIN32) && SUNSHINE_ENABLE_VIBE_SPEAKERS
  try{
    const auto root=std::filesystem::path(current_image()).parent_path();
    const auto script=root/L"VibeSpeakers"/L"Setup"/L"Show-Setup.ps1";
    safe_existing_path(script,false);
    // Never elevate a setup payload from an untrusted writable location.
    // The integrated dialog is an installed-product action, not a lab launcher.
    std::vector<std::filesystem::path> chain{root};
    for(auto path=script;path!=root;path=path.parent_path()){
      need(!path.empty()&&path!=path.root_path(),"Setup path escaped its installation");
      chain.push_back(path);
    }
    for(const auto& path:chain){
      handle_t file(CreateFileW(path.c_str(),READ_CONTROL,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_FLAG_BACKUP_SEMANTICS|FILE_FLAG_OPEN_REPARSE_POINT,nullptr));
      need(bool(file),"Cannot inspect installed setup permissions");
      PSID owner=nullptr;PACL acl=nullptr;PSECURITY_DESCRIPTOR descriptor=nullptr;
      need(GetSecurityInfo(file.value,SE_FILE_OBJECT,OWNER_SECURITY_INFORMATION|DACL_SECURITY_INFORMATION,&owner,nullptr,&acl,nullptr,&descriptor)==ERROR_SUCCESS,"Cannot inspect setup ownership");
      struct release{PSECURITY_DESCRIPTOR value;~release(){LocalFree(value);}} guard{descriptor};
      std::array<BYTE,SECURITY_MAX_SID_SIZE> system{},admins{};DWORD a=DWORD(system.size()),b=DWORD(admins.size());
      need(CreateWellKnownSid(WinLocalSystemSid,nullptr,system.data(),&a)&&CreateWellKnownSid(WinBuiltinAdministratorsSid,nullptr,admins.data(),&b),"Cannot inspect trusted principals");
      const auto trusted=[&](PSID sid){return EqualSid(sid,system.data())||EqualSid(sid,admins.data());};
      need(owner&&trusted(owner)&&acl,"Install Vibeshine in an administrator-protected directory before setup");
      for(DWORD i=0;i<acl->AceCount;++i){void* raw=nullptr;need(GetAce(acl,i,&raw),"Invalid payload ACL");const auto* header=static_cast<ACE_HEADER*>(raw);
        need(header->AceType==ACCESS_ALLOWED_ACE_TYPE||header->AceType==ACCESS_DENIED_ACE_TYPE,"Unsupported payload ACL");
        if(header->AceType==ACCESS_ALLOWED_ACE_TYPE){const auto* ace=static_cast<ACCESS_ALLOWED_ACE*>(raw);
          constexpr DWORD writes=GENERIC_ALL|GENERIC_WRITE|FILE_WRITE_DATA|FILE_APPEND_DATA|FILE_WRITE_EA|FILE_WRITE_ATTRIBUTES|DELETE|WRITE_DAC|WRITE_OWNER|FILE_DELETE_CHILD;
          need(trusted(const_cast<DWORD*>(&ace->SidStart))||!(ace->Mask&writes),"Setup files are writable by an untrusted principal");}
      }
    }
    std::array<wchar_t,MAX_PATH> windows{};need(GetWindowsDirectoryW(windows.data(),DWORD(windows.size())),"Resolve Windows directory");
    const auto executable=std::filesystem::path(windows.data())/L"System32"/L"WindowsPowerShell"/L"v1.0"/L"powershell.exe";
    auto command=quote(executable.wstring())+L" -NoProfile -STA -ExecutionPolicy RemoteSigned -File "+quote(script.wstring());
    STARTUPINFOW si{};si.cb=sizeof(si);si.lpDesktop=const_cast<LPWSTR>(L"winsta0\\default");PROCESS_INFORMATION pi{};
    if(token_sid()==L"S-1-5-18"){
      DWORD session=0;need(ProcessIdToSessionId(GetCurrentProcessId(),&session)&&session,"Interactive service session required");
      handle_t user;need(WTSQueryUserToken(session,&user.value),"Logged-on setup user unavailable");
      void* environment=nullptr;need(CreateEnvironmentBlock(&environment,user.value,FALSE),"Setup user environment unavailable");
      const auto launched=CreateProcessAsUserW(user.value,executable.c_str(),command.data(),nullptr,nullptr,FALSE,CREATE_UNICODE_ENVIRONMENT,environment,root.c_str(),&si,&pi);const auto code=GetLastError();DestroyEnvironmentBlock(environment);
      need(launched, ("Cannot open setup in the logged-on user session ("+std::to_string(code)+")").c_str());
    }else need(CreateProcessW(executable.c_str(),command.data(),nullptr,nullptr,FALSE,0,nullptr,root.c_str(),&si,&pi),"Cannot open setup dialog");
    handle_t process(pi.hProcess),thread(pi.hThread);return true;
  }catch(const std::exception& exception){error=exception.what();return false;}
#else
  error="Vibe Speakers is not included in this build";return false;
#endif
}

#endif // product entry points

#ifdef VIBE_SESSION_TESTING
#include "../tests/host714/usb_session_guard_cases.inl"
#endif

}

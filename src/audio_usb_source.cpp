#include "audio_usb_source.h"
#include "audio_usb_validation.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <utility>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <bcrypt.h>
#include <initguid.h>
#include "usb_audio_identity.h"
#include <audioclient.h>
#endif

namespace audio {
#ifdef _WIN32
namespace {
  struct handle_t {
    HANDLE value = INVALID_HANDLE_VALUE;
    ~handle_t() { if (value != INVALID_HANDLE_VALUE && value) CloseHandle(value); }
  };
  std::wstring wide(const std::string &text) {
    if (text.size() > 32700) throw std::runtime_error("USB argument exceeds Windows path limit");
    int n=MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,text.data(),static_cast<int>(text.size()),nullptr,0);
    if (!n) throw std::runtime_error("Invalid UTF-8 USB argument");
    std::wstring result(static_cast<std::size_t>(n),L'\0');
    MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,text.data(),static_cast<int>(text.size()),result.data(),n);
    return result;
  }
  template<std::size_t N> bool put_text(char16_t (&to)[N], const std::string &value) {
    auto text=wide(value);
    if (text.empty() || text.size() >= N) return false;
    std::copy(text.begin(),text.end(),to);
    return true;
  }
  std::string file_hash(HANDLE file) {
    BCRYPT_ALG_HANDLE algorithm=nullptr;
    if (BCryptOpenAlgorithmProvider(&algorithm,BCRYPT_SHA256_ALGORITHM,nullptr,0)<0) throw std::runtime_error("SHA256 unavailable");
    struct close_algorithm { BCRYPT_ALG_HANDLE p; ~close_algorithm(){BCryptCloseAlgorithmProvider(p,0);} } ac{algorithm};
    BCRYPT_HASH_HANDLE hash=nullptr;
    if (BCryptCreateHash(algorithm,&hash,nullptr,0,nullptr,0,0)<0) throw std::runtime_error("SHA256 allocation failed");
    struct close_hash { BCRYPT_HASH_HANDLE p; ~close_hash(){BCryptDestroyHash(p);} } hc{hash};
    std::array<UCHAR,65536> bytes{};
    DWORD n=0;
    do {
      if (!ReadFile(file,bytes.data(),static_cast<DWORD>(bytes.size()),&n,nullptr)) throw std::runtime_error("Bridge hash read failed");
      if (n && BCryptHashData(hash,bytes.data(),n,0)<0) throw std::runtime_error("Bridge hash update failed");
    } while(n);
    std::array<UCHAR,32> digest{};
    if (BCryptFinishHash(hash,digest.data(),static_cast<ULONG>(digest.size()),0)<0) throw std::runtime_error("Bridge hash finish failed");
    std::string text;
    constexpr char hex[]="0123456789abcdef";
    for(auto b:digest){text.push_back(hex[b>>4]);text.push_back(hex[b&15]);}
    return text;
  }
  bool config_valid(const usb_source_config_t &config, std::string &error) {
    const auto& format=config.format;
    if (!format.valid() || !VibeUsbProduct(format.channels,format.channel_mask,format.container_bits,format.valid_bits) ||
        !config.pipe.starts_with(R"(\\.\pipe\VibeSpeakersUsb)") || config.pipe.size()>180 ||
        config.pipe.find_first_of("\r\n")!=std::string::npos || config.endpoint.empty() ||
        config.usb_instance.empty() || config.controller_instance.empty() || config.serial.size()!=15 ||
        config.serial.substr(0,2)!="VA" || config.serial.substr(2).find_first_not_of("0123456789abcdefABCDEF")!=std::string::npos ||
        !config.port || config.port>255 || config.service.empty() || config.service.size()>5 ||
        config.service.find_first_not_of("0123456789")!=std::string::npos || std::stoul(config.service)<1 || std::stoul(config.service)>65535 || config.bridge_executable.empty() ||
        config.bridge_sha256.size()!=64 || config.bridge_sha256.find_first_not_of("0123456789abcdefABCDEF")!=std::string::npos) {
      error="Vibe Speakers USB requires an exact owned USB binding, native profile and pinned local bridge";
      return false;
    }
    return true;
  }
  std::uint64_t creation_time(HANDLE process) {
    FILETIME created{},exited{},kernel{},user{};
    if(!GetProcessTimes(process,&created,&exited,&kernel,&user))throw std::runtime_error("Cannot query USB peer creation time");
    return (static_cast<std::uint64_t>(created.dwHighDateTime)<<32)|created.dwLowDateTime;
  }
  std::vector<unsigned char> user_token(HANDLE process) {
    handle_t token;if(!OpenProcessToken(process,TOKEN_QUERY,&token.value))throw std::runtime_error("Cannot query USB peer owner");
    DWORD bytes=0;GetTokenInformation(token.value,TokenUser,nullptr,0,&bytes);
    if(bytes<sizeof(TOKEN_USER)||bytes>65536)throw std::runtime_error("USB owner token bound");
    std::vector<unsigned char> data(bytes);
    if(!GetTokenInformation(token.value,TokenUser,data.data(),bytes,&bytes))throw std::runtime_error("USB owner token query failed");
    return data;
  }
  void verify_endpoint(const usb_source_config_t& config) {
    const auto co=CoInitializeEx(nullptr,COINIT_MULTITHREADED);
    if(FAILED(co)&&co!=RPC_E_CHANGED_MODE)throw std::runtime_error("USB endpoint COM initialization failed");
    struct end_com {bool owned;~end_com(){if(owned)CoUninitialize();}} cleanup{SUCCEEDED(co)};
    const auto& f=config.format;
    const auto product=VibeUsbProduct(f.channels,f.channel_mask,f.container_bits,f.valid_bits);
    if(!product)throw std::runtime_error("USB format has no private product ID");
    usb_lab::Identity id{0x1209,product,wide(config.serial),wide(config.controller_instance),wide(config.service),static_cast<int>(config.port)};
    const auto bound=usb_lab::Resolve(wide(config.endpoint),id);
    if(!usb_lab::Same(bound.usbInstance,wide(config.usb_instance)))throw std::runtime_error("USB physical instance mismatch");
    Microsoft::WRL::ComPtr<IAudioClient> client;
    usb_lab::Check(bound.device->Activate(__uuidof(IAudioClient),CLSCTX_INPROC_SERVER,nullptr,reinterpret_cast<void**>(client.GetAddressOf())),"USB audio client unavailable");
    WAVEFORMATEX* format=nullptr;usb_lab::Check(client->GetMixFormat(&format),"USB mix format unavailable");
    struct release_format {WAVEFORMATEX* p;~release_format(){CoTaskMemFree(p);}} release{format};
    if(!format||format->wFormatTag!=WAVE_FORMAT_EXTENSIBLE||format->cbSize<22||format->nSamplesPerSec!=48000||
       format->nChannels!=config.format.channels||format->wBitsPerSample!=32||format->nBlockAlign!=config.format.channels*4)
      throw std::runtime_error("USB endpoint mix profile differs from requested profile");
    const auto* ex=reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(format);
    const GUID floatGuid{3,0,0x10,{0x80,0,0,0xaa,0,0x38,0x9b,0x71}};
    if(ex->dwChannelMask!=config.format.channel_mask||ex->SubFormat!=floatGuid||ex->Samples.wValidBitsPerSample!=32)
      throw std::runtime_error("USB endpoint mix layout is not exact Float32");
  }
}
struct usb_source_t::impl_t {
  usb_source_config_t config;
  usb_source_statistics_t stats;
  usb_end_reason_e end_reason=usb_end_reason_e::none;
  std::uint32_t prior_epoch=0;
  std::uint64_t prior_receive_qpc=0;
  vibe_input::AudioHeader overflow_origin{};
  std::uint64_t loss_frames=0;
  std::string failure;
  HANDLE pipe=INVALID_HANDLE_VALUE;
  handle_t event,peer,image;
  OVERLAPPED overlapped{};
  bool pending=false;
  usb_block_validator_t validator;
  std::uint64_t receive_qpc=0;
  vibe_input::WelcomeMessage welcome{};
  vibe_input::AudioMessage message{};
  vibe_input::AudioHeader previous{};
  explicit impl_t(usb_source_config_t value):config(std::move(value)) {
    event.value=CreateEventW(nullptr,TRUE,FALSE,nullptr);
    if(!event.value) throw std::runtime_error("Cannot create USB read event");
    overlapped.hEvent=event.value;
  }
  ~impl_t(){close();}
  void close() {
    if(pipe!=INVALID_HANDLE_VALUE){
      if(pending){CancelIoEx(pipe,&overlapped);DWORD ignored=0;GetOverlappedResult(pipe,&overlapped,&ignored,TRUE);pending=false;}
      CloseHandle(pipe);pipe=INVALID_HANDLE_VALUE;
    }
    if(peer.value&&peer.value!=INVALID_HANDLE_VALUE){CloseHandle(peer.value);peer.value=INVALID_HANDLE_VALUE;}
    if(image.value&&image.value!=INVALID_HANDLE_VALUE){CloseHandle(image.value);image.value=INVALID_HANDLE_VALUE;}
  }
  bool fail(const std::string &text){failure=text;++stats.protocol_errors;close();return false;}
  bool write_message(const void *data, DWORD bytes) {
    handle_t signal;signal.value=CreateEventW(nullptr,TRUE,FALSE,nullptr);
    if(!signal.value) return fail("Cannot create USB handshake event");
    OVERLAPPED ov{};ov.hEvent=signal.value;
    DWORD done=0;
    if(!WriteFile(pipe,data,bytes,&done,&ov)){
      if(GetLastError()!=ERROR_IO_PENDING) return fail("USB handshake write failed");
      if(WaitForSingleObject(signal.value,2000)!=WAIT_OBJECT_0){
        CancelIoEx(pipe,&ov);GetOverlappedResult(pipe,&ov,&done,TRUE);
        return fail("USB handshake write timed out");
      }
      if(!GetOverlappedResult(pipe,&ov,&done,FALSE)) return fail("USB handshake write completion failed");
    }
    return done==bytes || fail("USB handshake truncated");
  }
  // Preserve the outstanding read across timeouts. Cancelling a partially
  // transferred message would discard bytes and silently corrupt framing.
  int read_message(DWORD timeout, DWORD &bytes) {
    if(!pending){
      ResetEvent(event.value);overlapped={};overlapped.hEvent=event.value;
      if(ReadFile(pipe,&message,sizeof(message),&bytes,&overlapped)) {
        LARGE_INTEGER qpc{};QueryPerformanceCounter(&qpc);receive_qpc=static_cast<std::uint64_t>(qpc.QuadPart);return 1;
      }
      if(GetLastError()!=ERROR_IO_PENDING){failure="USB pipe disconnected or oversized message";++stats.disconnects;close();return -1;}
      pending=true;
    }
    auto status=WaitForSingleObject(event.value,timeout);
    if(status==WAIT_TIMEOUT){++stats.read_timeouts;return 0;}
    if(status!=WAIT_OBJECT_0){fail("USB read wait failed");return -1;}
    pending=false;
    if(!GetOverlappedResult(pipe,&overlapped,&bytes,FALSE)){
      failure="USB pipe read failed or message was oversized";++stats.disconnects;close();return -1;
    }
    LARGE_INTEGER qpc{};QueryPerformanceCounter(&qpc);receive_qpc=static_cast<std::uint64_t>(qpc.QuadPart);
    return 1;
  }
  bool connect() {
    if(pipe!=INVALID_HANDLE_VALUE) return fail("USB source already connected");
    if(!config_valid(config,failure)) return false;
    if(stats.blocks)return fail("USB source instances cannot reconnect or retain old PCM");
    verify_endpoint(config);
    pipe=CreateFileW(wide(config.pipe).c_str(),GENERIC_READ|GENERIC_WRITE,0,nullptr,OPEN_EXISTING,FILE_FLAG_OVERLAPPED,nullptr);
    if(pipe==INVALID_HANDLE_VALUE) return fail("USB capture bridge unavailable, Win32="+std::to_string(GetLastError()));
    ULONG server=0;DWORD peerSession=0,mySession=0;
    if(!GetNamedPipeServerProcessId(pipe,&server) || !ProcessIdToSessionId(server,&peerSession) ||
       !ProcessIdToSessionId(GetCurrentProcessId(),&mySession) || peerSession!=mySession)
      return fail("USB bridge peer session mismatch");
    peer.value=OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION|SYNCHRONIZE,FALSE,server);
    std::array<wchar_t,32768> path{};DWORD length=static_cast<DWORD>(path.size());
    if(!peer.value || !QueryFullProcessImageNameW(peer.value,0,path.data(),&length) ||
       _wcsicmp(std::wstring(path.data(),length).c_str(),wide(config.bridge_executable).c_str())!=0)
      return fail("USB bridge executable path mismatch");
    auto expected=config.bridge_sha256;
    std::transform(expected.begin(),expected.end(),expected.begin(),[](char c){return c>='A'&&c<='F'?static_cast<char>(c-'A'+'a'):c;});
    // Retain the exact hashed image read handle with no write/delete sharing
    // throughout the authenticated session, rather than releasing it after hash.
    image.value=CreateFileW(path.data(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
    if(image.value==INVALID_HANDLE_VALUE||file_hash(image.value)!=expected)return fail("USB bridge executable SHA256 mismatch");
    const auto mine=user_token(GetCurrentProcess()),theirs=user_token(peer.value);
    if(!EqualSid(reinterpret_cast<const TOKEN_USER*>(mine.data())->User.Sid,reinterpret_cast<const TOKEN_USER*>(theirs.data())->User.Sid)||
       WaitForSingleObject(peer.value,0)!=WAIT_TIMEOUT)return fail("USB bridge owner/lifetime mismatch");
    DWORD mode=PIPE_READMODE_MESSAGE;
    if(!SetNamedPipeHandleState(pipe,&mode,nullptr,nullptr)) return fail("USB pipe is not message-mode");
    vibe_input::HelloMessage hello{};
    if(BCryptGenRandom(nullptr,reinterpret_cast<PUCHAR>(hello.header.nonce),16,BCRYPT_USE_SYSTEM_PREFERRED_RNG)<0)
      return fail("USB challenge generation failed");
    hello.header=vibe_input::MakeHeader(vibe_input::Hello,sizeof(hello),hello.header.nonce);
    hello.clientPid=GetCurrentProcessId();hello.expectedServerPid=server;
    hello.sourceKind=vibe_input::UserModeUsb;hello.windowsSessionId=mySession;
    hello.clientCreationTime=creation_time(GetCurrentProcess());hello.serverCreationTime=creation_time(peer.value);
    hello.profile={48000,static_cast<std::uint16_t>(config.format.channels),240,config.format.channel_mask,config.format.container_bits,config.format.valid_bits};
    if(!put_text(hello.binding.endpointId,config.endpoint)||!put_text(hello.binding.usbInstance,config.usb_instance)||
       !put_text(hello.binding.controllerInstance,config.controller_instance))return fail("USB binding identifier too long");
    std::copy(config.serial.begin(),config.serial.end(),hello.binding.serial);std::copy(config.service.begin(),config.service.end(),hello.binding.service);
    hello.binding.port=config.port;hello.binding.vendor=0x1209;
    hello.binding.product=VibeUsbProduct(hello.profile.channels,hello.profile.channelMask,hello.profile.nativeContainerBits,hello.profile.nativeValidBits);
    if(!usb_binding_matches_profile(hello.binding,hello.profile))return fail("USB hello private product/profile mismatch");
    if(!write_message(&hello,sizeof(hello))) return false;
    DWORD received=0;
    if(read_message(2000,received)!=1) return fail("USB bridge welcome missing: "+failure);
    if(received!=sizeof(welcome)) return fail("USB bridge rejected handshake");
    std::memcpy(&welcome,&message,sizeof(welcome));
    LARGE_INTEGER frequency{};QueryPerformanceFrequency(&frequency);
    if(!vibe_input::ValidHeader(welcome.header,vibe_input::Welcome,sizeof(welcome)) ||
       !(welcome.header.nonce[0]||welcome.header.nonce[1]) ||
       !vibe_input::SameNonce(welcome.clientNonce,hello.header.nonce) ||
       welcome.qpcFrequency!=static_cast<std::uint64_t>(frequency.QuadPart) ||
       welcome.serverPid!=server || welcome.clientPid!=GetCurrentProcessId() ||
       welcome.sourceKind!=vibe_input::UserModeUsb||welcome.windowsSessionId!=mySession||
       welcome.serverCreationTime!=hello.serverCreationTime||welcome.clientCreationTime!=hello.clientCreationTime||
       !vibe_input::SameProfile(welcome.profile,hello.profile)||!usb_binding_matches_profile(welcome.binding,welcome.profile)||
       welcome.queueBlocks!=vibe_input::QueueBlocks || welcome.maximumAgeMs!=vibe_input::MaximumAgeMs ||
       std::memcmp(&welcome.binding,&hello.binding,sizeof(hello.binding))||WaitForSingleObject(peer.value,0)!=WAIT_TIMEOUT)
      return fail("USB bridge welcome contract mismatch");
    return true;
  }
  policy::sample_status_e sample(std::span<float> pcm) {
    if(pipe==INVALID_HANDLE_VALUE) return policy::sample_status_e::error;
    DWORD bytes=0;
    int result=read_message(50,bytes);
    if(result==0) return policy::sample_status_e::timeout;
    if(result<0) return policy::sample_status_e::error;
    const auto &h=message.info;
    auto bad=[&](const char *reason){fail(reason);return policy::sample_status_e::error;};
    if(bytes==sizeof(vibe_input::ErrorMessage)){
      vibe_input::ErrorMessage error{};std::memcpy(&error,&message,sizeof(error));
      if(!vibe_input::SameNonce(error.header.nonce,welcome.header.nonce) ||
         !(vibe_input::ValidHeader(error.header,vibe_input::Error,sizeof(error)) ||
           vibe_input::ValidHeader(error.header,vibe_input::Goodbye,sizeof(error))))
        return bad("Invalid USB termination message");
      if((error.code==vibe_input::NoError && error.header.type!=vibe_input::Goodbye) || error.code>vibe_input::InternalFailure ||
         error.lastBlockSequence!=(stats.blocks?stats.blocks-1:UINT64_MAX))
        return bad("USB termination code/last sequence mismatch");
      if(error.header.type==vibe_input::Error && error.code==vibe_input::QueueOverflow && error.systemError==0 && stats.blocks){
        end_reason=usb_end_reason_e::confirmed_queue_overflow;
        close();return policy::sample_status_e::interrupted;
      }
      if(error.header.type==vibe_input::Goodbye && (error.code==vibe_input::NoError || error.code==vibe_input::ControlledStop)) {
        if(error.systemError)return bad("USB clean termination contains a system error");
        if(error.code==vibe_input::NoError && !stats.blocks)return bad("USB terminal AS0 without a prior audio block");
        end_reason=error.code==vibe_input::NoError?usb_end_reason_e::terminal_as0:usb_end_reason_e::controlled_stop;
        close();return policy::sample_status_e::interrupted;
      }
      return bad(("USB bridge stopped, code="+std::to_string(error.code)+", Win32="+std::to_string(error.systemError)).c_str());
    }
    const auto wireBytes=vibe_input::AudioWireBytes(config.format.channels);
    if(pcm.size()!=config.format.samples() || bytes!=wireBytes ||
       !vibe_input::ValidHeader(h.header,vibe_input::Audio,wireBytes) ||
       !vibe_input::SameNonce(h.header.nonce,welcome.header.nonce)) return bad("USB message/session mismatch");
    if(const auto* error=validator.accept(h,welcome,std::span<const float>(message.samples,pcm.size()),receive_qpc))return bad(error);
    if(!stats.blocks && prior_epoch && !usb_fresh_native_epoch(prior_epoch,prior_receive_qpc,h))
      return bad("USB successor reused an old native epoch or URB timestamp");
    if(!stats.blocks && overflow_origin.epoch){
      if(!usb_overflow_successor(overflow_origin,h))return bad("USB overflow successor epoch/position/URB timing mismatch");
      loss_frames=h.firstFrame-(overflow_origin.firstFrame+vibe_input::Frames);
    }
    const double age=1000.0*(receive_qpc-h.firstReceiveQpc)/static_cast<double>(h.qpcFrequency);
    stats.maximum_age_ms=std::max(stats.maximum_age_ms,age);
    if(age>welcome.maximumAgeMs){++stats.stale_blocks;return bad("USB source block exceeds maximum age");}
    if(WaitForSingleObject(peer.value,0)!=WAIT_TIMEOUT)return bad("USB bridge process ended during session");
    std::copy_n(std::begin(message.samples),pcm.size(),pcm.begin());
    previous=h;++stats.blocks;
    return policy::sample_status_e::ok;
  }
};
#else
struct usb_source_t::impl_t {
  explicit impl_t(usb_source_config_t) {}
  usb_end_reason_e end_reason=usb_end_reason_e::none;
  std::uint64_t receive_qpc=0;
  std::uint64_t loss_frames=0;
  std::string failure="Project USB PCM 7.1.4 requires Windows";
  usb_source_statistics_t stats{};
  vibe_input::AudioHeader previous{};
  vibe_input::WelcomeMessage welcome{};
  bool connect(){return false;}
  void close(){}
  policy::sample_status_e sample(std::span<float>){return policy::sample_status_e::error;}
};
#endif
usb_source_t::usb_source_t(usb_source_config_t value):impl_(std::make_unique<impl_t>(std::move(value))){}
usb_source_t::~usb_source_t()=default;
bool usb_source_t::connect(){try{return impl_->connect();}catch(const std::exception &e){impl_->failure=e.what();impl_->close();return false;}}
usb_end_reason_e usb_source_t::end_reason()const{return impl_->end_reason;}
bool usb_source_t::connect_after_as0(const usb_source_t &ended,const std::function<bool()> &stopping) {
  return connect_successor(ended,stopping,usb_end_reason_e::terminal_as0);
}
bool usb_source_t::connect_after_overflow(const usb_source_t &ended,const std::function<bool()> &stopping) {
  return connect_successor(ended,stopping,usb_end_reason_e::confirmed_queue_overflow);
}
std::uint64_t usb_source_t::lost_native_frames()const{return impl_->loss_frames;}
bool usb_source_t::connect_successor(const usb_source_t &ended,const std::function<bool()> &stopping,usb_end_reason_e reason) {
#ifdef _WIN32
  try {
    if(ended.end_reason()!=reason || !ended.statistics().blocks || !ended.metadata().epoch ||
       impl_->pipe!=INVALID_HANDLE_VALUE || impl_->stats.blocks || impl_->welcome.serverPid)
      return impl_->fail("USB successor requires a fresh client and an authenticated boundary");
    const auto path=wide(impl_->config.pipe);
    const auto deadline=GetTickCount64()+2000;
    for(;;){
      if(stopping())return impl_->fail("USB rearm cancelled by session shutdown");
      const auto now=GetTickCount64();
      if(now>=deadline)return impl_->fail("Timed out waiting for owned USB bridge to rearm");
      if(WaitNamedPipeW(path.c_str(),static_cast<DWORD>(std::min<ULONGLONG>(25,deadline-now))))break;
      const auto code=GetLastError();
      if(code!=ERROR_SEM_TIMEOUT)return impl_->fail("Owned USB bridge no longer available: "+std::to_string(code));
    }
    if(stopping())return impl_->fail("USB rearm cancelled by session shutdown");
    if(!impl_->connect())return false;
    const auto &old=ended.session();const auto &next=impl_->welcome;
    if(!usb_same_producer_new_session(old,next))return impl_->fail("USB rearm peer, profile, binding or nonce mismatch");
    if(reason==usb_end_reason_e::confirmed_queue_overflow)impl_->overflow_origin=ended.metadata();
    else {impl_->prior_epoch=ended.metadata().epoch;impl_->prior_receive_qpc=ended.metadata().lastReceiveQpc;}
    return true;
  }catch(const std::exception &e){return impl_->fail(e.what());}
#else
  (void)ended;(void)stopping;(void)reason;
  return false;
#endif
}
policy::sample_status_e usb_source_t::sample(std::span<float> pcm){return impl_->sample(pcm);}
void usb_source_t::close(){impl_->close();}
const std::string &usb_source_t::error() const{return impl_->failure;}
const usb_source_statistics_t &usb_source_t::statistics() const{return impl_->stats;}
const vibe_input::AudioHeader &usb_source_t::metadata() const{return impl_->previous;}
const vibe_input::WelcomeMessage &usb_source_t::session() const{return impl_->welcome;}
std::uint64_t usb_source_t::receive_qpc() const{return impl_->receive_qpc;}
bool usb_source_t::inspect_binding(const usb_source_config_t& config,std::string& error){
#ifdef _WIN32
  try{if(!config_valid(config,error))return false;verify_endpoint(config);error.clear();return true;}
  catch(const std::exception& e){error=e.what();return false;}
#else
  (void)config;error="Vibe Speakers USB requires Windows";return false;
#endif
}
bool usb_source_t::available(const usb_source_config_t &config,std::string &error){
#ifdef _WIN32
  try{
    if(!config_valid(config,error))return false;
    if(!WaitNamedPipeW(wide(config.pipe).c_str(),1)){error="Project USB capture bridge is unavailable or already in use";return false;}
    return true;
  }catch(const std::exception &e){error=e.what();return false;}
#else
  (void)config;error="Project USB PCM 7.1.4 requires Windows";return false;
#endif
}
}

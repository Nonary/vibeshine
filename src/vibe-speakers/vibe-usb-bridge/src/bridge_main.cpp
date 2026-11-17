// SPDX-License-Identifier: GPL-3.0-or-later
#include "diagnostics.h"


#include <avrt.h>
#include <algorithm>
#include <cwctype>
#include <cctype>
#include <iostream>
#include <memory>
#include <optional>

namespace vibe_bridge {
struct Options {
 bool managed=false,raw=false,detailed=false;
 Profile profile{SampleRate,12,Frames,0x2d63f,32,24};
 std::wstring controller,pipeName,clientImage,out,readyName,stopName,readyFile;
 std::string clientHash;
 unsigned seconds=0;DWORD ownerPid=0;std::uint64_t ownerCreation=0;
};
Options Parse(int argc,wchar_t** argv){
 Options o;bool explicitMask=false,ownerPidSpecified=false,ownerCreationSpecified=false;
 for(int i=1;i<argc;++i){
  const std::wstring key=argv[i];
  if(key==L"--managed-runtime"){o.managed=true;continue;}
  if(key==L"--raw"){
#if defined(VIBE_AUDIO_TEST_INSTRUMENTATION) || defined(VIBE_BRIDGE_TESTING)
   o.raw=true;continue;
#else
   throw Failure(InvalidHandshake,"PCM recording is not available in the product build");
#endif
  }
  if(key==L"--summary-only"){o.detailed=false;continue;}
  Require(i+1<argc,InvalidHandshake,"Missing option value");const std::wstring value=argv[++i];
  auto number=[&](){std::size_t end=0;const auto n=std::stoull(value,&end,0);Require(end==value.size()&&n<=UINT32_MAX,InvalidHandshake,"Invalid numeric option");return static_cast<std::uint32_t>(n);};
  if(key==L"--controller-instance")o.controller=value;
  else if(key==L"--pipe-name"||key==L"--pipe")o.pipeName=value;
  else if(key==L"--allowed-client")o.clientImage=value;
  else if(key==L"--allowed-client-sha256")o.clientHash=Utf8(value);
  else if(key==L"--out")o.out=value;
  else if(key==L"--ready-event")o.readyName=value;
  else if(key==L"--ready-file")o.readyFile=value;
  else if(key==L"--stop-event")o.stopName=value;
  else if(key==L"--channels"){const auto n=number();Require(n<=UINT16_MAX,InvalidHandshake,"Channel count overflow");o.profile.channels=static_cast<std::uint16_t>(n);}
  else if(key==L"--channel-mask"){o.profile.channelMask=number();explicitMask=true;}
  else if(key==L"--native-format"||key==L"--sample-format"){Require(value==L"pcm24"||value==L"pcm16",InvalidHandshake,"Only pcm24 or pcm16 supported");o.profile.nativeContainerBits=value==L"pcm24"?32:16;o.profile.nativeValidBits=value==L"pcm24"?24:16;}
  else if(key==L"--seconds"){
#if defined(VIBE_AUDIO_TEST_INSTRUMENTATION) || defined(VIBE_BRIDGE_TESTING)
   o.seconds=number();
#else
   throw Failure(InvalidHandshake,"Finite session durations are test-only");
#endif
  }
  else if(key==L"--owner-pid"){Require(!ownerPidSpecified,InvalidHandshake,"Duplicate owner PID");o.ownerPid=number();ownerPidSpecified=true;}
  else if(key==L"--owner-creation-time"){
   Require(!ownerCreationSpecified&&!value.empty()&&value.find_first_not_of(L"0123456789")==std::wstring::npos,InvalidHandshake,"Invalid owner FILETIME");
   o.ownerCreation=std::stoull(value);ownerCreationSpecified=true;
  }
  else throw Failure(InvalidHandshake,"Unknown option");
 }
 Require(explicitMask||o.profile.channels!=6,InvalidHandshake,"Six channels require an explicit rear or side mask");
 Require(ownerPidSpecified==ownerCreationSpecified&&(!ownerPidSpecified||(o.ownerPid&&o.ownerCreation)),InvalidHandshake,"Managed owner requires both a positive PID and creation FILETIME");
 if(!explicitMask)o.profile.channelMask=o.profile.channels==2?3:o.profile.channels==8?0x63f:0x2d63f;
 Require(o.managed,BindingMismatch,"Owned managed runtime is required");
 if(o.managed)Require(o.ownerPid&&o.ownerCreation&&!o.readyName.empty()&&!o.stopName.empty(),BindingMismatch,"Managed runtime requires an exact live owner and control events; laboratory faults prohibited");
 Require(ValidProfile(o.profile)&&!o.controller.empty()&&!o.clientImage.empty()&&!o.out.empty(),InvalidHandshake,"Exact controller, profile, peer and evidence prefix required");
 Require(ValidDuration(o.seconds)&&!o.pipeName.empty()&&o.pipeName.size()<160&&
  o.pipeName.find_first_not_of(L"abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789._-")==std::wstring::npos,
  InvalidHandshake,"Invalid pipe name or duration");
 std::transform(o.clientHash.begin(),o.clientHash.end(),o.clientHash.begin(),[](unsigned char c){return static_cast<char>(std::tolower(c));});
 Require(o.clientHash.size()==64&&o.clientHash.find_first_not_of("0123456789abcdef")==std::string::npos,InvalidHandshake,"Exact peer SHA256 required");return o;
}
std::uint64_t Clock()noexcept{LARGE_INTEGER n{};QueryPerformanceCounter(&n);return static_cast<std::uint64_t>(n.QuadPart);}
std::uint64_t Creation(HANDLE process){FILETIME c{},e{},k{},u{};WinCheck(GetProcessTimes(process,&c,&e,&k,&u),BindingMismatch,"Get process lifetime");return(static_cast<std::uint64_t>(c.dwHighDateTime)<<32)|c.dwLowDateTime;}
std::wstring ProcessSid(HANDLE process){
 HANDLE raw=nullptr;WinCheck(OpenProcessToken(process,TOKEN_QUERY,&raw),BindingMismatch,"Read peer user token");Handle token(raw);
 DWORD size=0;GetTokenInformation(token.Get(),TokenUser,nullptr,0,&size);Require(size&&size<65536,BindingMismatch,"Peer token bounds");
 std::vector<std::uint8_t> bytes(size);WinCheck(GetTokenInformation(token.Get(),TokenUser,bytes.data(),size,&size),BindingMismatch,"Read peer SID");
 LPWSTR sid=nullptr;WinCheck(ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(bytes.data())->User.Sid,&sid),BindingMismatch,"Format peer SID");
 std::wstring result=sid;LocalFree(sid);return result;
}
// Holds the verified owner process and image from before USB attach until
// teardown. The process object remains specific to that lifetime after PID reuse.
class ManagedOwner {
 Handle process_,image_,cancel_;
 std::thread watcher_;
 DWORD pid_=0;
 std::uint64_t creation_=0;
 std::atomic<std::uint64_t> exitedQpc_{0};
 std::atomic<bool> watchFailed_{false};
public:
 ManagedOwner(const Options& o,const LockedPeerImage& pinned,HANDLE stop):pid_(o.ownerPid),creation_(o.ownerCreation){
  if(!pid_&&!creation_)return;
  Require(pid_&&creation_&&pid_!=GetCurrentProcessId(),BindingMismatch,"Invalid managed host identity");
  process_=Handle(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION|SYNCHRONIZE,FALSE,pid_));
  WinCheck(process_.Valid(),BindingMismatch,"Open managed host lifetime");
  DWORD ownSession=0,hostSession=0;
  WinCheck(ProcessIdToSessionId(GetCurrentProcessId(),&ownSession)&&ProcessIdToSessionId(pid_,&hostSession),BindingMismatch,"Read managed host session");
  Require(ownSession&&ownSession==hostSession&&Creation(process_.Get())==creation_&&ProcessSid(process_.Get())==CurrentSid(),BindingMismatch,"Managed host lifetime/user/session differs");
  image_=pinned.Match(ProcessImage(process_.Get()),o.clientHash);

  Require(WaitForSingleObject(process_.Get(),0)==WAIT_TIMEOUT,BindingMismatch,"Managed host exited before attach");
  cancel_=Handle(CreateEventW(nullptr,TRUE,FALSE,nullptr));WinCheck(cancel_.Valid(),InternalFailure,"Create managed owner watcher stop");
  const auto output=o.out;
  watcher_=std::thread([this,stop,output]{
   HANDLE waits[]={process_.Get(),cancel_.Get()};
   const auto result=WaitForMultipleObjects(2,waits,FALSE,INFINITE);
   if(result==WAIT_OBJECT_0+1)return;
   const auto ended=Clock();
   if(result==WAIT_OBJECT_0)exitedQpc_.store(ended,std::memory_order_release);else watchFailed_=true;
   const bool stopped=SetEvent(stop)!=FALSE; // wakes enumeration, accept and pipe I/O
   if(!stopped)watchFailed_=true;
   if(output.empty())return;
   try{
    auto file=CreateOutput(output+L".owner-exit.json");
    WriteText(file.Get(),"{\"Schema\":\"vibe-usb-managed-owner-exit/1\",\"owner_pid\":"+std::to_string(pid_)+
     ",\"owner_creation_time\":"+std::to_string(creation_)+",\"process_handle_signaled\":"+(result==WAIT_OBJECT_0?"true":"false")+
     ",\"observed_qpc\":"+std::to_string(ended)+",\"stop_event_set\":"+(stopped?"true":"false")+"}\n");
   }catch(...){/* optional owner-exit receipt; kernel process wait remains authoritative */}
  });
 }
 ~ManagedOwner(){Close();}
 ManagedOwner(const ManagedOwner&)=delete;ManagedOwner& operator=(const ManagedOwner&)=delete;
 void Close()noexcept{if(watcher_.joinable()){SetEvent(cancel_.Get());watcher_.join();}}
 bool Allows(DWORD pid,std::uint64_t created)const noexcept{return !pid_||(pid_==pid&&creation_==created);}
 bool VerifiedAlive()const noexcept{return pid_&&process_.Valid()&&WaitForSingleObject(process_.Get(),0)==WAIT_TIMEOUT;}
 std::uint64_t ExitQpc()const noexcept{return exitedQpc_.load(std::memory_order_acquire);}
 bool Failed()const noexcept{return watchFailed_.load(std::memory_order_acquire);}
};
template<std::size_t N>void WireText(char16_t(&dest)[N],const std::wstring& value){Require(value.size()<N,BindingMismatch,"USB identity exceeds wire field");for(std::size_t i=0;i<value.size();++i)dest[i]=static_cast<char16_t>(value[i]);}
template<std::size_t N>bool EqualText(const char16_t(&a)[N],const char16_t(&b)[N])noexcept{
 for(std::size_t i=0;i<N;++i)if(std::towupper(static_cast<wchar_t>(a[i]))!=std::towupper(static_cast<wchar_t>(b[i])))return false;return true;
}
UsbBinding ToWire(const usb::binding& b){
 UsbBinding w{};WireText(w.endpointId,b.endpoint_id);WireText(w.usbInstance,b.usb_instance);WireText(w.controllerInstance,b.host_instance);
 Require(b.port>0&&b.port<=255&&b.serial.size()<sizeof(w.serial)&&b.service.size()<sizeof(w.service),BindingMismatch,"Invalid exact imported tuple");
 Require(b.vendor==usb::vendor_id&&b.product,BindingMismatch,"Missing actual USB import product identity");
 std::memcpy(w.serial,b.serial.data(),b.serial.size());std::memcpy(w.service,b.service.data(),b.service.size());w.port=static_cast<std::uint32_t>(b.port);w.vendor=b.vendor;w.product=b.product;return w;
}
bool SameBinding(const UsbBinding& a,const UsbBinding& b)noexcept{return EqualText(a.endpointId,b.endpointId)&&EqualText(a.usbInstance,b.usbInstance)&&EqualText(a.controllerInstance,b.controllerInstance)&&
 !a.reserved&&!b.reserved&&a.port==b.port&&a.vendor==b.vendor&&a.product==b.product&&std::memcmp(a.serial,b.serial,sizeof(a.serial))==0&&std::memcmp(a.service,b.service,sizeof(a.service))==0;}
Handle MakePipe(const Options& o){
 const auto acl=L"D:P(A;;GA;;;SY)(A;;GRGW;;;"+CurrentSid()+L")";PSECURITY_DESCRIPTOR sd=nullptr;
 WinCheck(ConvertStringSecurityDescriptorToSecurityDescriptorW(acl.c_str(),SDDL_REVISION_1,&sd,nullptr),InternalFailure,"Create private pipe ACL");
 SECURITY_ATTRIBUTES sa{sizeof(sa),sd,FALSE};const auto path=L"\\\\.\\pipe\\"+o.pipeName;
 HANDLE p=CreateNamedPipeW(path.c_str(),PIPE_ACCESS_DUPLEX|FILE_FLAG_OVERLAPPED|FILE_FLAG_FIRST_PIPE_INSTANCE,
  PIPE_TYPE_MESSAGE|PIPE_READMODE_MESSAGE|PIPE_WAIT|PIPE_REJECT_REMOTE_CLIENTS,1,sizeof(AudioMessage),sizeof(HelloMessage),1000,&sa);
 const DWORD error=GetLastError();LocalFree(sd);if(p==INVALID_HANDLE_VALUE)throw Failure(InternalFailure,"Create private exclusive local pipe",error);return Handle(p);
}
bool AwaitClient(HANDLE pipe,HANDLE stop){
 Handle event(CreateEventW(nullptr,TRUE,FALSE,nullptr));WinCheck(event.Valid(),InternalFailure,"Create pipe accept completion");
 OVERLAPPED op{};op.hEvent=event.Get();if(ConnectNamedPipe(pipe,&op))return true;
 const auto error=GetLastError();if(error==ERROR_PIPE_CONNECTED)return true;
 if(error!=ERROR_IO_PENDING)throw Failure(InternalFailure,"Start pipe accept",error);
 HANDLE wait[]={event.Get(),stop};const auto result=WaitForMultipleObjects(2,wait,FALSE,INFINITE);DWORD count=0;
 if(result!=WAIT_OBJECT_0){CancelIoEx(pipe,&op);GetOverlappedResult(pipe,&op,&count,TRUE);if(result==WAIT_OBJECT_0+1)return false;throw Failure(InternalFailure,"Pipe accept wait");}
 WinCheck(GetOverlappedResult(pipe,&op,&count,FALSE),PeerDisconnected,"Complete pipe accept");return true;
}
struct Peer {DWORD pid=0,session=0;std::uint64_t creation=0;Handle process,image;};
class AudioScheduling {
 HANDLE task_=nullptr;bool& revertFailed_;
public:
 explicit AudioScheduling(bool& failure):revertFailed_(failure){DWORD index=0;task_=AvSetMmThreadCharacteristicsW(L"Audio",&index);WinCheck(task_!=nullptr,InternalFailure,"Register pipe writer MMCSS Audio");}
 ~AudioScheduling(){if(task_&&!AvRevertMmThreadCharacteristics(task_))revertFailed_=true;}
 AudioScheduling(const AudioScheduling&)=delete;AudioScheduling& operator=(const AudioScheduling&)=delete;
};
Peer Authenticate(HANDLE pipe,const LockedPeerImage& pinned,const Options& o){
 Peer p;ULONG pipeSession=0;WinCheck(GetNamedPipeClientProcessId(pipe,&p.pid)&&GetNamedPipeClientSessionId(pipe,&pipeSession),BindingMismatch,"Read pipe peer identity");
 DWORD ownSession=0;WinCheck(ProcessIdToSessionId(GetCurrentProcessId(),&ownSession)&&ProcessIdToSessionId(p.pid,&p.session),BindingMismatch,"Read Windows sessions");
 Require(ownSession!=0&&ownSession==p.session&&pipeSession==p.session,BindingMismatch,"Peer must use the same interactive session");
 p.process=Handle(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION|SYNCHRONIZE,FALSE,p.pid));WinCheck(p.process.Valid(),BindingMismatch,"Open peer lifetime handle");
 Require(WaitForSingleObject(p.process.Get(),0)==WAIT_TIMEOUT&&ProcessSid(p.process.Get())==CurrentSid(),BindingMismatch,"Peer user or lifetime mismatch");
 p.creation=Creation(p.process.Get());p.image=pinned.Match(ProcessImage(p.process.Get()),o.clientHash);return p;
}
bool ValidHello(const HelloMessage& hello,const Peer& peer,std::uint32_t serverPid,std::uint64_t serverCreation,const Profile& profile,const UsbBinding& binding)noexcept{
 return ValidHeader(hello.header,Hello,sizeof(hello))&&hello.sourceKind==UserModeUsb&&hello.clientPid==peer.pid&&hello.expectedServerPid==serverPid&&
  hello.windowsSessionId==peer.session&&hello.clientCreationTime==peer.creation&&hello.serverCreationTime==serverCreation&&
  ValidProfile(hello.profile)&&SameProfile(hello.profile,profile)&&binding.vendor==usb::vendor_id&&
  binding.product==VibeUsbProduct(profile.channels,profile.channelMask,profile.nativeContainerBits,profile.nativeValidBits)&&SameBinding(hello.binding,binding);
}

// Main thread owns activation/configuration, USB worker alone owns assembler and
// queue producer. Disarm publishes zero then waits for the short in-flight
// callback to retire. A later callback sees zero and never touches session data.
class Pipeline {
 Spsc<AudioMessage,QueueBlocks> queue_;
 BlockAssembler assembler_;
 Profile profile_{};std::uint64_t nonce_[2]{};
 std::atomic<std::uint64_t> active_{0};std::uint64_t observed_=0;
 std::atomic<unsigned> inFlight_{0};
 Handle idle_{CreateEventW(nullptr,TRUE,TRUE,nullptr)};
 std::atomic<ErrorCode> failure_{NoError};
 std::atomic<bool> seen_{false},terminal_{false};
 std::uint32_t terminalEpoch_=0;std::uint64_t terminalQpc_=0;
 std::uint64_t frames_=0,overruns_=0,rejected_=0;
public:
 enum class NextResult {Empty,Block,Terminal};
#ifdef VIBE_BRIDGE_TESTING
 std::function<void()> resetObservedHook,emptyAfterPopHook;
#endif
 Handle available{CreateEventW(nullptr,FALSE,FALSE,nullptr)},fault{CreateEventW(nullptr,TRUE,FALSE,nullptr)};
 explicit Pipeline(Profile p):profile_(p){WinCheck(idle_.Valid()&&available.Valid()&&fault.Valid(),InternalFailure,"Create bounded queue events");}
 void Fail(ErrorCode e)noexcept{ErrorCode empty=NoError;failure_.compare_exchange_strong(empty,e,std::memory_order_acq_rel);SetEvent(fault.Get());SetEvent(available.Get());}
 void Arm(std::uint64_t id,const std::uint64_t(&nonce)[2]){
  Require(!active_.load(),InternalFailure,"Session arm before disarm");
  nonce_[0]=nonce[0];nonce_[1]=nonce[1];frames_=overruns_=rejected_=0;seen_=false;terminal_=false;failure_=NoError;terminalEpoch_=0;terminalQpc_=0;
  assembler_.Reset(profile_,nonce_);observed_=id;queue_.ResetMaximumAfterProducerStopped();
  ResetEvent(fault.Get());active_.store(id,std::memory_order_release);
 }
 void Disarm(){active_.store(0,std::memory_order_release);while(inFlight_.load(std::memory_order_acquire)!=0)WaitForSingleObject(idle_.Get(),INFINITE);}
 ErrorCode Error()const noexcept{return failure_.load(std::memory_order_acquire);}
 const std::atomic<ErrorCode>& ErrorSource()const noexcept{return failure_;}
 void Reset(std::uint32_t epoch,bool terminalAs0)noexcept{
  inFlight_.fetch_add(1,std::memory_order_acq_rel);ResetEvent(idle_.Get());
  if(active_.load(std::memory_order_acquire)&&seen_.load(std::memory_order_acquire)){
#ifdef VIBE_BRIDGE_TESTING
   if(resetObservedHook)resetObservedHook();
#endif
   if(terminalAs0){terminalEpoch_=epoch;terminalQpc_=Clock();terminal_.store(true,std::memory_order_release);SetEvent(available.Get());}else Fail(EpochChanged);
  }
  inFlight_.fetch_sub(1,std::memory_order_acq_rel);SetEvent(idle_.Get());
 }
 bool Terminal()const noexcept{return terminal_.load(std::memory_order_acquire);}
 bool Native(const usb::pcm_metadata& m,std::span<const usb::iso_packet> packets,std::span<const std::uint8_t> bytes)noexcept{
  inFlight_.fetch_add(1,std::memory_order_acq_rel);ResetEvent(idle_.Get());
  const auto id=active_.load(std::memory_order_acquire);
  if(id&&Error()==NoError){
   if(Terminal()){Fail(EpochChanged);inFlight_.fetch_sub(1,std::memory_order_acq_rel);SetEvent(idle_.Get());return true;}
   if(observed_!=id){assembler_.Reset(profile_,nonce_);observed_=id;}
   const auto code=assembler_.Append(m,packets,bytes,Clock,[&](const AudioMessage& message){
    const bool pushed=queue_.Push([&](AudioMessage& dest,std::uint32_t depth){dest.info=message.info;std::memcpy(dest.samples,message.samples,message.info.payloadBytes);dest.info.queueDepthBlocks=depth;
     dest.info.queueOverruns=overruns_;dest.info.inputRejectedUrbs=rejected_;dest.info.enqueueQpc=Clock();});
    if(!pushed)++overruns_;else SetEvent(available.Get());return pushed;
   });
   if(m.frames)seen_.store(true,std::memory_order_release);frames_+=m.frames;
   if(code!=NoError){++rejected_;Fail(code);}
  }
  inFlight_.fetch_sub(1,std::memory_order_acq_rel);SetEvent(idle_.Get());return true;
 }
 bool Pop(AudioMessage& out)noexcept{return queue_.Pop(out);}
 NextResult Next(AudioMessage& out)noexcept{
  const bool terminalBeforeDrain=Terminal();
  if(Pop(out))return NextResult::Block;
#ifdef VIBE_BRIDGE_TESTING
  if(emptyAfterPopHook)emptyAfterPopHook();
#endif
  return terminalBeforeDrain?NextResult::Terminal:NextResult::Empty;
 }
 std::uint64_t DiscardAfterDisarm()noexcept{return queue_.DiscardAfterProducerStopped();}
 std::uint32_t MaximumAfterDisarm()const noexcept{return queue_.MaximumAfterProducerStopped();}
 std::uint32_t PartialAfterDisarm()const noexcept{return assembler_.PartialFrames();}
 std::uint64_t FramesAfterDisarm()const noexcept{return frames_;}
 std::uint64_t OverrunsAfterDisarm()const noexcept{return overruns_;}
 std::uint32_t TerminalEpochAfterDisarm()const noexcept{return terminalEpoch_;}
 std::uint64_t TerminalQpcAfterDisarm()const noexcept{return terminalQpc_;}
};

std::string BindingJson(const usb::binding& b,const Options& o,bool ownerVerified){DWORD session=0;ProcessIdToSessionId(GetCurrentProcessId(),&session);
 return "{\"Schema\":\"vibe-usb-bridge-binding/1\",\"event\":\"binding\",\"source_kind\":\"user_mode_usb\",\"pid\":"+std::to_string(GetCurrentProcessId())+",\"creation_time\":"+std::to_string(Creation(GetCurrentProcess()))+
  ",\"windows_session\":"+std::to_string(session)+",\"endpoint_id\":"+Json(b.endpoint_id)+",\"usb_instance\":"+Json(b.usb_instance)+",\"controller_instance\":"+Json(b.host_instance)+
  ",\"port\":"+std::to_string(b.port)+",\"serial\":"+Json(b.serial)+",\"service\":"+Json(b.service)+",\"vendor\":"+std::to_string(b.vendor)+",\"product\":"+std::to_string(b.product)+",\"channels\":"+std::to_string(o.profile.channels)+
  ",\"channel_mask\":"+std::to_string(o.profile.channelMask)+",\"sample_rate\":48000,\"container_bits\":"+std::to_string(o.profile.nativeContainerBits)+",\"valid_bits\":"+std::to_string(o.profile.nativeValidBits)+
  ",\"pipe_name\":"+Json(o.pipeName)+",\"qpc_frequency\":"+std::to_string(Frequency())+
  ",\"owner_pid\":"+std::to_string(o.ownerPid)+",\"owner_creation_time\":"+std::to_string(o.ownerCreation)+",\"owner_verified\":"+(ownerVerified?"true":"false")+
  ",\"actual_mix_hresult\":"+std::to_string(b.mix_hresult)+",\"actual_mix_channels\":"+std::to_string(b.mix_channels)+",\"actual_mix_mask\":"+std::to_string(b.mix_mask)+",\"worker_thread_id\":"+std::to_string(b.worker_thread_id)+",\"worker_mmcss_task\":\""+(b.worker_pro_audio?"Pro Audio":"Audio")+"\",\"worker_mmcss_registered\":"+(b.mmcss_registered?"true":"false")+",\"worker_mmcss_task_index\":"+std::to_string(b.mmcss_task_index)+"}\n";
}

struct OverflowBoundary {
 bool pending=false;
 DWORD peerPid=0;std::uint64_t peerCreation=0,oldNonce[2]{},epoch=0,endFrame=0,session=0;
 void VerifyFirst(const AudioHeader& h)const{
  Require(pending&&h.blockSequence==0&&h.epoch==epoch&&h.firstFrame>endFrame&&
          (h.header.nonce[0]!=oldNonce[0]||h.header.nonce[1]!=oldNonce[1]),
          SequenceGap,"Cannot prove positive same-native-epoch gap after overflow");
 }
};

int Run(const Options& o){
 const LockedPeerImage pinned(o.clientImage,o.clientHash);
 auto log=OptionalLog(o.out+L".jsonl");auto ready=OpenNamedEvent(o.readyName,EVENT_MODIFY_STATE);
 WriteLog(log.Get(),"{\"event\":\"starting\",\"controller_instance\":"+Json(o.controller)+",\"allowed_client_sha256\":"+Json(o.clientHash)+
  ",\"channels\":"+std::to_string(o.profile.channels)+",\"channel_mask\":"+std::to_string(o.profile.channelMask)+",\"container_bits\":"+std::to_string(o.profile.nativeContainerBits)+
  ",\"valid_bits\":"+std::to_string(o.profile.nativeValidBits)+",\"owner_pid\":"+std::to_string(o.ownerPid)+",\"owner_creation_time\":"+std::to_string(o.ownerCreation)+",\"qpc\":"+std::to_string(Clock())+"}\n");
 try {
 auto stop=o.stopName.empty()?Handle(CreateEventW(nullptr,TRUE,FALSE,nullptr)):OpenNamedEvent(o.stopName,SYNCHRONIZE|EVENT_MODIFY_STATE);
 WinCheck(stop.Valid(),InternalFailure,"Create/Open stop event");
 ManagedOwner owner(o,pinned,stop.Get());
 auto diagnosticsOwner=std::make_unique<Diagnostics>(o.out,o.profile,o.raw,o.detailed);auto& diagnostics=*diagnosticsOwner;
 auto pipelineOwner=std::make_unique<Pipeline>(o.profile);auto& pipeline=*pipelineOwner;
 usb::callbacks callbacks;callbacks.record_clock=false;
#ifdef VIBE_AUDIO_TEST_INSTRUMENTATION
 callbacks.record_clock=true;
#endif
 callbacks.diagnostic=[&](const std::string& message){WriteLog(log.Get(),"{\"event\":\"usb_startup\",\"message\":"+Json(message)+"}\n");};
 callbacks.qpc=Clock;callbacks.qpc_frequency=Frequency();
 callbacks.pcm=[&](const auto& m,auto packets,auto bytes){
  // PCM readiness/conversion has priority over optional evidence copying.
  pipeline.Native(m,packets,bytes);
  (void)diagnostics.Native(m,packets,bytes); // optional diagnostics cannot stop delivery
  return true;
 };
 callbacks.stream_reset=[&](std::uint32_t e,bool terminalAs0){pipeline.Reset(e,terminalAs0);};
 usb::profile format;format.channels=static_cast<std::uint8_t>(o.profile.channels);format.channel_mask=o.profile.channelMask;
 format.native_container_bits=o.profile.nativeContainerBits;format.native_valid_bits=o.profile.nativeValidBits;
 Require(WaitForSingleObject(stop.Get(),0)==WAIT_TIMEOUT,ControlledStop,"Stopped before USB attach");
 auto controller=usb::create(format,std::move(callbacks),o.controller,o.managed&&owner.VerifiedAlive());
 Require(controller!=nullptr,SourceUnavailable,"Cannot attach the exact owned USB controller");
 auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(10);usb::binding binding;
 do{binding=controller->inspect();if(!binding.endpoint_id.empty())break;
  Require(controller->connected()||std::chrono::steady_clock::now()<deadline,SourceUnavailable,"USB import unavailable");
  if(WaitForSingleObject(stop.Get(),25)==WAIT_OBJECT_0)throw Failure(ControlledStop,"Stopped before endpoint enumeration");
 }while(std::chrono::steady_clock::now()<deadline);
 Require(!binding.endpoint_id.empty() && (o.managed || (binding.mix_channels==o.profile.channels&&binding.mix_rate==SampleRate&&binding.mix_bits==32&&binding.mix_mask==o.profile.channelMask)),
  FormatMismatch,"Actual public endpoint mix differs from the explicit USB profile");
 Require(binding.mmcss_registered&&binding.worker_thread_id,InternalFailure,"USB worker MMCSS registration not established");
 Require(binding.product==format.product_id()&&binding.vendor==usb::vendor_id,BindingMismatch,"Imported product does not match requested USB profile");
 const auto wireBinding=ToWire(binding);const bool ownerVerified=owner.VerifiedAlive();
 Require(!o.ownerPid||ownerVerified,ControlledStop,"Managed owner exited before endpoint readiness");
 const auto bindingText=BindingJson(binding,o,ownerVerified);WriteLog(log.Get(),bindingText);
 {auto f=CreateOutput(o.readyFile.empty()?o.out+L".binding.json":o.readyFile);WriteText(f.Get(),bindingText);}
 auto pipe=MakePipe(o);
 // INFINITE waits on the same cancel event: no timer can terminate a normal stream.
 std::thread duration([&]{AwaitSessionStop(o.seconds,[&](DWORD milliseconds){return WaitForSingleObject(stop.Get(),milliseconds);},[&]{SetEvent(stop.Get());});});
 std::uint64_t sessions=0,rejections=0,totalBlocks=0,totalDiscarded=0,totalPartial=0,unconfirmed=0;
 std::uint64_t overflowEnds=0,resumedOverflows=0,gapFrames=0;OverflowBoundary boundary;bool failure=false;
 try{
  if(ready.Valid())SetEvent(ready.Get());
  while(WaitForSingleObject(stop.Get(),0)==WAIT_TIMEOUT){
   if(!AwaitClient(pipe.Get(),stop.Get()))break;
   std::uint64_t nonce[2]{};std::uint64_t blocks=0;bool armed=false,mmcssRevertFailed=false;ErrorCode endCode=NoError;std::uint32_t endSystem=0;
   DWORD peerPid=0;std::uint64_t peerCreation=0,lastEpoch=0,lastFrameEnd=0,attempted=UINT64_MAX;
   bool uncertain=false;
   std::optional<OverflowBoundary> resumedFrom;std::uint64_t resumedFirstFrame=0,resumedQpc=0;
   std::optional<PeerReadWatch> peerWatch;
   try{
    auto peer=Authenticate(pipe.Get(),pinned,o);
    peerPid=peer.pid;peerCreation=peer.creation;
    Require(owner.Allows(peer.pid,peer.creation),BindingMismatch,"Pipe client differs from the verified managed host lifetime");
    Require(!boundary.pending||(peer.pid==boundary.peerPid&&peer.creation==boundary.peerCreation),BindingMismatch,"Reconnect requires the same peer process lifetime");
    PipeOperation io;HelloMessage hello{};
    io.Transfer(pipe.Get(),&hello,sizeof(hello),false,stop.Get(),nullptr,2000);
    Require(ValidHello(hello,peer,GetCurrentProcessId(),Creation(GetCurrentProcess()),o.profile,wireBinding),InvalidHandshake,"Hello peer/profile/USB binding mismatch");
    Require(controller->connected()&&SameBinding(ToWire(controller->inspect()),wireBinding),BindingMismatch,"USB binding changed before handshake");
    NewNonce(nonce);WelcomeMessage welcome{};welcome.header=MakeHeader(Welcome,sizeof(welcome),nonce);welcome.clientNonce[0]=hello.header.nonce[0];welcome.clientNonce[1]=hello.header.nonce[1];
    welcome.qpcFrequency=Frequency();welcome.sourceKind=UserModeUsb;welcome.serverPid=GetCurrentProcessId();welcome.clientPid=peer.pid;welcome.windowsSessionId=peer.session;
    welcome.serverCreationTime=Creation(GetCurrentProcess());welcome.clientCreationTime=peer.creation;welcome.profile=o.profile;welcome.queueBlocks=QueueBlocks;welcome.maximumAgeMs=MaximumAgeMs;welcome.binding=wireBinding;
    AudioScheduling audioScheduling(mmcssRevertFailed);
    ++sessions;
    WriteLog(log.Get(),"{\"event\":\"session_start\",\"session\":"+std::to_string(sessions)+",\"nonce\":"+Json(NonceText(nonce))+",\"peer_pid\":"+std::to_string(peer.pid)+
     ",\"writer_thread_id\":"+std::to_string(GetCurrentThreadId())+",\"writer_mmcss\":true,\"qpc\":"+std::to_string(Clock())+"}\n");
    pipeline.Arm(sessions,nonce);armed=true;
    io.Transfer(pipe.Get(),&welcome,sizeof(welcome),true,stop.Get(),pipeline.fault.Get(),2000);
    peerWatch.emplace(pipe.Get());
    auto messageOwner=std::make_unique<AudioMessage>();auto& message=*messageOwner;
    for(;;){
     if(pipeline.Error()!=NoError)throw Failure(pipeline.Error(),"USB pipeline invalidated");
     if(WaitForSingleObject(stop.Get(),0)==WAIT_OBJECT_0)throw Failure(ControlledStop,"Controlled stop");
     if(!controller->connected())throw Failure(SourceUnavailable,"Owned USB import disconnected");
     const auto next=pipeline.Next(message);
     if(next==Pipeline::NextResult::Block){
      if(!blocks&&boundary.pending)boundary.VerifyFirst(message.info);
      message.info.sendQpc=Clock();
      Require(Fresh(message.info.sendQpc,message.info.blockReadyQpc,message.info.qpcFrequency),StaleBlock,"Ready USB block exceeded age bound");
      const auto remaining=AgeBudgetTicks(message.info.qpcFrequency)-(message.info.sendQpc-message.info.blockReadyQpc);
      const auto timeout=static_cast<DWORD>(std::max<std::uint64_t>(1,(remaining*1000+message.info.qpcFrequency-1)/message.info.qpcFrequency));
      attempted=message.info.blockSequence;
      try{io.Transfer(pipe.Get(),&message,AudioWireBytes(o.profile.channels),true,stop.Get(),pipeline.fault.Get(),timeout,
                      &pipeline.ErrorSource(),message.info.blockReadyQpc+AgeBudgetTicks(message.info.qpcFrequency),message.info.qpcFrequency);}
      catch(...){++unconfirmed;uncertain=true;throw;}
      const auto completed=Clock();++blocks;++totalBlocks;lastEpoch=message.info.epoch;lastFrameEnd=message.info.firstFrame+Frames;
      (void)diagnostics.Block(message.info,completed);
      Require(Fresh(completed,message.info.blockReadyQpc,message.info.qpcFrequency),StaleBlock,"Completed USB block exceeded age bound");
      if(blocks==1&&boundary.pending){
       const auto gap=message.info.firstFrame-boundary.endFrame;
       resumedFrom=boundary;resumedFirstFrame=message.info.firstFrame;resumedQpc=completed;
       gapFrames+=gap;++resumedOverflows;boundary.pending=false;
      }
      continue;
     }
     if(next==Pipeline::NextResult::Terminal)throw Failure(NoError,"Explicit USB AS0, complete blocks drained");
     HANDLE waits[]={stop.Get(),pipeline.fault.Get(),pipeline.available.Get(),peer.process.Get(),peerWatch->Event()};
     const auto result=WaitForMultipleObjects(5,waits,FALSE,100);
     if(result==WAIT_OBJECT_0+3)throw Failure(PeerDisconnected,"Peer process exited");
     if(result==WAIT_OBJECT_0+4)peerWatch->Completed();
     if(result==WAIT_FAILED)throw Failure(InternalFailure,"Wait for USB data failed",GetLastError());
    }
   }catch(const Failure& f){endCode=f.code;endSystem=f.systemError;
    if(!armed){++rejections;WriteLog(log.Get(),"{\"event\":\"connection_rejected\",\"code\":"+std::to_string(f.code)+",\"reason\":"+Json(f.what())+"}\n");}
   }
   if(mmcssRevertFailed){endCode=InternalFailure;failure=true;SetEvent(stop.Get());}
   if(armed){
    pipeline.Disarm();const auto discarded=pipeline.DiscardAfterDisarm();const auto partial=pipeline.PartialAfterDisarm();totalDiscarded+=discarded;totalPartial+=partial;
    if(endCode==NoError&&(partial||discarded||pipeline.Error()!=NoError))endCode=InputLoss;
    const bool terminalClose=endCode==NoError;
    if(pipeline.Error()==QueueOverflow&&uncertain){endCode=QueueOverflow;if(!endSystem)endSystem=ERROR_OPERATION_ABORTED;}
    if(endCode==QueueOverflow&&!blocks&&!endSystem)endSystem=ERROR_INVALID_DATA;
    const bool overflowClose=endCode==QueueOverflow&&!uncertain&&!endSystem&&blocks>0&&!boundary.pending;
    if(endCode==QueueOverflow&&!overflowClose&&!endSystem)endSystem=ERROR_INVALID_DATA;
    bool endWritten=false,terminalClientClosed=false;DWORD closeError=0;std::uint64_t endWriteQpc=0,clientCloseQpc=0;
    try{ErrorMessage end{};end.header=MakeHeader(endCode==ControlledStop||endCode==NoError?Goodbye:Error,sizeof(end),nonce);end.code=endCode;end.systemError=endSystem;end.lastBlockSequence=blocks?blocks-1:UINT64_MAX;
     PipeOperation io;io.Transfer(pipe.Get(),&end,sizeof(end),true,nullptr,nullptr,100);endWritten=true;endWriteQpc=Clock();
     if(terminalClose||overflowClose){
      Require(peerWatch.has_value(),InternalFailure,"Missing active terminal peer watch");
      closeError=peerWatch->AwaitTerminalClose(stop.Get(),2000);terminalClientClosed=true;clientCloseQpc=Clock();
     }
    }catch(const Failure& error){
     if(terminalClose){endCode=error.code;endSystem=error.systemError;failure=true;}
     if(overflowClose){endSystem=error.systemError?error.systemError:ERROR_INVALID_DATA;failure=true;}
    }
    const bool offered=overflowClose&&endWritten&&terminalClientClosed&&!endSystem;
    if(offered){boundary.pending=true;boundary.peerPid=peerPid;boundary.peerCreation=peerCreation;boundary.oldNonce[0]=nonce[0];boundary.oldNonce[1]=nonce[1];boundary.epoch=lastEpoch;boundary.endFrame=lastFrameEnd;boundary.session=sessions;++overflowEnds;}
    if(resumedFrom){
     const auto& previous=*resumedFrom;
     WriteLog(log.Get(),"{\"event\":\"overflow_resumed\",\"session\":"+std::to_string(sessions)+",\"prior_session\":"+std::to_string(previous.session)+
      ",\"prior_nonce\":"+Json(NonceText(previous.oldNonce))+",\"nonce\":"+Json(NonceText(nonce))+",\"epoch\":"+std::to_string(previous.epoch)+
      ",\"previous_frame_end\":"+std::to_string(previous.endFrame)+",\"first_frame\":"+std::to_string(resumedFirstFrame)+
      ",\"gap_frames\":"+std::to_string(resumedFirstFrame-previous.endFrame)+",\"first_write_complete_qpc\":"+std::to_string(resumedQpc)+"}\n");
    }
    WriteLog(log.Get(),"{\"event\":\"session_end\",\"session\":"+std::to_string(sessions)+",\"nonce\":"+Json(NonceText(nonce))+",\"code\":"+std::to_string(endCode)+
     ",\"blocks_written\":"+std::to_string(blocks)+",\"frames_admitted\":"+std::to_string(pipeline.FramesAfterDisarm())+",\"queued_discarded_blocks\":"+std::to_string(discarded)+
     ",\"partial_discarded_frames\":"+std::to_string(partial)+",\"queue_maximum\":"+std::to_string(pipeline.MaximumAfterDisarm())+",\"queue_overruns\":"+std::to_string(pipeline.OverrunsAfterDisarm())+
     ",\"writer_mmcss_reverted\":"+(mmcssRevertFailed?"false":"true")+",\"terminal_as0\":"+(pipeline.Terminal()?"true":"false")+
     ",\"terminal_epoch\":"+std::to_string(pipeline.TerminalEpochAfterDisarm())+",\"terminal_qpc\":"+std::to_string(pipeline.TerminalQpcAfterDisarm())+
     ",\"pipeline_error\":"+std::to_string(pipeline.Error())+",\"last_confirmed_sequence\":"+std::to_string(blocks?blocks-1:UINT64_MAX)+",\"last_attempted_sequence\":"+std::to_string(attempted)+
     ",\"last_confirmed_frame_end\":"+std::to_string(lastFrameEnd)+",\"native_epoch\":"+std::to_string(lastEpoch)+",\"unconfirmed_write\":"+(uncertain?"true":"false")+
     ",\"overflow_reconnect_offered\":"+(offered?"true":"false")+",\"end_written\":"+(endWritten?"true":"false")+",\"end_write_qpc\":"+std::to_string(endWriteQpc)+",\"terminal_client_closed\":"+(terminalClientClosed?"true":"false")+
     ",\"terminal_client_close_qpc\":"+std::to_string(clientCloseQpc)+",\"terminal_close_error\":"+std::to_string(closeError)+",\"end_system_error\":"+std::to_string(endSystem)+"}\n");
    // A disconnect may release the server for another authenticated client,
    // but cannot establish a clean media boundary or a successful lifetime.
    if(endCode!=ControlledStop&&endCode!=NoError&&!offered)failure=true;
   }
   peerWatch.reset();
   DisconnectNamedPipe(pipe.Get());
  }
 }catch(...){pipeline.Disarm();SetEvent(stop.Get());if(duration.joinable())duration.join();controller->stop();diagnostics.Stop();throw;}
 pipeline.Disarm();SetEvent(stop.Get());if(duration.joinable())duration.join();const auto native=controller->stop();diagnostics.Stop();owner.Close();
 try{if(const auto* clock=controller->clock_after_stop())usb::write_clock_evidence(o.out,*clock);}catch(...){/* optional clock diagnostics */}
 failure=failure||boundary.pending||(!sessions&&rejections)||native.callback_failures||native.protocol_failures||
  native.worker_failed||!native.mmcss_registered||!native.mmcss_reverted||owner.Failed();
 WriteLog(log.Get(),"{\"event\":\"summary\",\"sessions\":"+std::to_string(sessions)+",\"rejections\":"+std::to_string(rejections)+",\"blocks_written\":"+std::to_string(totalBlocks)+
  ",\"queued_discarded_blocks\":"+std::to_string(totalDiscarded)+",\"partial_discarded_frames\":"+std::to_string(totalPartial)+",\"unconfirmed_blocks\":"+std::to_string(unconfirmed)+
  ",\"native_audio_frames\":"+std::to_string(native.audio_frames)+",\"native_audio_urbs\":"+std::to_string(native.audio_urbs)+",\"native_protocol_failures\":"+std::to_string(native.protocol_failures)+
  ",\"native_callback_failures\":"+std::to_string(native.callback_failures)+",\"native_unlinked_after_capture_frames\":"+std::to_string(native.audio_frames_unlinked_after_capture)+
  ",\"native_canceled_urbs\":"+std::to_string(native.canceled)+",\"native_late_urbs\":"+std::to_string(native.late_urbs)+",\"native_pending_on_close\":"+std::to_string(native.pending_on_close)+
  ",\"native_discarded_frames_on_close\":"+std::to_string(native.audio_frames_discarded_on_close)+",\"worker_mmcss_registered\":"+(native.mmcss_registered?"true":"false")+
  ",\"worker_mmcss_reverted\":"+(native.mmcss_reverted?"true":"false")+",\"worker_failed\":"+(native.worker_failed?"true":"false")+
  ",\"raw_records_written\":"+std::to_string(diagnostics.NativeWrittenAfterStop())+",\"raw_records_rejected\":"+std::to_string(diagnostics.NativeRejected())+
  ",\"block_diagnostics_rejected\":"+std::to_string(diagnostics.BlocksRejected())+",\"owner_pid\":"+std::to_string(o.ownerPid)+
  ",\"owner_creation_time\":"+std::to_string(o.ownerCreation)+",\"owner_exit_qpc\":"+std::to_string(owner.ExitQpc())+",\"owner_watch_failed\":"+(owner.Failed()?"true":"false")+
  ",\"overflow_ends\":"+std::to_string(overflowEnds)+",\"resumed_overflows\":"+std::to_string(resumedOverflows)+",\"gap_frames\":"+std::to_string(gapFrames)+
  ",\"clean_lifecycle_exit\":"+(failure?"false":"true")+",\"accepted_transport\":"+(failure||overflowEnds?"false":"true")+"}\n");return failure?2:0;
 } catch(const Failure& e) {
   try { WriteLog(log.Get(),"{\"event\":\"failure\",\"code\":"+std::to_string(e.code)+",\"win32\":"+std::to_string(e.systemError)+",\"message\":"+Json(e.what())+"}\n"); } catch(...) {}
   throw;
 }
}
} // namespace vibe_bridge
#ifndef VIBE_BRIDGE_TESTING
int wmain(int argc,wchar_t** argv){
 if(argc==2 && std::wstring(argv[1])==L"--version"){
  std::cout<<"VibeUsbBridge 1.0-native; USB identity 1209:7140; IPC 1\n";return 0;
 }
 try{return vibe_bridge::Run(vibe_bridge::Parse(argc,argv));}
 catch(const vibe_bridge::Failure& e){std::cerr<<"Vibe USB bridge failure "<<e.code<<" / Windows "<<e.systemError<<": "<<e.what()<<'\n';return 2;}
 catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 2;}
}
#endif

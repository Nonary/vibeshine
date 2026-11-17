// SPDX-License-Identifier: GPL-3.0-or-later
#define VIBE_BRIDGE_TESTING
#include "../src/bridge_main.cpp"
#include <cstdlib>
#include <new>
#include <cmath>
static thread_local bool countNew=false;
static thread_local std::uint64_t newCount=0;
_Use_decl_annotations_ void* operator new(std::size_t n){if(countNew)++newCount;if(auto* p=std::malloc(n?n:1))return p;throw std::bad_alloc();}
_Use_decl_annotations_ void* operator new[](std::size_t n){return ::operator new(n);}
void operator delete(void* p)noexcept{std::free(p);}
void operator delete[](void* p)noexcept{std::free(p);}
void operator delete(void* p,std::size_t)noexcept{std::free(p);}
void operator delete[](void* p,std::size_t)noexcept{std::free(p);}
namespace {
using namespace vibe_bridge;
std::uint64_t checks=0;
void Check(bool v,const char* text){++checks;if(!v)throw std::runtime_error(text);}
struct Fixture {
 Profile p;usb::pcm_metadata m{};
 std::vector<std::uint8_t> bytes;
 std::vector<usb::iso_packet> packets;
 Fixture(Profile profile,std::uint32_t frames,std::uint64_t first=0,std::uint32_t sequence=1):p(profile){
  const auto align=p.channels*(p.nativeContainerBits/8u);bytes.resize(frames*align);
  m.sequence=sequence;m.epoch=3;m.packet_count=1;m.first_frame=first;m.frames=frames;m.receive_qpc=100+sequence;m.ready_qpc=200+sequence;m.qpc_frequency=10000000;
  m.native_bytes=static_cast<std::uint32_t>(bytes.size());m.channels=p.channels;m.channel_mask=p.channelMask;m.native_container_bits=p.nativeContainerBits;m.native_valid_bits=p.nativeValidBits;
  packets.push_back({0,static_cast<std::uint32_t>(bytes.size())});
  for(std::uint32_t f=0;f<frames;++f)for(std::uint32_t c=0;c<p.channels;++c){
   auto* dest=bytes.data()+f*align+c*(p.nativeContainerBits/8u);
   const auto sample=static_cast<std::int32_t>(((first+f)*13+c*791)%60001)-30000;
   if(p.nativeContainerBits==16){const auto n=static_cast<std::int16_t>(sample);std::memcpy(dest,&n,2);}else{const std::int32_t n=sample*256;std::memcpy(dest,&n,4);}
  }
 }
 float Expected(std::uint64_t frame,unsigned c)const{const auto sample=static_cast<std::int32_t>((frame*13+c*791)%60001)-30000;return p.nativeContainerBits==16?static_cast<float>(sample)/32768.f:static_cast<float>(sample*256)/2147483648.f;}
};
void ProfileTests(){
 const std::uint32_t channels[]={2,6,6,8,12},masks[]={3,0x3f,0x60f,0x63f,0x2d63f};
 for(unsigned layout=0;layout<5;++layout)for(unsigned bits=0;bits<2;++bits){
  Profile p{48000,static_cast<std::uint16_t>(channels[layout]),240,masks[layout],static_cast<std::uint16_t>(bits?32:16),static_cast<std::uint16_t>(bits?24:16)};
  Check(ValidProfile(p),"Exact native profile");
  BlockAssembler a;const std::uint64_t nonce[]{17,19};a.Reset(p,nonce);auto first=Fixture(p,341),last=Fixture(p,139,341,8);
  unsigned emitted=0;AudioHeader h[2]{};
  const auto emit=[&](const AudioMessage& b){
   Check(b.info.blockSequence==emitted&&b.info.firstFrame==emitted*240,"Block/frame continuity");
   Check(b.info.header.bytes==192u+240u*4u*p.channels&&b.info.payloadBytes==240u*4u*p.channels,"Wire excludes unused maximum channels");
   Check(b.info.blockReadyQpc==1000&&b.info.firstReceiveQpc<=b.info.lastReceiveQpc,"Readiness after admission");
   for(unsigned f=0;f<240;++f)for(unsigned c=0;c<p.channels;++c)Check(b.samples[f*p.channels+c]==first.Expected(emitted*240+f,c),"Exact lane/sample identity");
   h[emitted++]=b.info;return true;
  };
  Check(a.Append(first.m,first.packets,first.bytes,[]{return 1000ull;},emit)==NoError,"341 frames accepted");
  Check(a.PartialFrames()==101&&emitted==1,"Fragment preserved");
  Check(a.Append(last.m,last.packets,last.bytes,[]{return 1000ull;},emit)==NoError,"139 frames accepted");
  Check(emitted==2&&!a.PartialFrames(),"Two exact blocks");
  Check(h[0].firstUrbFrameOffset==0&&h[0].lastUrbFrameOffset==239&&h[1].firstUrbFrameOffset==240&&h[1].lastUrbFrameOffset==138,"Contributor offsets inclusive");
  Check(h[1].firstUrbSequence==1&&h[1].lastUrbSequence==8,"Control URB sequence gaps allowed");
  auto invalid=first;invalid.m.native_valid_bits=20;a.Reset(p,nonce);
  Check(a.Append(invalid.m,invalid.packets,invalid.bytes,[]{return 1000ull;},emit)==InvalidRecord,"Native bits mismatch rejected");
  if(bits){invalid=first;invalid.bytes[0]=1;Check(a.Append(invalid.m,invalid.packets,invalid.bytes,[]{return 1000ull;},emit)==InvalidRecord,"24-bit padding must be zero");}
 }
}
void IntegrityTests(){
 Profile p{48000,12,240,0x2d63f,32,24};const std::uint64_t nonce[]{1,2};BlockAssembler a;
 auto f=Fixture(p,480);const auto sink=[](const AudioMessage&){return true;};const auto clock=[]{return 1000ull;};
 {auto empty=Fixture(p,0);a.Reset(p,nonce);bool emitted=false;Check(a.Append(empty.m,empty.packets,empty.bytes,clock,[&](const AudioMessage&){emitted=true;return true;})==NoError&&!emitted,"Empty ISO request creates no PCM");}
 auto invalid=[&](Fixture bad,ErrorCode wanted){a.Reset(p,nonce);Check(a.Append(bad.m,bad.packets,bad.bytes,clock,sink)==wanted,"Malformed record rejected");};
 {auto b=f;b.m.frames++;invalid(b,InvalidRecord);}{auto b=f;b.m.channels=8;invalid(b,InvalidRecord);}
 {auto b=f;b.m.channel_mask=0x63f;invalid(b,InvalidRecord);}{auto b=f;b.m.ready_qpc=1;invalid(b,InvalidRecord);}
 {auto b=f;b.m.packet_count=2;invalid(b,InvalidRecord);}{auto b=f;b.packets[0].length--;invalid(b,InvalidRecord);}
 {auto b=f;b.packets[0].offset=48;invalid(b,InvalidRecord);}{auto b=f;b.m.first_frame=UINT64_MAX-10;invalid(b,InvalidRecord);}
 a.Reset(p,nonce);Check(a.Append(f.m,f.packets,f.bytes,clock,sink)==NoError,"Initial record accepted");
 auto next=Fixture(p,480,481,2);Check(a.Append(next.m,next.packets,next.bytes,clock,sink)==SequenceGap,"Later frame gap rejected");
 next=Fixture(p,480,480,2);next.m.epoch++;Check(a.Append(next.m,next.packets,next.bytes,clock,sink)==EpochChanged,"Epoch change rejected");
 next.m.epoch--;next.m.sequence=1;Check(a.Append(next.m,next.packets,next.bytes,clock,sink)==SequenceGap,"Duplicate URB rejected");
 a.Reset(p,nonce);Check(a.Append(f.m,f.packets,f.bytes,clock,[](const AudioMessage&){return false;})==QueueOverflow,"Explicit downstream backpressure");
 // Packet placement gaps are not audio; concatenate only original descriptor ranges.
 auto spaced=Fixture(p,480);spaced.bytes.insert(spaced.bytes.begin()+240*48,96,0xab);spaced.packets={{0,240*48},{240*48+96,240*48}};spaced.m.packet_count=2;
 a.Reset(p,nonce);unsigned blocks=0;Check(a.Append(spaced.m,spaced.packets,spaced.bytes,clock,[&](const AudioMessage& b){for(unsigned i=0;i<2880;++i)Check(b.samples[i]==spaced.Expected(blocks*240+i/12,i%12),"Ignore USB placement gap");++blocks;return true;})==NoError,"Packet-concatenated PCM");
}
void QueueTests(){
 Check(Fresh(200000,0,10000000)&&!Fresh(200001,0,10000000)&&!Fresh(0,1,10000000)&&!Fresh(0,0,0),"Exact full end-to-end bridge age bound");
 for(const auto seconds:{1u,3600u,3690u,14400u,86400u})Check(ValidDuration(seconds),"Managed finite duration accepted");
 for(const auto seconds:{86401u,UINT32_MAX})Check(!ValidDuration(seconds),"Out of range duration rejected");
 Check(ValidDuration(0)&&DurationWait(0)==INFINITE,"Production wait cannot expire at four hours");
 Check(DurationWait(1)==1000&&DurationWait(14400)==14400000,"Finite deadlines are test-only");
 {
  bool expired=false;std::uint64_t modelMs=0;
  AwaitSessionStop(0,[&](std::uint32_t timeout){modelMs=14400001;Check(timeout==INFINITE,"Actual product wait remains infinite across former four-hour boundary");return WAIT_OBJECT_0;},[&]{expired=true;});
  Check(modelMs>14400000&&!expired,"Virtual four-hour boundary cannot expire a product session");
  AwaitSessionStop(14400,[&](std::uint32_t timeout){Check(timeout==14400000,"Finite explicit test boundary");return WAIT_TIMEOUT;},[&]{expired=true;});
  Check(expired,"Actual expiry decision still works for bounded test runs");
 }
 auto owner=std::make_unique<Spsc<AudioMessage,4>>();auto& q=*owner;AudioMessage m{};
 for(unsigned i=0;i<4;++i)Check(q.Push([&](AudioMessage& b,std::uint32_t depth){b.info.blockSequence=i;b.info.queueDepthBlocks=depth;}),"Queue capacity");
 Check(!q.Push([](AudioMessage&,std::uint32_t){}),"Fifth block rejected");
 for(unsigned i=0;i<4;++i)Check(q.Pop(m)&&m.info.blockSequence==i&&m.info.queueDepthBlocks==i+1,"Queue order/depth");
 Check(!q.Pop(m),"No synthetic underrun samples");
 Check(q.Push([](AudioMessage&,std::uint32_t){}),"Reuse slot");Check(q.DiscardAfterProducerStopped()==1&&!q.Pop(m),"Reconnect flush");
}
void PipelineTests(){
 Profile p{48000,12,240,0x2d63f,32,24};const std::uint64_t nonce[]{71,73};auto owner=std::make_unique<Pipeline>(p);auto& pipe=*owner;auto f=Fixture(p,480);f.m.receive_qpc=Clock();f.m.ready_qpc=Clock();f.m.qpc_frequency=Frequency();
 pipe.Native(f.m,f.packets,f.bytes);AudioMessage b;Check(!pipe.Pop(b),"Inactive admission produces no stale blocks");
 pipe.Arm(1,nonce);pipe.Native(f.m,f.packets,f.bytes);Check(pipe.Pop(b)&&b.info.blockSequence==0,"Armed first block");Check(pipe.Pop(b)&&b.info.blockSequence==1,"Armed second block");
 pipe.Reset(4,true);Check(pipe.Terminal()&&pipe.Error()==NoError,"Exact AS0 is terminal candidate");pipe.Disarm();Check(!pipe.DiscardAfterDisarm()&&!pipe.PartialAfterDisarm(),"Terminal fully drained");
 pipe.Arm(2,nonce);pipe.Native(f.m,f.packets,f.bytes);pipe.Reset(4,false);Check(pipe.Error()==EpochChanged,"Nonterminal reset invalidates immediately");pipe.Disarm();Check(pipe.DiscardAfterDisarm()==2,"Reset queued data discarded visibly");
 pipe.Arm(3,nonce);auto partial=Fixture(p,1);partial.m.receive_qpc=Clock();partial.m.ready_qpc=Clock();partial.m.qpc_frequency=Frequency();pipe.Native(partial.m,partial.packets,partial.bytes);pipe.Reset(4,true);pipe.Disarm();Check(pipe.PartialAfterDisarm()==1,"AS0 partial loss remains visible");
 pipe.Arm(4,nonce);pipe.Disarm();Check(!pipe.PartialAfterDisarm()&&!pipe.MaximumAfterDisarm(),"Empty reconnect cannot inherit partial/peak");
}
void DiagnosticDrainRace(){
 Profile p{48000,2,240,3,32,24};auto f=Fixture(p,240);Handle reached(CreateEventW(nullptr,TRUE,FALSE,nullptr)),release(CreateEventW(nullptr,TRUE,FALSE,nullptr));
 std::atomic<bool> armed{true};
 const auto prefix=L"diagnostic-drain-race-"+std::to_wstring(GetCurrentProcessId())+L"-"+std::to_wstring(Clock());
 auto d=std::make_unique<Diagnostics>(prefix,p,true,false,[&]{if(armed.exchange(false)){SetEvent(reached.Get());WaitForSingleObject(release.Get(),INFINITE);}});
 Check(WaitForSingleObject(reached.Get(),2000)==WAIT_OBJECT_0,"Writer paused after empty read");
 Check(d->Native(f.m,f.packets,f.bytes),"Last real record published in empty/stop gap");
 d->RequestStopForTest(); // deterministic publication before writer resumes
 SetEvent(release.Get());d->Stop();
 Check(!d->Failed()&&d->NativeWrittenAfterStop()==1,"Last record survives shutdown race");
}
void TerminalAndResetRaces(){
 Profile p{48000,2,240,3,32,24};const std::uint64_t nonce[]{19,23};auto owner=std::make_unique<Pipeline>(p);auto& pipe=*owner;
 auto f=Fixture(p,480);f.m.receive_qpc=Clock();f.m.ready_qpc=Clock();f.m.qpc_frequency=Frequency();AudioMessage message{};
 pipe.Arm(1,nonce);bool inject=true;
 pipe.emptyAfterPopHook=[&]{if(inject){inject=false;pipe.Native(f.m,f.packets,f.bytes);pipe.Reset(4,true);}};
 Check(pipe.Next(message)==Pipeline::NextResult::Empty,"AS0 appearing after empty read requires another drain");
 Check(pipe.Next(message)==Pipeline::NextResult::Block&&message.info.blockSequence==0,"Late final first block preserved");
 Check(pipe.Next(message)==Pipeline::NextResult::Block&&message.info.blockSequence==1,"Late final second block preserved");
 Check(pipe.Next(message)==Pipeline::NextResult::Terminal,"AS0 only completes after full drain");pipe.Disarm();pipe.DiscardAfterDisarm();
 pipe.Arm(2,nonce);pipe.Native(f.m,f.packets,f.bytes);
 Handle observed(CreateEventW(nullptr,TRUE,FALSE,nullptr)),release(CreateEventW(nullptr,TRUE,FALSE,nullptr)),retired(CreateEventW(nullptr,TRUE,FALSE,nullptr));
 pipe.resetObservedHook=[&]{SetEvent(observed.Get());WaitForSingleObject(release.Get(),INFINITE);};
 std::thread reset([&]{pipe.Reset(4,false);});
 if(WaitForSingleObject(observed.Get(),2000)!=WAIT_OBJECT_0){SetEvent(release.Get());reset.join();throw std::runtime_error("Reset race setup timeout");}
 std::thread disarm([&]{pipe.Disarm();pipe.DiscardAfterDisarm();pipe.Arm(3,nonce);SetEvent(retired.Get());});
 const bool blocked=WaitForSingleObject(retired.Get(),20)==WAIT_TIMEOUT;
 SetEvent(release.Get());reset.join();disarm.join();
 Check(blocked,"Disarm waits for old Reset callback retirement");
 Check(pipe.Error()==NoError&&!pipe.Terminal(),"Old Reset cannot contaminate a new session");pipe.Disarm();
}
void SteadyAllocation(){
 Profile p{48000,12,240,0x2d63f,32,24};const std::uint64_t nonce[]{3,4};BlockAssembler a;a.Reset(p,nonce);auto f=Fixture(p,480);auto q=std::make_unique<Spsc<AudioMessage,4>>();auto& queue=*q;auto out=std::make_unique<AudioMessage>();auto& output=*out;
 newCount=0;countNew=true;
 for(unsigned i=0;i<1000;++i){f.m.first_frame=i*480ull;f.m.sequence=i+1;f.m.receive_qpc=100+i;f.m.ready_qpc=200+i;
  Check(a.Append(f.m,f.packets,f.bytes,[]{return 10000ull;},[&](const AudioMessage& b){return queue.Push([&](AudioMessage& d,std::uint32_t depth){d=b;d.info.queueDepthBlocks=depth;});})==NoError,"Allocation loop admission");
  Check(queue.Pop(output)&&queue.Pop(output),"Immediate two-block consume");
 }
 countNew=false;Check(newCount==0,"Zero successful-path allocations");
}
void PipeTests(){
 Options o;o.pipeName=L"VibeUsbBridgeOfflineTest."+std::to_wstring(GetCurrentProcessId());auto pipe=MakePipe(o);
 o.clientImage=ProcessImage(GetCurrentProcess());Handle ownImage(CreateFileW(o.clientImage.c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,0,nullptr));Check(ownImage.Valid(),"Open offline peer image");
 o.clientHash=Sha256(ownImage.Get());const LockedPeerImage pinned(o.clientImage,o.clientHash);
 bool duplicate=false;try{auto another=MakePipe(o);}catch(const Failure&){duplicate=true;}Check(duplicate,"Exclusive first pipe instance");
 Handle stop(CreateEventW(nullptr,TRUE,FALSE,nullptr)),closePeer(CreateEventW(nullptr,TRUE,FALSE,nullptr)),releasePeer(CreateEventW(nullptr,TRUE,FALSE,nullptr));std::exception_ptr failure;
 std::thread client([&]{try{
  const auto path=L"\\\\.\\pipe\\"+o.pipeName;Handle h(CreateFileW(path.c_str(),GENERIC_READ|GENERIC_WRITE,0,nullptr,OPEN_EXISTING,FILE_FLAG_OVERLAPPED,nullptr));WinCheck(h.Valid(),InternalFailure,"Offline pipe client");
  DWORD mode=PIPE_READMODE_MESSAGE;WinCheck(SetNamedPipeHandleState(h.Get(),&mode,nullptr,nullptr),InternalFailure,"Set message mode");
  const std::uint64_t challenge[]{1,2};HelloMessage hello{};hello.header=MakeHeader(Hello,sizeof(hello),challenge);
  PipeOperation operation;operation.Transfer(h.Get(),&hello,sizeof(hello),true,stop.Get(),nullptr,1000);
  WaitForSingleObject(closePeer.Get(),INFINITE);h.Close();
  WaitForSingleObject(releasePeer.Get(),INFINITE);
 }catch(...){failure=std::current_exception();}});
 try{Check(AwaitClient(pipe.Get(),stop.Get()),"Actual overlapped accept");
  auto bad=o;bad.clientHash[0]=bad.clientHash[0]=='0'?'1':'0';bool rejected=false;try{auto peer=Authenticate(pipe.Get(),pinned,bad);}catch(const Failure& e){rejected=e.code==BindingMismatch;}Check(rejected,"Actual wrong peer hash rejected");
  auto peer=Authenticate(pipe.Get(),pinned,o);Check(peer.pid==GetCurrentProcessId()&&peer.creation==Creation(GetCurrentProcess()),"Actual peer PID/SID/session/lifetime/image binding");
  HelloMessage hello{};PipeOperation io;io.Transfer(pipe.Get(),&hello,sizeof(hello),false,stop.Get(),nullptr,1000);Check(ValidHeader(hello.header,Hello,sizeof(hello)),"Actual complete message");
  unsigned char byte=0;bool timedOut=false;try{io.Transfer(pipe.Get(),&byte,1,false,stop.Get(),nullptr,1);}catch(const Failure& e){timedOut=e.code==PipeTimeout;}Check(timedOut,"Actual pending read timeout retired");
  SetEvent(stop.Get());bool stopped=false;try{io.Transfer(pipe.Get(),&byte,1,false,stop.Get(),nullptr,1000);}catch(const Failure& e){stopped=e.code==ControlledStop;}Check(stopped,"Actual pending read canceled and retired on stop");
  {PeerReadWatch canceled(pipe.Get());Check(WaitForSingleObject(canceled.Event(),0)==WAIT_TIMEOUT,"Idle close watch really pending");}
  PeerReadWatch watch(pipe.Get());SetEvent(closePeer.Get());Check(WaitForSingleObject(watch.Event(),2000)==WAIT_OBJECT_0,"Living idle peer close signals without audio");
  bool closed=false;try{watch.Completed();}catch(const Failure& e){closed=e.code==PeerDisconnected;}Check(closed,"Living peer pipe EOF precisely classified");
 }
 catch(...){SetEvent(stop.Get());SetEvent(closePeer.Get());SetEvent(releasePeer.Get());client.join();throw;}SetEvent(closePeer.Get());SetEvent(releasePeer.Get());client.join();if(failure)std::rethrow_exception(failure);
}
void TerminalCloseTests(){
 for(const bool extraClientByte:{false,true}){
  Options options;options.pipeName=L"VibeUsbTerminalCloseTest."+std::to_wstring(GetCurrentProcessId());auto server=MakePipe(options);
  Handle stop(CreateEventW(nullptr,TRUE,FALSE,nullptr));
  const auto address=L"\\\\.\\pipe\\"+options.pipeName;
  Handle client(CreateFileW(address.c_str(),GENERIC_READ|GENERIC_WRITE,0,nullptr,OPEN_EXISTING,FILE_FLAG_OVERLAPPED,nullptr));
  Check(client.Valid()&&AwaitClient(server.Get(),stop.Get()),"Connected terminal test pipe");
  PeerReadWatch watch(server.Get());const std::uint64_t nonce[]={71,91};
  ErrorMessage sent{};sent.header=MakeHeader(Goodbye,sizeof(sent),nonce);sent.code=NoError;sent.lastBlockSequence=42;
  PipeOperation serverIo,clientIo;serverIo.Transfer(server.Get(),&sent,sizeof(sent),true,nullptr,nullptr,100);
  bool timedOut=false;try{watch.AwaitTerminalClose(stop.Get(),1);}catch(const Failure& error){timedOut=error.code==PipeTimeout;}
  Check(timedOut,"Client has not read Goodbye yet: no EOF evidence");
  SetEvent(stop.Get());bool canceled=false;try{watch.AwaitTerminalClose(stop.Get(),100);}catch(const Failure& error){canceled=error.code==ControlledStop;}
  Check(canceled,"External stop does not prove terminal receipt");ResetEvent(stop.Get());
  ErrorMessage received{};clientIo.Transfer(client.Get(),&received,sizeof(received),false,nullptr,nullptr,100);
  Check(ValidHeader(received.header,Goodbye,sizeof(received))&&received.code==NoError&&received.lastBlockSequence==42&&received.header.nonce[1]==91,"Delayed client reads exact terminal message before server disconnect");
  if(extraClientByte){unsigned char extra=1;clientIo.Transfer(client.Get(),&extra,1,true,nullptr,nullptr,100);}
  client.Close();
  if(extraClientByte){
   bool rejected=false;try{watch.AwaitTerminalClose(stop.Get(),1000);}catch(const Failure& error){rejected=error.code==InvalidHandshake;}
   Check(rejected,"Unexpected client byte is not terminal EOF");
  }else{
   const auto closed=watch.AwaitTerminalClose(stop.Get(),1000);
   Check(closed==ERROR_BROKEN_PIPE||closed==ERROR_NO_DATA||closed==ERROR_PIPE_NOT_CONNECTED,"Client closed naturally after exact terminal receipt");
  }
 }
}
void OverflowRecoveryTests(){

 OverflowBoundary b;b.pending=true;b.peerPid=17;b.peerCreation=123;b.epoch=5;b.endFrame=1440;b.oldNonce[0]=1;b.oldNonce[1]=2;
 AudioHeader h{};h.header.nonce[0]=3;h.header.nonce[1]=4;h.firstFrame=2160;h.epoch=5;b.VerifyFirst(h);
 auto rejects=[&](const AudioHeader& value){try{b.VerifyFirst(value);}catch(const Failure& e){return e.code==SequenceGap;}return false;};
 auto bad=h;bad.firstFrame=1440;Check(rejects(bad),"Recovery cannot silently claim a missing gap");bad=h;bad.epoch=6;Check(rejects(bad),"Different native epochs cannot establish absolute overflow continuity");
 bad=h;bad.blockSequence=1;Check(rejects(bad),"Recovered session must start at sequence zero");bad=h;bad.header.nonce[0]=1;bad.header.nonce[1]=2;Check(rejects(bad),"Old nonce cannot resume");
 for(unsigned trial:{0u,1u,2u}){
  const bool readPending=trial!=0, winCancelRace=trial==2;
  Options opts;opts.pipeName=L"VibeUsbOverflowTest."+std::to_wstring(GetCurrentProcessId());
  auto server=MakePipe(opts);Handle stop(CreateEventW(nullptr,TRUE,FALSE,nullptr)),fault(CreateEventW(nullptr,TRUE,TRUE,nullptr)),allow(CreateEventW(nullptr,TRUE,FALSE,nullptr));
  const auto address=L"\\\\.\\pipe\\"+opts.pipeName;
  Handle client(CreateFileW(address.c_str(),GENERIC_READ|GENERIC_WRITE,0,nullptr,OPEN_EXISTING,FILE_FLAG_OVERLAPPED,nullptr));
  Check(client.Valid()&&AwaitClient(server.Get(),stop.Get()),"Overflow test connected");
  std::vector<std::uint8_t> message(4*sizeof(AudioMessage),0xa7),data(message.size());
  // Larger than the one-message server pipe quota. Admit the reader only
  // after the pending write has observed the real producer overflow event.
  std::exception_ptr clientError;std::thread reader;
  if(readPending)reader=std::thread([&]{try{
   if(WaitForSingleObject(allow.Get(),2000)!=WAIT_OBJECT_0)throw std::runtime_error("Missing overflow wait signal");
   PipeOperation clientIo;clientIo.Transfer(client.Get(),data.data(),static_cast<DWORD>(data.size()),false,nullptr,nullptr,2000);
  }catch(...){clientError=std::current_exception();}});
  PipeOperation writer;bool entered=false,confirmed=false,unsafe=false;const std::atomic<ErrorCode> reason{QueueOverflow};
  writer.overflowObserved=[&]{entered=true;if(readPending&&!winCancelRace)SetEvent(allow.Get());};
  bool cancelRaced=false;
  if(winCancelRace)writer.beforeCancel=[&](HANDLE completed){SetEvent(allow.Get());cancelRaced=WaitForSingleObject(completed,2000)==WAIT_OBJECT_0;};
  try{writer.Transfer(server.Get(),message.data(),static_cast<DWORD>(message.size()),true,stop.Get(),fault.Get(),
                      readPending?1000:2,&reason,Qpc()+Frequency()/((readPending&&!winCancelRace)?1:500),Frequency());confirmed=true;}
  catch(const Failure& e){unsafe=e.code==QueueOverflow&&e.systemError!=0;}
  SetEvent(allow.Get());if(reader.joinable())reader.join();if(clientError)std::rethrow_exception(clientError);
  Check(entered,"Actual pending transfer reached overflow path");
  if(readPending)Check(confirmed&&!unsafe&&data==message,"Confirmed original in-flight message delivered exactly once");
  else Check(!confirmed&&unsafe,"Timeout before confirmed write retains code8 with nonzero system error, no recovery");
  if(winCancelRace)Check(cancelRaced&&confirmed,"Completion won cancellation race: original message is included in last confirmed sequence");
 }
}
void HelloTests(){
 Peer peer;peer.pid=17;peer.creation=101;peer.session=3;Profile p{48000,12,240,0x2d63f,32,24};UsbBinding binding{};
 WireText(binding.endpointId,L"endpoint");WireText(binding.usbInstance,L"usb-instance");WireText(binding.controllerInstance,L"ROOT\\USB\\0000");std::memcpy(binding.serial,"test-serial",11);std::memcpy(binding.service,"12345",5);binding.port=1;binding.vendor=0x1209;binding.product=0x7140;
 const std::uint64_t challenge[]{3,4};HelloMessage h{};h.header=MakeHeader(Hello,sizeof(h),challenge);h.sourceKind=UserModeUsb;h.clientPid=17;h.expectedServerPid=19;h.windowsSessionId=3;h.clientCreationTime=101;h.serverCreationTime=103;h.profile=p;h.binding=binding;
 const auto valid=[&](const HelloMessage& b){return ValidHello(b,peer,19,103,p,binding);};Check(valid(h),"Exact Hello accepted");
 const std::uint32_t counts[]={2,2,6,6,6,6,8,8,12,12};
 const std::uint32_t masks[]={3,3,63,63,1551,1551,1599,1599,185919,185919};
 const std::uint16_t products[]={0x7140,0x7140,0x7140,0x7140,0x7140,0x7140,0x7140,0x7140,0x7140,0x7140};
 for(unsigned i=0;i<10;++i){
  auto sample=h;sample.profile.channels=static_cast<std::uint16_t>(counts[i]);sample.profile.channelMask=masks[i];
  const bool pcm16=(i%2)==1;
  sample.profile.nativeContainerBits=pcm16?16:32;sample.profile.nativeValidBits=pcm16?16:24;sample.binding.product=products[i];
  Check(ValidHello(sample,peer,19,103,sample.profile,sample.binding),"Matched actual profile PID in Hello");
  sample.binding.product=0x7141;
  Check(!ValidHello(sample,peer,19,103,sample.profile,sample.binding),"Reject wrong PID even when both supplied binding structs agree");
 }
 const auto mutate=[&](auto edit){auto b=h;edit(b);Check(!valid(b),"Tampered Hello rejected");};
 mutate([](auto& b){b.header.version++;});mutate([](auto& b){b.header.magic++;});mutate([](auto& b){b.header.bytes--;});mutate([](auto& b){b.header.flags=1;});
 mutate([](auto& b){b.header.nonce[0]=b.header.nonce[1]=0;});mutate([](auto& b){b.sourceKind=2;});mutate([](auto& b){b.clientPid++;});mutate([](auto& b){b.expectedServerPid++;});
 mutate([](auto& b){b.clientCreationTime++;});mutate([](auto& b){b.serverCreationTime++;});mutate([](auto& b){b.windowsSessionId++;});
 mutate([](auto& b){b.profile.nativeValidBits=16;});mutate([](auto& b){b.profile.channelMask=0x63f;});mutate([](auto& b){b.binding.port++;});
 mutate([](auto& b){b.binding.vendor++;});mutate([](auto& b){b.binding.product++;});mutate([](auto& b){b.binding.service[0]='9';});mutate([](auto& b){b.binding.serial[0]='x';});
 mutate([](auto& b){b.binding.endpointId[0]='x';});mutate([](auto& b){b.binding.usbInstance[0]='x';});mutate([](auto& b){b.binding.controllerInstance[0]='x';});mutate([](auto& b){b.binding.reserved=1;});
}
void ManagedOwnerTests(){
 const auto image=ProcessImage(GetCurrentProcess());
 Handle ownImage(CreateFileW(image.c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,0,nullptr));
 Check(ownImage.Valid(),"Open owner fixture image");
 const auto name=L"Local\\VibeUsbOwnerFixture-"+std::to_wstring(GetCurrentProcessId())+L"-"+std::to_wstring(Clock());
 Handle trigger(CreateEventW(nullptr,TRUE,FALSE,name.c_str()));Handle stop(CreateEventW(nullptr,TRUE,FALSE,nullptr));
 std::wstring command=L"\""+image+L"\" --owner-crash-fixture "+name;
 STARTUPINFOW startup{};startup.cb=sizeof(startup);PROCESS_INFORMATION info{};
 WinCheck(CreateProcessW(image.c_str(),command.data(),nullptr,nullptr,FALSE,CREATE_NO_WINDOW,nullptr,nullptr,&startup,&info),InternalFailure,"Start offline owner fixture");
 Handle child(info.hProcess),thread(info.hThread);Options options;
 try{
  options.ownerPid=info.dwProcessId;options.ownerCreation=Creation(child.Get());options.clientHash=Sha256(ownImage.Get());
  const LockedPeerImage pinned(image,options.clientHash);
  auto wrong=options;++wrong.ownerCreation;bool rejected=false;
  try{ManagedOwner invalid(wrong,pinned,stop.Get());}catch(const Failure& e){rejected=e.code==BindingMismatch;}
  Check(rejected,"Reused PID with different creation time rejected before attach");
  wrong=options;wrong.clientHash[0]=wrong.clientHash[0]=='0'?'1':'0';rejected=false;
  try{ManagedOwner invalid(wrong,pinned,stop.Get());}catch(const Failure& e){rejected=e.code==BindingMismatch;}
  Check(rejected,"Managed owner wrong image hash rejected");
  {ManagedOwner canceled(options,pinned,stop.Get());canceled.Close();Check(!canceled.ExitQpc()&&!canceled.Failed()&&WaitForSingleObject(stop.Get(),0)==WAIT_TIMEOUT,"Stopping watcher preserves a living owner");}
  ManagedOwner owner(options,pinned,stop.Get());
  Check(owner.VerifiedAlive()&&owner.Allows(info.dwProcessId,options.ownerCreation)&&!owner.Allows(GetCurrentProcessId(),options.ownerCreation)&&!owner.Allows(info.dwProcessId,options.ownerCreation+1),"Only exact living owner can reconnect");
  SetEvent(trigger.Get());Check(WaitForSingleObject(stop.Get(),3000)==WAIT_OBJECT_0,"Exact owner crash signals bridge stop without USB or pipe traffic");owner.Close();
  DWORD exitCode=0;Check(GetExitCodeProcess(child.Get(),&exitCode)&&exitCode==73&&owner.ExitQpc()>0&&!owner.Failed()&&!owner.VerifiedAlive(),"Owner process lifetime signals on actual self-termination");
 }catch(...){SetEvent(trigger.Get());WaitForSingleObject(child.Get(),3000);throw;}
}
void LockedImageTests(){
 wchar_t temp[MAX_PATH]{};const auto length=GetTempPathW(MAX_PATH,temp);
 Check(length>0&&length<MAX_PATH,"Find local image test directory");
 const std::wstring root=std::wstring(temp)+L"VibeUsbPeerFixture-"+std::to_wstring(GetCurrentProcessId())+L"-"+std::to_wstring(Clock());
 Check(CreateDirectoryW(root.c_str(),nullptr)!=FALSE,"Create unique peer fixture");
 const std::wstring original=root+L"\\expected.bin",alias=root+L"\\alias.bin",other=root+L"\\copy.bin";
 struct Cleanup {const std::wstring& root;const std::wstring& original;const std::wstring& alias;const std::wstring& other;
  ~Cleanup(){DeleteFileW(alias.c_str());DeleteFileW(other.c_str());DeleteFileW(original.c_str());RemoveDirectoryW(root.c_str());}
 } cleanup{root,original,alias,other};
 const std::array<unsigned char,4> fixture{1,2,3,4};
 for(const auto* path:{&original,&other}){
  Handle file(CreateFileW(path->c_str(),GENERIC_WRITE,0,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr));
  Check(file.Valid(),"Create exact test fixture");DWORD n=0;
  Check(WriteFile(file.Get(),fixture.data(),static_cast<DWORD>(fixture.size()),&n,nullptr)&&n==fixture.size(),"Write identical bytes to two different files");
 }
 Check(CreateHardLinkW(alias.c_str(),original.c_str(),nullptr)!=FALSE,"Create real second name of pinned file");
 std::string hash;{Handle file(CreateFileW(original.c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,0,nullptr));hash=Sha256(file.Get());}
 {
  const auto count=peerImageHashCalls.load();const LockedPeerImage pinned(original,hash);
  {auto match=pinned.Match(alias,hash);Check(match.Valid(),"Actual hardlink to same file accepted");}
  bool rejected=false;try{auto wrong=pinned.Match(other,hash);}catch(const Failure& e){rejected=e.code==BindingMismatch;}
  Check(rejected,"Byte-identical distinct file ID rejected");
  rejected=false;auto otherHash=hash;otherHash[0]=hash[0]=='0'?'1':'0';
  try{auto wrong=pinned.Match(original,otherHash);}catch(const Failure& e){rejected=e.code==BindingMismatch;}
  Check(rejected,"Configured hash differs from startup pin");
  Handle write(CreateFileW(alias.c_str(),GENERIC_WRITE,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,0,nullptr));
  Check(!write.Valid()&&GetLastError()==ERROR_SHARING_VIOLATION,"Global anchor denies modifying a hardlink after peer disconnect");
  Check(!MoveFileExW(other.c_str(),original.c_str(),MOVEFILE_REPLACE_EXISTING),"Pinned image cannot be replaced by same-content candidate");
  {auto next=pinned.Match(original,hash);Check(next.Valid(),"Second session retains original file under the same anchor");}
  Check(peerImageHashCalls.load()==count+1,"Exactly one complete hash across owner/peer reconnects");
 }
 Handle write(CreateFileW(original.c_str(),GENERIC_WRITE,FILE_SHARE_READ,nullptr,OPEN_EXISTING,0,nullptr));
 Check(write.Valid(),"Write denied throughout anchor lifetime and allowed after release");
}
}
int main(int argc,char** argv){
 if(argc==3&&std::strcmp(argv[1],"--owner-crash-fixture")==0){
  vibe_bridge::Handle trigger(OpenEventA(SYNCHRONIZE,FALSE,argv[2]));
  if(!trigger.Valid()||WaitForSingleObject(trigger.Get(),6000)!=WAIT_OBJECT_0)return 3;
  TerminateProcess(GetCurrentProcess(),73);return 73;
 }
 try{ProfileTests();IntegrityTests();QueueTests();PipelineTests();SteadyAllocation();HelloTests();PipeTests();TerminalCloseTests();OverflowRecoveryTests();DiagnosticDrainRace();TerminalAndResetRaces();ManagedOwnerTests();LockedImageTests();std::cout<<"PASS "<<checks<<" checks; 10 native profiles, fragments, resets, backpressure, actual pipe/owner crash; steady new="<<newCount<<"\n";return 0;}
 catch(const std::exception& e){countNew=false;std::cerr<<"FAIL: "<<e.what()<<" after "<<checks<<" checks\n";return 1;}
}

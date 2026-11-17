#include "mmcss_audio.h"
// SPDX-License-Identifier: GPL-3.0-or-later
// Offline synthetic USB/IP protocol regression; NOT an audio runtime proof.
#include "vibe_usb_audio.h"
#include <algorithm>
#include <iostream>
#include <stdexcept>
#include <string>
#include <atomic>
#include <cstdlib>
#include <new>
#ifdef _MSC_VER
#include <sal.h>
#else
#define _Use_decl_annotations_
#endif

namespace { std::atomic<std::uint64_t> allocations{0};thread_local bool observe_allocations=false; }
_Use_decl_annotations_ void* operator new(std::size_t bytes){if(observe_allocations)allocations.fetch_add(1,std::memory_order_relaxed);if(auto* p=std::malloc(bytes?bytes:1))return p;throw std::bad_alloc();}
_Use_decl_annotations_ void* operator new[](std::size_t bytes){return ::operator new(bytes);}
void operator delete(void* p) noexcept {std::free(p);}
void operator delete[](void* p) noexcept {std::free(p);}
void operator delete(void* p,std::size_t) noexcept {std::free(p);}
void operator delete[](void* p,std::size_t) noexcept {std::free(p);}

using namespace vibe::usb_audio;
namespace {
unsigned checks=0;
void check(bool v,const char* why){++checks;if(!v)throw std::runtime_error(why);}
void be(std::vector<std::uint8_t>& v,std::uint32_t x){for(int n=24;n>=0;n-=8)v.push_back(std::uint8_t(x>>n));}
std::uint32_t get(const std::vector<std::uint8_t>& v,std::size_t p){return (std::uint32_t(v.at(p))<<24)|(std::uint32_t(v.at(p+1))<<16)|(std::uint32_t(v.at(p+2))<<8)|v.at(p+3);}
identity id(unsigned channels=12){return {"0123456789ABCDEF0123456789ABCD","VA0123456789ABC",profile::from_channels(channels,channels==2?3:channels==6?0x3f:channels==8?0x63f:0x2d63f)};}
identity id(profile format){return {"0123456789ABCDEF0123456789ABCD","VA0123456789ABC",format};}
std::vector<std::uint8_t> import(const identity& d){std::vector<std::uint8_t> v{1,0x11,0x80,3,0,0,0,0};v.insert(v.end(),d.busid.begin(),d.busid.end());v.resize(40);return v;}
std::vector<std::uint8_t> urb(std::uint32_t seq,std::uint32_t ep,bool in,std::uint32_t length,std::span<const std::uint8_t> bytes={},std::span<const iso_packet> packets={},std::array<std::uint8_t,8> setup={}){
 std::vector<std::uint8_t> v;for(auto n:{1u,seq,0x10001u,in?1u:0u,ep,0u,length,0u,packets.empty()?0xffffffffu:std::uint32_t(packets.size()),1u})be(v,n);
 v.insert(v.end(),setup.begin(),setup.end());if(!in)v.insert(v.end(),bytes.begin(),bytes.end());for(const auto& p:packets){be(v,p.offset);be(v,p.length);be(v,0);be(v,0);}return v;
}
void imported(session& s,const identity& d){
 check(s.feed(import(d)),"import feed");check(s.imported(),"imported");auto reply=s.take_output();check(reply.size()==320&&get(reply,4)==0,"import record");
 check(reply[308]==0x12&&reply[309]==0x09&&((unsigned(reply[310])<<8)|reply[311])==d.format.product_id(),"Actual USB/IP import VID/PID, network byte order");
}
void descriptor_and_clock(profile format_value,bool sync){
 const auto channels=format_value.channels;
 const bool is24=format_value.native_valid_bits==24;
 const auto expected=channels==2?(is24?1u:2u):channels==6&&format_value.channel_mask==0x3f?(is24?3u:4u):
                     channels==6?(is24?5u:6u):channels==8?(is24?7u:8u):(is24?14u:9u);
 check(format_value.product_id()==expected,"Independent expected private PID for each exact profile");
 check(VibeLabUsbProduct(channels,0,format_value.native_container_bits,format_value.native_valid_bits)==0&&
       VibeLabUsbProduct(channels,format_value.channel_mask,24,24)==0&&VibeLabUsbProduct(channels,format_value.channel_mask,32,16)==0,"Reject unknown private product");
 const auto d=id(format_value);callbacks cb;cb.synchronous_out=sync;cb.record_clock=sync;std::uint64_t captured=0;
 cb.pcm=[&](const auto& m,auto,auto){captured+=m.frames;return true;};session s(d,cb);
 const auto config=s.configuration_descriptor();check(config.size()==(sync?127u:134u)&&config[2]+256u*config[3]==config.size(),"configuration total bytes");
 unsigned count=0,endpoints=0;bool format=false,mask=false,feedback=false,out=false,alt=false,clockMode=false;
 const auto device=s.device_descriptor();
 check(device.size()==18&&device[8]==0x09&&device[9]==0x12&&(unsigned(device[10])|(unsigned(device[11])<<8))==format_value.product_id(),"USB device descriptor VID/PID, little endian");
 for(std::size_t p=0;p<config.size();p+=config[p]){check(config[p]>=2&&p+config[p]<=config.size(),"descriptor bounds");++count;
  if(config[p]==6&&config[p+1]==0x24&&config[p+2]==2)format=config[p+4]==format_value.native_container_bits/8&&config[p+5]==format_value.native_valid_bits;
  if(config[p]==16&&config[p+1]==0x24){std::uint32_t m=0;for(unsigned b=0;b<4;++b)m|=std::uint32_t(config[p+11+b])<<(b*8);mask=m==d.format.channel_mask&&config[p+10]==channels;}
  if(config[p+1]==5&&config[p+2]==0x82)feedback=config[p+3]==0x11&&config[p+4]==4&&config[p+6]==4;
  if(config[p+1]==5){++endpoints;if(config[p+2]==1)out=config[p+3]==(sync?0x0d:5)&&config[p+4]+256u*config[p+5]==(sync?6u:7u)*format_value.frame_bytes()&&config[p+6]==1;}
  if(config[p+1]==4&&config[p+2]==1&&config[p+3]==1)alt=config[p+4]==(sync?1:2);
  if(config[p+1]==0x24&&config[p+2]==0x0a)clockMode=config[p+3]==4&&config[p+4]==(sync?5:1)&&config[p+5]==5;
 }
 check(count==(sync?13u:14u)&&format&&mask&&feedback==!sync&&endpoints==(sync?1u:2u)&&out&&alt&&clockMode,"Exact UAC2 clock, endpoints, format, mask and bandwidth");imported(s,d);
 auto clock=urb(1,0,true,4,{}, {},{0xa1,1,0,1,0,4,4,0});check(s.feed(clock),"clock CUR");auto r=s.take_output();check(r.size()==52&&r[48]==0x80&&r[49]==0xbb&&r[50]==0&&r[51]==0,"48000 CUR");
 auto range=urb(2,0,true,14,{}, {},{0xa1,2,0,1,0,4,14,0});check(s.feed(range),"clock RANGE");r=s.take_output();check(r.size()==62&&r[48]==1&&r[49]==0&&r[50]==0x80&&r[54]==0x80&&r[58]==0,"discrete48k range");
 if(sync){
  const std::array<iso_packet,1> fb{{{0,4}}};check(s.feed(urb(3,2,true,4,{},fb)),"unexpected feedback parsed");r=s.take_output();check(get(r,20)==std::uint32_t(-32)&&s.stats().feedback_urbs==0,"undeclared feedback endpoint stalls");
  const auto bytes=6*format_value.frame_bytes();const auto now=std::chrono::steady_clock::now();
  std::vector<std::uint8_t> data(bytes+format_value.frame_bytes());const std::array<iso_packet,1> over{{{0,std::uint32_t(data.size())}}},nominal{{{0,bytes}}};
  check(s.feed(urb(4,1,false,std::uint32_t(data.size()),data,over),now),"oversized sync packet parsed");r=s.take_output();check(get(r,20)==std::uint32_t(-32)&&captured==0,"seventh frame exceeds sync wMaxPacketSize, no invented PCM");
  check(s.feed(urb(5,1,false,bytes,std::span(data).first(bytes),nominal),now)&&captured==6&&s.output().empty(),"six samples admitted before ACK");
  const auto* c=s.clock_trace();if(!c)throw std::runtime_error("Missing SOF flight telemetry");
  check(c->synchronous_out&&c->admissions[0]==1&&c->admissions[1]==0&&c->tail[0].start_ns%125000==0&&c->tail[0].due_ns-c->tail[0].start_ns==125000,"sync first packet starts on virtual SOF grid with one tick duration");
  s.poll(now+std::chrono::milliseconds(1));r=s.take_output();check(get(r,20)==0&&get(r,24)==bytes,"original sync packet ACK length");
 }
}
void clock_flight_and_spacing(bool spaced){
 auto idValue=id();callbacks cb;std::uint64_t ticks=100;cb.qpc=[&]{return ++ticks;};cb.qpc_frequency=10000000;cb.record_clock=true;cb.space_expired_out=spaced;
 std::uint64_t captured=0;cb.pcm=[&](const auto& m,auto,auto){captured+=m.frames;return true;};session s(idValue,cb);imported(s,idValue);
 const auto now=std::chrono::steady_clock::now();const auto late=now+std::chrono::milliseconds(10);
 const std::array<iso_packet,1> packets{{{0,288}}};const std::array<std::uint8_t,288> samples{};
 check(s.feed(urb(10,1,false,288,samples,packets),now)&&s.feed(urb(11,1,false,288,samples,packets),now),"two admitted OUT requests");
 check(captured==12,"both policies publish native PCM before ACK");s.poll(late);
 check(s.output().size()==(spaced?64u:128u),"baseline publishes co-due ACKs; diagnostic policy publishes just the first");
 const auto* trace=s.clock_trace();
 if(!trace)throw std::runtime_error("Requested clock flight diagnostics were not created");
 check(trace->observations==2&&trace->tail[0].packets==1&&trace->tail[0].returned_start==std::uint32_t(trace->tail[0].start_ns/1000000)&&trace->tail[1].returned_start==std::uint32_t(trace->tail[1].start_ns/1000000)&&trace->tail[1].start_ns==trace->tail[0].due_ns&&trace->tail[0].due_ns-trace->tail[0].start_ns==125000,"exact packet clock markers");
 s.consume_output(32,late);check(trace->tail[0].first_send_qpc&&!trace->tail[0].complete_send_qpc,"partial socket acceptance is not whole RET acceptance");
 if(spaced){s.poll(late+std::chrono::microseconds(100));check(s.output().size()==32&&s.next_deadline()==std::chrono::steady_clock::time_point::max(),"cannot queue another expired OUT behind incomplete send");}
 const auto sent=late+std::chrono::microseconds(500);s.consume_output(s.output().size(),sent);
 if(spaced){
  check(s.next_deadline()==sent+std::chrono::milliseconds(1),"next expired OUT waits one real bus frame after last accepted RET");
  s.poll(sent+std::chrono::microseconds(999));check(s.output().empty(),"no early alternative RET");
  s.poll(sent+std::chrono::milliseconds(1));check(s.output().size()==64,"second expired RET delivered, no drop");s.consume_output(64,sent+std::chrono::milliseconds(1));
 }
 check(trace->socket_completed[0]==2&&trace->tail[0].complete_send_qpc&&trace->tail[1].complete_send_qpc&&((trace->tail[0].send_batch==trace->tail[1].send_batch)!=spaced),"actual send batching distinguishes both policies");
 const std::array<iso_packet,1> fb{{{0,4}}};const auto gap=now+std::chrono::milliseconds(100);
 check(s.feed(urb(12,2,true,4,{},fb),gap)&&s.feed(urb(13,1,false,288,samples,packets),gap),"admit feedback + later OUT");
 check(trace->tail[2].feedback_16_16==(6u<<16)&&trace->tail[2].due_ns-trace->tail[2].start_ns==1000000&&trace->tail[3].anchor==2&&trace->late_reanchors[0]==1,"feedback and real >50ms reanchor recorded separately");
 check(s.feed(urb(14,0,false,0,{}, {},{1,11,0,0,1,0,0,0}),gap),"explicit AS0 control");
 check(trace->reset_count==1&&trace->resets[0].sequence==14&&trace->resets[0].reason==1&&trace->tail[3].status==-104,"AS0 and canceled original OUT remain visible");
}
void boot_frame_origin(){
 const std::uint64_t wrap=UINT64_C(0x100000000)*10000;
 check(usb_start_frame(9999,99)==0&&usb_start_frame(9999,100)==1,"Keep partial milliseconds until after adding boot phase");
 check(usb_start_frame(wrap-1,0)==UINT32_MAX&&usb_start_frame(wrap-1,100)==0&&usb_start_frame(wrap-1,1000100)==1,"Natural 32-bit USBDI frame rollover");
 callbacks c,d;c.synchronous_out=d.synchronous_out=true;d.record_clock=true;
 unsigned queries=0;d.boot_interrupt_time_100ns=[&]{++queries;return wrap+30000000u;};
 auto identityValue=id();session sync(identityValue,c),boot(identityValue,d);imported(boot,identityValue);
 check(sync.configuration_descriptor()==boot.configuration_descriptor()&&sync.device_descriptor()==boot.device_descriptor(),"C/D entire descriptors identical");
 const auto* trace=boot.clock_trace();if(!trace)throw std::runtime_error("Missing D clock diagnostics");
 check(queries==9&&trace->boot_frame_origin&&trace->synchronous_out&&trace->boot_epoch_100ns>wrap,"Calibrate boot frame origin before any requests");
 const auto start=std::chrono::steady_clock::now();const std::array<iso_packet,1> one{{{0,288}}};const std::array<std::uint8_t,288> bytes{};
 check(boot.feed(urb(101,1,false,288,bytes,one),start)&&boot.feed(urb(102,1,false,288,bytes,one),start),"Admit D original payloads");
 const auto& a=trace->tail[0];const auto& b=trace->tail[1];
 check(a.returned_start==usb_start_frame(trace->boot_epoch_100ns,std::uint64_t(a.start_ns))&&b.returned_start==usb_start_frame(trace->boot_epoch_100ns,std::uint64_t(b.start_ns))&&a.due_ns-a.start_ns==125000&&b.start_ns==a.due_ns&&b.due_ns-b.start_ns==125000,"D modifies only returned epoch, not the C service grid");
 check(queries==9,"No interrupt-time calls on audio admissions");
}
void audio_and_unlink(profile format_value){
 const auto channels=format_value.channels;
 auto d=id(format_value);std::uint64_t calls=0,frames=0,ticks=100;std::vector<std::uint8_t> got;
 callbacks cb;cb.qpc=[&]{return ++ticks;};cb.qpc_frequency=10000000;cb.pcm=[&](const pcm_metadata& m,auto packets,auto bytes){++calls;frames+=m.frames;check(m.receive_qpc<m.ready_qpc&&m.qpc_frequency==10000000,"QPC order");check(m.channels==channels&&m.channel_mask==d.format.channel_mask&&m.native_container_bits==format_value.native_container_bits&&m.native_valid_bits==format_value.native_valid_bits,"exact native profile metadata");for(const auto& p:packets)got.insert(got.end(),bytes.begin()+p.offset,bytes.begin()+p.offset+p.length);return true;};
 session s(d,cb);imported(s,d);const auto now=std::chrono::steady_clock::now();
 std::vector<std::uint8_t> pcm(48*d.format.frame_bytes());for(std::size_t n=0;n<pcm.size();++n)pcm[n]=(format_value.native_container_bits==16||n%4)?std::uint8_t(n):0;
 std::vector<iso_packet> packets;for(unsigned n=0;n<8;++n)packets.push_back({n*6*d.format.frame_bytes(),6*d.format.frame_bytes()});
 const auto request=urb(10,1,false,std::uint32_t(pcm.size()),pcm,packets);
 for(std::size_t i=0;i<request.size();++i){check(s.feed(std::span(request).subspan(i,1),now),"fragmented stream");check(calls==(i+1==request.size()?1u:0u),"callback only at whole-URB admission");}check(calls==1&&frames==48&&got==pcm&&s.output().empty(),"exact native PCM before USB clock completion");s.poll(now+std::chrono::milliseconds(1));check(calls==1,"clock completion does not duplicate capture");auto r=s.take_output();check(get(r,24)==pcm.size()&&r.size()==48+16*8,"audio OUT ack excludes payload");
 check(s.feed(urb(11,1,false,std::uint32_t(pcm.size()),pcm,packets),now+std::chrono::milliseconds(1)),"pending second");std::vector<std::uint8_t> unlink;for(auto n:{2u,12u,0x10001u,0u,1u,11u})be(unlink,n);unlink.resize(48);check(s.feed(unlink,now),"unlink");r=s.take_output();check(get(r,0)==4&&get(r,20)==std::uint32_t(-104),"unlink ack");s.poll(now+std::chrono::seconds(1));check(calls==2&&s.stats().canceled==1&&s.stats().audio_frames_unlinked_after_capture==48,"UNLINK only cancels ACK; actual admitted audio remains with explicit counter");
 std::vector<iso_packet> fp{{0,4},{4,4}};check(s.feed(urb(20,2,true,8,{},fp),now),"feedback request");s.poll(now+std::chrono::seconds(1));r=s.take_output();check(r.size()==88&&get(r,24)==8&&r[50]==6&&r[54]==6,"16.16 microframe feedback");
 check(s.feed(urb(21,0,false,0,{}, {},{1,11,0,0,1,0,0,0}),now),"alt0");s.take_output();check(s.feed(request,now),"audio alt0 rejected");r=s.take_output();check(get(r,20)==std::uint32_t(-32),"alt0 stall");
}
void negative(){
 auto d=id();session denied(d);auto wrong=import(d);wrong[8]='F';check(denied.feed(wrong)&&!denied.imported()&&denied.finished(),"nonce mismatch rejects");check(get(denied.take_output(),4)==4,"nonce status");
 session listing(d);const std::uint8_t list[]{1,0x11,0x80,5,0,0,0,0};check(listing.feed(list),"devlist");auto r=listing.take_output();check(r.size()==12&&get(r,8)==0,"DEVLIST no identity leak");
 session invalid(d);imported(invalid,d);auto too_big=urb(1,1,true,std::uint32_t(max_transfer+1));check(!invalid.feed(too_big)&&invalid.failed(),"transfer bound");
 session overlap(d);imported(overlap,d);std::vector<std::uint8_t> data(96);std::vector<iso_packet> packets{{0,48},{0,48}};check(!overlap.feed(urb(1,1,false,96,data,packets)),"overlap rejects");
 session align(d);imported(align,d);packets={{0,95}};check(align.feed(urb(1,1,false,96,data,packets)),"bad alignment response");check(get(align.take_output(),20)==std::uint32_t(-32),"bad alignment stall");
 callbacks cb;cb.pcm=[](auto&,auto,auto){return false;};session consumer(d,cb);imported(consumer,d);packets={{0,96}};const auto now=std::chrono::steady_clock::now();consumer.feed(urb(1,1,false,96,data,packets),now);consumer.poll(now+std::chrono::seconds(1));check(consumer.failed()&&consumer.stats().callback_failures==1,"overflow fails closed");
 session duplicate(d);imported(duplicate,d);auto u=urb(1,1,false,96,data,packets);duplicate.feed(u,now);check(!duplicate.feed(u,now),"duplicate pending sequence");
 session backpressure(d);imported(backpressure,d);for(unsigned i=0;i<max_pending;++i)check(backpressure.feed(urb(i+1,1,false,96,data,packets),now),"bounded queue admission");check(!backpressure.feed(urb(1000,1,false,96,data,packets),now),"queue overflow fails closed");
}
void zero_allocation_audio(profile format_value){
 auto d=id(format_value);std::uint64_t calls=0,traces=0;callbacks cb;cb.record_clock=true;cb.pcm=[&](auto&,auto,auto){++calls;return true;};cb.trace=[&](const urb_trace&){++traces;};session s(d,cb);imported(s,d);
 std::vector<std::uint8_t> pcm(48*d.format.frame_bytes(),0);std::vector<iso_packet> packets;for(unsigned i=0;i<8;++i)packets.push_back({i*6*d.format.frame_bytes(),6*d.format.frame_bytes()});
 auto audio=urb(100,1,false,std::uint32_t(pcm.size()),pcm,packets);const std::array<iso_packet,1> fp{{{0,4}}};auto feedback=urb(101,2,true,4,{},fp);
 const auto base=std::chrono::steady_clock::now();allocations=0;observe_allocations=true;
 for(unsigned cycle=0;cycle<1000;++cycle){
  const auto seq=100u+cycle*2;for(unsigned b=0;b<4;++b){audio[4+b]=std::uint8_t(seq>>(24-b*8));feedback[4+b]=std::uint8_t((seq+1)>>(24-b*8));}
  const auto now=base+std::chrono::milliseconds(cycle);
  check(s.feed(audio,now),"pooled audio");check(s.feed(feedback,now),"pooled feedback");s.poll(now+std::chrono::milliseconds(1));check(!s.output().empty(),"pooled completion");
  // Same output span/consume API as the actual TCP worker; no test copy path.
  s.consume_output(s.output().size());
 }
 observe_allocations=false;check(allocations==0&&calls==1000&&s.stats().audio_frames==48000&&traces==64,"zero steady-audio/feedback/trace allocations and exact frames");
}
void profile_and_reset_contract(){
 bool rejected=false;try{profile::from_channels(6);}catch(const std::invalid_argument&){rejected=true;}check(rejected,"6ch cannot infer rear/side layout");
 for(auto p:std::array<profile,7>{{{6,0x63f,32,24},{8,0x60f,32,24},{12,0x2d63f,24,24},{12,0x2d63f,32,16},{2,3,16,24},{1,1,16,16},{12,0x2d63f,32,32}}})check(!p.valid(),"unsupported exact profile rejected");
 check(profile::from_channels(6,0x3f,16,16).frame_bytes()==12&&profile::from_channels(6,0x60f).frame_bytes()==24,"explicit six-channel frame bytes");
 auto d=id();std::vector<std::pair<std::uint32_t,bool>> resets;callbacks cb;cb.stream_reset=[&](auto epoch,auto terminal){resets.emplace_back(epoch,terminal);};session s(d,cb);imported(s,d);
 const auto send=[&](unsigned seq,std::array<std::uint8_t,8> setup){check(s.feed(urb(seq,0,false,0,{}, {},setup)),"reset control feed");s.consume_output(s.output().size());};
 send(1,{1,11,1,0,1,0,0,0});check(resets.size()==1&&resets.back()==std::make_pair(2u,false),"AS1 starts discontinuous epoch");
 send(2,{1,11,0,0,1,0,0,0});check(resets.size()==2&&resets.back()==std::make_pair(3u,true),"only direct AS0 is terminal");
 send(3,{0,9,0,0,0,0,0,0});check(resets.size()==3&&resets.back()==std::make_pair(4u,false),"configuration0 is discontinuity");
 send(4,{0x23,3,4,0,0,0,0,0});check(resets.size()==4&&resets.back()==std::make_pair(5u,false),"port reset is discontinuity");
 send(5,{1,11,2,0,1,0,0,0});check(resets.size()==4,"invalid alternate setting cannot signal terminal callback");
}
void clock_and_full_tx(){
 const auto d=id();session s(d);imported(s,d);std::vector<std::uint8_t> pcm(48*d.format.frame_bytes());std::vector<iso_packet> p;for(unsigned i=0;i<8;++i)p.push_back({i*6*d.format.frame_bytes(),6*d.format.frame_bytes()});
 const auto now=std::chrono::steady_clock::now();s.feed(urb(1,1,false,std::uint32_t(pcm.size()),pcm,p),now);check(s.next_deadline()==now+std::chrono::milliseconds(1),"precise scheduled deadline");s.poll(now+std::chrono::milliseconds(1));auto reply=s.take_output();const auto first=get(reply,28);
 s.feed(urb(2,1,false,std::uint32_t(pcm.size()),pcm,p),now+std::chrono::milliseconds(1));s.poll(now+std::chrono::milliseconds(2));reply=s.take_output();check(get(reply,28)==first+1,"USB first-frame advances by1ms, not8microframes");
 const std::array<iso_packet,2> fp{{{0,4},{4,4}}};s.feed(urb(3,2,true,8,{},fp),now);s.poll(now+std::chrono::milliseconds(2));reply=s.take_output();check(get(reply,28)==first,"OUT/feedback share first-frame clock despite different duration");check(s.next_deadline()==std::chrono::steady_clock::time_point::max(),"idle has no periodic deadline");
 session full(d);imported(full,d);const std::array<iso_packet,1> one{{{0,48}}};const std::array<std::uint8_t,48> frame{};
 for(unsigned i=0;i<3;++i)check(full.feed(urb(i+1,1,false,48,frame,one),now),"fullTX pending setup");
 const auto status=urb(100,0,true,0,{}, {},{0x80,0,0,0,0,0,0,0});
 constexpr unsigned replies=(2*1024*1024-80)/48;
 for(unsigned i=0;i<replies;++i)check(full.feed(status,now),"fill fixed TX without polling");
 check(full.output().size()==2*1024*1024-80,"fullTX exact setup");full.poll(now+std::chrono::seconds(1));
 check(full.failed()&&full.stats().protocol_failures==1,"TX failure clears pending safely during poll");
}
}
namespace {
unsigned mm_set_calls=0,mm_revert_calls=0;bool mm_set_fail=false,mm_revert_fail=false;
HANDLE WINAPI fake_mm_set(LPCWSTR name,LPDWORD index){
 ++mm_set_calls;
#ifdef VIBE_USB_EXPERIMENT_PRO_AUDIO_WORKER
 if(std::wstring_view(name)!=L"Pro Audio")return nullptr;
#else
 if(std::wstring_view(name)!=L"Audio")return nullptr;
#endif
 *index=42;if(mm_set_fail){SetLastError(ERROR_ACCESS_DENIED);return nullptr;}return reinterpret_cast<HANDLE>(std::uintptr_t(123));
}
BOOL WINAPI fake_mm_revert(HANDLE h){++mm_revert_calls;if(h!=reinterpret_cast<HANDLE>(std::uintptr_t(123)))return FALSE;if(mm_revert_fail){SetLastError(ERROR_INVALID_HANDLE);return FALSE;}return TRUE;}
void mmcss_lifecycle(){
 const mmcss_api api{fake_mm_set,fake_mm_revert};
 {mmcss_status s;{mmcss_audio a(s,api);check(a.start()&&s.registered&&s.task_index==42&&s.thread_id==GetCurrentThreadId(),"MMCSS Audio startup published");check(!a.start()&&mm_set_calls==1,"MMCSS cannot re-register");const auto owner=s.thread_id;s.thread_id=owner+1;check(!a.close()&&mm_revert_calls==0&&s.revert_error==ERROR_INVALID_THREAD_ID,"MMCSS wrong-thread close refused");s.thread_id=owner;check(a.close()&&s.reverted&&mm_revert_calls==1,"MMCSS same-thread close");check(a.close()&&mm_revert_calls==1,"MMCSS close idempotent");}check(mm_revert_calls==1,"MMCSS destructor does not duplicate close");}
 mm_set_fail=true;{mmcss_status s;mmcss_audio a(s,api);check(!a.start()&&!s.registered&&s.register_error==ERROR_ACCESS_DENIED,"MMCSS registration failure visible");check(a.close()&&mm_revert_calls==1,"MMCSS failed registration no revert");}mm_set_fail=false;
 mm_revert_fail=true;{mmcss_status s;{mmcss_audio a(s,api);check(a.start(),"MMCSS fail-close setup");check(!a.close()&&s.revert_error==ERROR_INVALID_HANDLE&&!s.reverted,"MMCSS failed revert visible");check(!a.close()&&mm_revert_calls==2,"MMCSS failed revert not reused");}check(mm_revert_calls==2,"MMCSS failed revert destructor no retry");}mm_revert_fail=false;
}
}int main(){try{mmcss_lifecycle();boot_frame_origin();clock_flight_and_spacing(false);clock_flight_and_spacing(true);for(const auto bits:{16u,32u})for(const auto shape:std::array<std::pair<unsigned,std::uint32_t>,5>{{{2,3},{6,0x3f},{6,0x60f},{8,0x63f},{12,0x2d63f}}}){const auto p=profile::from_channels(shape.first,shape.second,std::uint16_t(bits),std::uint16_t(bits==32?24:16));descriptor_and_clock(p,false);descriptor_and_clock(p,true);audio_and_unlink(p);zero_allocation_audio(p);}negative();profile_and_reset_contract();clock_and_full_tx();std::cout<<"PASS "<<checks<<" offline protocol checks, zero steady-audio heap allocations; no driver/device/audio runtime\n";return 0;}catch(const std::exception& e){observe_allocations=false;std::cerr<<"FAIL "<<checks<<": "<<e.what()<<'\n';return 1;}}

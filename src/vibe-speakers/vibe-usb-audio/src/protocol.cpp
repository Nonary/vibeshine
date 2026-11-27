// SPDX-License-Identifier: GPL-3.0-or-later
// USB/IP framing adapted from Vibeshine dualsense_usbip_protocol.cpp.
// All audio descriptors/control topology below are new UAC2 lab code.
#include "vibe_usb_audio.h"
#include <algorithm>
#include <cstring>
#include <stdexcept>
#include <utility>

namespace vibe::usb_audio {
namespace {
constexpr std::size_t header_size=48, max_buffer=2*1024*1024;
constexpr std::uint32_t non_iso=0xffffffffu;
constexpr std::int32_t stall=-32, canceled=-104;
std::uint32_t be32(const std::uint8_t* b) { return (std::uint32_t(b[0])<<24)|(std::uint32_t(b[1])<<16)|(std::uint32_t(b[2])<<8)|b[3]; }
std::uint16_t be16(const std::uint8_t* b) { return std::uint16_t((unsigned(b[0])<<8)|b[1]); }
std::uint16_t le16(const std::uint8_t* b) { return std::uint16_t(b[0]|(unsigned(b[1])<<8)); }
void put32(std::vector<std::uint8_t>& v,std::uint32_t x) { for(int s=24;s>=0;s-=8)v.push_back(std::uint8_t(x>>s)); }
void put16(std::vector<std::uint8_t>& v,std::uint16_t x) { v.push_back(std::uint8_t(x>>8));v.push_back(std::uint8_t(x)); }
void little32(std::vector<std::uint8_t>& v,std::uint32_t x) { for(unsigned s=0;s<32;s+=8)v.push_back(std::uint8_t(x>>s)); }
void fixed(std::vector<std::uint8_t>& v,const std::string& s,std::size_t n) { const auto p=v.size();v.resize(p+n);std::memcpy(v.data()+p,s.data(),std::min(n-1,s.size())); }
std::vector<std::uint8_t> usb_string(const std::string& s) { std::vector<std::uint8_t> v{std::uint8_t(s.size()*2+2),3};for(auto c:s){v.push_back(std::uint8_t(c));v.push_back(0);}return v; }
}
profile profile::from_channels(unsigned n) {
  switch(n){case 2:return from_channels(n,3);case 8:return from_channels(n,0x63f);case 12:return from_channels(n,0x2d63f);default:throw std::invalid_argument("Channels6 require explicit rear/side mask; supported channels2/6/8/12");}
}
profile profile::from_channels(unsigned n,std::uint32_t mask,std::uint16_t container,std::uint16_t valid_bits) {
 if(n>255)throw std::invalid_argument("Channel count overflow");profile result{std::uint8_t(n),mask,container,valid_bits};if(!result.valid())throw std::invalid_argument("Unsupported exact channel/mask/PCM profile");return result;
}
bool profile::valid() const noexcept {
 return product_id()!=0;
}
session::session(identity id,callbacks cb):identity_(std::move(id)),callbacks_(std::move(cb)) {
  const auto epoch_qpc=callbacks_.record_clock?qpc():0;
  if(!identity_.format.valid()||identity_.busid.size()!=30||identity_.serial.size()!=15||
     !std::all_of(identity_.busid.begin(),identity_.busid.end(),[](char c){return (c>='0'&&c<='9')||(c>='A'&&c<='F');}))
    throw std::invalid_argument("Invalid lab profile or generated identity");
  pool_=std::make_unique<urb[]>(max_pending+1);
  pending_.reserve(max_pending);receive_.reserve(max_buffer);transmit_.reserve(max_buffer);
  if(callbacks_.space_expired_out&&callbacks_.synchronous_out)throw std::invalid_argument("Independent USB clock experiments cannot be combined");
  if(callbacks_.record_clock){clock_=std::make_unique<clock_flight>();clock_->epoch_qpc=epoch_qpc;clock_->qpc_frequency=callbacks_.qpc_frequency;clock_->spaced_expired_out=callbacks_.space_expired_out;clock_->synchronous_out=callbacks_.synchronous_out;}
  if(callbacks_.boot_interrupt_time_100ns){
   if(!callbacks_.synchronous_out)throw std::invalid_argument("Boot-relative StartFrame experiment requires the synchronous descriptor");
   std::int64_t best=INT64_MAX;
   for(unsigned i=0;i<9;++i){
    const auto a=std::chrono::steady_clock::now();
    const auto boot=callbacks_.boot_interrupt_time_100ns();
    const auto b=std::chrono::steady_clock::now();
    const auto span=std::chrono::duration_cast<std::chrono::nanoseconds>(b-a).count();
    if(span>=best)continue;
    const auto elapsed=bus_ns(a+(b-a)/2);
    if(elapsed<0||boot<std::uint64_t(elapsed)/100u)throw std::invalid_argument("Unusable Windows interrupt-time calibration");
    best=span;boot_epoch_100ns_=boot-std::uint64_t(elapsed)/100u;
   }
   if(clock_){clock_->boot_frame_origin=true;clock_->boot_epoch_100ns=boot_epoch_100ns_;clock_->boot_calibration_bracket_ns=static_cast<std::uint64_t>(best);}
  }
}
void session::diagnostic(const std::string& s){if(callbacks_.diagnostic)callbacks_.diagnostic(s);}
std::uint64_t session::qpc() const{return callbacks_.qpc?callbacks_.qpc():0;}
void session::trace(const urb& u,unsigned phase) const {
 if(!u.traced||!callbacks_.trace)return;
 urb_trace t{};t.phase=phase;t.sequence=u.sequence;t.endpoint=u.endpoint;t.direction=u.direction;t.flags=u.flags;
 t.requested_start_frame=u.requested_start_frame;t.returned_start_frame=u.start_frame;t.wire_interval=u.wire_interval;
 t.epoch=epoch_;t.packet_count=u.packet_count;t.transfer_bytes=u.length;t.packet_min=0xffffffffu;
 for(const auto& p:packets(u)){t.packet_min=std::min(t.packet_min,p.length);t.packet_max=std::max(t.packet_max,p.length);t.packet_total+=p.length;}
 t.qpc=qpc();t.receive_qpc=u.receive_qpc;t.qpc_frequency=callbacks_.qpc_frequency;t.scheduled_duration_us=(u.direction?1000u:125u)*u.packet_count;callbacks_.trace(t);
}
void session::close(){stats_.pending_on_close+=pending_.size();for(auto* u:pending_){if(u->endpoint==1&&!u->direction&&!u->captured)for(const auto& p:packets(*u))stats_.audio_frames_discarded_on_close+=p.length/identity_.format.frame_bytes();u->used=false;}pending_.clear();}
void session::fail(const char* reason){if(!failed_)++stats_.protocol_failures;diagnostic(std::string("protocol_failure:")+reason);failed_=finished_=true;receive_.clear();close();}
std::vector<std::uint8_t> session::take_output(){std::vector<std::uint8_t> result(transmit_.begin(),transmit_.end());consume_output(result.size());return result;} // simulated socket acceptance in tests only
std::int64_t session::bus_ns(std::chrono::steady_clock::time_point t)const noexcept{return std::chrono::duration_cast<std::chrono::nanoseconds>(t-bus_epoch_).count();}
void session::consume_output(std::size_t bytes,std::chrono::steady_clock::time_point now){
 if(bytes>transmit_.size())throw std::out_of_range("consume output");
 if(!bytes)return;
 const auto old=socket_bytes_;socket_bytes_+=bytes;
 if(old<last_audio_return_end_&&socket_bytes_>=last_audio_return_end_)last_audio_send_=now;
 if(clock_)clock_->socket_accepted(socket_bytes_,qpc(),bus_ns(now));
 transmit_.erase(transmit_.begin(),transmit_.begin()+bytes);
}
std::chrono::steady_clock::time_point session::effective_deadline(const urb& u)const noexcept{
 if(callbacks_.space_expired_out&&u.endpoint==1&&!u.direction){
  if(last_audio_return_end_>socket_bytes_)return std::chrono::steady_clock::time_point::max();
  if(last_audio_send_!=std::chrono::steady_clock::time_point{}&&u.due<=last_audio_send_)return std::max(u.due,last_audio_send_+std::chrono::milliseconds(1));
 }
 return u.due;
}
std::chrono::steady_clock::time_point session::next_deadline() const noexcept {auto next=std::chrono::steady_clock::time_point::max();for(const auto* u:pending_)next=std::min(next,effective_deadline(*u));return next;}
void session::header(std::uint16_t op,std::uint32_t status){put16(transmit_,0x0111);put16(transmit_,op);put32(transmit_,status);}
std::vector<std::uint8_t> session::device_descriptor() const {
 const auto product=identity_.format.product_id();
 return {18,1,0,2,0xef,2,1,64,std::uint8_t(vendor_id&255),std::uint8_t(vendor_id>>8),std::uint8_t(product&255),std::uint8_t(product>>8),0,1,1,2,3,1};
}
std::vector<std::uint8_t> session::configuration_descriptor() const {
 const auto n=identity_.format.channels;const auto m=identity_.format.channel_mask;
 const bool sync=callbacks_.synchronous_out;
 const auto packet=std::uint16_t((sync?6u:7u)*identity_.format.frame_bytes());
 const auto total=std::uint8_t(sync?127:134);
 std::vector<std::uint8_t> v{
   9,2,total,0,2,1,0,0xc0,1,            // self-powered, two interfaces
   8,11,0,2,1,0,0x20,2,                // IAD Audio function, UAC2
   9,4,0,0,0,1,1,0x20,0,               // AC, no interrupt endpoint
   9,0x24,1,0,2,1,46,0,0,              // AC header: 46 CS bytes, desktop speaker
   8,0x24,0x0a,4,std::uint8_t(sync?5:1),5,0,0, // fixed internal clock, D2 locks sync profile to SOF
   17,0x24,2,1,1,1,0,4,n,              // USB streaming input, clock4
   std::uint8_t(m),std::uint8_t(m>>8),std::uint8_t(m>>16),std::uint8_t(m>>24),0,0,0,0,
   12,0x24,3,3,1,3,0,1,4,0,0,0,       // speaker output <- input1; no fake hardware gain
   9,4,1,0,0,1,2,0x20,0,               // AS alt0 zero bandwidth
   9,4,1,1,std::uint8_t(sync?1:2),1,2,0x20,0,
   16,0x24,1,1,0,1,1,0,0,0,n,
   std::uint8_t(m),std::uint8_t(m>>8),std::uint8_t(m>>16),std::uint8_t(m>>24),0,
   6,0x24,2,1,std::uint8_t(identity_.format.native_container_bits/8),std::uint8_t(identity_.format.native_valid_bits), // exact native PCM, no conversion
   7,5,1,std::uint8_t(sync?0x0d:5),std::uint8_t(packet),std::uint8_t(packet>>8),1,
   8,0x25,1,0,0,0,0,0                  // class endpoint; no controls
 };
 if(!sync)v.insert(v.end(),{7,5,0x82,0x11,4,0,4}); // explicit IN feedback only for async OUT
 if(v.size()!=total)throw std::logic_error("UAC2 descriptor length");
 return v;
}
void session::device_record(){fixed(transmit_,"/vibe-speakers/private-lab",256);fixed(transmit_,identity_.busid,32);put32(transmit_,1);put32(transmit_,1);put32(transmit_,3);put16(transmit_,vendor_id);put16(transmit_,identity_.format.product_id());put16(transmit_,0x0100);transmit_.insert(transmit_.end(),{0xef,2,1,1,1,2});}
bool session::feed(std::span<const std::uint8_t> data,std::chrono::steady_clock::time_point now){
 if(failed_||finished_||data.size()>max_buffer-receive_.size()){fail("receive bound");return false;}
 receive_time_=now;receive_.insert(receive_.end(),data.begin(),data.end());
 while(!failed_&&!finished_&&parse_one()){if(transmit_.size()>max_buffer)fail("transmit bound");}
 return !failed_;
}
bool session::parse_one(){
 if(!imported_){
  if(receive_.size()<8)return false;
  if(be16(receive_.data())!=0x0111||be32(receive_.data()+4)){fail("handshake");return false;}
  const auto op=be16(receive_.data()+2);
  if(op==0x8005){header(5);put32(transmit_,0);finished_=true;return false;} // do not disclose nonce
  if(op!=0x8003){fail("operation");return false;}
  if(receive_.size()<40)return false;
  const auto end=std::find(receive_.begin()+8,receive_.begin()+40,0);
  if(end==receive_.begin()+40||std::string(receive_.begin()+8,end)!=identity_.busid){header(3,4);finished_=true;return false;}
  header(3);device_record();imported_=true;configuration_=1;
  receive_.erase(receive_.begin(),receive_.begin()+40);diagnostic("imported");return true;
 }
 if(receive_.size()<header_size)return false;
 const auto* w=receive_.data();const auto cmd=be32(w),seq=be32(w+4);
 if(be32(w+8)!=0x10001u){fail("devid");return false;}
 if(cmd==2){
  const auto victim=be32(w+20);auto it=std::find_if(pending_.begin(),pending_.end(),[&](const urb* u){return u->sequence==victim;});
  const bool removed=it!=pending_.end();if(removed){auto* u=*it;trace(*u,3);if(u->captured)for(const auto& p:packets(*u))stats_.audio_frames_unlinked_after_capture+=p.length/identity_.format.frame_bytes();u->used=false;pending_.erase(it);++stats_.canceled;}
  put32(transmit_,4);put32(transmit_,seq);transmit_.resize(transmit_.size()+12);put32(transmit_,removed?std::uint32_t(canceled):0);transmit_.resize(transmit_.size()+24);
  receive_.erase(receive_.begin(),receive_.begin()+header_size);return true;
 }
 if(cmd!=1){fail("command");return false;}
 const auto direction=be32(w+12),endpoint=be32(w+16),length=be32(w+24),packet_count=be32(w+32);
 if(direction>1||endpoint>15||length>max_transfer||(packet_count!=non_iso&&packet_count>max_iso_packets)){fail("URB bound");return false;}
 const auto count=packet_count==non_iso?0:packet_count;const auto data_size=direction?0:length;
 const auto full=header_size+std::size_t(data_size)+std::size_t(count)*16;
 if(receive_.size()<full)return false;
 urb* slot=nullptr;for(std::size_t i=0;i<=max_pending;++i)if(!pool_[i].used){slot=&pool_[i];break;}
 if(!slot){fail("URB pool exhausted");return false;}auto& u=*slot;u.used=true;u.captured=false;u.traced=false;u.clock_id=0;
 u.sequence=seq;u.direction=direction;u.endpoint=endpoint;u.length=length;u.start_frame=be32(w+28);u.packet_count=packet_count;
 u.requested_start_frame=u.start_frame;u.flags=be32(w+20);u.wire_interval=be32(w+36);
 u.receive_qpc=qpc(); // whole original URB available, before copies/callback/diagnostic
 std::copy_n(w+40,8,u.setup.begin());std::copy_n(w+header_size,data_size,u.data.begin());
 const auto* p=w+header_size+data_size;std::uint64_t end=0;
 for(unsigned i=0;i<count;++i,p+=16){iso_packet packet{be32(p),be32(p+4)};if(packet.offset<end||packet.offset>u.length||packet.length>u.length-packet.offset){fail("ISO packet bounds/overlap");return false;}end=std::uint64_t(packet.offset)+packet.length;u.packets[i]=packet;}
 if(std::any_of(pending_.begin(),pending_.end(),[&](const urb* x){return x->sequence==seq;})){fail("duplicate outstanding sequence");return false;}
 receive_.erase(receive_.begin(),receive_.begin()+full);++stats_.submits;submit(slot);return true;
}
void session::reply(const urb& u,std::int32_t status,std::span<const std::uint8_t> data,std::uint32_t out_length){
 const auto iso=packets(u);
 const auto required=header_size+((u.direction&&!status)?data.size():0)+iso.size()*16;
 if(required>max_buffer-transmit_.size()){fail("transmit bound");return;}
 const auto begin=socket_bytes_+transmit_.size();
 put32(transmit_,3);put32(transmit_,u.sequence);transmit_.resize(transmit_.size()+12);
 put32(transmit_,std::uint32_t(status));put32(transmit_,status?0:u.direction?std::uint32_t(data.size()):out_length);
 put32(transmit_,iso.empty()?0:u.start_frame);put32(transmit_,iso.empty()?non_iso:u.packet_count);
 put32(transmit_,status?std::uint32_t(iso.size()):0);transmit_.resize(transmit_.size()+8);
 if(u.direction&&!status)transmit_.insert(transmit_.end(),data.begin(),data.end());
 for(const auto& p:iso){put32(transmit_,p.offset);put32(transmit_,p.length);put32(transmit_,status?0:u.direction?4:p.length);put32(transmit_,std::uint32_t(status));}
 const auto end=socket_bytes_+transmit_.size();
 if(!status&&u.endpoint==1&&!u.direction)last_audio_return_end_=end;
 if(clock_&&u.clock_id)clock_->returned(u.clock_id,status,qpc(),begin,end);
 ++stats_.completed;if(status==stall)++stats_.stalls;
}
void session::reset_stream(bool terminal_as0,std::uint32_t sequence,std::uint32_t reason){
 if(clock_)clock_->reset({epoch_,epoch_+1,sequence,reason,qpc()});
 while(!pending_.empty()){auto* u=pending_.back();pending_.pop_back();trace(*u,4);reply(*u,canceled);++stats_.canceled;u->used=false;if(failed_)break;}next_audio_=next_feedback_={};alternate_=0;++epoch_;frame_position_=0;audio_trace_count_=feedback_trace_count_=0;
 last_audio_return_end_=0;last_audio_send_={};
 if(callbacks_.stream_reset)callbacks_.stream_reset(epoch_,terminal_as0);
 diagnostic("stream_reset epoch="+std::to_string(epoch_));
}
void session::submit(urb* slot){
 auto& u=*slot;
 const bool iso=u.packet_count!=0&&u.packet_count!=non_iso;
 if(u.endpoint==0&&!iso){control(u);u.used=false;return;}
 const bool audio=u.endpoint==1&&u.direction==0,feedback=!callbacks_.synchronous_out&&u.endpoint==2&&u.direction==1;
 if(!configuration_||!iso||(!audio&&!feedback)||(alternate_explicit_&&!alternate_)){reply(u,stall);u.used=false;return;}
 const auto maximum=(callbacks_.synchronous_out?6u:7u)*identity_.format.frame_bytes();
 for(const auto& p:packets(u)){if(audio?(p.length>maximum||p.length%identity_.format.frame_bytes()):(p.length!=4)){reply(u,stall);u.used=false;return;}}
 if(pending_.size()>=max_pending){u.used=false;fail("pending URB bound");return;}
 alternate_=1;auto& cursor=audio?next_audio_:next_feedback_;
 const auto prior=cursor;
 const std::uint32_t anchor=cursor==std::chrono::steady_clock::time_point{}?1u:cursor<receive_time_-std::chrono::milliseconds(50)?2u:0u;
 if(anchor){
  cursor=receive_time_;
  if(callbacks_.synchronous_out){
   // Start on the next whole 125-us high-speed bus tick after receipt. The
   // virtual SOF timeline, not a feedback stream, clocks six frames per tick.
   constexpr std::int64_t tick=125000;
   const auto elapsed=std::max<std::int64_t>(0,bus_ns(cursor));
   cursor=bus_epoch_+std::chrono::nanoseconds(((elapsed+tick-1)/tick)*tick);
  }
 }
 const auto scheduled_start=cursor;
 // USB StartFrame is a 1-ms bus-frame number EVEN for high-speed transfers.
 // Return the first frame, before adding this URB's duration; NEVER its end
 // or a 125-us microframe count. Release UDE forces ASAP on the USB/IP wire.
 const auto elapsed=std::max<std::int64_t>(0,bus_ns(cursor));
 u.start_frame=usb_start_frame(boot_epoch_100ns_,static_cast<std::uint64_t>(elapsed));
 cursor+=std::chrono::microseconds((audio?125:1000)*u.packet_count);
 if(cursor>receive_time_+std::chrono::milliseconds(250)){u.used=false;fail("clock backlog bound");return;}
 u.due=cursor;
 if(clock_){
  clock_observation row{};row.first_frame=frame_position_;row.receive_qpc=u.receive_qpc;row.receive_ns=bus_ns(receive_time_);
  row.prior_cursor_ns=prior==std::chrono::steady_clock::time_point{}?-1:bus_ns(prior);row.start_ns=bus_ns(scheduled_start);row.due_ns=bus_ns(u.due);
  row.epoch=epoch_;row.sequence=u.sequence;row.endpoint=u.endpoint;row.flags=u.flags;row.interval=u.wire_interval;row.requested_start=u.requested_start_frame;row.returned_start=u.start_frame;
  row.packets=u.packet_count;row.minimum_bytes=UINT32_MAX;row.anchor=anchor;row.pending_at_admission=static_cast<std::uint32_t>(pending_.size());row.feedback_16_16=feedback?6u<<16:0;
  for(const auto& p:packets(u)){row.minimum_bytes=std::min(row.minimum_bytes,p.length);row.maximum_bytes=std::max(row.maximum_bytes,p.length);row.actual_bytes+=p.length;}
  u.clock_id=clock_->admitted(row);
 }
 auto& trace_count=audio?audio_trace_count_:feedback_trace_count_;u.traced=trace_count<16;if(u.traced)++trace_count;trace(u,1);
 if(audio){
  std::uint32_t actual=0;for(const auto& p:packets(u)){actual+=p.length;if(identity_.format.native_container_bits==32)for(std::size_t b=p.offset;b<p.offset+p.length;b+=4)if(u.data[b])++stats_.low_byte_nonzero;}
  const std::uint64_t frames=actual/identity_.format.frame_bytes();
  pcm_metadata m{u.sequence,u.requested_start_frame,epoch_,u.packet_count,frame_position_,frames,u.receive_qpc,qpc(),callbacks_.qpc_frequency,actual,identity_.format.channels,identity_.format.channel_mask,identity_.format.native_container_bits,identity_.format.native_valid_bits};
  if(callbacks_.pcm&&!callbacks_.pcm(m,packets(u),payload(u))){++stats_.callback_failures;u.used=false;fail("PCM consumer overflow/failure");return;}
  u.captured=true;frame_position_+=frames;stats_.audio_frames+=frames;++stats_.audio_urbs;
 }
 pending_.push_back(slot);stats_.pending_high_water=std::max(stats_.pending_high_water,pending_.size());
}
void session::poll(std::chrono::steady_clock::time_point now){
 if(failed_||finished_)return;
 for(auto it=pending_.begin();it!=pending_.end();){
  if(now<effective_deadline(**it)){++it;continue;}auto& u=**it;it=pending_.erase(it);
  if(clock_)clock_->polled(u.clock_id,bus_ns(now));
  if(now-u.due>std::chrono::milliseconds(2))++stats_.late_urbs;
  trace(u,2);
  if(u.direction){
   std::array<std::uint8_t,max_iso_packets*4> feedback{};
   for(std::size_t i=0;i<packets(u).size();++i)feedback[i*4+2]=6; // 48000/8000 exactly, LE16.16
   reply(u,0,std::span(feedback).first(packets(u).size()*4));++stats_.feedback_urbs;
  }else{
   std::uint32_t actual=0;for(const auto& p:packets(u))actual+=p.length;reply(u,0,{},actual);
  }
  u.used=false;
  if(failed_)return; // reply() may fail-close and invalidate all pending iterators
  if(transmit_.size()>max_buffer){fail("transmit bound");return;}
 }
}
void session::control(const urb& u){
 const auto type=u.setup[0],code=u.setup[1];const auto value=le16(u.setup.data()+2),index=le16(u.setup.data()+4),length=le16(u.setup.data()+6);
 const bool input=(type&0x80)!=0;
 if(input!=bool(u.direction)||u.length<length||(!input&&u.length!=length)){reply(u,stall);return;}
 const auto send=[&](std::span<const std::uint8_t> v){reply(u,0,v.first(std::min<std::size_t>(v.size(),length)));};
 const auto accept=[&]{reply(u,0,{},u.length);};
 if(type==0x23&&code==3&&value==4&&!input){reset_stream(false,u.sequence,5);configuration_=1;alternate_explicit_=false;accept();return;} // VHCI virtual port reset
 if((type&0x60)==0){
  if(type==0x80&&code==6){
   switch(value>>8){
    case 1:send(device_descriptor());return;
    case 2:send(configuration_descriptor());return;
    case 3:{const auto s=value&255;if(s==0){const std::uint8_t lang[]{4,3,9,4};send(lang);}else if(s<=3)send(usb_string(s==1?"Vibe Speakers":s==2?"Vibe Speakers":identity_.serial));else reply(u,stall);return;}
    default:reply(u,stall);return;
   }
  }
  if(code==0&&input&&(type==0x80||type==0x81||type==0x82)){if(type==0x82&&callbacks_.synchronous_out&&index==0x82){reply(u,stall);return;}const std::uint8_t status[]{std::uint8_t(type==0x80?1:0),0};send(status);return;}
  if(type==0&&code==5&&value<=127&&!length){accept();return;}
  if(type==0&&code==9&&value<=1&&!length){reset_stream(false,u.sequence,value?4u:3u);configuration_=std::uint8_t(value);alternate_explicit_=false;accept();return;}
  if(type==0x80&&code==8){send(std::span(&configuration_,1));return;}
  if(type==1&&code==11&&index<=1&&value<=(index==1?1:0)&&!length){if(index==1){reset_stream(value==0,u.sequence,value?2u:1u);alternate_=std::uint8_t(value);alternate_explicit_=true;}accept();return;}
  if(type==0x81&&code==10&&index<=1){const std::uint8_t alt=index==1?alternate_:0;send(std::span(&alt,1));return;}
  if(type==2&&code==1&&value==0&&(index==1||(!callbacks_.synchronous_out&&index==0x82))&&!length){accept();return;}
  reply(u,stall);return;
 }
 // UAC2 fixed read-only Clock Source4 on AC interface0. RANGE: one
 // discrete 48000-Hz subrange (resolution0), not a continuous fake range.
 if(type==0xa1&&index==0x0400&&(value&255)==0){
  if(value==0x0100&&code==1){std::vector<std::uint8_t> v;little32(v,sample_rate);send(v);return;}
  if(value==0x0100&&code==2){std::vector<std::uint8_t> v{1,0};little32(v,sample_rate);little32(v,sample_rate);little32(v,0);send(v);return;}
  if(value==0x0200&&code==1){const std::uint8_t valid=1;send(std::span(&valid,1));return;}
 }
 diagnostic("control_stall type="+std::to_string(type)+" request="+std::to_string(code)+" value="+std::to_string(value)+" index="+std::to_string(index));reply(u,stall);
}
} // namespace vibe::usb_audio

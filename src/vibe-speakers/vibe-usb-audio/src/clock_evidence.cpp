// SPDX-License-Identifier: GPL-3.0-or-later
#include "clock_flight.h"
#include <windows.h>
#include <locale>
#include <sstream>
#include <stdexcept>

namespace vibe::usb_audio {
namespace {
struct file {
  HANDLE value;
  explicit file(const std::wstring& name):value(CreateFileW(name.c_str(),GENERIC_WRITE,FILE_SHARE_READ,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr)) {
    if(value==INVALID_HANDLE_VALUE)throw std::runtime_error("Cannot create new clock evidence");
  }
  ~file(){if(value!=INVALID_HANDLE_VALUE)CloseHandle(value);}
  void write(const std::string& text){DWORD written=0;if(text.size()>UINT32_MAX||!WriteFile(value,text.data(),static_cast<DWORD>(text.size()),&written,nullptr)||written!=text.size())throw std::runtime_error("Clock evidence write failed");}
};
void write_row(file& f,const char* group,const clock_observation& r){
  std::ostringstream s;s.imbue(std::locale::classic());
  s<<group<<','<<r.id<<','<<r.epoch<<','<<r.sequence<<','<<r.endpoint<<','<<r.first_frame<<','<<r.flags<<','<<r.interval<<','<<r.requested_start<<','<<r.returned_start<<','<<r.packets<<','<<r.minimum_bytes<<','<<r.maximum_bytes<<','<<r.actual_bytes<<','<<r.feedback_16_16<<','<<r.anchor<<','<<r.pending_at_admission<<','<<r.receive_qpc<<','<<r.receive_ns<<','<<r.admission_gap_ns<<','<<r.prior_cursor_ns<<','<<r.start_ns<<','<<r.due_ns<<','<<r.poll_ns<<','<<r.status<<','<<r.return_queued_qpc<<','<<r.tx_begin<<','<<r.tx_end<<','<<r.first_send_qpc<<','<<r.complete_send_qpc<<','<<r.complete_send_ns<<','<<r.send_batch<<'\n';f.write(s.str());
}
}
void write_clock_evidence(const std::wstring& prefix,const clock_flight& c){
  file csv(prefix+L".clock.csv");
  csv.write("set,id,epoch,urb_sequence,endpoint,first_native_frame,flags,wire_interval,requested_start_frame,returned_start_frame,packet_count,packet_min_bytes,packet_max_bytes,packet_total_bytes,feedback_16_16,anchor_reason,pending_before,receive_qpc,receive_bus_ns,admission_gap_ns,prior_cursor_ns,first_packet_bus_ns,due_bus_ns,poll_bus_ns,ret_status,ret_queued_qpc,tx_begin,tx_end,first_socket_accept_qpc,complete_socket_accept_qpc,complete_socket_accept_bus_ns,send_batch\n");
  for(std::uint64_t i=0;i<std::min<std::uint64_t>(c.observations,c.prefix.size());++i)write_row(csv,"prefix",c.prefix[i]);
  const auto first=c.observations>c.tail.size()?c.observations-c.tail.size():0;
  for(auto i=first;i<c.observations;++i)write_row(csv,"tail",c.tail[i%c.tail.size()]);
  const auto first_notable=c.notable_count>c.notable.size()?c.notable_count-c.notable.size():0;
  for(auto i=first_notable;i<c.notable_count;++i)write_row(csv,"notable",c.notable[i%c.notable.size()]);
  file summary(prefix+L".clock.json");std::ostringstream s;s.imbue(std::locale::classic());
  s<<"{\"Schema\":\"vibe-usb-clock-flight/1\",\"policy\":\""<<(c.boot_frame_origin?"synchronous_out_windows_boot_ms":c.synchronous_out?"synchronous_out_sof":c.spaced_expired_out?"space_expired_out_1ms":"baseline")<<"\",\"bus_epoch_approx_qpc\":"<<c.epoch_qpc<<",\"boot_frame_origin\":"<<(c.boot_frame_origin?"true":"false")<<",\"boot_epoch_100ns\":"<<c.boot_epoch_100ns<<",\"boot_calibration_bracket_ns\":"<<c.boot_calibration_bracket_ns<<",\"qpc_frequency\":"<<c.qpc_frequency<<",\"observations\":"<<c.observations<<",\"tail_capacity\":"<<c.tail.size()<<",\"notable_count\":"<<c.notable_count<<",\"marker_overflow\":"<<c.marker_overflow<<",\"expired_updates\":"<<c.expired_updates<<",\"pending_send_markers\":"<<c.pending_send_markers()<<",\"socket_send_calls\":"<<c.send_batches<<",\"endpoints\":[";
  for(unsigned i=0;i<2;++i){if(i)s<<',';s<<"{\"endpoint\":"<<(i?2:1)<<",\"admissions\":"<<c.admissions[i]<<",\"socket_completed\":"<<c.socket_completed[i]<<",\"late_polls_over_2ms\":"<<c.late_polls[i]<<",\"reanchors_over_50ms\":"<<c.late_reanchors[i]<<",\"max_admission_gap_ns\":"<<c.max_admission_gap_ns[i]<<",\"max_poll_late_ns\":"<<c.max_poll_late_ns[i]<<",\"max_send_late_ns\":"<<c.max_send_late_ns[i]<<'}';}
  s<<"],\"reset_count\":"<<c.reset_count<<",\"resets\":[";
  const auto first_reset=c.reset_count>c.resets.size()?c.reset_count-c.resets.size():0;
  for(auto i=first_reset;i<c.reset_count;++i){if(i!=first_reset)s<<',';const auto& e=c.resets[i%c.resets.size()];s<<"{\"old_epoch\":"<<e.old_epoch<<",\"new_epoch\":"<<e.new_epoch<<",\"sequence\":"<<e.sequence<<",\"reason\":"<<e.reason<<",\"qpc\":"<<e.qpc<<'}';}
  s<<"],\"socket_meaning\":\"Last RET byte accepted by local send; kernel UDE delivery and physical completion are not observed\"}\n";summary.write(s.str());
}
}

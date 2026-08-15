#define WIN32_LEAN_AND_MEAN
#include "src/platform/windows/ipc/pipes.h"
#include "src/terminal_session_protocol.h"
#include <Windows.h>
#include <array>
#include <algorithm>
#include <string>

int main(int argc, char **argv) {
  std::string pipe_name;
  DWORD expected_broker_pid = 0;
  std::uint16_t ports[4] {};
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg.rfind("--pipe=", 0) == 0) pipe_name = arg.substr(7);
    else if (arg.rfind("--broker-pid=", 0) == 0) expected_broker_pid = static_cast<DWORD>(std::stoul(arg.substr(13)));
    else if (arg.rfind("--rtsp=", 0) == 0) ports[0] = static_cast<std::uint16_t>(std::stoi(arg.substr(7)));
    else if (arg.rfind("--control=", 0) == 0) ports[1] = static_cast<std::uint16_t>(std::stoi(arg.substr(10)));
    else if (arg.rfind("--video=", 0) == 0) ports[2] = static_cast<std::uint16_t>(std::stoi(arg.substr(8)));
    else if (arg.rfind("--audio=", 0) == 0) ports[3] = static_cast<std::uint16_t>(std::stoi(arg.substr(8)));
  }
  if (pipe_name.empty() || expected_broker_pid == 0 || ports[0] == 0 || ports[1] == 0 || ports[2] == 0 || ports[3] == 0) return 2;
  platf::dxgi::FramedPipeFactory factory {std::make_unique<platf::dxgi::NamedPipeFactory>()};
  auto pipe = factory.create_server(pipe_name);
  if (!pipe) return 3;
  pipe->wait_for_client_connection(5000);
  if (!pipe->is_connected()) return 4;
  DWORD client_pid = 0;
  if (!pipe->get_client_process_id(client_pid) || client_pid != expected_broker_pid) return 7;
  std::array<std::uint8_t, terminal_session::protocol::max_message_size> input {};
  std::size_t size = 0;
  if (pipe->receive(input, size, 5000) != platf::dxgi::PipeResult::Success) return 5;
  auto request = terminal_session::protocol::decode_request(std::span<const std::uint8_t> {input.data(), size});
  terminal_session::protocol::response_t response;
  const bool nonce_present = request && std::any_of(request->ticket.nonce.begin(), request->ticket.nonce.end(), [](std::uint8_t value) { return value != 0; });
  if (!request || request->operation != terminal_session::protocol::opcode::prepare || request->client_uuid.empty() || request->generation == 0 || request->launch_id == 0 || !nonce_present || request->ticket.client_uuid != request->client_uuid || request->ticket.generation != request->generation || request->ticket.launch_id != request->launch_id) {
    response.reason = terminal_session::protocol::reject_reason::malformed;
  } else {
    response.accepted = true; response.client_uuid = request->client_uuid; response.generation = request->generation; response.launch_id = request->launch_id;
    response.rtsp_port = ports[0]; response.control_port = ports[1]; response.video_port = ports[2]; response.audio_port = ports[3];
  }
  const auto bytes = terminal_session::protocol::encode(response);
  if (!bytes.empty()) (void) pipe->send(bytes, 1000);
  return response.accepted ? 0 : 6;
}

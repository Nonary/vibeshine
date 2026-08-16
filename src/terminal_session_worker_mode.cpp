#include "terminal_session_worker_mode.h"

#ifdef _WIN32
  #include "config.h"
  #include "globals.h"
  #include "network.h"
  #include "process.h"
  #include "rtsp.h"
  #include "stream.h"
  #include "terminal_session_launch_codec.h"
  #include "terminal_session_protocol.h"
  #include "terminal_session_service.h"
  #include "video.h"
  #include "platform/windows/ipc/pipes.h"

  #include <Windows.h>

  #include <array>
  #include <atomic>
  #include <charconv>
  #include <chrono>
  #include <cstddef>
  #include <cstdint>
  #include <cstring>
  #include <memory>
  #include <optional>
  #include <thread>
  #include <vector>
#endif

namespace terminal_session::worker_mode {
#ifdef _WIN32
  namespace {
    std::atomic_bool private_worker_active {false};

    std::optional<std::string> environment(const wchar_t *name) {
      const DWORD size = GetEnvironmentVariableW(name, nullptr, 0);
      if (!size) return std::nullopt;
      std::wstring value(size, L'\0');
      const DWORD written = GetEnvironmentVariableW(name, value.data(), size);
      if (!written || written >= size) return std::nullopt;
      value.resize(written);
      const int utf8_size = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
      if (utf8_size <= 0) return std::nullopt;
      std::string result(utf8_size, '\0');
      if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), result.data(), utf8_size, nullptr, nullptr) != utf8_size) return std::nullopt;
      return result;
    }

    route_t expected_route() {
      return {.accepted = true, .ready = true,
        .rtsp_port = net::map_port(rtsp_stream::RTSP_SETUP_PORT),
        .control_port = net::map_port(stream::CONTROL_PORT),
        .video_port = net::map_port(stream::VIDEO_STREAM_PORT),
        .audio_port = net::map_port(stream::AUDIO_STREAM_PORT)};
    }

    struct hdr_target_t {
      DWORD session_id {};
      LUID source_adapter {};
      UINT32 source_id {};
      LUID target_adapter {};
      UINT32 target_id {};
      bool active {};
    };

    // MinGW hides the Win11 advanced-color record behind NTDDI_VERSION. Keep
    // the documented 36-byte ABI local so the worker retains the Windows 10
    // target baseline while querying the newer opcode at runtime.
    struct advanced_color_info_v2_t {
      DISPLAYCONFIG_DEVICE_INFO_HEADER header {};
      UINT32 value {};
      DISPLAYCONFIG_COLOR_ENCODING color_encoding {};
      UINT32 bits_per_color_channel {};
      UINT32 active_color_mode {};
    };
    static_assert(sizeof(advanced_color_info_v2_t) == 36);

    constexpr UINT32 advanced_color_active = 1u << 1;
    constexpr UINT32 advanced_color_mode_hdr = 2;

    std::optional<hdr_target_t> current_hdr_target(std::string &error) {
      DWORD session_id = 0xffffffffu;
      if (!ProcessIdToSessionId(GetCurrentProcessId(), &session_id) || session_id == 0 ||
          session_id == WTSGetActiveConsoleSessionId()) {
        error = "Native HDR activation is restricted to a non-console managed seat.";
        return std::nullopt;
      }
      constexpr UINT32 flags = QDC_ONLY_ACTIVE_PATHS | QDC_VIRTUAL_MODE_AWARE;
      for (int attempt = 0; attempt < 2; ++attempt) {
        UINT32 path_count = 0;
        UINT32 mode_count = 0;
        if (GetDisplayConfigBufferSizes(flags, &path_count, &mode_count) != ERROR_SUCCESS) break;
        std::vector<DISPLAYCONFIG_PATH_INFO> paths(path_count);
        std::vector<DISPLAYCONFIG_MODE_INFO> modes(mode_count);
        const LONG queried = QueryDisplayConfig(flags, &path_count, paths.data(), &mode_count, modes.data(), nullptr);
        if (queried == ERROR_INSUFFICIENT_BUFFER) continue;
        if (queried != ERROR_SUCCESS || path_count != 1) break;
        const auto &path = paths.front();
        advanced_color_info_v2_t color {};
        color.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_ADVANCED_COLOR_INFO_2;
        color.header.size = sizeof(color);
        color.header.adapterId = path.targetInfo.adapterId;
        color.header.id = path.targetInfo.id;
        if (DisplayConfigGetDeviceInfo(&color.header) != ERROR_SUCCESS) break;
        return hdr_target_t {
          .session_id = session_id,
          .source_adapter = path.sourceInfo.adapterId,
          .source_id = path.sourceInfo.id,
          .target_adapter = path.targetInfo.adapterId,
          .target_id = path.targetInfo.id,
          .active = (color.value & advanced_color_active) != 0 && color.active_color_mode == advanced_color_mode_hdr,
        };
      }
      error = "The managed seat did not expose exactly one queryable display target.";
      return std::nullopt;
    }

    bool same_target(const hdr_target_t &left, const hdr_target_t &right) {
      return left.session_id == right.session_id && left.source_adapter.HighPart == right.source_adapter.HighPart &&
             left.source_adapter.LowPart == right.source_adapter.LowPart && left.source_id == right.source_id &&
             left.target_adapter.HighPart == right.target_adapter.HighPart && left.target_adapter.LowPart == right.target_adapter.LowPart &&
             left.target_id == right.target_id;
    }

    std::string hdr_activator_error(const DWORD code) {
      switch (code) {
        case 10: return "invalid helper arguments";
        case 11: return "DisplayConfig query failed";
        case 12: return "the managed seat did not expose exactly one active display path";
        case 13: return "the active display source mode was missing";
        case 14: return "the Remote IDD target was not HDR-enabled and 10-bpc capable before the deadline";
        case 15: return "DXGI factory creation failed";
        case 16: return "the session render adapter could not be opened";
        case 17: return "the D3D11 device could not be created";
        case 18: return "the FP16 shared-displayable texture could not be created";
        case 19: return "the FP16 allocation could not be shared";
        case 20: return "a required documented D3DKMT entry point was unavailable";
        case 21: return "the display adapter could not be opened through D3DKMT";
        case 22: return "the D3DKMT device could not be created";
        case 23: return "the FP16 resource contract could not be queried";
        case 24: return "the FP16 resource could not be reopened through D3DKMT";
        case 25: return "the FP16 allocation did not expose a single clonable driver contract";
        case 26: return "the temporary FP16 primary clone could not be created";
        case 27: return "exclusive source ownership could not be acquired";
        case 28: return "the non-preserving D3DKMTSetDisplayMode transition failed";
        case 29: return "Windows did not report active HDR after the source-mode transition";
        case 30: return "Windows did not retain active HDR after temporary source ownership was released";
        case 31: return "the temporary HDR source owner or allocation could not be released cleanly";
        case 32: return "the active display no longer matched the admitted adapter/source/target";
        case 33: return "the helper was not running in an exact managed non-console seat";
        case 34: return "the helper parent was not the installed LocalSystem terminal broker";
        case 35: return "the helper did not receive the worker's one-shot named-pipe capability";
        default: return "unknown helper failure " + std::to_string(code);
      }
    }

    bool ensure_native_hdr(const rtsp_stream::launch_session_t &launch, platf::dxgi::INamedPipe &pipe,
                           const protocol::request_t &envelope, std::string &error) {
      if (!rtsp_stream::effective_hdr_requested(launch)) return true;

      auto expected_target = current_hdr_target(error);
      if (!expected_target) return false;
      terminal_session::hdr::activation_capability_t capability {
        .session_id = expected_target->session_id,
        .source_adapter_low = expected_target->source_adapter.LowPart,
        .source_adapter_high = static_cast<UINT32>(expected_target->source_adapter.HighPart),
        .source_id = expected_target->source_id,
        .target_adapter_low = expected_target->target_adapter.LowPart,
        .target_adapter_high = static_cast<UINT32>(expected_target->target_adapter.HighPart),
        .target_id = expected_target->target_id,
      };
      protocol::request_t request {
        .operation = protocol::opcode::worker_hdr_activate,
        .release = envelope.release,
        .client_uuid = envelope.client_uuid,
        .generation = envelope.generation,
        .launch_id = envelope.launch_id,
        .launch_payload = std::vector<std::uint8_t>(sizeof(capability)),
        .ticket = envelope.ticket,
      };
      std::memcpy(request.launch_payload.data(), &capability, sizeof(capability));
      const auto encoded = protocol::encode(request);
      if (encoded.empty() || !pipe.send(encoded, 2000)) {
        error = "Private worker could not request broker-authenticated native HDR activation.";
        return false;
      }
      std::array<std::uint8_t, protocol::max_message_size> response_bytes {};
      std::size_t response_size = 0;
      if (pipe.receive(response_bytes, response_size, 20000) != platf::dxgi::PipeResult::Success) {
        error = "The terminal broker did not complete native HDR activation before the watchdog deadline.";
        return false;
      }
      const auto response = protocol::decode_response(std::span<const std::uint8_t> {response_bytes.data(), response_size});
      if (!response || response->client_uuid != envelope.client_uuid || response->generation != envelope.generation ||
          response->launch_id != envelope.launch_id || !response->accepted) {
        if (response && response->owner_launch_id) {
          error = "Native HDR activation failed: " + hdr_activator_error(response->owner_launch_id) + ".";
        } else {
          error = response && !response->error.empty() ? response->error : "The terminal broker rejected native HDR activation.";
        }
        return false;
      }
      std::string verification_error;
      const auto verified = current_hdr_target(verification_error);
      if (!verified || !same_target(*verified, *expected_target) || !verified->active) {
        error = verification_error.empty() ? "Native HDR activation did not survive helper process exit on the admitted target." : verification_error;
        return false;
      }
      BOOST_LOG(info) << "Native terminal-session HDR is active and persisted after the temporary D3DKMT primary was released.";
      return true;
    }
  }

  class context_t::impl_t {
  public:
    std::unique_ptr<platf::dxgi::INamedPipe> pipe;
    protocol::request_t envelope;
    request_t launch;
    std::jthread monitor;
    std::atomic_bool stopped {false};

    bool reply(const bool accepted, const std::string &error = {}) {
      const auto route = expected_route();
      protocol::response_t response {.accepted = accepted,
        .reason = accepted ? protocol::reject_reason::malformed : protocol::reject_reason::invalid_state,
        .error = error, .client_uuid = envelope.client_uuid, .generation = envelope.generation,
        .launch_id = envelope.launch_id, .rtsp_port = route.rtsp_port, .control_port = route.control_port,
        .video_port = route.video_port, .audio_port = route.audio_port};
      const auto bytes = protocol::encode(response);
      return !bytes.empty() && pipe->send(bytes, 2000);
    }

    bool decode_admission(const protocol::request_t &request, terminal_session::request_t &decoded, std::string &error) {
      if ((request.operation != protocol::opcode::prepare && request.operation != protocol::opcode::resume) ||
          request.ticket.operation != request.operation || request.ticket.client_uuid != request.client_uuid ||
          request.ticket.generation != request.generation || request.ticket.launch_id != request.launch_id) {
        error = "Private worker admission envelope is inconsistent.";
        return false;
      }
      auto material = launch_codec::decode(request.launch_payload, error);
      if (!material || !material->launch_session || material->launch_session->client_uuid != request.client_uuid ||
          material->launch_session->role_generation != request.generation || material->launch_session->id != request.launch_id) {
        if (error.empty()) error = "Private worker launch material does not match its admission envelope.";
        return false;
      }
      decoded = std::move(*material);
      return true;
    }

    void monitor_admissions(std::stop_token token) {
      std::array<std::uint8_t, protocol::max_message_size> buffer {};
      while (!token.stop_requested() && !stopped.load()) {
        std::size_t size = 0;
        const auto result = pipe->receive(buffer, size, 500);
        if (result == platf::dxgi::PipeResult::Timeout) continue;
        if (result != platf::dxgi::PipeResult::Success) break;
        if (size == 1 && buffer[0] == 1) {
          stopped.store(true);
          if (mail::man) mail::man->event<bool>(mail::shutdown)->raise(true);
          break;
        }
        const auto request = protocol::decode_request(std::span<const std::uint8_t> {buffer.data(), size});
        std::string error;
        terminal_session::request_t decoded;
        bool accepted = request && request->client_uuid == envelope.client_uuid && request->generation == envelope.generation &&
                        decode_admission(*request, decoded, error);
        if (accepted) {
          try {
            config::set_runtime_config_overrides(std::move(decoded.runtime_config_overrides));
            config::apply_config_now();
          } catch (...) {
            accepted = false;
            error = "Private worker could not apply the authenticated reconnect overrides.";
          }
        }
        if (accepted && !ensure_native_hdr(*decoded.launch_session, *pipe, *request, error)) {
          accepted = false;
        }
        if (accepted && video::probe_encoders()) {
          accepted = false;
          error = "Private worker could not recapture the reconnected managed display.";
        }
        if (accepted) accepted = rtsp_stream::launch_session_raise(decoded.launch_session);
        if (!accepted && error.empty()) error = "Private worker rejected reconnect launch admission.";
        if (request) envelope = *request;
        if (!reply(accepted, error)) break;
      }
    }
  };

  bootstrap_t connect_from_environment() {
    const auto pipe_name = environment(L"VIBESHINE_TERMINAL_WORKER_PIPE");
    const auto broker_text = environment(L"VIBESHINE_TERMINAL_BROKER_PID");
    if (!pipe_name && !broker_text) return {};
    bootstrap_t result {.requested = true};
    if (!pipe_name || !broker_text || pipe_name->empty()) {
      result.error = "Private worker environment is incomplete.";
      return result;
    }
    DWORD broker_pid = 0;
    const auto parsed = std::from_chars(broker_text->data(), broker_text->data() + broker_text->size(), broker_pid);
    if (parsed.ec != std::errc {} || parsed.ptr != broker_text->data() + broker_text->size() || broker_pid == 0) {
      result.error = "Private worker broker PID is invalid.";
      return result;
    }

    platf::dxgi::FramedPipeFactory factory {std::make_unique<platf::dxgi::NamedPipeFactory>()};
    std::unique_ptr<platf::dxgi::INamedPipe> pipe;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds {5};
    do {
      pipe = factory.create_client(*pipe_name);
      if (pipe) break;
      std::this_thread::sleep_for(std::chrono::milliseconds {50});
    } while (std::chrono::steady_clock::now() < deadline);
    if (!pipe) { result.error = "Private worker could not connect to the service-owned first-instance pipe."; return result; }
    DWORD server_pid = 0;
    if (!pipe->get_server_process_id(server_pid) || server_pid != broker_pid ||
        !service::authenticate_service_peer(*pipe)) {
      result.error = "Private worker broker process identity does not match.";
      return result;
    }
    std::array<std::uint8_t, protocol::max_message_size> buffer {};
    std::size_t size = 0;
    if (pipe->receive(buffer, size, 5000) != platf::dxgi::PipeResult::Success) {
      result.error = "Private worker did not receive its one-use launch admission.";
      return result;
    }
    auto request = protocol::decode_request(std::span<const std::uint8_t> {buffer.data(), size});
    if (!request) { result.error = "Private worker admission frame is malformed."; return result; }
    auto impl = std::make_unique<context_t::impl_t>();
    impl->pipe = std::move(pipe);
    impl->envelope = *request;
    if (!impl->decode_admission(*request, impl->launch, result.error)) return result;
    FILETIME created {}, exited {}, kernel {}, user {};
    if (!GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user)) {
      result.error = "Private worker process creation identity could not be recorded.";
      return result;
    }
    ULARGE_INTEGER creation_value {};
    creation_value.LowPart = created.dwLowDateTime;
    creation_value.HighPart = created.dwHighDateTime;
    SetEnvironmentVariableW(L"VIBESHINE_TERMINAL_WORKER_PID", std::to_wstring(GetCurrentProcessId()).c_str());
    SetEnvironmentVariableW(L"VIBESHINE_TERMINAL_WORKER_CREATION", std::to_wstring(creation_value.QuadPart).c_str());
    private_worker_active.store(true, std::memory_order_release);
    SetEnvironmentVariableW(L"VIBESHINE_TERMINAL_WORKER_PIPE", nullptr);
    SetEnvironmentVariableW(L"VIBESHINE_TERMINAL_BROKER_PID", nullptr);
    result.context = std::unique_ptr<context_t>(new context_t(std::move(impl)));
    return result;
  }

  bool active() noexcept { return private_worker_active.load(std::memory_order_acquire); }

  context_t::context_t(std::unique_ptr<impl_t> impl): impl_(std::move(impl)) {}
  context_t::~context_t() { stop(); }
  context_t::context_t(context_t &&) noexcept = default;
  context_t &context_t::operator=(context_t &&) noexcept = default;

  bool context_t::publish_ready(std::string &error) {
    if (!impl_ || !impl_->launch.launch_session) { error = "Private worker launch context is unavailable."; return false; }
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds {5};
    while (!rtsp_stream::listener_ready() && std::chrono::steady_clock::now() < deadline) std::this_thread::sleep_for(std::chrono::milliseconds {25});
    if (!rtsp_stream::listener_ready()) { error = "Private worker RTSP listener did not become ready."; (void) impl_->reply(false, error); return false; }
    try {
      config::set_runtime_config_overrides(std::move(impl_->launch.runtime_config_overrides));
      config::apply_config_now();
    } catch (...) {
      error = "Private worker could not apply the authenticated launch overrides.";
      (void) impl_->reply(false, error);
      return false;
    }
    if (!ensure_native_hdr(*impl_->launch.launch_session, *impl_->pipe, impl_->envelope, error)) {
      (void) impl_->reply(false, error);
      return false;
    }
    if (video::probe_encoders()) {
      error = "Private worker could not capture and encode the managed session display.";
      (void) impl_->reply(false, error);
      return false;
    }
    if (impl_->launch.operation == operation_e::launch &&
        proc::proc.execute(impl_->launch.launch_session->appid, impl_->launch.launch_session) != 0) {
      error = "Private worker could not launch the configured application.";
      (void) impl_->reply(false, error);
      return false;
    }
    if (!rtsp_stream::launch_session_raise(impl_->launch.launch_session)) {
      if (impl_->launch.operation == operation_e::launch) proc::proc.terminate();
      error = "Private worker launch registry rejected the admitted session.";
      (void) impl_->reply(false, error);
      return false;
    }
    if (!impl_->reply(true)) { error = "Private worker could not publish its ready route."; return false; }
    impl_->monitor = std::jthread([impl = impl_.get()](std::stop_token token) { impl->monitor_admissions(token); });
    return true;
  }

  void context_t::stop() {
    if (!impl_) return;
    impl_->stopped.store(true);
    if (impl_->monitor.joinable()) impl_->monitor.request_stop();
    if (impl_->monitor.joinable()) impl_->monitor.join();
  }
#else
  class context_t::impl_t {};
  bootstrap_t connect_from_environment() { return {}; }
  bool active() noexcept { return false; }
  context_t::context_t(std::unique_ptr<impl_t> impl): impl_(std::move(impl)) {}
  context_t::~context_t() = default;
  context_t::context_t(context_t &&) noexcept = default;
  context_t &context_t::operator=(context_t &&) noexcept = default;
  bool context_t::publish_ready(std::string &error) { error = "Private terminal workers are Windows-only."; return false; }
  void context_t::stop() {}
#endif
}

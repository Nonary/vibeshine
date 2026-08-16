#include "terminal_session_launch_codec.h"

#include "rtsp.h"
#include "terminal_session_protocol.h"

#include <future>
#include <limits>
#include <nlohmann/json.hpp>
#include <stdexcept>
#include <unordered_map>

namespace terminal_session::launch_codec {
  namespace {
    using json = nlohmann::json;

    template<class T>
    void optional(json &out, const char *name, const std::optional<T> &value) {
      if (value) out[name] = *value;
    }

    template<class T>
    std::optional<T> optional_value(const json &in, const char *name) {
      if (!in.contains(name) || in.at(name).is_null()) return std::nullopt;
      return in.at(name).get<T>();
    }

    bool bounded_string(const std::string &value, const std::size_t maximum) {
      return value.size() <= maximum;
    }

    bool worker_safe_override(const std::string_view name) {
      if (name == "audio_sink" || name == "virtual_sink" || name == "rtss_frame_limit_type") return false;
      return !name.starts_with("dd_") && !name.starts_with("frame_limiter_") && !name.starts_with("rtx_hdr");
    }
  }

  std::vector<std::uint8_t> encode(const request_t &request, std::string &error) {
    if (!request.launch_session) {
      error = "Terminal launch material is missing.";
      return {};
    }
    const auto &s = *request.launch_session;
    if (s.id == 0 || s.role_generation == 0 || s.client_uuid.empty() ||
        !bounded_string(s.client_uuid, protocol::max_uuid_size) ||
        !bounded_string(s.client_cert, 16 * 1024) || s.gcm_key.size() != 16 || s.iv.size() != 16 ||
        !s.rtsp_cipher || s.rtsp_url_scheme != "rtspenc://") {
      error = "Terminal launch material exceeds its identity or key bounds.";
      return {};
    }

    std::unordered_map<std::string, std::string> safe_overrides;
    for (const auto &[name, value] : request.runtime_config_overrides) {
      if (worker_safe_override(name)) safe_overrides.emplace(name, value);
    }
    json out {
      {"v", 1},
      {"op", static_cast<std::uint8_t>(request.operation)},
      {"id", s.id},
      {"role", static_cast<std::uint8_t>(s.role)},
      {"generation", s.role_generation},
      {"source", s.rtsp_source_address},
      {"gcm", s.gcm_key},
      {"iv", s.iv},
      {"ping", s.av_ping_payload},
      {"control", s.control_connect_data},
      {"host_audio", s.host_audio},
      {"unique", s.unique_id},
      {"client", s.client_uuid},
      {"client_name", s.client_name},
      {"device_name", s.device_name},
      {"steam_offline_isolation", s.terminal_session_requested && s.steam_offline_isolation},
      {"width", s.width},
      {"height", s.height},
      {"fps", s.fps},
      {"gcmap", s.gcmap},
      {"appid", s.appid},
      {"surround_info", s.surround_info},
      {"surround_params", s.surround_params},
      {"continuous_audio", s.continuous_audio},
      {"enable_hdr", s.enable_hdr},
      {"prefer_sdr_10bit", s.prefer_sdr_10bit},
      {"force_sdr", s.force_sdr},
      {"enable_sops", s.enable_sops},
      {"client_vrr", s.client_vrr_requested},
      {"display_mode_override", s.client_display_mode_override},
      {"refresh_millihz", s.client_display_refresh_millihz},
      {"gen1_framegen_fix", s.gen1_framegen_fix},
      {"gen2_framegen_fix", s.gen2_framegen_fix},
      {"frame_generation", s.frame_generation_enabled},
      {"lossless_framegen", s.lossless_scaling_framegen},
      {"framegen_multiplier", s.framegen_refresh_multiplier},
      {"framegen_provider", s.frame_generation_provider},
      {"encrypted_rtsp", s.rtsp_cipher.has_value()},
      {"rtsp_scheme", s.rtsp_url_scheme},
      {"rtsp_iv_counter", s.rtsp_iv_counter},
      {"client_cert", s.client_cert},
      {"overrides", safe_overrides},
    };
    optional(out, "hdr_profile", s.hdr_profile);
    optional(out, "framegen_refresh", s.framegen_refresh_rate);
    optional(out, "framegen_refresh_millihz", s.framegen_refresh_millihz);
    optional(out, "lossless_target_fps", s.lossless_scaling_target_fps);
    optional(out, "lossless_rtss_limit", s.lossless_scaling_rtss_limit);
    if (s.resolution_override) {
      out["resolution_override"] = {{"width", s.resolution_override->width}, {"height", s.resolution_override->height}};
    }
    if (s.app_metadata) {
      out["app"] = {
        {"id", s.app_metadata->id}, {"uuid", s.app_metadata->uuid}, {"name", s.app_metadata->name},
        {"virtual_screen", s.app_metadata->virtual_screen}, {"has_command", s.app_metadata->has_command},
        {"has_playnite", s.app_metadata->has_playnite}, {"playnite_fullscreen", s.app_metadata->playnite_fullscreen},
      };
    }

    auto payload = json::to_cbor(out);
    if (payload.empty() || payload.size() > protocol::max_launch_payload_size) {
      error = "Serialized terminal launch material exceeds the protected IPC bound.";
      return {};
    }
    return payload;
  }

  std::optional<request_t> decode(const std::span<const std::uint8_t> payload, std::string &error) {
    if (payload.empty() || payload.size() > protocol::max_launch_payload_size) {
      error = "Terminal launch payload is empty or oversized.";
      return std::nullopt;
    }
    try {
      const auto in = json::from_cbor(payload, true, true);
      if (in.at("v").get<int>() != 1) throw std::runtime_error("unsupported launch version");
      request_t result;
      result.operation = static_cast<operation_e>(in.at("op").get<std::uint8_t>());
      if (result.operation != operation_e::launch && result.operation != operation_e::resume) throw std::runtime_error("invalid operation");
      auto session = std::make_shared<rtsp_stream::launch_session_t>();
      session->id = in.at("id").get<std::uint32_t>();
      session->role = static_cast<remote_session::role_e>(in.at("role").get<std::uint8_t>());
      session->role_generation = in.at("generation").get<std::uint64_t>();
      session->rtsp_source_address = in.at("source").get<std::string>();
      session->gcm_key = in.at("gcm").get<crypto::aes_t>();
      session->iv = in.at("iv").get<crypto::aes_t>();
      session->av_ping_payload = in.at("ping").get<std::string>();
      session->control_connect_data = in.at("control").get<std::uint32_t>();
      session->host_audio = in.at("host_audio").get<bool>();
      session->unique_id = in.at("unique").get<std::string>();
      session->client_uuid = in.at("client").get<std::string>();
      session->client_name = in.at("client_name").get<std::string>();
      session->device_name = in.at("device_name").get<std::string>();
      session->terminal_session_requested = true;
      session->steam_offline_isolation = in.value("steam_offline_isolation", false);
      session->hdr_profile = optional_value<std::string>(in, "hdr_profile");
      session->width = in.at("width").get<int>();
      session->height = in.at("height").get<int>();
      session->fps = in.at("fps").get<int>();
      session->gcmap = in.at("gcmap").get<int>();
      session->appid = in.at("appid").get<int>();
      session->surround_info = in.at("surround_info").get<int>();
      session->surround_params = in.at("surround_params").get<std::string>();
      session->continuous_audio = in.at("continuous_audio").get<bool>();
      session->enable_hdr = in.at("enable_hdr").get<bool>();
      session->prefer_sdr_10bit = in.at("prefer_sdr_10bit").get<bool>();
      session->force_sdr = in.at("force_sdr").get<bool>();
      session->enable_sops = in.at("enable_sops").get<bool>();
      session->client_vrr_requested = in.at("client_vrr").get<bool>();
      session->client_display_mode_override = in.at("display_mode_override").get<bool>();
      session->client_display_refresh_millihz = in.at("refresh_millihz").get<std::uint32_t>();
      session->client_requests_virtual_display = false;
      session->client_virtual_display_override = false;
      session->virtual_display = false;
      session->display_config_preapplied = true;
      session->gen1_framegen_fix = in.at("gen1_framegen_fix").get<bool>();
      session->gen2_framegen_fix = in.at("gen2_framegen_fix").get<bool>();
      session->frame_generation_enabled = in.at("frame_generation").get<bool>();
      session->lossless_scaling_framegen = in.at("lossless_framegen").get<bool>();
      session->framegen_refresh_rate = optional_value<int>(in, "framegen_refresh");
      session->framegen_refresh_millihz = optional_value<std::uint32_t>(in, "framegen_refresh_millihz");
      session->framegen_refresh_multiplier = in.at("framegen_multiplier").get<int>();
      session->frame_generation_provider = in.at("framegen_provider").get<std::string>();
      session->lossless_scaling_target_fps = optional_value<int>(in, "lossless_target_fps");
      session->lossless_scaling_rtss_limit = optional_value<int>(in, "lossless_rtss_limit");
      session->rtsp_url_scheme = in.at("rtsp_scheme").get<std::string>();
      session->rtsp_iv_counter = in.at("rtsp_iv_counter").get<std::uint32_t>();
      session->client_cert = in.at("client_cert").get<std::string>();
      if (in.at("encrypted_rtsp").get<bool>()) session->rtsp_cipher.emplace(session->gcm_key, false);
      if (in.contains("resolution_override")) {
        session->resolution_override = rtsp_stream::launch_session_t::resolution_override_t {
          in.at("resolution_override").at("width").get<int>(), in.at("resolution_override").at("height").get<int>()};
      }
      if (in.contains("app")) {
        const auto &app = in.at("app");
        session->app_metadata = rtsp_stream::launch_session_t::app_metadata_t {
          app.at("id").get<std::string>(), app.at("uuid").get<std::string>(), app.at("name").get<std::string>(), app.at("virtual_screen").get<bool>(),
          app.at("has_command").get<bool>(), app.at("has_playnite").get<bool>(), app.at("playnite_fullscreen").get<bool>()};
        // The WTS provider already owns the Remote IDD display. An app-level
        // Sunshine virtual-display preference must not create a second target.
        session->app_metadata->virtual_screen = false;
      }
#ifdef _WIN32
      std::promise<rtsp_stream::launch_session_t::display_helper_gate_status_e> gate;
      gate.set_value(rtsp_stream::launch_session_t::display_helper_gate_status_e::proceed);
      session->display_helper_gate = gate.get_future().share();
#endif
      result.runtime_config_overrides = in.at("overrides").get<std::unordered_map<std::string, std::string>>();
      result.launch_session = std::move(session);
      if (result.launch_session->id == 0 || result.launch_session->role_generation == 0 ||
          result.launch_session->client_uuid.empty() || result.launch_session->client_uuid.size() > protocol::max_uuid_size ||
          result.launch_session->client_cert.size() > 16 * 1024 || result.launch_session->gcm_key.size() != 16 ||
          result.launch_session->iv.size() != 16 || result.launch_session->role != remote_session::role_e::game ||
          result.launch_session->appid <= 0 || result.launch_session->width < 320 || result.launch_session->width > 16384 ||
          result.launch_session->height < 200 || result.launch_session->height > 16384 ||
          result.launch_session->fps <= 0 || result.launch_session->fps > 1000 ||
          result.launch_session->rtsp_source_address.size() > 128 || result.launch_session->unique_id.size() > 512 ||
          result.launch_session->client_name.size() > 512 || result.launch_session->device_name.size() > 512 ||
          result.launch_session->rtsp_url_scheme != "rtspenc://" || !result.launch_session->rtsp_cipher.has_value()) {
        throw std::runtime_error("decoded launch identity is invalid");
      }
      if (result.runtime_config_overrides.size() > 128) throw std::runtime_error("too many runtime overrides");
      for (const auto &[name, value] : result.runtime_config_overrides) {
        if (name.empty() || name.size() > 128 || value.size() > 4096 || !worker_safe_override(name)) {
          throw std::runtime_error("runtime override exceeds its worker isolation contract");
        }
      }
      if (result.launch_session->app_metadata &&
          (result.launch_session->app_metadata->id.size() > 128 || result.launch_session->app_metadata->uuid.size() > 128 ||
           result.launch_session->app_metadata->name.size() > 512)) {
        throw std::runtime_error("decoded application metadata exceeds its bound");
      }
      return result;
    } catch (const std::exception &exception) {
      error = "Terminal launch payload was rejected: " + std::string {exception.what()};
      return std::nullopt;
    }
  }
}

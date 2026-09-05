/**
 * @file src/nvhttp.cpp
 * @brief Definitions for the nvhttp (GameStream) server.
 */
// macros
#define BOOST_BIND_GLOBAL_PLACEHOLDERS

// standard includes
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <format>
#include <functional>
#include <future>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

// lib includes
#include <boost/algorithm/string/predicate.hpp>
#include <boost/algorithm/string/trim.hpp>
#include <boost/asio/ssl/context.hpp>
#include <boost/asio/ssl/context_base.hpp>
#include <boost/property_tree/json_parser.hpp>
#include <boost/property_tree/ptree.hpp>
#include <boost/property_tree/xml_parser.hpp>
#include <Simple-Web-Server/server_http.hpp>

// local includes
#include "app_display_policy.h"
#include "config.h"
#include "display_device.h"
#include "display_helper_integration.h"
#include "file_handler.h"
#include "globals.h"
#include "httpcommon.h"
#include "http_pairing_policy.h"
#include "hdr_request_policy.h"
#include "logging.h"
#include "network.h"
#include "nvhttp.h"
#include "remote_session.h"
#include "remote_display_topology.h"
#include "platform/common.h"
#include "state_storage.h"
#ifdef _WIN32
  #include "platform/windows/display.h"
  #include "platform/windows/display_helper_request_policy.h"
  #include "platform/windows/display_helper_request_helpers.h"
  #include "platform/windows/misc.h"
  #include "platform/windows/virtual_display.h"
  #include "platform/windows/virtual_display_cleanup.h"
#elif defined(__linux__)
  #include "platform/linux/private_display.h"
  #include "platform/linux/private_display_resume_policy.h"
#endif

#include "process.h"
#include "rtsp.h"
#include "rtsp_pending_policy.h"
#include "stream.h"
#include "system_tray.h"
#include "update.h"
#include "utility.h"
#include "uuid.h"
#include "video.h"
#include "webrtc_stream.h"

using namespace std::literals;

namespace nvhttp {

  namespace {
    struct remote_role_owner_t {
      remote_session::role_e role {remote_session::role_e::none};
      std::uint64_t generation {};
    };

    std::mutex remote_role_owners_mutex;
    std::unordered_map<std::string, remote_role_owner_t> remote_role_owners;

    std::uint64_t active_session_generation(const proc::active_session_guard_t &session) {
      if (!session.has_active_app) return 0;
      const auto ticks = std::chrono::duration_cast<std::chrono::nanoseconds>(session.launch_started_at.time_since_epoch()).count();
      return ticks > 0 ? static_cast<std::uint64_t>(ticks) : 0;
    }
    // Display creation/topology apply is deliberately synchronous and can
    // outlive Moonlight's patience for a launch response. Serialize synthetic
    // state transitions so a retry cannot make its dispatch decision against
    // half-published ownership or race an explicit disconnect.
    std::mutex remote_http_control_transition_mutex;

    std::string remote_role_owner_key(std::string_view uuid, remote_session::role_e role) {
      return std::string {uuid} + ':' + std::to_string(static_cast<unsigned int>(role));
    }

    struct remote_role_gate_snapshot_t {
      remote_session::owner_t owner;
      bool active {};
    };

    remote_role_gate_snapshot_t remote_role_gate_snapshot_for_client(std::string_view uuid) {
      remote_role_gate_snapshot_t result;
      std::optional<remote_role_owner_t> caller_owner;
      {
        std::lock_guard lock {remote_role_owners_mutex};
        result.active = !remote_role_owners.empty();
        for (const auto role : {remote_session::role_e::monitor, remote_session::role_e::input}) {
          const auto it = remote_role_owners.find(remote_role_owner_key(uuid, role));
          if (it == remote_role_owners.end()) continue;
          caller_owner = it->second;
          break;
        }
      }
      if (!caller_owner) return result;
      result.owner.role = caller_owner->role;
      if (caller_owner->role == remote_session::role_e::monitor) {
        const auto state = remote_session::monitor_runtime_snapshot(uuid, caller_owner->generation);
        result.owner.retained = state.accepted;
        result.owner.ready = state.ready;
        result.owner.retryable = state.retryable;
        if (!state.output.empty()) result.owner.output = state.output;
      }
      return result;
    }

    void remember_remote_owner(std::string_view uuid, remote_session::role_e role, std::uint64_t generation) {
      std::lock_guard lock {remote_role_owners_mutex};
      remote_role_owners.insert_or_assign(remote_role_owner_key(uuid, role), remote_role_owner_t {role, generation});
    }

    std::optional<std::uint64_t> remote_owner_generation(std::string_view uuid, remote_session::role_e role) {
      std::lock_guard lock {remote_role_owners_mutex};
      const auto it = remote_role_owners.find(remote_role_owner_key(uuid, role));
      return it == remote_role_owners.end() ? std::nullopt : std::make_optional(it->second.generation);
    }

    void forget_remote_owner(std::string_view uuid, remote_session::role_e role, std::uint64_t generation) {
      std::lock_guard lock {remote_role_owners_mutex};
      const auto key = remote_role_owner_key(uuid, role);
      const auto it = remote_role_owners.find(key);
      if (it != remote_role_owners.end() && it->second.generation == generation) remote_role_owners.erase(it);
    }

    void forget_remote_client(std::string_view uuid) {
      {
        std::lock_guard lock {remote_role_owners_mutex};
        remote_role_owners.erase(remote_role_owner_key(uuid, remote_session::role_e::monitor));
        remote_role_owners.erase(remote_role_owner_key(uuid, remote_session::role_e::input));
      }
      remote_session::clear_app_replacement_confirmation(uuid);
    }

    bool has_stream_session_activity() {
      // RTSP removes STOPPING sessions from session_count() before stream::session::join()
      // returns; pending launches/creations reserve the process-wide runtime layer
      // before either protocol publishes an active session.
      return rtsp_stream::has_pending_launch_or_startup() ||
             rtsp_stream::session_count_no_cleanup() > 0 ||
             stream::session::running_sessions.load(std::memory_order_acquire) != 0 ||
             stream::session::teardown_sessions.load(std::memory_order_acquire) != 0 ||
             webrtc_stream::has_active_or_pending_sessions() ||
             webrtc_stream::has_capture_active() ||
             webrtc_stream::has_teardown_in_progress();
    }
  }  // namespace

  static constexpr std::string_view EMPTY_PROPERTY_TREE_ERROR_MSG = "Property tree is empty. Probably, control flow got interrupted by an unexpected C++ exception. This is a bug in Sunshine. Moonlight-qt will report Malformed XML (missing root element)."sv;

  void notify_remote_input_transport_lost(const std::string_view client_uuid, const std::uint64_t generation) {
    forget_remote_owner(client_uuid, remote_session::role_e::input, generation);
    if (!has_stream_session_activity() && remote_display_topology::instance().managed_client_identity_count() == 0) {
      config::clear_runtime_config_overrides();
      config::apply_config_now();
    }
  }

  void notify_remote_monitor_released(const std::string_view client_uuid, const std::uint64_t generation) {
    forget_remote_owner(client_uuid, remote_session::role_e::monitor, generation);
  }

  namespace fs = std::filesystem;
  namespace pt = boost::property_tree;

  crypto::cert_chain_t cert_chain;

#ifdef _WIN32
  namespace {
    bool remote_device_id_equals(const std::string &lhs, const std::string &rhs) {
      return !lhs.empty() && !rhs.empty() && boost::iequals(lhs, rhs);
    }

    int remote_refresh_hz(const display_device::FloatingPoint &refresh) {
      if (const auto *ratio = std::get_if<display_device::Rational>(&refresh)) {
        return ratio->m_denominator == 0 ? 0 : static_cast<int>(std::lround(static_cast<double>(ratio->m_numerator) / ratio->m_denominator));
      }
      if (const auto *value = std::get_if<double>(&refresh)) {
        return static_cast<int>(std::lround(*value));
      }
      return 0;
    }

    void refresh_remote_monitor_baseline(const bool extend_active_stream) {
      const auto devices = display_helper_integration::enumerate_devices(display_device::DeviceEnumerationDetail::Minimal);
      if (!devices) return;
      const auto active_stream_output = extend_active_stream ? config::get_active_output_name() : std::string {};
      const bool active_stream_uses_virtual =
        !active_stream_output.empty() && VDISPLAY::is_virtual_display_output(active_stream_output);
      const auto managed_client_ids = remote_display_topology::instance().managed_client_identity_ids();
      std::vector<std::string> managed_device_ids;
      if (active_stream_uses_virtual) {
        for (const auto &client_id : managed_client_ids) {
          if (const auto resolved = VDISPLAY::resolveActiveVirtualDisplayDeviceIdForStableId(client_id, active_stream_output, {}, false)) {
            managed_device_ids.push_back(*resolved);
          }
        }
      }
      const auto active_game = proc::proc.active_session_guard();
      std::vector<remote_display_topology::node_t> baseline;
      for (const auto &device : *devices) {
        if (device.m_device_id.empty() || device.m_display_name.empty() || !device.m_info) continue;
        const bool is_virtual = VDISPLAY::is_virtual_display_output(device.m_device_id);
        if (is_virtual) {
          // A virtual output is a baseline anchor only when another stream is
          // actively capturing that exact output. First-connection Remote
          // Monitor deliberately ignores global virtual-display preferences.
          if (!active_stream_uses_virtual ||
              (!remote_device_id_equals(device.m_device_id, active_stream_output) &&
               !boost::iequals(device.m_display_name, active_stream_output))) continue;
          if (std::any_of(managed_device_ids.begin(), managed_device_ids.end(), [&](const auto &managed_id) {
                return remote_device_id_equals(managed_id, device.m_device_id);
              })) continue;
        } else if (active_stream_uses_virtual) {
          // An existing virtual stream defines the desktop being extended;
          // do not reintroduce physical outputs that it intentionally replaced.
          continue;
        }
        remote_display_topology::node_t node;
        const bool owner_already_managed = std::find(managed_client_ids.begin(), managed_client_ids.end(), active_game.client_uuid) != managed_client_ids.end();
        node.id = is_virtual && !active_game.client_uuid.empty() ?
                    active_game.client_uuid + (owner_already_managed ? ":streamed" : "") :
                    device.m_device_id;
        node.device_id = device.m_device_id;
        node.label = device.m_friendly_name.empty() ? device.m_display_name : device.m_friendly_name;
        node.preexisting = true;
        node.physical = !is_virtual;
        node.active = true;
        node.primary = device.m_info->m_primary;
        node.x = device.m_info->m_origin_point.m_x;
        node.y = device.m_info->m_origin_point.m_y;
        node.configured_mode = {
          .width = static_cast<int>(device.m_info->m_resolution.m_width),
          .height = static_cast<int>(device.m_info->m_resolution.m_height),
          .refresh_hz = remote_refresh_hz(device.m_info->m_refresh_rate),
          .hdr = device.m_info->m_hdr_state.value_or(display_device::HdrState::Disabled) == display_device::HdrState::Enabled,
        };
        baseline.push_back(std::move(node));
      }
      remote_display_topology::instance().set_physical_baseline(std::move(baseline));
    }

    bool apply_remote_monitor_composition(const std::vector<remote_display_topology::node_t> &nodes) {
      display_helper_integration::DisplayTopologyDefinition topology;
      const auto physical = display_helper_integration::capture_physical_topology();
      if (!physical) return false;
      topology.topology = *physical;

      auto has_device = [&topology](const std::string &id) {
        return std::any_of(topology.topology.begin(), topology.topology.end(), [&id](const auto &group) {
          return std::any_of(group.begin(), group.end(), [&id](const auto &candidate) { return remote_device_id_equals(candidate, id); });
        });
      };
      for (const auto &node : nodes) {
        std::string device_id = node.device_id.empty() ? node.id : node.device_id;
        if (!node.physical && !node.preexisting) {
          const auto resolved = VDISPLAY::resolveActiveVirtualDisplayDeviceIdForStableId(node.id, {}, {}, false);
          if (!resolved) return false;
          device_id = *resolved;
        }
        if (!has_device(device_id)) topology.topology.push_back({device_id});
        topology.monitor_positions.emplace(device_id, display_device::Point {node.x, node.y});
        if (node.primary) topology.primary_device = device_id;
      }
      return display_helper_integration::apply_remote_composed_topology(topology);
    }

    std::optional<std::string> remote_monitor_exact_capture_output(
      const std::string &client_uuid,
      const remote_display_topology::mode_t &mode
    ) {
      const auto expected_device = VDISPLAY::resolveActiveVirtualDisplayDeviceIdForStableId(client_uuid, {}, {}, false);
      if (!expected_device) return std::nullopt;
      const auto devices = display_helper_integration::enumerate_devices(display_device::DeviceEnumerationDetail::Minimal);
      if (!devices) return std::nullopt;
      const auto capture_outputs = platf::display_names(platf::mem_type_e::dxgi);
      for (const auto &device : *devices) {
        if (!remote_device_id_equals(device.m_device_id, *expected_device) || !device.m_info || device.m_display_name.empty()) continue;
        const auto refresh = remote_refresh_hz(device.m_info->m_refresh_rate);
        if (static_cast<int>(device.m_info->m_resolution.m_width) != mode.width ||
            static_cast<int>(device.m_info->m_resolution.m_height) != mode.height || refresh != mode.refresh_hz) {
          return std::nullopt;
        }
        const auto output = std::find_if(capture_outputs.begin(), capture_outputs.end(), [&](const auto &candidate) {
          return remote_device_id_equals(candidate, device.m_display_name);
        });
        if (output != capture_outputs.end()) return *output;
        return std::nullopt;
      }
      return std::nullopt;
    }

    void register_remote_monitor_runtime() {
      remote_display_topology::instance().set_runtime_callbacks({
        .create_or_reclaim = [](const std::string &client_uuid, const std::string &client_label, const remote_display_topology::mode_t &mode) {
          if (!VDISPLAY::ensure_driver_is_ready()) return false;
          const auto stable_uuid = VDISPLAY::virtualDisplayUuidFromStableId(client_uuid);
          GUID guid {};
          std::memcpy(&guid, stable_uuid.b8, sizeof(guid));
          return VDISPLAY::createVirtualDisplay(
            client_uuid.c_str(), client_label.c_str(), nullptr,
            static_cast<std::uint32_t>(mode.width), static_cast<std::uint32_t>(mode.height),
            static_cast<std::uint32_t>(mode.refresh_hz * 1000), guid,
            static_cast<std::uint32_t>(mode.refresh_hz * 1000), false, 1, false, false, true, true
          ).has_value();
        },
        .apply_composed_topology = apply_remote_monitor_composition,
        .exact_target_has_current_mode_and_dxgi = remote_monitor_exact_capture_output,
        .remove_owned_display = [](const std::string &client_uuid) {
          const auto stable_uuid = VDISPLAY::virtualDisplayUuidFromStableId(client_uuid);
          GUID guid {};
          std::memcpy(&guid, stable_uuid.b8, sizeof(guid));
          return VDISPLAY::removeVirtualDisplay(guid);
        },
      });
      remote_display_topology::instance().set_plaintext_rtsp_warning_provider([](const std::string &) {
        return rtsp_stream::plaintext_route_warning();
      });
      remote_session::register_monitor_runtime_hooks({
        .activate_or_resume = [](std::string_view uuid, std::string_view label, std::string_view requested_mode, bool, std::uint64_t generation) -> remote_session::monitor_runtime_state_t {
          remote_display_topology::mode_t mode;
          if (std::sscanf(std::string {requested_mode}.c_str(), "%dx%d@%d", &mode.width, &mode.height, &mode.refresh_hz) != 3 ||
              mode.width <= 0 || mode.height <= 0 || mode.refresh_hz <= 0) {
            return remote_session::monitor_runtime_state_t {.retryable = true, .error = "Remote Monitor requested an invalid display mode."};
          }
          refresh_remote_monitor_baseline(has_stream_session_activity());
          const auto state = remote_display_topology::instance().activate_or_resume(std::string {uuid}, std::string {label}, mode, generation);
          return {.accepted = state.accepted, .ready = state.ready, .retryable = state.retryable, .output = state.output, .error = state.error};
        },
        .snapshot = [](std::string_view uuid, std::uint64_t generation) {
          const auto state = remote_display_topology::instance().snapshot(std::string {uuid}, generation);
          return remote_session::monitor_runtime_state_t {.accepted = state.accepted, .ready = state.ready, .retryable = state.retryable, .output = state.output, .error = state.error};
        },
        .explicit_release = [](std::string_view uuid, std::uint64_t generation, std::string_view reason) {
          remote_display_topology::instance().explicit_release(std::string {uuid}, generation, std::string {reason});
        },
        .transport_lost = [](std::string_view uuid, std::uint64_t generation) {
          remote_display_topology::instance().transport_lost(std::string {uuid}, generation);
        },
        .unpair = [](std::string_view uuid) { remote_display_topology::instance().unpair_client(std::string {uuid}); },
        .shutdown = [] { remote_display_topology::instance().shutdown(); },
      });
    }
  }  // namespace
#elif defined(__linux__)
  namespace {
    int linux_remote_refresh_hz(const display_device::FloatingPoint &refresh) {
      if (const auto *ratio = std::get_if<display_device::Rational>(&refresh)) {
        return ratio->m_denominator == 0 ? 0 : static_cast<int>(std::lround(static_cast<double>(ratio->m_numerator) / ratio->m_denominator));
      }
      if (const auto *value = std::get_if<double>(&refresh)) {
        return static_cast<int>(std::lround(*value));
      }
      return 0;
    }

    int linux_remote_logical_dimension(const unsigned int pixels, const display_device::FloatingPoint &scale) {
      double factor = 1.0;
      if (const auto *ratio = std::get_if<display_device::Rational>(&scale)) {
        if (ratio->m_denominator != 0) {
          factor = static_cast<double>(ratio->m_numerator) / ratio->m_denominator;
        }
      } else if (const auto *value = std::get_if<double>(&scale)) {
        factor = *value;
      }
      return static_cast<int>(std::lround(pixels / std::max(factor, 0.01)));
    }

    void refresh_remote_monitor_baseline(const bool extend_active_stream) {
      const auto devices = display_helper_integration::enumerate_devices(
        display_device::DeviceEnumerationDetail::Full
      );
      if (!devices) {
        return;
      }

      const auto active_stream_output = extend_active_stream ? config::get_active_output_name() : std::string {};
      const bool active_stream_uses_private =
        !active_stream_output.empty() && platf::linux_private_display::is_private_output(active_stream_output);
      const auto managed_client_ids = remote_display_topology::instance().managed_client_identity_ids();
      std::vector<std::string> managed_outputs;
      managed_outputs.reserve(managed_client_ids.size());
      for (const auto &client_id : managed_client_ids) {
        if (const auto output = platf::linux_private_display::output_for_client(client_id)) {
          managed_outputs.push_back(*output);
        }
      }

      const auto active_game = proc::proc.active_session_guard();
      std::vector<remote_display_topology::node_t> baseline;
      for (const auto &device : *devices) {
        if (device.m_device_id.empty() || device.m_display_name.empty() || !device.m_info) {
          continue;
        }
        const bool is_private = platf::linux_private_display::is_private_output(device.m_device_id);
        if (is_private) {
          // Coordinator-managed normal/Remote Monitor identities are emitted by
          // compose_locked(). Only an older or shared private stream that is not
          // in the coordinator remains a pre-existing baseline anchor.
          if (!active_stream_uses_private || (device.m_device_id != active_stream_output && device.m_display_name != active_stream_output) || std::find(managed_outputs.begin(), managed_outputs.end(), device.m_device_id) != managed_outputs.end()) {
            continue;
          }
        } else if (active_stream_uses_private) {
          // An existing private stream defines the desktop being extended; do
          // not expose physical outputs that its exclusive policy hid.
          continue;
        }

        remote_display_topology::node_t node;
        const bool owner_already_managed =
          std::find(managed_client_ids.begin(), managed_client_ids.end(), active_game.client_uuid) != managed_client_ids.end();
        node.id = is_private && !active_game.client_uuid.empty() ?
                    active_game.client_uuid + (owner_already_managed ? ":streamed" : "") :
                    device.m_device_id;
        node.device_id = device.m_device_id;
        node.label = device.m_friendly_name.empty() ? device.m_display_name : device.m_friendly_name;
        node.preexisting = true;
        node.physical = !is_private;
        node.active = true;
        node.primary = device.m_info->m_primary;
        node.x = device.m_info->m_origin_point.m_x;
        node.y = device.m_info->m_origin_point.m_y;
        node.configured_mode = {
          .width = static_cast<int>(device.m_info->m_resolution.m_width),
          .height = static_cast<int>(device.m_info->m_resolution.m_height),
          .refresh_hz = linux_remote_refresh_hz(device.m_info->m_refresh_rate),
          .hdr = device.m_info->m_hdr_state.value_or(display_device::HdrState::Disabled) == display_device::HdrState::Enabled,
        };
        node.layout_width = linux_remote_logical_dimension(
          device.m_info->m_resolution.m_width,
          device.m_info->m_resolution_scale
        );
        node.layout_height = linux_remote_logical_dimension(
          device.m_info->m_resolution.m_height,
          device.m_info->m_resolution_scale
        );
        baseline.push_back(std::move(node));
      }
      remote_display_topology::instance().set_physical_baseline(std::move(baseline));
    }

    enum class linux_normal_identity_result_e {
      not_needed,
      ready,
      capacity_rejected,
      topology_failed,
    };

    linux_normal_identity_result_e reserve_linux_normal_display_identity(
      const std::shared_ptr<rtsp_stream::launch_session_t> &launch_session
    ) {
      const auto mode = launch_session->virtual_display_mode_override.value_or(config::video.virtual_display_mode);
      if (!launch_session->virtual_display || mode == config::video_t::virtual_display_mode_e::shared || launch_session->role != remote_session::role_e::game) {
        return linux_normal_identity_result_e::not_needed;
      }
      if (launch_session->normal_vdd_identity_token != 0) {
        return linux_normal_identity_result_e::ready;
      }

      const auto reservation = remote_display_topology::instance().reserve_normal_game_identity(
        launch_session->client_uuid,
        launch_session->client_name,
        {
          .width = launch_session->width,
          .height = launch_session->height,
          .refresh_hz = launch_session->fps,
          .hdr = rtsp_stream::effective_hdr_requested(*launch_session),
        }
      );
      if (!reservation.accepted) {
        launch_session->normal_vdd_capacity_rejected = true;
        launch_session->virtual_display_failed = true;
        return linux_normal_identity_result_e::capacity_rejected;
      }

      launch_session->normal_vdd_identity_token = reservation.token;
      launch_session->normal_vdd_identity_newly_reserved = reservation.newly_reserved;
      const bool reapply_topology = platf::linux_private_display::resume_policy::requires_topology_reapply(
        reservation.newly_reserved,
        launch_session->virtual_display_needs_resume_apply
      );
      if (reapply_topology) {
        refresh_remote_monitor_baseline(has_stream_session_activity());
      } else {
        BOOST_LOG(debug) << "Linux private display: reusing the retained game topology without a resume modeset.";
      }
      if (reapply_topology && !remote_display_topology::instance().reapply_composed_topology()) {
        if (reservation.newly_reserved) {
          remote_display_topology::instance().rollback_normal_game_identity(
            launch_session->client_uuid,
            reservation.token
          );
          const auto protected_clients = remote_display_topology::instance().protected_remote_monitor_client_ids();
          const bool topology_restored = remote_display_topology::instance().reapply_composed_topology();
          if (topology_restored && std::find(protected_clients.begin(), protected_clients.end(), launch_session->client_uuid) == protected_clients.end()) {
            (void) platf::linux_private_display::remote_remove_owned_display(launch_session->client_uuid);
          }
        }
        launch_session->normal_vdd_identity_token = 0;
        launch_session->normal_vdd_identity_newly_reserved = false;
        launch_session->virtual_display_failed = true;
        return linux_normal_identity_result_e::topology_failed;
      }
      if (!platf::linux_private_display::publish_current_session_state(*launch_session)) {
        BOOST_LOG(error) << "Linux private display: composed output did not publish verified session state.";
        if (reservation.newly_reserved) {
          remote_display_topology::instance().rollback_normal_game_identity(
            launch_session->client_uuid,
            reservation.token
          );
          if (remote_display_topology::instance().reapply_composed_topology()) {
            (void) platf::linux_private_display::remote_remove_owned_display(launch_session->client_uuid);
          }
        }
        launch_session->normal_vdd_identity_token = 0;
        launch_session->normal_vdd_identity_newly_reserved = false;
        launch_session->virtual_display_failed = true;
        return linux_normal_identity_result_e::topology_failed;
      }
      return linux_normal_identity_result_e::ready;
    }

    void rollback_linux_normal_display_identity(
      const std::shared_ptr<rtsp_stream::launch_session_t> &launch_session
    ) {
      if (!launch_session->normal_vdd_identity_newly_reserved) {
        return;
      }
      remote_display_topology::instance().rollback_normal_game_identity(
        launch_session->client_uuid,
        launch_session->normal_vdd_identity_token
      );
      const auto protected_clients = remote_display_topology::instance().protected_remote_monitor_client_ids();
      const bool topology_restored = remote_display_topology::instance().reapply_composed_topology();
      if (topology_restored && std::find(protected_clients.begin(), protected_clients.end(), launch_session->client_uuid) == protected_clients.end()) {
        (void) platf::linux_private_display::remote_remove_owned_display(launch_session->client_uuid);
      }
    }

    void register_remote_monitor_runtime() {
      remote_display_topology::instance().set_runtime_callbacks({
        .create_or_reclaim = [](const std::string &client_uuid, const std::string &, const remote_display_topology::mode_t &mode) {
          return platf::linux_private_display::remote_create_or_reclaim(client_uuid, mode);
        },
        .resolve_mode = platf::linux_private_display::remote_resolve_mode,
        .apply_composed_topology = platf::linux_private_display::remote_apply_composed_topology,
        .exact_target_has_current_mode_and_dxgi = platf::linux_private_display::remote_exact_capture_output,
        .remove_owned_display = platf::linux_private_display::remote_remove_owned_display,
      });
      remote_display_topology::instance().set_plaintext_rtsp_warning_provider([](const std::string &) {
        return rtsp_stream::plaintext_route_warning();
      });
      remote_session::register_monitor_runtime_hooks({
        .activate_or_resume = [](std::string_view uuid, std::string_view label, std::string_view requested_mode, const bool hdr_requested, std::uint64_t generation) -> remote_session::monitor_runtime_state_t {
          remote_display_topology::mode_t mode;
          if (std::sscanf(std::string {requested_mode}.c_str(), "%dx%d@%d", &mode.width, &mode.height, &mode.refresh_hz) != 3 || mode.width <= 0 || mode.height <= 0 || mode.refresh_hz <= 0) {
            return remote_session::monitor_runtime_state_t {.retryable = true, .error = "Remote Monitor requested an invalid display mode."};
          }
          mode.hdr = hdr_requested;
          refresh_remote_monitor_baseline(has_stream_session_activity());
          const auto state = remote_display_topology::instance().activate_or_resume(
            std::string {uuid},
            std::string {label},
            mode,
            generation
          );
          return {.accepted = state.accepted, .ready = state.ready, .retryable = state.retryable, .output = state.output, .error = state.error, .hdr_enabled = state.hdr_enabled};
        },
        .snapshot = [](std::string_view uuid, std::uint64_t generation) {
          const auto state = remote_display_topology::instance().snapshot(std::string {uuid}, generation);
          return remote_session::monitor_runtime_state_t {.accepted = state.accepted, .ready = state.ready, .retryable = state.retryable, .output = state.output, .error = state.error, .hdr_enabled = state.hdr_enabled};
        },
        .explicit_release = [](std::string_view uuid, std::uint64_t generation, std::string_view reason) {
          remote_display_topology::instance().explicit_release(std::string {uuid}, generation, std::string {reason});
        },
        .transport_lost = [](std::string_view uuid, std::uint64_t generation) {
          remote_display_topology::instance().transport_lost(std::string {uuid}, generation);
        },
        .unpair = [](std::string_view uuid) {
          remote_display_topology::instance().unpair_client(std::string {uuid});
        },
        .shutdown = [] {
          remote_display_topology::instance().shutdown(
            platf::linux_private_display::process_shutdown_preserve_requested()
          );
        },
      });
    }
  }  // namespace
#endif

  std::string cert_subject_name_for_log(const crypto::x509_t &cert) {
    auto subject_name = pairing_policy::certificate_subject_name(crypto::subject_name(cert.get()));
    if (subject_name.empty()) {
      return "unknown"s;
    }
    return subject_name;
  }

  class SunshineHTTPSServer: public SimpleWeb::ServerBase<SunshineHTTPS> {
  public:
    SunshineHTTPSServer(const std::string &certification_file, const std::string &private_key_file):
        ServerBase<SunshineHTTPS>::ServerBase(443),
        context(boost::asio::ssl::context::tls_server) {
      // Disabling TLS 1.0 and 1.1 (see RFC 8996)
      context.set_options(boost::asio::ssl::context::no_tlsv1);
      context.set_options(boost::asio::ssl::context::no_tlsv1_1);
      context.use_certificate_chain_file(certification_file);
      context.use_private_key_file(private_key_file, boost::asio::ssl::context::pem);
    }

    std::function<int(SSL *, const boost::asio::ip::tcp::endpoint &)> verify;
    std::function<void(std::shared_ptr<Response>, std::shared_ptr<Request>)> on_verify_failed;

  protected:
    boost::asio::ssl::context context;

    void after_bind() override {
      if (verify) {
        context.set_verify_mode(boost::asio::ssl::verify_peer | boost::asio::ssl::verify_fail_if_no_peer_cert | boost::asio::ssl::verify_client_once);
        context.set_verify_callback([](int verified, boost::asio::ssl::verify_context &ctx) {
          // To respond with an error message, a connection must be established
          return 1;
        });
      }
    }

    // This is Server<HTTPS>::accept() with SSL validation support added
    void accept() override {
      auto connection = create_connection(*io_service, context);

      acceptor->async_accept(connection->socket->lowest_layer(), [this, connection](const SimpleWeb::error_code &ec) {
        auto lock = connection->handler_runner->continue_lock();
        if (!lock) {
          return;
        }

        if (ec != SimpleWeb::error::operation_aborted) {
          this->accept();
        }

        auto session = std::make_shared<Session>(config.max_request_streambuf_size, connection);

        if (!ec) {
          boost::asio::ip::tcp::no_delay option(true);
          SimpleWeb::error_code ec;
          session->connection->socket->lowest_layer().set_option(option, ec);

          session->connection->set_timeout(config.timeout_request);
          session->connection->socket->async_handshake(boost::asio::ssl::stream_base::server, [this, session](const SimpleWeb::error_code &ec) {
            session->connection->cancel_timeout();
            auto lock = session->connection->handler_runner->continue_lock();
            if (!lock) {
              return;
            }
            if (!ec) {
              SimpleWeb::error_code remote_endpoint_ec;
              const auto remote_endpoint = session->connection->socket->lowest_layer().remote_endpoint(remote_endpoint_ec);
              if (remote_endpoint_ec) {
                if (this->on_error) {
                  this->on_error(session->request, remote_endpoint_ec);
                }
                return;
              }

              if (verify && !verify(session->connection->socket->native_handle(), remote_endpoint)) {
                this->write(session, on_verify_failed);
              } else {
                this->read(session);
              }
            } else if (this->on_error) {
              this->on_error(session->request, ec);
            }
          });
        } else if (this->on_error) {
          this->on_error(session->request, ec);
        }
      });
    }
  };

  using https_server_t = SunshineHTTPSServer;
  using http_server_t = SimpleWeb::Server<SimpleWeb::HTTP>;

  struct conf_intern_t {
    std::string servercert;
    std::string pkey;
  } conf_intern;

  struct http_encoder_capabilities_t {
    // Keep verification paired with the advertised values. On Windows, the
    // helper may use a temporary adapter identity that is cleared on return.
    video::advertised_encoder_capabilities_t advertised;
    bool probe_complete;
  };

#ifdef _WIN32
  namespace {
    bool display_helper_session_available() {
      if (platf::is_running_as_system()) {
        return true;
      }
      HANDLE user_token = platf::retrieve_users_token(false);
      const bool available = (user_token != nullptr);
      if (user_token) {
        CloseHandle(user_token);
      }
      return available;
    }

    bool has_active_virtual_display() {
      const auto virtual_displays = VDISPLAY::enumerateVirtualDisplays();
      return std::any_of(
        virtual_displays.begin(),
        virtual_displays.end(),
        [](const VDISPLAY::VirtualDisplayInfo &info) {
          return info.is_active;
        }
      );
    }

    bool has_active_or_stopping_stream_session();

    http_encoder_capabilities_t advertised_encoder_capabilities_for_http() {
      std::optional<video::encoder_probe_adapter_hint_lease_t> idle_virtual_adapter_hint;
      std::optional<LUID> idle_virtual_required_adapter;
      if (config::video.virtual_display_mode != config::video_t::virtual_display_mode_e::disabled &&
          !has_stream_session_activity()) {
        const auto intended_adapter = platf::resolve_preferred_render_adapter(
          config::video.adapter_name,
          config::video.adapter_pnp_id
        );
        if (intended_adapter) {
          idle_virtual_required_adapter = *intended_adapter.luid;
          idle_virtual_adapter_hint =
            video::set_pending_virtual_display_adapter_hint(*intended_adapter.luid);
        }
      }
      auto idle_virtual_adapter_hint_guard = util::fail_guard([&] {
        if (idle_virtual_adapter_hint) {
          (void) video::clear_pending_virtual_display_adapter_hint(*idle_virtual_adapter_hint);
        }
      });

      const auto publish = [](
                             video::advertised_encoder_capabilities_t caps,
                             const bool probe_complete,
                             const std::string_view reason
                           ) {
        BOOST_LOG(debug)
          << "HTTP encoder capabilities: probe_complete=" << probe_complete
          << ", hdr="
          << (caps.hevc_mode == 3 || caps.av1_mode == 3)
          << ", hevc_mode=" << caps.hevc_mode
          << ", av1_mode=" << caps.av1_mode
          << ", source=" << reason << '.';
        return http_encoder_capabilities_t {
          .advertised = std::move(caps),
          .probe_complete = probe_complete,
        };
      };

      bool probe_complete = false;
      auto caps = video::advertised_encoder_capabilities(false, &probe_complete);
      if (probe_complete) {
        return publish(std::move(caps), true, "matching-cache");
      }

      // Session starts publish their pending owner while holding this gate.
      // Hold it through the idle decision and temporary-display probe so a
      // new start cannot enter between the counter samples below.
      std::unique_lock<std::mutex> lifecycle_lock(
        stream_lifecycle_mutex(),
        std::try_to_lock
      );
      if (!lifecycle_lock.owns_lock()) {
        BOOST_LOG(debug) << "Skipping HTTP encoder capability probe while stream lifecycle work owns the gate.";
        return publish(std::move(caps), false, "lifecycle-gate");
      }
      caps = video::advertised_encoder_capabilities(false, &probe_complete);
      if (probe_complete) {
        return publish(std::move(caps), true, "matching-cache-after-gate");
      }

      if (has_active_or_stopping_stream_session()) {
        BOOST_LOG(debug) << "Skipping HTTP encoder capability probe while a streaming session is active or stopping.";
        return publish(std::move(caps), false, "active-or-stopping-session");
      }

      auto ensure_result = VDISPLAY::ensure_display(idle_virtual_required_adapter);
      auto cleanup_probe_display = util::fail_guard([&ensure_result]() {
        VDISPLAY::cleanup_ensure_display(ensure_result);
      });
      if (idle_virtual_required_adapter && !ensure_result.owns_temporary_probe_request()) {
        BOOST_LOG(warning)
          << "HTTP capability discovery could not acquire a temporary-display lease; continuing with exact-adapter synthetic validation.";
      } else if (ensure_result.owns_temporary_probe_request() && !ensure_result.ready_for_capture()) {
        BOOST_LOG(info)
          << "HTTP capability discovery is probing synthetic surfaces before the temporary display is published.";
      }
      caps = video::advertised_encoder_capabilities(true, &probe_complete);
      return publish(std::move(caps), probe_complete, "idle-probe");
    }

    void cleanup_virtual_display_state(const bool force_display_restore = false) {
      stream::session::cleanup_reservation_t cleanup_reservation;
      const bool has_active_display = has_active_virtual_display();
      const bool has_retained_probe_display = VDISPLAY::has_retained_ensure_display();
      if (!has_active_display && !has_retained_probe_display) {
        if (force_display_restore) {
          (void) display_helper_integration::revert();
          return;
        }
        BOOST_LOG(debug) << "Skipping virtual display cleanup after cancel because no active virtual display exists.";
        return;
      }
      if (!has_active_display) {
        BOOST_LOG(info) << "Removing retained encoder-probe virtual display after cancel.";
        VDISPLAY::cleanup_retained_ensure_display();
        return;
      }
      (void) platf::virtual_display_cleanup::run(
        "cancel",
        force_display_restore || config::video.dd.config_revert_on_disconnect
      );
    }

    bool has_active_or_stopping_stream_session() {
      // Sample the generic/VDD cleanup signals on both sides of the protocol
      // activity snapshot. This closes both counter handoffs:
      //   launch: generic reservation -> pending protocol owner
      //   teardown: protocol owner -> generic cleanup reservation
      // Each writer publishes the successor before releasing the predecessor.
      if (platf::virtual_display_cleanup::in_progress() ||
          stream::session::cleanup_reservations.load(std::memory_order_acquire) != 0) {
        return true;
      }
      if (has_stream_session_activity()) {
        return true;
      }
      return platf::virtual_display_cleanup::in_progress() ||
             stream::session::cleanup_reservations.load(std::memory_order_acquire) != 0;
    }

    // Same idleness test as has_active_or_stopping_stream_session() minus the
    // generic cleanup-reservation term. Teardown-path callers hold a reservation
    // across their whole request, so that term reports their own frame as
    // activity and makes the predicate unconditionally true; launch()/resume()
    // teardown guards are reservation-insensitive for the same reason. A
    // concurrent virtual-display cleanup is still honoured, double-sampled
    // around the activity snapshot to close the same handoff.
    bool has_stream_session_activity_or_display_cleanup() {
      if (platf::virtual_display_cleanup::in_progress()) {
        return true;
      }
      if (has_stream_session_activity()) {
        return true;
      }
      return platf::virtual_display_cleanup::in_progress();
    }

    void cleanup_virtual_display_if_idle_locked() {
      try {
        if (has_stream_session_activity_or_display_cleanup()) {
          BOOST_LOG(info) << "Skipping virtual display cleanup because a streaming session is active or stopping.";
          return;
        }

        if (!remote_display_topology::instance().generic_virtual_display_cleanup_allowed()) {
          BOOST_LOG(info) << "Deferring virtual display cleanup until the remaining managed client display sessions release ownership.";
          return;
        }

        // The shared finalizer stays armed while managed display owners exist.
        // Re-run it after each owner release so the final release consumes a
        // queued app REVERT exactly once. If no shared finalization is armed,
        // fall back to the bounded cancel cleanup path.
        if (stream::session::finalize_shared_runtime_if_idle("managed_display_owner_release")) {
          return;
        }
        cleanup_virtual_display_state(proc::consume_deferred_display_revert());
      } catch (const std::exception &e) {
        BOOST_LOG(warning) << "Virtual display cleanup failed: " << e.what();
      } catch (...) {
        BOOST_LOG(warning) << "Virtual display cleanup failed with an unknown exception.";
      }
    }

    void cleanup_virtual_display_if_idle() {
      // Serialize the final owner check through cleanup and exclude a
      // concurrent RTSP or WebRTC start. Callers already holding the lifecycle
      // gate must use cleanup_virtual_display_if_idle_locked().
      std::unique_lock<std::mutex> lifecycle_lock(stream_lifecycle_mutex());
      cleanup_virtual_display_if_idle_locked();
    }

    void prepare_virtual_display_for_session(
      const std::shared_ptr<rtsp_stream::launch_session_t> &launch_session,
      bool no_active_sessions,
      bool allow_display_changes,
      std::optional<std::string> &pending_output_override,
      std::optional<video::encoder_probe_adapter_hint_lease_t> &pending_adapter_hint,
      const std::function<bool()> &display_startup_cancelled,
      const std::chrono::steady_clock::time_point display_startup_deadline
    ) {
      std::optional<std::string> app_output_override;
      auto app_display_override = proc::display_policy::app_override_e::inherit;
      if (launch_session->output_name_override) {
        app_output_override = boost::algorithm::trim_copy(*launch_session->output_name_override);
      }

      if (app_output_override && !app_output_override->empty() && VDISPLAY::is_virtual_display_selection(*app_output_override)) {
        launch_session->virtual_display = true;
        app_display_override = proc::display_policy::app_override_e::virtual_display;
        app_output_override.reset();
      } else if (app_output_override) {
        app_display_override = proc::display_policy::app_override_e::physical;
      }
      launch_session->virtual_display_recreated_on_demand = false;
      launch_session->virtual_display_needs_resume_apply = false;

      bool config_requests_virtual = config::video.virtual_display_mode != config::video_t::virtual_display_mode_e::disabled;
      if (launch_session->virtual_display_mode_override) {
        config_requests_virtual =
          *launch_session->virtual_display_mode_override != config::video_t::virtual_display_mode_e::disabled;
      }
      const bool client_requests_virtual = launch_session->client_virtual_display_override.value_or(
        launch_session->client_requests_virtual_display
      );
      const bool session_requests_virtual = launch_session->app_metadata && launch_session->app_metadata->virtual_screen;
      const bool launch_requests_physical = launch_session->client_virtual_display_override &&
                                            !*launch_session->client_virtual_display_override;
      bool request_virtual_display = proc::display_policy::resolve_virtual_display_request(
        (config_requests_virtual && !launch_requests_physical) ||
          launch_session->virtual_display || session_requests_virtual,
        app_display_override
      ) || client_requests_virtual;
      const auto requested_virtual_display_mode =
        launch_session->virtual_display_mode_override.value_or(config::video.virtual_display_mode);
      const bool shared_virtual_display_mode =
        requested_virtual_display_mode == config::video_t::virtual_display_mode_e::shared;
      auto shared_virtual_display_uuid = VDISPLAY::persistentVirtualDisplayUuid();
      if (shared_virtual_display_mode && !http::shared_virtual_display_guid.empty()) {
        try {
          shared_virtual_display_uuid =
            uuid_util::uuid_t::parse(http::shared_virtual_display_guid);
        } catch (...) {
          // Creation uses the same persistent fallback and repairs the stored value.
        }
      }
      const std::string virtual_display_stable_id =
        shared_virtual_display_mode ?
          shared_virtual_display_uuid.string() :
          (!launch_session->client_uuid.empty() ?
             launch_session->client_uuid :
             launch_session->unique_id);
      const auto virtual_display_stable_uuid =
        VDISPLAY::virtualDisplayUuidFromStableId(virtual_display_stable_id);
      GUID virtual_display_stable_guid {};
      std::memcpy(
        &virtual_display_stable_guid,
        virtual_display_stable_uuid.b8,
        sizeof(virtual_display_stable_guid)
      );
      bool has_app_output_override = app_output_override.has_value();
      auto make_framegen_policy = [&](bool uses_virtual_display) {
        return framegen::make_stream_start_policy({
          .fps = launch_session->fps,
          .display_refresh_millihz = launch_session->client_display_refresh_millihz,
          .frame_generation_enabled = launch_session->frame_generation_enabled,
          .gen1_framegen_fix = launch_session->gen1_framegen_fix,
          .gen2_framegen_fix = launch_session->gen2_framegen_fix,
          .lossless_scaling_framegen = launch_session->lossless_scaling_framegen,
          .lossless_rtss_limit = launch_session->lossless_scaling_rtss_limit,
          .frame_generation_provider = launch_session->frame_generation_provider,
          .uses_virtual_display = uses_virtual_display,
          .capture_mode = config::video.capture,
          .auto_capture_uses_wgc = platf::dxgi::should_use_wgc_default(),
          .auto_virtual_framegen_limiter = config::frame_limiter.virtual_display_limiter_enabled(),
          .virtual_display_refresh_multiplier = config::frame_limiter.fixed_virtual_display_refresh_multiplier(),
        });
      };
      const auto requested_display_framegen_policy = make_framegen_policy(request_virtual_display);
      const bool framegen_requires_virtual_display = requested_display_framegen_policy.requires_virtual_display;
      if (framegen_requires_virtual_display) {
        request_virtual_display = true;
        app_output_override.reset();
        has_app_output_override = false;
      }
      auto apply_framegen_refresh_policy = [&](bool uses_virtual_display) {
        const auto framegen_policy = make_framegen_policy(uses_virtual_display);
        launch_session->framegen_refresh_rate = framegen_policy.framegen_refresh_rate;
        launch_session->framegen_refresh_millihz = framegen_policy.framegen_refresh_millihz;
        launch_session->framegen_refresh_multiplier = framegen_policy.refresh_multiplier;
      };
      auto reserve_normal_vdd_identity = [&]() {
        if (shared_virtual_display_mode || launch_session->role != remote_session::role_e::game) return true;
        if (launch_session->normal_vdd_identity_token != 0) return true;
        const remote_display_topology::mode_t mode {
          .width = launch_session->width,
          .height = launch_session->height,
          .refresh_hz = launch_session->fps,
          .hdr = rtsp_stream::effective_hdr_requested(*launch_session),
        };
        const auto reservation = remote_display_topology::instance().reserve_normal_game_identity(
          launch_session->client_uuid,
          launch_session->client_name,
          mode
        );
        if (!reservation.accepted) {
          launch_session->normal_vdd_capacity_rejected = true;
          launch_session->virtual_display_failed = true;
          BOOST_LOG(warning) << "Rejecting per-client virtual display for client '" << launch_session->client_uuid
                             << "' because all four client identities are already reserved.";
          return false;
        }
        launch_session->normal_vdd_identity_token = reservation.token;
        launch_session->normal_vdd_identity_newly_reserved = reservation.newly_reserved;
        return true;
      };
      BOOST_LOG(debug) << "Display helper: session prep client='" << launch_session->client_name
                       << "' allow_display_changes=" << allow_display_changes
                       << " no_active_sessions=" << no_active_sessions
                       << " request_virtual_display=" << request_virtual_display
                       << " framegen_requires_virtual_display=" << framegen_requires_virtual_display
                       << " previous_virtual_device_id='" << launch_session->virtual_display_device_id
                       << "' active_output='" << config::get_active_output_name()
                       << "' app_output_override='" << (app_output_override ? *app_output_override : std::string {})
                       << "'.";

      if (!VDISPLAY::policy::should_prepare_display_for_new_session(no_active_sessions)) {
        const auto previous_virtual_display_device_id = launch_session->virtual_display_device_id;
        launch_session->virtual_display = false;
        launch_session->virtual_display_failed = request_virtual_display;
        launch_session->virtual_display_guid_bytes.fill(0);
        launch_session->virtual_display_device_id.clear();
        launch_session->virtual_display_ready_since.reset();
        launch_session->virtual_display_hdr_enabled.reset();
        launch_session->virtual_display_recreated_on_demand = false;
        launch_session->virtual_display_needs_resume_apply = false;
        if (request_virtual_display) {
          if (!reserve_normal_vdd_identity()) return;
          const auto existing_device =
            VDISPLAY::resolveActiveVirtualDisplayDeviceIdForStableId(
              virtual_display_stable_id,
              previous_virtual_display_device_id,
              launch_session->client_name,
              VDISPLAY::policy::allow_generic_resume_fallback()
            );
          if (existing_device &&
              VDISPLAY::configuredRenderAdapterMatchesVirtualDisplay(
                virtual_display_stable_guid,
                "active RTSP/shared virtual display reuse"
              )) {
            launch_session->virtual_display = true;
            launch_session->virtual_display_failed = false;
            launch_session->virtual_display_device_id = *existing_device;
            launch_session->virtual_display_ready_since = std::chrono::steady_clock::now();
            launch_session->virtual_display_hdr_enabled.reset();
            apply_framegen_refresh_policy(true);
            BOOST_LOG(info) << "Display helper: another session is active; joining its validated virtual capture target (device_id="
                            << *existing_device << ").";
          } else if (existing_device) {
            BOOST_LOG(error) << "Existing virtual display does not match the configured capture adapter; refusing to claim shared-session virtual-display reuse.";
          } else {
            BOOST_LOG(warning) << "Another session is active, but no reusable virtual display was found for this request.";
          }
        } else {
          apply_framegen_refresh_policy(false);
          BOOST_LOG(info) << "Display helper: another session is active; joining its existing capture target without display changes.";
        }
        return;
      }

      if (has_app_output_override && !client_requests_virtual && !framegen_requires_virtual_display) {
        request_virtual_display = false;
        if (!launch_session->virtual_display_mode_override) {
          launch_session->virtual_display_mode_override = config::video_t::virtual_display_mode_e::disabled;
        }
      }

      if (!allow_display_changes) {
        if (request_virtual_display) {
          if (!reserve_normal_vdd_identity()) return;
          if (auto existing_device =
                VDISPLAY::resolveActiveVirtualDisplayDeviceIdForStableId(
                  virtual_display_stable_id,
                  launch_session->virtual_display_device_id,
                  launch_session->client_name,
                  VDISPLAY::policy::allow_generic_resume_fallback()
                )) {
            if (VDISPLAY::configuredRenderAdapterMatchesVirtualDisplay(
                  virtual_display_stable_guid,
                  "RTSP resume virtual display reuse"
                )) {
              launch_session->virtual_display = true;
              launch_session->virtual_display_failed = false;
              launch_session->virtual_display_device_id = *existing_device;
              launch_session->virtual_display_ready_since = std::chrono::steady_clock::now();
              launch_session->virtual_display_hdr_enabled.reset();
              launch_session->virtual_display_needs_resume_apply = true;
              config::set_runtime_output_name_override(*existing_device);
              pending_output_override = *existing_device;
              apply_framegen_refresh_policy(true);
              BOOST_LOG(info) << "Display helper: preserving virtual display capture target for resume (device_id="
                              << *existing_device << ").";
              BOOST_LOG(debug) << "Display helper: preserving capture target and refreshing display state for resume.";
              return;
            }

            launch_session->virtual_display = false;
            launch_session->virtual_display_failed = true;
            launch_session->virtual_display_device_id.clear();
            launch_session->virtual_display_ready_since.reset();
            launch_session->virtual_display_hdr_enabled.reset();
            BOOST_LOG(warning) << "Display helper: existing resume virtual display is on a different or unknown adapter; recreating it on demand.";
          }

          BOOST_LOG(info) << "Display helper: resume requested virtual display capture but no reusable virtual display was found;"
                          << " recreating one on demand.";
          launch_session->virtual_display_recreated_on_demand = true;
        } else {
          if (app_output_override) {
            config::set_runtime_output_name_override(*app_output_override);
            pending_output_override = *app_output_override;
            apply_framegen_refresh_policy(false);
            BOOST_LOG(info) << "Display helper: preserving output override for resume: "
                            << (app_output_override->empty() ? "primary display" : *app_output_override);
          } else {
            BOOST_LOG(debug) << "Display helper: skipping virtual display changes for resume.";
          }
          return;
        }
      }

      // Snapshot current display state BEFORE any display enumeration.
      // queryDisplayConfig(QueryType::All) in output_exists() and other calls can activate
      // external dummy plugs, which would pollute the snapshot used for session restore.
      if (no_active_sessions) {
        if (!display_helper_integration::snapshot_current_display_state(
              display_startup_cancelled,
              display_startup_deadline)) {
          BOOST_LOG(warning) << "Display helper snapshot before session start was not accepted.";
        }
      }

      if (app_output_override) {
        config::set_runtime_output_name_override(*app_output_override);
        pending_output_override = *app_output_override;
        BOOST_LOG(info) << "App-specific display override requested: output_name="
                        << (app_output_override->empty() ? "primary display" : *app_output_override);
      }
      BOOST_LOG(debug) << "config_requests_virtual: " << config_requests_virtual;
      BOOST_LOG(debug) << "client_requests_virtual: " << client_requests_virtual;
      BOOST_LOG(debug) << "session_requests_virtual: " << session_requests_virtual;
      BOOST_LOG(debug) << "framegen_requires_virtual_display: " << framegen_requires_virtual_display;
      BOOST_LOG(debug) << "request_virtual_display: " << request_virtual_display;
      const auto requested_output_name = config::get_active_output_name();
      if (!request_virtual_display && !requested_output_name.empty()) {
        if (!display_device::output_exists(requested_output_name)) {
          BOOST_LOG(warning) << "Requested display '" << requested_output_name
                             << "' not found; initializing virtual display instead.";
          if (!has_app_output_override) {
            request_virtual_display = true;
          }
        } else if (!display_device::output_is_active(requested_output_name)) {
          // The output exists but is currently inactive (no \\\\.\\DISPLAY# assigned). If we cannot
          // run the display helper to activate it (no signed-in user session), fall back to a
          // virtual display so capture can still start.
          if (no_active_sessions && !display_helper_session_available()) {
            BOOST_LOG(warning) << "Requested display '" << requested_output_name
                               << "' is present but inactive and cannot be activated without a signed-in user; initializing virtual display instead.";
            if (!has_app_output_override) {
              request_virtual_display = true;
            }
          } else {
            BOOST_LOG(info) << "Requested display '" << requested_output_name
                            << "' is present but inactive; attempting activation via display helper.";
          }
        }
      }

      auto apply_virtual_display_request = [&](bool should_request_virtual_display) {
        if (!should_request_virtual_display) {
          launch_session->virtual_display = false;
          launch_session->virtual_display_failed = false;
          launch_session->virtual_display_guid_bytes.fill(0);
          launch_session->virtual_display_device_id.clear();
          launch_session->virtual_display_ready_since.reset();
          launch_session->virtual_display_hdr_enabled.reset();
          return;
        }

        if (!reserve_normal_vdd_identity()) return;

        const auto intended_adapter = platf::resolve_preferred_render_adapter(
          config::video.adapter_name,
          config::video.adapter_pnp_id
        );
        if (intended_adapter) {
          pending_adapter_hint =
            video::set_pending_virtual_display_adapter_hint(*intended_adapter.luid);
        } else {
          BOOST_LOG(warning)
            << "Cannot publish the pending virtual-display adapter identity before creation (status="
            << platf::adapter_resolution_status_name(intended_adapter.status) << ").";
        }

        if (proc::vDisplayDriverStatus.load(std::memory_order_acquire) != VDISPLAY::DRIVER_STATUS::OK) {
          proc::initVDisplayDriver();
          const auto driver_status = proc::vDisplayDriverStatus.load(std::memory_order_acquire);
          if (driver_status != VDISPLAY::DRIVER_STATUS::OK) {
            BOOST_LOG(warning) << "SudaVDA driver unavailable (status=" << static_cast<int>(driver_status) << "). Continuing with best-effort virtual display creation.";
          }
        }
        auto parse_uuid = [](const std::string &value) -> std::optional<uuid_util::uuid_t> {
          if (value.empty()) {
            return std::nullopt;
          }
          try {
            return uuid_util::uuid_t::parse(value);
          } catch (...) {
            return std::nullopt;
          }
        };

        auto ensure_shared_guid = [&]() -> uuid_util::uuid_t {
          if (!http::shared_virtual_display_guid.empty()) {
            if (auto parsed = parse_uuid(http::shared_virtual_display_guid)) {
              return *parsed;
            }
          }
          auto generated = VDISPLAY::persistentVirtualDisplayUuid();
          http::shared_virtual_display_guid = generated.string();
          nvhttp::save_state();
          return generated;
        };

        const bool shared_mode = shared_virtual_display_mode;
        uuid_util::uuid_t session_uuid;
        if (shared_mode) {
          session_uuid = ensure_shared_guid();
          launch_session->unique_id = session_uuid.string();
        } else if (auto parsed = parse_uuid(launch_session->unique_id)) {
          session_uuid = *parsed;
        } else {
          session_uuid = VDISPLAY::persistentVirtualDisplayUuid();
          launch_session->unique_id = session_uuid.string();
        }

        std::string display_uuid_source;
        if (!shared_mode && !launch_session->client_uuid.empty()) {
          display_uuid_source = launch_session->client_uuid;
          BOOST_LOG(debug) << "Using client UUID for virtual display: " << display_uuid_source;
        } else {
          display_uuid_source = session_uuid.string();
          BOOST_LOG(debug) << "Using session UUID for virtual display: " << display_uuid_source;
        }

        GUID virtual_display_guid {};
        if (!shared_mode && !launch_session->client_uuid.empty()) {
          const auto client_virtual_display_uuid = VDISPLAY::virtualDisplayUuidFromStableId(launch_session->client_uuid);
          std::memcpy(&virtual_display_guid, client_virtual_display_uuid.b8, sizeof(virtual_display_guid));
          std::copy_n(
            std::cbegin(client_virtual_display_uuid.b8),
            sizeof(client_virtual_display_uuid.b8),
            launch_session->virtual_display_guid_bytes.begin()
          );
        } else {
          std::memcpy(&virtual_display_guid, session_uuid.b8, sizeof(virtual_display_guid));
          std::copy_n(std::cbegin(session_uuid.b8), sizeof(session_uuid.b8), launch_session->virtual_display_guid_bytes.begin());
        }

        uint32_t vd_width = launch_session->resolution_override ?
                              static_cast<uint32_t>(launch_session->resolution_override->width) :
                              (launch_session->width > 0 ? static_cast<uint32_t>(launch_session->width) : 1920u);
        uint32_t vd_height = launch_session->resolution_override ?
                               static_cast<uint32_t>(launch_session->resolution_override->height) :
                               (launch_session->height > 0 ? static_cast<uint32_t>(launch_session->height) : 1080u);
        // Virtual-display creation may eagerly enable HDR. Default to no state change so
        // "Do not change HDR" preserves the retained Windows setting.
        bool virtual_display_hdr_requested = false;
        display_helper_integration::helpers::SessionDisplayConfigurationHelper initial_display_helper(config::video, *launch_session, true);
        if (auto initial_configuration = initial_display_helper.initial_virtual_display_configuration()) {
          if (initial_configuration->m_resolution &&
              initial_configuration->m_resolution->m_width > 0 &&
              initial_configuration->m_resolution->m_height > 0) {
            vd_width = initial_configuration->m_resolution->m_width;
            vd_height = initial_configuration->m_resolution->m_height;
            BOOST_LOG(info) << "Virtual display initial resolution resolved from display configuration: "
                            << vd_width << 'x' << vd_height;
          }
          if (initial_configuration->m_hdr_state) {
            const bool source_hdr_requested =
              *initial_configuration->m_hdr_state == display_device::HdrState::Enabled;
            if (source_hdr_requested != virtual_display_hdr_requested) {
              BOOST_LOG(info) << "Virtual display creation HDR state aligned with source-display policy: "
                              << (source_hdr_requested ? "enabled" : "disabled") << '.';
            }
            virtual_display_hdr_requested = source_hdr_requested;
          }
        }
        const uint32_t base_vd_fps_millihz = launch_session->client_display_refresh_millihz > 0 ?
                                                   launch_session->client_display_refresh_millihz :
                                                   (launch_session->fps > 0 ?
                                                      framegen::saturating_refresh_millihz(static_cast<uint32_t>(launch_session->fps), 1000) :
                                                      0u);
        uint32_t vd_fps = rtsp_stream::effective_display_refresh_millihz(*launch_session);
        if (vd_fps == 0) {
          vd_fps = 60000u;
        }
        const bool framegen_refresh_active =
          (launch_session->framegen_refresh_millihz && *launch_session->framegen_refresh_millihz > 0) ||
          (launch_session->framegen_refresh_rate && *launch_session->framegen_refresh_rate > 0);
        const int refresh_multiplier =
          framegen_refresh_active ? rtsp_stream::framegen_refresh_multiplier(*launch_session) : 1;
        if (base_vd_fps_millihz > 0 && refresh_multiplier > 1) {
          const uint64_t minimum = static_cast<uint64_t>(base_vd_fps_millihz) * static_cast<uint64_t>(refresh_multiplier);
          vd_fps = std::max(vd_fps, static_cast<uint32_t>(std::min<uint64_t>(minimum, std::numeric_limits<uint32_t>::max())));
        }

        std::string client_label;
        if (shared_mode) {
          client_label = config::nvhttp.sunshine_name.empty() ? "Sunshine Shared Display" : config::nvhttp.sunshine_name + " Shared";
        } else {
          if (!launch_session->client_name.empty()) {
            client_label = launch_session->client_name;
          } else if (!launch_session->device_name.empty()) {
            client_label = launch_session->device_name;
          } else {
            client_label = config::nvhttp.sunshine_name;
          }
          if (client_label.empty()) {
            client_label = "Sunshine";
          }
        }

        const auto desired_layout = launch_session->virtual_display_layout_override.value_or(config::video.virtual_display_layout);
        const bool wants_extended_layout = desired_layout != config::video_t::virtual_display_layout_e::exclusive;
        if (wants_extended_layout) {
          // HTTP capability discovery can retain a temporary virtual display. The
          // stream creation path removes that probe before it creates the session
          // display, so never carry any existing virtual identity into the stream
          // baseline. The request helper adds exactly the new session display.
          auto topology_snapshot = display_helper_integration::capture_physical_topology();
          if (topology_snapshot) {
            launch_session->virtual_display_topology_snapshot = *topology_snapshot;
          } else {
            launch_session->virtual_display_topology_snapshot.reset();
          }

          // Capture physical monitor refresh rates before VD creation so they can be
          // restored after the virtual display is configured (VD creation at (0,0) can
          // cause Windows to reset other monitors' refresh rates).
          if (auto pre_vd_devices = display_helper_integration::enumerate_devices()) {
            std::map<std::string, std::pair<unsigned int, unsigned int>> rates;
            for (const auto &device : *pre_vd_devices) {
              if (device.m_device_id.empty() || !device.m_info ||
                  VDISPLAY::is_virtual_display_output(device.m_device_id)) {
                continue;
              }
              if (const auto *rat = std::get_if<display_device::Rational>(&device.m_info->m_refresh_rate)) {
                rates[device.m_device_id] = {rat->m_numerator, rat->m_denominator};
              } else if (const auto *dbl = std::get_if<double>(&device.m_info->m_refresh_rate)) {
                auto num = static_cast<unsigned int>(std::round(*dbl * 1000));
                rates[device.m_device_id] = {num, 1000u};
              }
            }
            if (!rates.empty()) {
              launch_session->pre_virtual_display_refresh_rates = std::move(rates);
            }
          }
        } else {
          launch_session->virtual_display_topology_snapshot.reset();
        }

        VDISPLAY::setWatchdogFeedingEnabled(true);
        auto display_info = VDISPLAY::createVirtualDisplay(
          display_uuid_source.c_str(),
          client_label.c_str(),
          launch_session->hdr_profile ? launch_session->hdr_profile->c_str() : nullptr,
          vd_width,
          vd_height,
          vd_fps,
          virtual_display_guid,
          base_vd_fps_millihz,
          framegen_refresh_active,
          refresh_multiplier,
          virtual_display_hdr_requested,
          false,
          !shared_mode,
          !remote_display_topology::instance().protected_remote_monitor_client_ids().empty()
        );

        if (display_info) {
          launch_session->virtual_display = true;
          launch_session->virtual_display_failed = false;
          if (display_info->device_id && !display_info->device_id->empty()) {
            launch_session->virtual_display_device_id = *display_info->device_id;
          } else if (auto resolved_device = VDISPLAY::resolveActiveVirtualDisplayDeviceIdForStableId(
                       display_uuid_source,
                       launch_session->virtual_display_device_id,
                       client_label,
                       VDISPLAY::policy::allow_generic_resume_fallback()
                     )) {
            launch_session->virtual_display_device_id = *resolved_device;
          } else {
            launch_session->virtual_display_device_id.clear();
          }
          if (!launch_session->virtual_display_device_id.empty()) {
            config::set_runtime_output_name_override(launch_session->virtual_display_device_id);
            pending_output_override = launch_session->virtual_display_device_id;
            if (pending_adapter_hint) {
              (void) video::mark_pending_virtual_display_adapter_hint_ready_for_verification(
                *pending_adapter_hint
              );
            }
          }
          launch_session->virtual_display_ready_since = display_info->ready_since;
          launch_session->virtual_display_hdr_enabled = display_info->hdr_enabled;
          if (display_info->display_name && !display_info->display_name->empty()) {
            BOOST_LOG(info) << "Virtual display created at " << platf::to_utf8(*display_info->display_name);
          } else {
            BOOST_LOG(info) << "Virtual display created (device name pending enumeration).";
          }

          VDISPLAY::VirtualDisplayRecoveryParams recovery_params;
          recovery_params.guid = virtual_display_guid;
          recovery_params.width = vd_width;
          recovery_params.height = vd_height;
          recovery_params.fps = vd_fps;
          recovery_params.base_fps_millihz = base_vd_fps_millihz;
          recovery_params.framegen_refresh_active = framegen_refresh_active;
          recovery_params.framegen_refresh_multiplier = refresh_multiplier;
          recovery_params.hdr_requested = virtual_display_hdr_requested;
          recovery_params.client_uid = display_uuid_source;
          recovery_params.client_name = client_label;
          recovery_params.hdr_profile = launch_session->hdr_profile;
          recovery_params.display_name = display_info->display_name;
          recovery_params.monitor_device_path = display_info->monitor_device_path;
          recovery_params.confirmed_active_at_schedule = display_info->confirmed_active;
          if (display_info->device_id && !display_info->device_id->empty()) {
            recovery_params.device_id = *display_info->device_id;
          } else if (!launch_session->virtual_display_device_id.empty()) {
            recovery_params.device_id = launch_session->virtual_display_device_id;
          }
          recovery_params.max_attempts = 3;

          GUID recovery_guid = virtual_display_guid;
          recovery_params.should_abort = [recovery_guid]() {
            return !VDISPLAY::is_virtual_display_guid_tracked(recovery_guid);
          };
          auto recovery_session = std::make_shared<rtsp_stream::launch_session_t>(
            display_helper_integration::helpers::make_display_request_session_snapshot(*launch_session)
          );
          recovery_params.on_recovery_success = [recovery_session](const VDISPLAY::VirtualDisplayCreationResult &result, std::stop_token stop_token) -> std::function<void()> {
              const auto cancelled = [&] {
                return stop_token.stop_requested();
              };
              std::optional<config::runtime_output_override_lease_t> recovery_output_override_lease;
              auto clear_recovery_output_override = util::fail_guard([&] {
                if (recovery_output_override_lease) {
                  (void) config::clear_runtime_output_name_override_if_lease(*recovery_output_override_lease);
                }
              });
              const auto wait_or_cancel = [&](std::chrono::milliseconds delay) {
                const auto deadline = std::chrono::steady_clock::now() + delay;
                while (!cancelled()) {
                  const auto now = std::chrono::steady_clock::now();
                  if (now >= deadline) {
                    return false;
                  }
                  const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now);
                  std::this_thread::sleep_for(std::min(std::max(remaining, std::chrono::milliseconds(1)), std::chrono::milliseconds(50)));
                }
                return true;
              };

              if (cancelled()) {
                return {};
              }
              if (result.device_id && !result.device_id->empty()) {
                recovery_session->virtual_display_device_id = *result.device_id;
                if (cancelled()) {
                  return {};
                }
                recovery_output_override_lease = config::set_runtime_output_name_override_with_lease(
                  recovery_session->virtual_display_device_id
                );
              }
              if (cancelled()) {
                return {};
              }
              recovery_session->virtual_display_ready_since = result.ready_since;
              recovery_session->virtual_display_hdr_enabled = result.hdr_enabled;
              if (recovery_session->virtual_display) {
                constexpr int kMaxApplyAttempts = 5;
                bool applied = false;

                for (int attempt = 1; attempt <= kMaxApplyAttempts; ++attempt) {
                  if (cancelled()) {
                    return {};
                  }
                  (void) display_helper_integration::disarm_pending_restore(cancelled);
                  if (cancelled()) {
                    return {};
                  }

                  auto request = display_helper_integration::helpers::build_request_from_session(config::video, *recovery_session);
                  if (!request) {
                    BOOST_LOG(warning) << "Virtual display recovery: failed to rebuild display helper request after recreation (attempt "
                                       << attempt << "/" << kMaxApplyAttempts << ").";
                    if (wait_or_cancel(std::chrono::milliseconds(250 + (attempt - 1) * 250))) {
                      return {};
                    }
                    continue;
                  }

                  if (cancelled()) {
                    return {};
                  }
                  // This recovery worker is torn down with the session, so it
                  // keeps the short shutdown-class helper IPC timeouts.
                  if (display_helper_integration::apply(
                        *request,
                        nullptr,
                        cancelled,
                        display_helper_integration::ApplyRetryPolicy::Full,
                        {},
                        true)) {
                    BOOST_LOG(info) << "Virtual display recovery: re-applied session display configuration (including exclusivity) after recreation.";
                    applied = true;
                    break;
                  }
                  if (cancelled()) {
                    return {};
                  }

                  BOOST_LOG(warning) << "Virtual display recovery: display helper apply failed after recreation (attempt "
                                     << attempt << "/" << kMaxApplyAttempts << ").";
                  if (wait_or_cancel(std::chrono::milliseconds(250 + (attempt - 1) * 250))) {
                    return {};
                  }
                }

                if (!cancelled() && mail::man) {
                  mail::man->event<int>(mail::switch_display)->raise(-1);
                }
                if (cancelled()) {
                  return {};
                }
                BOOST_LOG(info) << "Virtual display recovery: requested capture reinit to pick up recreated display"
                                << (applied ? "." : " (apply did not succeed).");
              }
              std::function<void()> rollback_output_override;
              if (recovery_output_override_lease) {
                const auto lease = *recovery_output_override_lease;
                rollback_output_override = [lease] {
                  (void) config::clear_runtime_output_name_override_if_lease(lease);
                };
              }
              clear_recovery_output_override.disable();
              return rollback_output_override;
          };

          VDISPLAY::schedule_virtual_display_recovery_monitor(recovery_params);
        } else {
          launch_session->virtual_display = false;
          launch_session->virtual_display_failed = true;
          launch_session->virtual_display_guid_bytes.fill(0);
          launch_session->virtual_display_device_id.clear();
          launch_session->virtual_display_ready_since.reset();
          launch_session->virtual_display_hdr_enabled.reset();
          launch_session->framegen_refresh_rate.reset();
          launch_session->framegen_refresh_millihz.reset();
          launch_session->framegen_refresh_multiplier = 1;
          BOOST_LOG(warning) << "Virtual display creation failed.";
        }
      };

      if (!request_virtual_display && VDISPLAY::should_auto_enable_virtual_display()) {
        BOOST_LOG(info) << "No physical monitors detected. Automatically enabling virtual display.";
        request_virtual_display = true;
      }

      apply_framegen_refresh_policy(request_virtual_display);

      if (request_virtual_display) {
        // A new virtual-display session supersedes the prior session's restore.
        // Disarm it before any driver mutation; checking first used to return
        // early and made the DISARM below unreachable in the exact race it was
        // intended to prevent.
        const bool virtual_display_mutation_allowed =
          display_helper_integration::request_policy::supersede_restore_for_virtual_display(
            [&] {
              (void) display_helper_integration::disarm_pending_restore(
                display_startup_cancelled,
                display_startup_deadline
              );
            },
            [&] {
              return display_helper_integration::restore_in_progress(
                display_startup_cancelled,
                display_startup_deadline
              );
            }
          );
        if (!virtual_display_mutation_allowed) {
          BOOST_LOG(warning) << "Display helper: virtual display creation deferred because physical display restoration is still in progress; using physical fallback for this session.";
          launch_session->virtual_display = false;
          launch_session->virtual_display_failed = true;
          launch_session->virtual_display_guid_bytes.fill(0);
          launch_session->virtual_display_device_id.clear();
          launch_session->virtual_display_ready_since.reset();
          launch_session->virtual_display_hdr_enabled.reset();
          apply_framegen_refresh_policy(false);
          return;
        }
      }

      apply_virtual_display_request(request_virtual_display);
    }
  }  // namespace
#endif

#ifdef __linux__
  namespace {
    void cleanup_virtual_display_if_idle_locked() {
      try {
        if (has_stream_session_activity()) {
          BOOST_LOG(info) << "Skipping Linux private-display cleanup because a streaming session is active or stopping.";
          return;
        }
        if (!remote_display_topology::instance().generic_virtual_display_cleanup_allowed()) {
          BOOST_LOG(info) << "Deferring Linux private-display cleanup until the remaining managed client identities release ownership.";
          return;
        }
        if (stream::session::finalize_shared_runtime_if_idle("managed_display_owner_release")) {
          return;
        }
        (void) platf::linux_private_display::revert();
      } catch (const std::exception &error) {
        BOOST_LOG(warning) << "Linux private-display cleanup failed: " << error.what();
      } catch (...) {
        BOOST_LOG(warning) << "Linux private-display cleanup failed with an unknown exception.";
      }
    }

    void cleanup_virtual_display_if_idle() {
      std::unique_lock<std::mutex> lifecycle_lock(stream_lifecycle_mutex());
      cleanup_virtual_display_if_idle_locked();
    }
  }  // namespace
#endif

#ifndef _WIN32
  namespace {
    bool has_stream_session_activity_for_http_probe() {
      return rtsp_stream::has_pending_launch_or_startup() ||
             rtsp_stream::session_count_no_cleanup() > 0 ||
             stream::session::running_sessions.load(std::memory_order_acquire) != 0 ||
             stream::session::teardown_sessions.load(std::memory_order_acquire) != 0 ||
             webrtc_stream::has_active_or_pending_sessions() ||
             webrtc_stream::has_capture_active() ||
             webrtc_stream::has_teardown_in_progress();
    }

    http_encoder_capabilities_t advertised_encoder_capabilities_for_http() {
      const auto publish = [](video::advertised_encoder_capabilities_t caps, const std::string_view reason) {
        const bool probe_complete = video::has_successful_encoder_probe();
        BOOST_LOG(debug)
          << "HTTP encoder capabilities: probe_complete=" << probe_complete
          << ", hdr="
          << (caps.hevc_mode == 3 || caps.av1_mode == 3)
          << ", hevc_mode=" << caps.hevc_mode
          << ", av1_mode=" << caps.av1_mode
          << ", source=" << reason << '.';
        return http_encoder_capabilities_t {
          .advertised = std::move(caps),
          .probe_complete = probe_complete,
        };
      };

      if (video::has_successful_encoder_probe()) {
        return publish(video::advertised_encoder_capabilities(false), "matching-cache");
      }

      std::unique_lock<std::mutex> lifecycle_lock(stream_lifecycle_mutex(), std::try_to_lock);
      if (!lifecycle_lock.owns_lock()) {
        BOOST_LOG(debug) << "Skipping HTTP encoder capability probe while stream lifecycle work owns the gate.";
        return publish(video::advertised_encoder_capabilities(false), "lifecycle-gate");
      }
      if (video::has_successful_encoder_probe()) {
        return publish(video::advertised_encoder_capabilities(false), "matching-cache-after-gate");
      }
      if (has_stream_session_activity_for_http_probe()) {
        BOOST_LOG(debug) << "Skipping HTTP encoder capability probe while a streaming session is active or stopping.";
        return publish(video::advertised_encoder_capabilities(false), "active-or-stopping-session");
      }

      return publish(video::advertised_encoder_capabilities(true), "idle-probe");
    }
  }  // namespace
#endif

  web_stream_capabilities_t get_web_stream_capabilities() {
    const auto snapshot = advertised_encoder_capabilities_for_http();
    const auto &caps = snapshot.advertised;
    const bool probe_complete = snapshot.probe_complete;
    return {
      .probe_complete = probe_complete,
      .h264 = probe_complete,
      .hevc = probe_complete && caps.hevc_mode >= 2,
      .av1 = probe_complete && caps.av1_mode >= 2,
      .hevc_hdr = probe_complete && caps.hevc_mode >= 3,
      .av1_hdr = probe_complete && caps.av1_mode >= 3,
    };
  }

  struct named_cert_t {
    std::string name;
    std::string uuid;
    std::string cert;
    std::string hdr_profile;
    std::string display_mode;
    std::string output_name_override;
    std::string virtual_display_mode_override;
    std::string virtual_display_layout_override;
    bool always_use_virtual_display = false;
    bool enabled = true;
    bool prefer_10bit_sdr = false;
    std::optional<std::int64_t> last_seen;
    std::unordered_map<std::string, std::string> config_overrides;
  };

  namespace {
    std::int64_t now_seconds() {
      return std::chrono::duration_cast<std::chrono::seconds>(
               std::chrono::system_clock::now().time_since_epoch()
      )
        .count();
    }
  }  // namespace

  struct client_t {
    std::vector<named_cert_t> named_devices;
    std::string remote_display_layout_json {R"({"version":1,"placements":{}})"};
  };

  // uniqueID, session
  std::unordered_map<std::string, pair_session_t> map_id_sess;
  std::mutex pairing_sessions_mutex;
  constexpr auto pairing_session_expiry = std::chrono::minutes(10);
  client_t client_root;
  std::mutex client_mutex;
  // This is deliberately separate from client_root: an unreadable or
  // semantically invalid state file must never turn into an empty in-memory
  // database that a later metadata save can persist over the real one.
  std::atomic_bool authorization_state_ready {false};
  std::atomic<uint32_t> session_id_counter;

  using args_t = SimpleWeb::CaseInsensitiveMultimap;
  using resp_https_t = std::shared_ptr<typename SimpleWeb::ServerBase<SunshineHTTPS>::Response>;
  using req_https_t = std::shared_ptr<typename SimpleWeb::ServerBase<SunshineHTTPS>::Request>;
  using resp_http_t = std::shared_ptr<typename SimpleWeb::ServerBase<SimpleWeb::HTTP>::Response>;
  using req_http_t = std::shared_ptr<typename SimpleWeb::ServerBase<SimpleWeb::HTTP>::Request>;

  enum class op_e {
    ADD,  ///< Add certificate
    REMOVE  ///< Remove certificate
  };

  client_t client_root_snapshot() {
    std::lock_guard<std::mutex> lock(client_mutex);
    return client_root;
  }

  std::optional<std::string> exact_certificate_identity(const crypto::x509_t &certificate) {
    if (!certificate) {
      return std::nullopt;
    }
    const auto encoded_length = i2d_X509(certificate.get(), nullptr);
    if (encoded_length <= 0 || static_cast<std::size_t>(encoded_length) > pairing_policy::max_paired_certificate_length) {
      return std::nullopt;
    }
    std::string identity(static_cast<std::size_t>(encoded_length), '\0');
    auto *cursor = reinterpret_cast<unsigned char *>(identity.data());
    if (i2d_X509(certificate.get(), &cursor) != encoded_length) {
      return std::nullopt;
    }
    return identity;
  }

  bool build_paired_client_records(
    const client_t &client,
    std::vector<std::string> &certificate_identities,
    std::vector<pairing_policy::paired_client_record_view_t> &records,
    std::vector<crypto::x509_t> *parsed_certificates = nullptr
  ) {
    certificate_identities.clear();
    records.clear();
    if (client.named_devices.size() > pairing_policy::max_paired_clients) {
      return false;
    }
    certificate_identities.reserve(client.named_devices.size());
    records.reserve(client.named_devices.size());
    if (parsed_certificates) {
      parsed_certificates->clear();
      parsed_certificates->reserve(client.named_devices.size());
    }

    for (const auto &named_cert : client.named_devices) {
      if (named_cert.name.size() > pairing_policy::max_paired_client_name_length || named_cert.cert.empty() || named_cert.cert.size() > pairing_policy::max_paired_certificate_length) {
        return false;
      }
      auto certificate = crypto::x509(named_cert.cert);
      auto identity = exact_certificate_identity(certificate);
      if (!identity) {
        return false;
      }
      certificate_identities.emplace_back(std::move(*identity));
      records.push_back({named_cert.uuid, certificate_identities.back(), named_cert.enabled});
      if (parsed_certificates) {
        parsed_certificates->emplace_back(std::move(certificate));
      }
    }
    return pairing_policy::paired_client_state_valid(records);
  }

  void clear_tls_client_identities();

  std::string get_arg(const args_t &args, const char *name, const char *default_value = nullptr) {
    auto it = args.find(name);
    if (it == std::end(args)) {
      if (default_value != nullptr) {
        return std::string(default_value);
      }

      throw std::out_of_range(name);
    }
    return it->second;
  }

  bool save_state_snapshot_locked(const client_t &client, const bool allow_missing_state = false) {
    // The caller owns statefile::state_mutex(). Authorization-expanding
    // callers also keep client_mutex locked until this write succeeds or their
    // in-memory mutation is rolled back.
    if (!authorization_state_ready.load(std::memory_order_acquire)) {
      BOOST_LOG(error) << "Refusing to save pairing state because no valid state snapshot is loaded."sv;
      return false;
    }

    const auto &sunshine_path = statefile::sunshine_state_path();
    const auto &vibeshine_path = statefile::vibeshine_state_path();

    pt::ptree root;

    const auto primary_result = statefile::load_json(sunshine_path, root);
    if (primary_result != statefile::json_load_result_e::loaded) {
      pt::ptree backup_root;
      const auto backup_result = statefile::load_json(statefile::sunshine_state_backup_path(), backup_root);
      if (backup_result == statefile::json_load_result_e::loaded) {
        BOOST_LOG(warning) << "Using the Sunshine state recovery copy while saving "sv << sunshine_path;
        root = std::move(backup_root);
      } else if (allow_missing_state &&
                 primary_result == statefile::json_load_result_e::missing &&
                 backup_result == statefile::json_load_result_e::missing) {
        // Only first-run initialization may create a state tree with no
        // existing snapshot. Runtime saves must never replace an unavailable
        // state file with a partial tree.
        root = {};
      } else {
        BOOST_LOG(error) << "Refusing to replace unavailable Sunshine state "sv << sunshine_path
                         << " while saving pairing data."sv;
        return false;
      }
    }

    pt::ptree root_node;
    if (auto existing_root = root.get_child_optional("root")) {
      root_node = *existing_root;
    }

    root_node.put("uniqueid", http::unique_id);
    root_node.put("remote_display_layout", client.remote_display_layout_json);

    pt::ptree named_cert_nodes;
    for (const auto &named_cert : client.named_devices) {
      pt::ptree named_cert_node;
      named_cert_node.put("name"s, named_cert.name);
      named_cert_node.put("cert"s, named_cert.cert);
      named_cert_node.put("uuid"s, named_cert.uuid);
      named_cert_node.put("enabled"s, named_cert.enabled);
      if (!named_cert.hdr_profile.empty()) {
        named_cert_node.put("hdr_profile"s, named_cert.hdr_profile);
      }
      if (!named_cert.display_mode.empty()) {
        named_cert_node.put("display_mode"s, named_cert.display_mode);
      }
      if (!named_cert.output_name_override.empty()) {
        named_cert_node.put("output_name_override"s, named_cert.output_name_override);
      }
      if (!named_cert.virtual_display_mode_override.empty()) {
        named_cert_node.put("virtual_display_mode"s, named_cert.virtual_display_mode_override);
      }
      if (!named_cert.virtual_display_layout_override.empty()) {
        named_cert_node.put("virtual_display_layout"s, named_cert.virtual_display_layout_override);
      }
      if (named_cert.always_use_virtual_display) {
        named_cert_node.put("always_use_virtual_display"s, true);
      }
      if (named_cert.prefer_10bit_sdr) {
        named_cert_node.put("prefer_10bit_sdr"s, true);
      }
      if (named_cert.last_seen.has_value()) {
        named_cert_node.put("last_seen"s, *named_cert.last_seen);
      }
      if (!named_cert.config_overrides.empty()) {
        pt::ptree overrides_node;
        for (const auto &[k, v] : named_cert.config_overrides) {
          overrides_node.put(k, v);
        }
        named_cert_node.put_child("config_overrides", overrides_node);
      }
      named_cert_nodes.push_back(std::make_pair(""s, named_cert_node));
    }
    root_node.put_child("named_devices", named_cert_nodes);
    root.put_child("root", root_node);

    try {
      statefile::write_sunshine_state_atomic(root);
    } catch (std::exception &e) {
      BOOST_LOG(error) << "Couldn't write "sv << sunshine_path << ": "sv << e.what();
      return false;
    }

    if (!vibeshine_path.empty()) {
      auto ensure_root = [](pt::ptree &tree) -> pt::ptree & {
        auto it = tree.find("root");
        if (it == tree.not_found()) {
          auto inserted = tree.insert(tree.end(), std::make_pair(std::string("root"), pt::ptree {}));
          return inserted->second;
        }
        return it->second;
      };

      pt::ptree vibeshine_tree;
      if (!statefile::load_json_for_update(vibeshine_path, vibeshine_tree)) {
        return true;
      }

      auto &vibe_root = ensure_root(vibeshine_tree);
      vibe_root.put("last_notified_version", update::state.last_notified_version);

#ifdef _WIN32
      if (!http::shared_virtual_display_guid.empty()) {
        vibe_root.put("shared_virtual_display_guid", http::shared_virtual_display_guid);
      }
#endif
      {
        pt::ptree last_seen_nodes;
        for (const auto &named_cert : client.named_devices) {
          if (!named_cert.last_seen.has_value()) {
            continue;
          }
          last_seen_nodes.put(named_cert.uuid, *named_cert.last_seen);
        }
        vibe_root.put_child("client_last_seen", last_seen_nodes);
      }

      try {
        statefile::write_json_atomic(vibeshine_path, vibeshine_tree);
      } catch (std::exception &e) {
        BOOST_LOG(error) << "Couldn't write "sv << vibeshine_path << ": "sv << e.what();
      }
    }
    return true;
  }

  bool save_state() {
    if (config::sunshine.flags[config::flag::FRESH_STATE]) {
      return true;
    }
    const bool fresh_state = config::sunshine.flags[config::flag::FRESH_STATE];
    if (!fresh_state) {
      statefile::migrate_recent_state_keys();
    }
    std::lock_guard<std::mutex> state_lock(statefile::state_mutex());
    // Keep every persisted snapshot ordered with load_state(): state first,
    // then client. A metadata save that began before a disable/unpair must not
    // write an older authorization snapshot after that revocation is saved.
    return save_state_snapshot_locked(client_root_snapshot());
  }

  bool load_state() {
    statefile::migrate_recent_state_keys();
    const auto &sunshine_path = statefile::sunshine_state_path();
    const auto &vibeshine_path = statefile::vibeshine_state_path();
    const auto sunshine_backup_path = statefile::sunshine_state_backup_path();

    std::lock_guard<std::mutex> state_lock(statefile::state_mutex());

    struct parsed_state_t {
      std::string unique_id;
      client_t client;
      std::vector<crypto::x509_t> certificates;
      nlohmann::json remote_display_layout;
    };

    const auto parse_state = [](const pt::ptree &tree) -> std::optional<parsed_state_t> {
      try {
        auto unique_id = tree.get_optional<std::string>("root.uniqueid");
        if (!unique_id || !pairing_policy::valid_unique_id(*unique_id)) {
          return std::nullopt;
        }

        parsed_state_t parsed;
        parsed.unique_id = std::move(*unique_id);
        if (auto root = tree.get_child_optional("root")) {
          // Import from old format.
          if (auto device_nodes = root->get_child_optional("devices")) {
            for (auto &[_, device_node] : *device_nodes) {
              if (device_node.count("certs")) {
                for (auto &[_, element] : device_node.get_child("certs")) {
                  if (parsed.client.named_devices.size() >= pairing_policy::max_paired_clients) {
                    throw std::length_error("too many legacy paired clients");
                  }
                  named_cert_t named_cert;
                  named_cert.name = ""s;
                  named_cert.cert = element.get_value<std::string>();
                  named_cert.uuid = uuid_util::uuid_t::generate().string();
                  parsed.client.named_devices.emplace_back(std::move(named_cert));
                }
              }
            }
          }

          if (root->count("named_devices")) {
            for (auto &[_, element] : root->get_child("named_devices")) {
              if (parsed.client.named_devices.size() >= pairing_policy::max_paired_clients) {
                throw std::length_error("too many paired clients");
              }
              named_cert_t named_cert;
              named_cert.name = element.get_child("name").get_value<std::string>();
              named_cert.cert = element.get_child("cert").get_value<std::string>();
              named_cert.uuid = element.get_child("uuid").get_value<std::string>();
              named_cert.hdr_profile = element.get<std::string>("hdr_profile", "");
              named_cert.display_mode = element.get<std::string>("display_mode", "");
              named_cert.output_name_override = element.get<std::string>("output_name_override", "");
              named_cert.virtual_display_mode_override = element.get<std::string>("virtual_display_mode", "");
              named_cert.virtual_display_layout_override = element.get<std::string>("virtual_display_layout", "");
              named_cert.always_use_virtual_display = element.get<bool>("always_use_virtual_display", false);
              named_cert.enabled = element.get<bool>("enabled", true);
              named_cert.prefer_10bit_sdr = element.get<bool>("prefer_10bit_sdr", false);
              if (auto last_seen = element.get_optional<std::int64_t>("last_seen")) {
                named_cert.last_seen = *last_seen;
              } else {
                named_cert.last_seen.reset();
              }
              if (auto overrides_node = element.get_child_optional("config_overrides")) {
                for (auto &[key, value] : *overrides_node) {
                  if (!key.empty()) {
                    named_cert.config_overrides[key] = value.get_value<std::string>();
                  }
                }
              }
              std::unordered_map<std::string, std::string> normalized_overrides;
              config::merge_config_overrides(normalized_overrides, named_cert.config_overrides);
              named_cert.config_overrides = std::move(normalized_overrides);
              parsed.client.named_devices.emplace_back(std::move(named_cert));
            }
          }
          parsed.client.remote_display_layout_json = root->get<std::string>(
            "remote_display_layout", parsed.client.remote_display_layout_json
          );
        }

        try {
          parsed.remote_display_layout = remote_display_topology::normalize_layout(
            nlohmann::json::parse(parsed.client.remote_display_layout_json)
          );
        } catch (...) {
          parsed.remote_display_layout = remote_display_topology::normalize_layout(nlohmann::json {});
        }
        parsed.client.remote_display_layout_json = parsed.remote_display_layout.dump();

        std::vector<std::string> certificate_identities;
        std::vector<pairing_policy::paired_client_record_view_t> paired_client_records;
        if (!build_paired_client_records(
              parsed.client,
              certificate_identities,
              paired_client_records,
              &parsed.certificates
            )) {
          return std::nullopt;
        }
        return parsed;
      } catch (const std::exception &e) {
        BOOST_LOG(error) << "Pairing state is malformed: "sv << e.what();
        return std::nullopt;
      }
    };

    pt::ptree tree;
    const auto primary_result = statefile::load_json(sunshine_path, tree);
    std::optional<parsed_state_t> parsed;
    bool recovered_from_backup = false;
    auto backup_result = statefile::json_load_result_e::missing;

    if (primary_result == statefile::json_load_result_e::loaded) {
      parsed = parse_state(tree);
    }

    pt::ptree backup_tree;
    if (!parsed) {
      backup_result = statefile::load_json(sunshine_backup_path, backup_tree);
      if (backup_result == statefile::json_load_result_e::loaded) {
        parsed = parse_state(backup_tree);
        recovered_from_backup = parsed.has_value();
      }
    }

    if (!parsed) {
      if (primary_result == statefile::json_load_result_e::missing &&
          backup_result == statefile::json_load_result_e::missing &&
          http::credentials_created_this_run) {
        // A genuinely new profile has no state or prior credential material.
        // Establish and persist the identity before opening the network
        // listeners, so a later restart cannot manufacture a different host
        // identity. An existing profile with both snapshots missing fails
        // closed below instead of being mistaken for first run.
        http::unique_id = uuid_util::uuid_t::generate().string();
        update::state.last_notified_version.clear();
#ifdef _WIN32
        http::shared_virtual_display_guid.clear();
#endif
        {
          std::lock_guard<std::mutex> lock(client_mutex);
          cert_chain.clear();
          client_root = client_t {};
        }
        authorization_state_ready.store(true, std::memory_order_release);
        if (!save_state_snapshot_locked(client_t {}, true)) {
          authorization_state_ready.store(false, std::memory_order_release);
          BOOST_LOG(error) << "Could not establish durable Sunshine pairing state at "sv << sunshine_path;
          return false;
        }
        return true;
      }

      BOOST_LOG(error) << "Could not load a valid Sunshine pairing state from "sv
                       << sunshine_path << " or its recovery copy " << sunshine_backup_path
                       << "; refusing to start with a new host identity."sv;
      return false;
    }

    http::unique_id = parsed->unique_id;

    if (recovered_from_backup) {
      // Restore the primary so the next process does not depend on the backup.
      // If this repair cannot be written, the in-memory state is still valid
      // and the backup remains available for the next restart.
      try {
        statefile::write_sunshine_state_atomic(backup_tree);
      } catch (const std::exception &e) {
        BOOST_LOG(error) << "Could not restore Sunshine state from recovery copy: "sv << e.what();
      }
    } else {
      // Keep the recovery copy current even when it was absent or stale. This
      // is best effort because the primary is already valid and usable.
      try {
        statefile::write_json_atomic(sunshine_backup_path, tree);
      } catch (const std::exception &e) {
        BOOST_LOG(warning) << "Could not refresh Sunshine state recovery copy: "sv << e.what();
      }
    }

    if (!vibeshine_path.empty()) {
      try {
        pt::ptree vibeshine_tree;
        if (statefile::load_json(vibeshine_path, vibeshine_tree) != statefile::json_load_result_e::loaded) {
          throw std::runtime_error("notification state is unavailable");
        }
        update::state.last_notified_version = vibeshine_tree.get("root.last_notified_version", "");
#ifdef _WIN32
        http::shared_virtual_display_guid = vibeshine_tree.get("root.shared_virtual_display_guid", "");
#endif
      } catch (const std::exception &e) {
        BOOST_LOG(warning) << "Couldn't read "sv << vibeshine_path << " for notification state: "sv << e.what();
        update::state.last_notified_version.clear();
#ifdef _WIN32
        http::shared_virtual_display_guid.clear();
#endif
      }
    } else {
      update::state.last_notified_version.clear();
#ifdef _WIN32
      http::shared_virtual_display_guid.clear();
#endif
    }

    {
      std::lock_guard<std::mutex> lock(client_mutex);
      cert_chain.clear();
      for (auto &certificate : parsed->certificates) {
        cert_chain.add(std::move(certificate));
      }
      client_root = std::move(parsed->client);
    }
    authorization_state_ready.store(true, std::memory_order_release);
    remote_display_topology::instance().set_layout(std::move(parsed->remote_display_layout));
    return true;
  }

  bool is_placeholder_client_name(const std::string &name) {
    return pairing_policy::is_placeholder_client_name(name);
  }

  std::string display_client_name_for_session(const std::string &paired_name, const std::string &device_name, const std::string &host_name) {
    return pairing_policy::display_client_name(paired_name, device_name, host_name);
  }

  bool add_authorized_client(const std::string &name, std::string &&cert) {
    auto candidate_certificate = crypto::x509(cert);
    auto candidate_identity = exact_certificate_identity(candidate_certificate);
    if (!candidate_identity) {
      return false;
    }

    named_cert_t named_cert;
    named_cert.name = display_client_name_for_session(name, std::string {}, "Moonlight Client"s);
    named_cert.cert = std::move(cert);
    named_cert.uuid = uuid_util::uuid_t::generate().string();
    named_cert.hdr_profile.clear();
    named_cert.display_mode.clear();
    named_cert.output_name_override.clear();
    named_cert.virtual_display_mode_override.clear();
    named_cert.virtual_display_layout_override.clear();
    named_cert.always_use_virtual_display = false;
    named_cert.enabled = true;
    named_cert.prefer_10bit_sdr = false;
    named_cert.last_seen.reset();
    named_cert.config_overrides.clear();
    const bool fresh_state = config::sunshine.flags[config::flag::FRESH_STATE];
    if (!fresh_state) {
      statefile::migrate_recent_state_keys();
    }
    std::lock_guard<std::mutex> state_lock(statefile::state_mutex());
    std::lock_guard<std::mutex> client_lock(client_mutex);

    if (!fresh_state && !authorization_state_ready.load(std::memory_order_acquire)) {
      BOOST_LOG(error) << "Refusing to pair a client because durable pairing state is unavailable."sv;
      return false;
    }

    std::vector<std::string> certificate_identities;
    std::vector<pairing_policy::paired_client_record_view_t> records;
    if (!build_paired_client_records(client_root, certificate_identities, records)) {
      return false;
    }

    const auto admission = pairing_policy::admit_paired_client(records, *candidate_identity);
    if (admission.status == pairing_policy::paired_client_admission_e::reject) {
      return false;
    }

    if (admission.status == pairing_policy::paired_client_admission_e::reauthorize) {
      auto &existing = client_root.named_devices[admission.index];
      if (existing.enabled) {
        return true;
      }

      existing.enabled = true;
      if (!fresh_state && !save_state_snapshot_locked(client_root)) {
        existing.enabled = false;
        return false;
      }
      BOOST_LOG(info) << "Re-authorized existing paired client UUID: " << existing.uuid;
      return true;
    }

    client_root.named_devices.emplace_back(std::move(named_cert));
    if (!build_paired_client_records(client_root, certificate_identities, records)) {
      client_root.named_devices.pop_back();
      return false;
    }

    if (!fresh_state && !save_state_snapshot_locked(client_root)) {
      // Keep the state and client locks through rollback. No request can
      // observe the new authority, and no concurrent metadata save can persist
      // it after this pairing attempt reports failure.
      client_root.named_devices.pop_back();
      return false;
    }
    return true;
  }

  struct resolved_client_identity_t {
    std::string uuid;
    std::string name;
  };

  std::optional<resolved_client_identity_t> resolve_client_identity_from_peer_cert_locked(
    const crypto::x509_t &client_cert
  ) {
    auto presented_identity = exact_certificate_identity(client_cert);
    if (!presented_identity) {
      BOOST_LOG(debug) << "No valid client certificate identity available";
      return std::nullopt;
    }

    std::vector<std::string> certificate_identities;
    std::vector<pairing_policy::paired_client_record_view_t> paired_client_records;
    if (!build_paired_client_records(client_root, certificate_identities, paired_client_records)) {
      BOOST_LOG(error) << "Refusing client authentication because pairing state is invalid or ambiguous.";
      return std::nullopt;
    }

    const auto resolution = pairing_policy::resolve_paired_client(
      paired_client_records,
      *presented_identity
    );
    switch (resolution.status) {
      case pairing_policy::paired_client_resolution_e::authorized:
        {
          const auto &named_cert = client_root.named_devices[resolution.index];
          BOOST_LOG(debug) << "Found exact enabled client UUID: " << named_cert.uuid
                           << " for client: " << named_cert.name;
          return resolved_client_identity_t {named_cert.uuid, named_cert.name};
        }
      case pairing_policy::paired_client_resolution_e::disabled:
        BOOST_LOG(warning) << "Client certificate belongs to a disabled paired device.";
        break;
      case pairing_policy::paired_client_resolution_e::invalid_state:
        BOOST_LOG(error) << "Refusing client authentication because pairing state is invalid or ambiguous.";
        break;
      case pairing_policy::paired_client_resolution_e::unknown_certificate:
        BOOST_LOG(debug) << "No exact paired client certificate found";
        break;
    }
    return std::nullopt;
  }

  constexpr std::size_t max_tls_client_identity_cache_entries =
    pairing_policy::max_paired_clients * 8;
  std::mutex tls_client_identity_mutex;
  std::unordered_map<std::string, resolved_client_identity_t> tls_client_identity_by_endpoint;

  bool paired_client_uuid_enabled_locked(const std::string_view uuid) {
    if (!pairing_policy::valid_paired_client_uuid(uuid) || client_root.named_devices.size() > pairing_policy::max_paired_clients) {
      return false;
    }

    const named_cert_t *match = nullptr;
    std::unordered_set<std::string_view> seen_uuids;
    seen_uuids.reserve(client_root.named_devices.size());
    for (const auto &client : client_root.named_devices) {
      if (!pairing_policy::valid_paired_client_uuid(client.uuid) || client.name.size() > pairing_policy::max_paired_client_name_length || client.cert.empty() || client.cert.size() > pairing_policy::max_paired_certificate_length || !seen_uuids.insert(client.uuid).second) {
        return false;
      }
      if (client.uuid == uuid) {
        match = &client;
      }
    }
    return match && match->enabled;
  }

  bool paired_client_uuid_enabled(const std::string_view uuid) {
    std::lock_guard<std::mutex> lock(client_mutex);
    return paired_client_uuid_enabled_locked(uuid);
  }

  std::string endpoint_key(const boost::asio::ip::tcp::endpoint &endpoint) {
    if (endpoint.address().is_unspecified() || endpoint.port() == 0) {
      return {};
    }

    return endpoint.address().to_string() + ":" + std::to_string(endpoint.port());
  }

  void remember_tls_client_identity(const boost::asio::ip::tcp::endpoint &endpoint, const resolved_client_identity_t &identity) {
    const auto key = endpoint_key(endpoint);
    if (key.empty() || identity.uuid.empty()) {
      return;
    }

    std::lock_guard<std::mutex> lock(tls_client_identity_mutex);
    if (!tls_client_identity_by_endpoint.contains(key) && tls_client_identity_by_endpoint.size() >= max_tls_client_identity_cache_entries) {
      // This cache only bridges TLS verification to request dispatch. Evicting
      // one old endpoint is fail-closed for that connection and keeps a paired
      // client from growing process memory with connection churn.
      tls_client_identity_by_endpoint.erase(tls_client_identity_by_endpoint.begin());
    }
    tls_client_identity_by_endpoint.insert_or_assign(key, identity);
  }

  void forget_tls_client_identity(const boost::asio::ip::tcp::endpoint &endpoint) {
    const auto key = endpoint_key(endpoint);
    if (key.empty()) {
      return;
    }

    std::lock_guard<std::mutex> lock(tls_client_identity_mutex);
    tls_client_identity_by_endpoint.erase(key);
  }

  void forget_tls_client_identities_for_uuid(const std::string_view uuid) {
    std::lock_guard<std::mutex> lock(tls_client_identity_mutex);
    for (auto it = tls_client_identity_by_endpoint.begin(); it != tls_client_identity_by_endpoint.end();) {
      if (it->second.uuid == uuid) {
        it = tls_client_identity_by_endpoint.erase(it);
      } else {
        ++it;
      }
    }
  }

  void clear_tls_client_identities() {
    std::lock_guard<std::mutex> lock(tls_client_identity_mutex);
    tls_client_identity_by_endpoint.clear();
  }

  std::optional<resolved_client_identity_t> get_remembered_tls_client_identity(req_https_t request) {
    if (!request) {
      return std::nullopt;
    }

    const auto key = endpoint_key(request->remote_endpoint());
    if (key.empty()) {
      return std::nullopt;
    }

    // Verification and revocation use the same client -> endpoint-cache lock
    // order. A cached transport identity is only a lookup acceleration; the
    // live machine pairing record remains authoritative on every request.
    std::lock_guard<std::mutex> client_lock(client_mutex);
    std::lock_guard<std::mutex> cache_lock(tls_client_identity_mutex);
    auto it = tls_client_identity_by_endpoint.find(key);
    if (it == tls_client_identity_by_endpoint.end()) {
      return std::nullopt;
    }
    if (!paired_client_uuid_enabled_locked(it->second.uuid)) {
      tls_client_identity_by_endpoint.erase(it);
      return std::nullopt;
    }
    return it->second;
  }

  std::string get_client_uuid_from_request(req_https_t request, std::string *client_name_out = nullptr) {
    if (auto remembered = get_remembered_tls_client_identity(request)) {
      if (client_name_out) {
        *client_name_out = remembered->name;
      }
      return remembered->uuid;
    }
    return {};
  }

  resolved_client_identity_t resolve_client_identity_from_request(req_https_t request) {
    if (auto remembered = get_remembered_tls_client_identity(request)) {
      return *remembered;
    }
    return {};
  }

  std::optional<named_cert_t> get_named_cert_by_uuid(const std::string &uuid) {
    if (uuid.empty()) {
      return std::nullopt;
    }

    std::lock_guard<std::mutex> lock(client_mutex);
    for (const auto &named_cert : client_root.named_devices) {
      if (named_cert.uuid == uuid) {
        return named_cert;
      }
    }
    return std::nullopt;
  }

  std::mutex launch_request_mutex;
  // Configured-app process transitions must be serialized independently from
  // RTSP admission. Synthetic remote controls use per-entry admission and do
  // not take this mutex.
  remote_session::normal_app_transition_gate_t normal_http_app_transition_mutex;
  std::mutex stream_lifecycle_gate;

  std::mutex &stream_lifecycle_mutex() {
    return stream_lifecycle_gate;
  }

  std::unique_lock<std::mutex> acquire_stream_start_lifecycle_lock() {
    bool waited_for_teardown = false;
    for (;;) {
      std::unique_lock<std::mutex> lifecycle_lock {stream_lifecycle_gate};
      if (stream::session::teardown_sessions.load(std::memory_order_acquire) == 0) {
        if (waited_for_teardown) {
          BOOST_LOG(debug) << "Stream start: prior RTSP teardown completed; lifecycle gate acquired.";
        }
        return lifecycle_lock;
      }

      if (!waited_for_teardown) {
        waited_for_teardown = true;
        BOOST_LOG(debug) << "Stream start: yielding lifecycle gate to prior RTSP teardown.";
      }
      lifecycle_lock.unlock();
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
  }

  std::string resolve_known_client_uuid_from_launch_id(const std::string &launch_unique_id) {
    if (launch_unique_id.empty()) {
      return {};
    }

    if (get_named_cert_by_uuid(launch_unique_id)) {
      return launch_unique_id;
    }

    BOOST_LOG(debug) << "Ignoring unmatched launch uniqueid for per-client settings: " << launch_unique_id;
    return {};
  }

  std::string client_name_for_uuid(const std::string &uuid) {
    if (auto named_cert = get_named_cert_by_uuid(uuid)) {
      return named_cert->name;
    }
    return {};
  }

  std::optional<config::video_t::virtual_display_mode_e> parse_virtual_display_mode_override(const std::string &value) {
    const auto trimmed = boost::algorithm::trim_copy(value);
    if (trimmed.empty()) {
      return std::nullopt;
    }
    using mode_e = config::video_t::virtual_display_mode_e;
    if (boost::iequals(trimmed, "disabled")) {
      return mode_e::disabled;
    }
    if (boost::iequals(trimmed, "per_client")) {
      return mode_e::per_client;
    }
    if (boost::iequals(trimmed, "shared")) {
      return mode_e::shared;
    }
    return std::nullopt;
  }

  std::optional<config::video_t::virtual_display_layout_e> parse_virtual_display_layout_override(const std::string &value) {
    const auto trimmed = boost::algorithm::trim_copy(value);
    if (trimmed.empty()) {
      return std::nullopt;
    }
    using layout_e = config::video_t::virtual_display_layout_e;
    if (boost::iequals(trimmed, "exclusive")) {
      return layout_e::exclusive;
    }
    if (boost::iequals(trimmed, "extended")) {
      return layout_e::extended;
    }
    if (boost::iequals(trimmed, "extended_primary")) {
      return layout_e::extended_primary;
    }
    if (boost::iequals(trimmed, "extended_isolated")) {
      return layout_e::extended_isolated;
    }
    if (boost::iequals(trimmed, "extended_primary_isolated")) {
      return layout_e::extended_primary_isolated;
    }
    return std::nullopt;
  }

  std::shared_ptr<rtsp_stream::launch_session_t> make_launch_session(
    bool host_audio,
    const args_t &args,
    req_https_t request = nullptr,
    bool allow_display_changes = true,
    const resolved_client_identity_t *resolved_client_identity = nullptr
  ) {
    auto launch_session = std::make_shared<rtsp_stream::launch_session_t>();

    launch_session->id = ++session_id_counter;
    launch_session->gen1_framegen_fix = false;
    launch_session->gen2_framegen_fix = false;
    launch_session->frame_generation_enabled = false;
    launch_session->lossless_scaling_framegen = false;
    launch_session->framegen_refresh_rate.reset();
    launch_session->framegen_refresh_millihz.reset();
    launch_session->framegen_refresh_multiplier = 1;
    launch_session->lossless_scaling_target_fps.reset();
    launch_session->lossless_scaling_rtss_limit.reset();
    launch_session->frame_generation_provider = "lossless-scaling";
    launch_session->device_name = config::nvhttp.sunshine_name;
    launch_session->client_vrr_requested = false;
    launch_session->virtual_display = false;
    launch_session->virtual_display_guid_bytes.fill(0);
    launch_session->virtual_display_device_id.clear();
    launch_session->virtual_display_ready_since.reset();
    launch_session->virtual_display_hdr_enabled.reset();
    launch_session->app_metadata.reset();
    launch_session->client_uuid.clear();
    launch_session->client_name.clear();
    launch_session->hdr_profile.reset();
    launch_session->client_display_mode_override = false;
    launch_session->client_display_refresh_millihz = 0;
    launch_session->client_requests_virtual_display = false;
    launch_session->client_virtual_display_override.reset();
    launch_session->resolution_override.reset();
    launch_session->virtual_display_failed = false;
    launch_session->hdr_profile.reset();

    if (resolved_client_identity && !resolved_client_identity->uuid.empty()) {
      launch_session->client_uuid = resolved_client_identity->uuid;
      launch_session->client_name = resolved_client_identity->name;
    } else if (request) {
      launch_session->client_uuid = get_client_uuid_from_request(request, &launch_session->client_name);
    }

    // A launch uniqueid is client supplied and cannot authorize a caller. The
    // paired TLS certificate is the only identity used for a session role,
    // per-client settings, or monitor ownership.
    const auto launch_client_uuid = resolve_known_client_uuid_from_launch_id(get_arg(args, "uniqueid", ""));
    if (!launch_client_uuid.empty() && launch_client_uuid != launch_session->client_uuid) {
      BOOST_LOG(warning) << "Ignoring launch uniqueid that conflicts with the authenticated TLS client identity.";
    }

    auto client_name_arg = get_arg(args, "clientName", "");
    if (!client_name_arg.empty()) {
      launch_session->device_name = client_name_arg;
    }

    const auto original_client_name = boost::algorithm::trim_copy(launch_session->client_name);
    if (!original_client_name.empty() && is_placeholder_client_name(original_client_name)) {
      const auto resolved_display_client_name =
        display_client_name_for_session(launch_session->client_name, launch_session->device_name, config::nvhttp.sunshine_name);
      BOOST_LOG(warning) << "Resolved paired client name '" << launch_session->client_name
                         << "' is not safe for display identity; using '" << resolved_display_client_name << "' instead.";
      launch_session->client_name = resolved_display_client_name;
    } else {
      launch_session->client_name = original_client_name;
    }

    auto rikey = util::from_hex_vec(get_arg(args, "rikey"), true);
    std::copy(rikey.cbegin(), rikey.cend(), std::back_inserter(launch_session->gcm_key));

    launch_session->host_audio = host_audio;
    auto client_settings = get_named_cert_by_uuid(launch_session->client_uuid);
    struct parsed_display_mode_t {
      int width = 0;
      int height = 0;
      std::uint32_t refresh_millihz = 0;
    };

    const auto parse_mode_string = [](const std::string &mode_str) -> std::optional<parsed_display_mode_t> {
      constexpr std::uint32_t kMinRefreshMillihz = 10'000;
      constexpr std::uint32_t kMaxRefreshMillihz = 1'000'000;

      const auto parse_unsigned = [](std::string_view value, std::uint32_t maximum, std::uint32_t &result) {
        if (value.empty()) {
          return false;
        }
        std::uint64_t parsed = 0;
        for (const char ch : value) {
          if (ch < '0' || ch > '9') {
            return false;
          }
          const auto digit = static_cast<unsigned int>(ch - '0');
          if (digit > maximum || parsed > (maximum - digit) / 10) {
            return false;
          }
          parsed = parsed * 10 + digit;
        }
        result = static_cast<std::uint32_t>(parsed);
        return true;
      };

      const auto first_separator = mode_str.find('x');
      const auto second_separator = first_separator == std::string::npos ? std::string::npos : mode_str.find('x', first_separator + 1);
      if (first_separator == std::string::npos || second_separator == std::string::npos ||
          mode_str.find('x', second_separator + 1) != std::string::npos) {
        return std::nullopt;
      }

      std::uint32_t width = 0;
      std::uint32_t height = 0;
      if (!parse_unsigned(std::string_view(mode_str).substr(0, first_separator), static_cast<std::uint32_t>(std::numeric_limits<int>::max()), width) ||
          !parse_unsigned(std::string_view(mode_str).substr(first_separator + 1, second_separator - first_separator - 1), static_cast<std::uint32_t>(std::numeric_limits<int>::max()), height)) {
        return std::nullopt;
      }

      const std::string_view refresh_text {mode_str.data() + second_separator + 1, mode_str.size() - second_separator - 1};
      const auto decimal_point = refresh_text.find('.');
      std::uint32_t refresh_millihz = 0;
      if (decimal_point == std::string_view::npos) {
        std::uint32_t raw_refresh = 0;
        if (!parse_unsigned(refresh_text, kMaxRefreshMillihz, raw_refresh)) {
          return std::nullopt;
        }
        // Plain 1000 means 1000 Hz, while the larger legacy values (for
        // example 60000) retain their millihertz interpretation.
        refresh_millihz = raw_refresh > 1000 ? raw_refresh : raw_refresh * 1000;
      } else {
        if (refresh_text.find('.', decimal_point + 1) != std::string_view::npos) {
          return std::nullopt;
        }
        std::uint32_t whole_hertz = 0;
        std::uint32_t fractional_millihz = 0;
        const auto fractional = refresh_text.substr(decimal_point + 1);
        if (fractional.empty() || fractional.size() > 3 ||
            !parse_unsigned(refresh_text.substr(0, decimal_point), 1000, whole_hertz) ||
            !parse_unsigned(fractional, 999, fractional_millihz)) {
          return std::nullopt;
        }
        const auto scale = fractional.size() == 1 ? 100u : fractional.size() == 2 ? 10u : 1u;
        refresh_millihz = whole_hertz * 1000 + fractional_millihz * scale;
      }

      if (width == 0 || height == 0 || refresh_millihz < kMinRefreshMillihz || refresh_millihz > kMaxRefreshMillihz) {
        return std::nullopt;
      }

      return parsed_display_mode_t {
        .width = static_cast<int>(width),
        .height = static_cast<int>(height),
        .refresh_millihz = refresh_millihz,
      };
    };

    // Start with the client requested mode
    if (const auto requested_mode = parse_mode_string(get_arg(args, "mode", "0x0x0"))) {
      launch_session->width = requested_mode->width;
      launch_session->height = requested_mode->height;
      launch_session->fps = framegen::rounded_fps_from_millihz(requested_mode->refresh_millihz);
    }

    // Apply client display mode override if present
    if (client_settings && !client_settings->display_mode.empty()) {
      if (const auto display_mode = parse_mode_string(client_settings->display_mode)) {
        // The override supplies the physical/virtual display mode, while the
        // client-requested FPS remains the stream cadence. This lets a 120 FPS
        // stream use a precise 59.94 Hz presentation/RTSS limit instead of
        // silently downshifting the encoder to 60 FPS.
        launch_session->width = display_mode->width;
        launch_session->height = display_mode->height;
        launch_session->client_display_mode_override = true;
        launch_session->client_display_refresh_millihz = display_mode->refresh_millihz;
      } else {
        BOOST_LOG(warning) << "Failed to parse client display mode override: " << client_settings->display_mode;
      }
    }

    if (client_settings) {
      launch_session->client_requests_virtual_display = client_settings->always_use_virtual_display;
      if (!client_settings->output_name_override.empty()) {
        launch_session->output_name_override = client_settings->output_name_override;
      }
      if (!client_settings->virtual_display_mode_override.empty()) {
        if (auto parsed_mode = parse_virtual_display_mode_override(client_settings->virtual_display_mode_override)) {
          launch_session->virtual_display_mode_override = *parsed_mode;
        }
      }
      if (!client_settings->virtual_display_layout_override.empty()) {
        if (auto parsed_layout = parse_virtual_display_layout_override(client_settings->virtual_display_layout_override)) {
          launch_session->virtual_display_layout_override = *parsed_layout;
        }
      }
      if (!client_settings->hdr_profile.empty()) {
        launch_session->hdr_profile = client_settings->hdr_profile;
      }
    }

    if (const auto virtual_display_arg = args.find("virtualDisplay"); virtual_display_arg != std::end(args)) {
      launch_session->client_virtual_display_override = util::from_view(virtual_display_arg->second) != 0;
      if (!*launch_session->client_virtual_display_override) {
        launch_session->virtual_display_mode_override = config::video_t::virtual_display_mode_e::disabled;
      }
    }

    const auto scale_factor = util::from_view(get_arg(args, "scaleFactor", "100"));
    if (scale_factor > 0 && scale_factor != 100 && launch_session->width > 0 && launch_session->height > 0) {
      const auto scale_dimension = [scale_factor](const int dimension) -> std::optional<int> {
        const auto unsigned_dimension = static_cast<std::uint64_t>(dimension);
        const auto unsigned_scale_factor = static_cast<std::uint64_t>(scale_factor);
        if (unsigned_dimension > std::numeric_limits<std::uint64_t>::max() / unsigned_scale_factor) {
          return std::nullopt;
        }

        const auto scaled_dimension = (unsigned_dimension * unsigned_scale_factor) / 100u;
        const auto even_dimension = scaled_dimension & ~1ull;
        if (even_dimension == 0 || even_dimension > static_cast<std::uint64_t>(std::numeric_limits<int>::max())) {
          return std::nullopt;
        }
        return static_cast<int>(even_dimension);
      };

      if (const auto width = scale_dimension(launch_session->width), height = scale_dimension(launch_session->height); width && height) {
        launch_session->resolution_override = rtsp_stream::launch_session_t::resolution_override_t {
          .width = *width,
          .height = *height,
        };
        BOOST_LOG(info) << "Using launch resolution override " << *width << "x" << *height
                        << " for client scaleFactor=" << scale_factor << ".";
      } else {
        BOOST_LOG(warning) << "Ignoring invalid client scaleFactor=" << scale_factor
                           << " for requested mode " << launch_session->width << "x" << launch_session->height << ".";
      }
    }

    launch_session->unique_id = (get_arg(args, "uniqueid", "unknown"));
    const auto launch_appid_arg = get_arg(args, "appid", "0");
    const auto launch_appuuid_arg = get_arg(args, "appuuid", "");
    auto launch_app_ctx = proc::proc.resolve_app(launch_appid_arg, launch_appuuid_arg);
    launch_session->appid = launch_app_ctx ? (int) util::from_view(launch_app_ctx->id) : (int) util::from_view(launch_appid_arg);
    if (launch_app_ctx || launch_session->appid > 0) {
      try {
        if (auto app_ctx = launch_app_ctx ? launch_app_ctx : proc::proc.resolve_app(launch_session->appid)) {
          launch_session->appid = (int) util::from_view(app_ctx->id);
          launch_session->gen1_framegen_fix = app_ctx->gen1_framegen_fix;
          launch_session->gen2_framegen_fix = app_ctx->gen2_framegen_fix;
          launch_session->frame_generation_enabled = app_ctx->frame_generation_enabled;
          launch_session->lossless_scaling_framegen = app_ctx->lossless_scaling_framegen;
          launch_session->lossless_scaling_target_fps = app_ctx->lossless_scaling_target_fps;
          launch_session->lossless_scaling_rtss_limit = app_ctx->lossless_scaling_rtss_limit;
          launch_session->frame_generation_provider = app_ctx->frame_generation_provider;
          rtsp_stream::launch_session_t::app_metadata_t metadata;
          metadata.id = app_ctx->id;
          metadata.name = app_ctx->name;
          metadata.virtual_screen = app_ctx->virtual_screen;
          metadata.has_command = !app_ctx->cmd.empty();
          metadata.has_playnite = !app_ctx->playnite_id.empty();
          metadata.playnite_fullscreen = app_ctx->playnite_fullscreen;
          launch_session->virtual_display = app_ctx->virtual_screen;
          if (!launch_session->virtual_display_mode_override && app_ctx->virtual_display_mode_override) {
            launch_session->virtual_display_mode_override = app_ctx->virtual_display_mode_override;
          }
          if (!launch_session->virtual_display_layout_override && app_ctx->virtual_display_layout_override) {
            launch_session->virtual_display_layout_override = app_ctx->virtual_display_layout_override;
          }
          if (!launch_session->dd_config_option_override && app_ctx->dd_config_option_override) {
            launch_session->dd_config_option_override = app_ctx->dd_config_option_override;
          }
          if (!launch_session->output_name_override && app_ctx->output_name_override) {
            launch_session->output_name_override = *app_ctx->output_name_override;
          }
          launch_session->app_metadata = std::move(metadata);
        }
      } catch (...) {
      }
    }

    launch_session->framegen_refresh_rate.reset();
    launch_session->framegen_refresh_millihz.reset();
    launch_session->framegen_refresh_multiplier = 1;
    launch_session->enable_sops = util::from_view(get_arg(args, "sops", "0"));
    launch_session->surround_info = (int) util::from_view(get_arg(args, "surroundAudioInfo", "196610"));
    launch_session->surround_params = (get_arg(args, "surroundParams", ""));
    launch_session->continuous_audio = util::from_view(get_arg(args, "continuousAudio", "0"));
    launch_session->gcmap = (int) util::from_view(get_arg(args, "gcmap", "0"));
    launch_session->enable_hdr = util::from_view(get_arg(args, "hdrMode", "0"));
    launch_session->client_vrr_requested = util::from_view(get_arg(args, "clientVrrRequested", "0"));
    // Resume requests usually omit appid. Resolve the running application's
    // preference as well, before display preparation and HDR request overrides.
    auto color_app_ctx = launch_app_ctx;
    if (!color_app_ctx && launch_session->appid <= 0 && launch_appuuid_arg.empty()) {
      color_app_ctx = proc::proc.resolve_app(proc::proc.current_app_id());
    }
    launch_session->prefer_sdr_10bit = rtsp_stream::hdr_request_policy::resolve_prefer_10bit_sdr(
      client_settings && client_settings->prefer_10bit_sdr,
      color_app_ctx ? color_app_ctx->prefer_10bit_sdr : std::nullopt
    );
#if defined(_WIN32) || defined(__linux__)
    {
      const auto hdr_request = rtsp_stream::hdr_request_policy::apply(
        {
          launch_session->enable_hdr,
          launch_session->prefer_sdr_10bit,
          launch_session->force_sdr,
        },
        config::video.dd.hdr_request_override
      );
      launch_session->enable_hdr = hdr_request.enable_hdr;
      launch_session->prefer_sdr_10bit = hdr_request.prefer_sdr_10bit;
      launch_session->force_sdr = hdr_request.force_sdr;
    }
#endif

    // Encrypted RTSP is enabled with client reported corever >= 1
    auto corever = util::from_view(get_arg(args, "corever", "0"));
    if (corever >= 1) {
      launch_session->rtsp_cipher = crypto::cipher::gcm_t {
        launch_session->gcm_key,
        false
      };
      launch_session->rtsp_iv_counter = 0;
    }
    launch_session->rtsp_url_scheme = launch_session->rtsp_cipher ? "rtspenc://"s : "rtsp://"s;
    if (request) {
      launch_session->rtsp_source_address = request->remote_endpoint().address().to_string();
    }

    // Generate the unique identifiers for this connection that we will send later during RTSP handshake
    unsigned char raw_payload[8];
    RAND_bytes(raw_payload, sizeof(raw_payload));
    launch_session->av_ping_payload = util::hex_vec(raw_payload);
    RAND_bytes((unsigned char *) &launch_session->control_connect_data, sizeof(launch_session->control_connect_data));

    launch_session->iv.resize(16);
    uint32_t prepend_iv = util::endian::big<uint32_t>((int) util::from_view(get_arg(args, "rikeyid")));
    auto prepend_iv_p = (uint8_t *) &prepend_iv;
    std::copy(prepend_iv_p, prepend_iv_p + sizeof(prepend_iv), std::begin(launch_session->iv));

#ifdef _WIN32
    {
      // Default the capture gate to "proceed"; launch/resume replace it when an
      // APPLY is dispatched so capture can wait for the helper's verification.
      std::promise<rtsp_stream::launch_session_t::display_helper_gate_status_e> gate_promise;
      gate_promise.set_value(rtsp_stream::launch_session_t::display_helper_gate_status_e::proceed);
      launch_session->display_helper_gate = gate_promise.get_future().share();
    }
#endif
    return launch_session;
  }

  void remove_session(const pair_session_t &sess) {
    const std::string unique_id = sess.client.uniqueID;
    map_id_sess.erase(unique_id);
  }

  /**
   * @brief Answer the client still waiting on a pending pairing request and close that connection.
   * @param session The pending session whose getservercert response is still open.
   * @param status_code The HTTP-style status code to report.
   * @param status_message The status message to report.
   */
  void close_pending_pairing_response(pair_session_t &session, const int status_code, const std::string_view status_message) {
    pt::ptree tree;
    tree.put("root.paired", 0);
    tree.put("root.<xmlattr>.status_code", status_code);
    tree.put("root.<xmlattr>.status_message", std::string(status_message));
    std::ostringstream data;
    pt::write_xml(data, tree);
    auto &response = session.async_insert_pin.response;
    try {
      if (response.has_left() && response.left()) {
        response.left()->close_connection_after_response = true;
        response.left()->write(data.str());
      } else if (response.has_right() && response.right()) {
        response.right()->close_connection_after_response = true;
        response.right()->write(data.str());
      }
    } catch (const std::exception &error) {
      BOOST_LOG(debug) << "Closing a pending pairing response failed: " << error.what();
    } catch (...) {
      BOOST_LOG(debug) << "Closing a pending pairing response failed";
    }
  }

  void expire_pairing_sessions_locked(const std::chrono::steady_clock::time_point now) {
    for (auto session = map_id_sess.begin(); session != map_id_sess.end();) {
      if (now - session->second.created_at <= pairing_session_expiry) {
        ++session;
        continue;
      }
      close_pending_pairing_response(session->second, 408, "Pairing request expired"sv);
      session = map_id_sess.erase(session);
    }
  }

  void fail_pair(pair_session_t &sess, pt::ptree &tree, const std::string status_msg) {
    tree.put("root.paired", 0);
    tree.put("root.<xmlattr>.status_code", 400);
    tree.put("root.<xmlattr>.status_message", status_msg);
    remove_session(sess);  // Security measure, delete the session when something went wrong and force a re-pair
  }

  void getservercert(pair_session_t &sess, pt::ptree &tree, const std::string &pin) {
    const auto decision = pairing_policy::begin_get_server_certificate(
      {static_cast<pairing_policy::phase_e>(sess.last_phase), static_cast<bool>(sess.cipher_key), !sess.serversecret.empty()},
      sess.async_insert_pin.salt.size()
    );
    sess.last_phase = static_cast<PAIR_PHASE>(decision.next_phase);
    if (!decision.accepted) {
      fail_pair(sess, tree, std::string {decision.failure_message});
      return;
    }

    std::string_view salt_view {sess.async_insert_pin.salt.data(), 32};

    auto salt = util::from_hex<std::array<uint8_t, 16>>(salt_view, true);

    auto key = crypto::gen_aes_key(salt, pin);
    sess.cipher_key = std::make_unique<crypto::aes_t>(key);

    tree.put("root.paired", 1);
    tree.put("root.plaincert", util::hex_vec(conf_intern.servercert, true));
    tree.put("root.<xmlattr>.status_code", 200);
  }

  void clientchallenge(pair_session_t &sess, pt::ptree &tree, const std::string &challenge) {
    const auto decision = pairing_policy::begin_client_challenge(
      {static_cast<pairing_policy::phase_e>(sess.last_phase), static_cast<bool>(sess.cipher_key), !sess.serversecret.empty()}
    );
    sess.last_phase = static_cast<PAIR_PHASE>(decision.next_phase);
    if (!decision.accepted) {
      fail_pair(sess, tree, std::string {decision.failure_message});
      return;
    }
    crypto::cipher::ecb_t cipher(*sess.cipher_key, false);

    std::vector<uint8_t> decrypted;
    cipher.decrypt(challenge, decrypted);

    auto x509 = crypto::x509(conf_intern.servercert);
    auto sign = crypto::signature(x509);
    auto serversecret = crypto::rand(16);

    decrypted.insert(std::end(decrypted), std::begin(sign), std::end(sign));
    decrypted.insert(std::end(decrypted), std::begin(serversecret), std::end(serversecret));

    auto hash = crypto::hash({(char *) decrypted.data(), decrypted.size()});
    auto serverchallenge = crypto::rand(16);

    std::string plaintext;
    plaintext.reserve(hash.size() + serverchallenge.size());

    plaintext.insert(std::end(plaintext), std::begin(hash), std::end(hash));
    plaintext.insert(std::end(plaintext), std::begin(serverchallenge), std::end(serverchallenge));

    std::vector<uint8_t> encrypted;
    cipher.encrypt(plaintext, encrypted);

    sess.serversecret = std::move(serversecret);
    sess.serverchallenge = std::move(serverchallenge);

    tree.put("root.paired", 1);
    tree.put("root.challengeresponse", util::hex_vec(encrypted, true));
    tree.put("root.<xmlattr>.status_code", 200);
  }

  void serverchallengeresp(pair_session_t &sess, pt::ptree &tree, const std::string &encrypted_response) {
    const auto decision = pairing_policy::begin_server_challenge_response(
      {static_cast<pairing_policy::phase_e>(sess.last_phase), static_cast<bool>(sess.cipher_key), !sess.serversecret.empty()}
    );
    sess.last_phase = static_cast<PAIR_PHASE>(decision.next_phase);
    if (!decision.accepted) {
      fail_pair(sess, tree, std::string {decision.failure_message});
      return;
    }

    std::vector<uint8_t> decrypted;
    crypto::cipher::ecb_t cipher(*sess.cipher_key, false);

    cipher.decrypt(encrypted_response, decrypted);

    sess.clienthash = std::move(decrypted);

    auto serversecret = sess.serversecret;
    auto sign = crypto::sign256(crypto::pkey(conf_intern.pkey), serversecret);

    serversecret.insert(std::end(serversecret), std::begin(sign), std::end(sign));

    tree.put("root.pairingsecret", util::hex_vec(serversecret, true));
    tree.put("root.paired", 1);
    tree.put("root.<xmlattr>.status_code", 200);
  }

  void clientpairingsecret(pair_session_t &sess, std::shared_ptr<safe::queue_t<crypto::x509_t>> &add_cert, pt::ptree &tree, const std::string &client_pairing_secret) {
    auto &client = sess.client;

    const auto begin_decision = pairing_policy::begin_client_pairing_secret(
      {static_cast<pairing_policy::phase_e>(sess.last_phase), static_cast<bool>(sess.cipher_key), !sess.serversecret.empty()},
      client_pairing_secret.size()
    );
    sess.last_phase = static_cast<PAIR_PHASE>(begin_decision.next_phase);
    if (!begin_decision.accepted) {
      fail_pair(sess, tree, std::string {begin_decision.failure_message});
      return;
    }

    std::string_view secret {client_pairing_secret.data(), 16};
    std::string_view sign {client_pairing_secret.data() + secret.size(), client_pairing_secret.size() - secret.size()};

    auto x509 = crypto::x509(client.cert);
    std::string_view x509_sign;
    crypto::sha256_t hash {};
    bool same_hash = false;
    bool verify = false;
    if (x509 && client_pairing_secret.size() > 16) {
      x509_sign = crypto::signature(x509);
      std::string data;
      data.reserve(sess.serverchallenge.size() + x509_sign.size() + secret.size());

      data.insert(std::end(data), std::begin(sess.serverchallenge), std::end(sess.serverchallenge));
      data.insert(std::end(data), std::begin(x509_sign), std::end(x509_sign));
      data.insert(std::end(data), std::begin(secret), std::end(secret));

      hash = crypto::hash(data);
      same_hash = hash.size() == sess.clienthash.size() && std::equal(hash.begin(), hash.end(), sess.clienthash.begin());
      verify = crypto::verify256(crypto::x509(client.cert), secret, sign);
    }

    const auto decision = pairing_policy::decide_client_pairing_secret(
      {static_cast<pairing_policy::phase_e>(sess.last_phase), static_cast<bool>(sess.cipher_key), !sess.serversecret.empty()},
      static_cast<bool>(x509),
      same_hash,
      verify
    );
    sess.last_phase = static_cast<PAIR_PHASE>(decision.next_phase);
    if (decision.accepted) {
      // The client is now successfully paired and will be authorized to connect
      // in every machine session. Reject a duplicate or over-limit record
      // before it can make the sole shared pairing database ambiguous.
      if (!add_authorized_client(client.name, std::move(client.cert))) {
        fail_pair(sess, tree, "Pairing database cannot accept this client");
        return;
      }
      add_cert->raise(std::move(x509));
      tree.put("root.paired", 1);
    } else if (!decision.failure_message.empty()) {
      fail_pair(sess, tree, std::string {decision.failure_message});
      return;
    } else {
      tree.put("root.paired", 0);
    }

    tree.put("root.<xmlattr>.status_code", 200);
    // Success and cryptographic rejection both complete the sole immutable
    // request. A later PIN must never target this certificate implicitly.
    remove_session(sess);
  }

  template<class T>
  struct tunnel;

  template<>
  struct tunnel<SunshineHTTPS> {
    static auto constexpr to_string = "HTTPS"sv;
  };

  template<>
  struct tunnel<SimpleWeb::HTTP> {
    static auto constexpr to_string = "NONE"sv;
  };

  template<class T>
  void print_req(std::shared_ptr<typename SimpleWeb::ServerBase<T>::Request> request) {
    BOOST_LOG(verbose) << "HTTP "sv << request->method << ' ' << request->path << " tunnel="sv << tunnel<T>::to_string;

    if (!request->header.empty()) {
      BOOST_LOG(verbose) << "Headers:"sv;
      for (auto &[name, val] : request->header) {
        BOOST_LOG(verbose) << name << " -- " << val;
      }
    }

    auto query = request->parse_query_string();
    if (!query.empty()) {
      BOOST_LOG(verbose) << "Query Params:"sv;
      for (auto &[name, val] : query) {
        BOOST_LOG(verbose) << name << " -- " << val;
      }
    }
  }

  template<class T>
  void not_found(std::shared_ptr<typename SimpleWeb::ServerBase<T>::Response> response, std::shared_ptr<typename SimpleWeb::ServerBase<T>::Request> request) {
    print_req<T>(request);

    pt::ptree tree;
    tree.put("root.<xmlattr>.status_code", 404);

    std::ostringstream data;

    pt::write_xml(data, tree);
    response->write(data.str());

    *response
      << "HTTP/1.1 404 NOT FOUND\r\n"
      << data.str();

    response->close_connection_after_response = true;
  }

  template<class T>
  void unpair(std::shared_ptr<typename SimpleWeb::ServerBase<T>::Response> response, std::shared_ptr<typename SimpleWeb::ServerBase<T>::Request> request) {
    print_req<T>(request);
    std::lock_guard pairing_lock {pairing_sessions_mutex};

    pt::ptree tree;

    auto fg = util::fail_guard([&]() {
      std::ostringstream data;

      pt::write_xml(data, tree);
      response->write(data.str());
      response->close_connection_after_response = true;
    });

    auto args = request->parse_query_string();
    auto unique_id = get_arg(args, "uniqueid", "");

    bool removed = false;

    if constexpr (std::is_same_v<T, SunshineHTTPS>) {
      if (auto uuid = get_client_uuid_from_request(request); !uuid.empty()) {
        removed = unpair_client(uuid);
      }
    }

    tree.put("root.unpaired", removed ? 1 : 0);
    tree.put("root.<xmlattr>.status_code", 200);

    // Pairing admission is immutable until the protocol completes or its
    // bounded expiry fires. In particular, the unauthenticated HTTP endpoint
    // must not erase the sole request that the next administrator PIN targets.
    (void) unique_id;
  }

  template<class T>
  void pair(std::shared_ptr<safe::queue_t<crypto::x509_t>> &add_cert, std::shared_ptr<typename SimpleWeb::ServerBase<T>::Response> response, std::shared_ptr<typename SimpleWeb::ServerBase<T>::Request> request) {
    print_req<T>(request);
    std::lock_guard pairing_lock {pairing_sessions_mutex};

    pt::ptree tree;

    auto fg = util::fail_guard([&]() {
      std::ostringstream data;

      pt::write_xml(data, tree);
      response->write(data.str());
      response->close_connection_after_response = true;
    });

    auto args = request->parse_query_string();
    if (args.find("uniqueid"s) == std::end(args)) {
      tree.put("root.<xmlattr>.status_code", 400);
      tree.put("root.<xmlattr>.status_message", "Missing uniqueid parameter");

      return;
    }

    auto uniqID {get_arg(args, "uniqueid")};
    if (!pairing_policy::valid_unique_id(uniqID)) {
      tree.put("root.<xmlattr>.status_code", 400);
      tree.put("root.<xmlattr>.status_message", "Invalid uniqueid");
      return;
    }

    expire_pairing_sessions_locked(std::chrono::steady_clock::now());

    args_t::const_iterator it;
    if (it = args.find("phrase"); it != std::end(args)) {
      if (it->second == "getservercert"sv) {
        const auto client_certificate = get_arg(args, "clientcert", "");
        const auto salt = get_arg(args, "salt", "");
        const auto existing = map_id_sess.find(uniqID);
        const bool replacing = existing != std::end(map_id_sess);
        // Moonlight retries with the same uniqueid and certificate after a
        // cancelled or abandoned attempt. Only that exact identity may take
        // over its own pending request; any other client waits for expiry.
        const bool same_identity = replacing &&
                                   pairing_policy::valid_hex_field(client_certificate, 2) &&
                                   existing->second.client.cert == util::from_hex_vec(client_certificate, true);
        const auto admission = pairing_policy::admit_pending_session(
          uniqID,
          client_certificate,
          salt,
          map_id_sess.size(),
          replacing,
          same_identity
        );
        if (!admission.accepted) {
          tree.put("root.<xmlattr>.status_code", admission.failure_message == "Too many pending pairing sessions"sv ? 429 : 400);
          tree.put("root.<xmlattr>.status_message", admission.failure_message);
          return;
        }
        if (replacing) {
          close_pending_pairing_response(existing->second, 409, "Pairing request replaced by a newer request from the same client"sv);
          map_id_sess.erase(existing);
        }
        pair_session_t sess;

        sess.client.uniqueID = std::move(uniqID);
        sess.client.cert = util::from_hex_vec(client_certificate, true);

        BOOST_LOG(verbose) << sess.client.cert;
        auto session_id = sess.client.uniqueID;
        auto [ptr, inserted] = map_id_sess.emplace(std::move(session_id), std::move(sess));
        if (!inserted) {
          tree.put("root.<xmlattr>.status_code", 429);
          tree.put("root.<xmlattr>.status_message", "Too many pending pairing sessions");
          return;
        }

        ptr->second.async_insert_pin.salt = salt;
        if (config::sunshine.flags[config::flag::PIN_STDIN]) {
          std::string pin;

          std::cout << "Please insert pin: "sv;
          std::getline(std::cin, pin);

          getservercert(ptr->second, tree, pin);
        } else {
#if defined SUNSHINE_TRAY && SUNSHINE_TRAY >= 1
          system_tray::update_tray_require_pin();
#endif
          ptr->second.async_insert_pin.response = std::move(response);

          fg.disable();
          return;
        }
      } else if (it->second == "pairchallenge"sv) {
        tree.put("root.paired", 1);
        tree.put("root.<xmlattr>.status_code", 200);
        return;
      }
    }

    auto sess_it = map_id_sess.find(uniqID);
    if (sess_it == std::end(map_id_sess)) {
      tree.put("root.<xmlattr>.status_code", 400);
      tree.put("root.<xmlattr>.status_message", "Invalid uniqueid");

      return;
    }

    if (it = args.find("clientchallenge"); it != std::end(args)) {
      if (!pairing_policy::valid_hex_field(it->second, 2)) {
        tree.put("root.<xmlattr>.status_code", 400);
        tree.put("root.<xmlattr>.status_message", "Invalid client challenge");
        return;
      }
      auto challenge = util::from_hex_vec(it->second, true);
      clientchallenge(sess_it->second, tree, challenge);
    } else if (it = args.find("serverchallengeresp"); it != std::end(args)) {
      if (!pairing_policy::valid_hex_field(it->second, 2)) {
        tree.put("root.<xmlattr>.status_code", 400);
        tree.put("root.<xmlattr>.status_message", "Invalid server challenge response");
        return;
      }
      auto encrypted_response = util::from_hex_vec(it->second, true);
      serverchallengeresp(sess_it->second, tree, encrypted_response);
    } else if (it = args.find("clientpairingsecret"); it != std::end(args)) {
      if (!pairing_policy::valid_hex_field(it->second, 34)) {
        tree.put("root.<xmlattr>.status_code", 400);
        tree.put("root.<xmlattr>.status_message", "Invalid client pairing secret");
        return;
      }
      auto pairingsecret = util::from_hex_vec(it->second, true);
      clientpairingsecret(sess_it->second, add_cert, tree, pairingsecret);
    } else {
      tree.put("root.<xmlattr>.status_code", 404);
      tree.put("root.<xmlattr>.status_message", "Invalid pairing request");
    }
  }

  bool pin(std::string pin, std::string name) {
    std::lock_guard pairing_lock {pairing_sessions_mutex};
    pt::ptree tree;
    expire_pairing_sessions_locked(std::chrono::steady_clock::now());
    if (map_id_sess.empty()) {
      BOOST_LOG(warning) << "PIN submitted but no pending pairing session exists";
      return false;
    }

    // ensure pin is 4 digits
    if (pin.size() != 4) {
      tree.put("root.paired", 0);
      tree.put("root.<xmlattr>.status_code", 400);
      tree.put(
        "root.<xmlattr>.status_message",
        std::format("Pin must be 4 digits, {} provided", pin.size())
      );
      return false;
    }

    // ensure all pin characters are numeric
    if (!std::all_of(pin.begin(), pin.end(), ::isdigit)) {
      tree.put("root.paired", 0);
      tree.put("root.<xmlattr>.status_code", 400);
      tree.put("root.<xmlattr>.status_message", "Pin must be numeric");
      return false;
    }

    if (map_id_sess.size() != 1 || map_id_sess.begin()->second.last_phase != PAIR_PHASE::NONE) {
      BOOST_LOG(warning) << "PIN submitted but no active pending pairing session is ready";
      return false;
    }
    auto sess_it = map_id_sess.begin();

    auto &sess = sess_it->second;
    if (sess.async_insert_pin.salt.size() < 32) {
      BOOST_LOG(warning) << "PIN submitted but pending pairing session has an invalid salt";
      remove_session(sess);
      return false;
    }

    getservercert(sess, tree, pin);

    if (!name.empty()) {
      if (is_placeholder_client_name(name)) {
        BOOST_LOG(warning) << "PIN submitted with reserved client name '" << name << "'; refusing to pair with placeholder identity.";
        remove_session(sess);
        return false;
      }
      sess.client.name = name;
    }

    // response to the request for pin
    std::ostringstream data;
    pt::write_xml(data, tree);

    auto &async_response = sess.async_insert_pin.response;
    // Keep Content-Length on this delayed response; Moonlight waits for a complete body.
    if (async_response.has_left() && async_response.left()) {
      async_response.left()->write(data.str());
    } else if (async_response.has_right() && async_response.right()) {
      async_response.right()->write(data.str());
    } else {
      BOOST_LOG(warning) << "PIN submitted but pending pairing session has no response channel";
      remove_session(sess);
      return false;
    }

    // reset async_response
    async_response = std::decay_t<decltype(async_response.left())>();
    // response to the current request
    return true;
  }

  template<class T>
  void serverinfo(std::shared_ptr<typename SimpleWeb::ServerBase<T>::Response> response, std::shared_ptr<typename SimpleWeb::ServerBase<T>::Request> request) {
    print_req<T>(request);

    int pair_status = 0;
    if constexpr (std::is_same_v<SunshineHTTPS, T>) {
      pair_status = !resolve_client_identity_from_request(request).uuid.empty();
    }

    auto local_endpoint = request->local_endpoint();

    pt::ptree tree;

    tree.put("root.<xmlattr>.status_code", 200);
    tree.put("root.hostname", config::nvhttp.sunshine_name);

    tree.put("root.appversion", VERSION);
    tree.put("root.GfeVersion", GFE_VERSION);
    tree.put("root.uniqueid", http::unique_id);
    tree.put("root.HttpsPort", net::map_port(PORT_HTTPS));
    tree.put("root.ExternalPort", net::map_port(PORT_HTTP));
#ifdef _WIN32
    // Artemis discovers virtual-display support from /serverinfo before launch.
    // Driver readiness remains a separate runtime state so it can offer recovery
    // when the Windows device is present but unavailable.
    tree.put("root.VirtualDisplayCapable", true);
    tree.put("root.VirtualDisplayDriverReady", proc::vDisplayDriverStatus.load(std::memory_order_acquire) == VDISPLAY::DRIVER_STATUS::OK);
    tree.put("root.VirtualDisplayHDRCapable", true);
#elif defined(__linux__)
    tree.put("root.VirtualDisplayCapable", platf::linux_private_display::capable());
    tree.put("root.VirtualDisplayDriverReady", platf::linux_private_display::ready());
    tree.put("root.VirtualDisplayHDRCapable", platf::linux_private_display::hdr_capable());
#else
    tree.put("root.VirtualDisplayCapable", false);
    tree.put("root.VirtualDisplayDriverReady", false);
    tree.put("root.VirtualDisplayHDRCapable", false);
#endif

    // Only include the MAC address for requests sent from paired clients over HTTPS.
    // For HTTP requests, use a placeholder MAC address that Moonlight knows to ignore.
    if constexpr (std::is_same_v<SunshineHTTPS, T>) {
      tree.put("root.mac", platf::get_mac_address(net::addr_to_normalized_string(local_endpoint.address())));
    } else {
      tree.put("root.mac", "00:00:00:00:00:00");
    }

    // Moonlight clients track LAN IPv6 addresses separately from LocalIP which is expected to
    // always be an IPv4 address. If we return that same IPv6 address here, it will clobber the
    // stored LAN IPv4 address. To avoid this, we need to return an IPv4 address in this field
    // when we get a request over IPv6.
    //
    // HACK: We should return the IPv4 address of local interface here, but we don't currently
    // have that implemented. For now, we will emulate the behavior of GFE+GS-IPv6-Forwarder,
    // which returns 127.0.0.1 as LocalIP for IPv6 connections. Moonlight clients with IPv6
    // support know to ignore this bogus address.
    if (local_endpoint.address().is_v6() && !local_endpoint.address().to_v6().is_v4_mapped()) {
      tree.put("root.LocalIP", "127.0.0.1");
    } else {
      tree.put("root.LocalIP", net::addr_to_normalized_string(local_endpoint.address()));
    }

#ifdef _WIN32
    const auto advertised_video = advertised_encoder_capabilities_for_http().advertised;
#else
    const auto advertised_video = video::advertised_encoder_capabilities(true);
#endif

    tree.put("root.MaxLumaPixelsHEVC", advertised_video.hevc_mode > 1 ? "1869449984" : "0");

    uint32_t codec_mode_flags = SCM_H264;
    if (advertised_video.yuv444_for_codec[0]) {
      codec_mode_flags |= SCM_H264_HIGH8_444;
    }
    if (advertised_video.hevc_mode >= 2) {
      codec_mode_flags |= SCM_HEVC;
      if (advertised_video.yuv444_for_codec[1]) {
        codec_mode_flags |= SCM_HEVC_REXT8_444;
      }
    }
    if (advertised_video.hevc_mode >= 3) {
      codec_mode_flags |= SCM_HEVC_MAIN10;
      if (advertised_video.yuv444_for_codec[1]) {
        codec_mode_flags |= SCM_HEVC_REXT10_444;
      }
    }
    if (advertised_video.av1_mode >= 2) {
      codec_mode_flags |= SCM_AV1_MAIN8;
      if (advertised_video.yuv444_for_codec[2]) {
        codec_mode_flags |= SCM_AV1_HIGH8_444;
      }
    }
    if (advertised_video.av1_mode >= 3) {
      codec_mode_flags |= SCM_AV1_MAIN10;
      if (advertised_video.yuv444_for_codec[2]) {
        codec_mode_flags |= SCM_AV1_HIGH10_444;
      }
    }
    tree.put("root.ServerCodecModeSupport", codec_mode_flags);

    auto current_appid = proc::proc.running();
    auto current_app = proc::proc.resolve_app(current_appid);
    const auto active_session = proc::proc.active_session_guard();
    remote_role_gate_snapshot_t remote_gate;
    remote_session::caller_t caller;
    if constexpr (std::is_same_v<SunshineHTTPS, T>) {
      const auto identity = resolve_client_identity_from_request(request);
      remote_gate = remote_role_gate_snapshot_for_client(identity.uuid);
      caller.uuid = identity.uuid;
      caller.paired = !identity.uuid.empty();
    } else {
      remote_gate = remote_role_gate_snapshot_for_client({});
    }
    const remote_session::game_t game {
      .running = current_appid > 0,
      .owner_uuid = active_session.client_uuid,
      .generation = active_session_generation(active_session),
      .app = current_app ? remote_session::app_t {static_cast<std::int32_t>(util::from_view(current_app->id)), current_app->uuid, current_app->name, false} : remote_session::app_t {},
    };
    bool replacement_confirmation_active = false;
    if (caller.paired) {
      if (remote_gate.active) {
        remote_session::clear_app_replacement_confirmation(caller.uuid);
      } else if (config::video.remote_monitor_confirm_app_replacement) {
        replacement_confirmation_active = remote_session::app_replacement_confirmation_active(caller.uuid, game.generation);
      }
    }
    tree.put("root.PairStatus", pair_status);
    // Before a special owner exists, advertise the host as free so selecting
    // Remote Input or Remote Monitor does not trigger Moonlight's generic
    // replace-running-app warning. The launch handler distinguishes attach,
    // resume, and normal-app replacement from the requested catalogue entry.
    // Once a special owner exists, restore owner-aware busy reporting.
    const bool expose_active_game = remote_session::exposes_active_game(
      caller,
      game,
      remote_gate.owner,
      remote_gate.active,
      replacement_confirmation_active
    );
    tree.put("root.currentgame", expose_active_game ? current_appid : 0);
    tree.put("root.currentgameuuid", expose_active_game && current_app ? current_app->uuid : "");
    tree.put("root.state", expose_active_game ? "SUNSHINE_SERVER_BUSY" : "SUNSHINE_SERVER_FREE");

    std::ostringstream data;

    pt::write_xml(data, tree);
    SimpleWeb::CaseInsensitiveMultimap headers;
    headers.emplace("Cache-Control", "no-store, no-cache, must-revalidate");
    headers.emplace("Pragma", "no-cache");
    response->write(SimpleWeb::StatusCode::success_ok, data.str(), headers);
    response->close_connection_after_response = true;
  }

  nlohmann::json get_all_clients() {
    nlohmann::json named_cert_nodes = nlohmann::json::array();
    const client_t client = client_root_snapshot();
    std::list<std::string> connected_uuids = rtsp_stream::get_all_session_client_uuids();
    for (const auto &named_cert : client.named_devices) {
      nlohmann::json named_cert_node;
      named_cert_node["name"] = named_cert.name;
      named_cert_node["uuid"] = named_cert.uuid;
      named_cert_node["enabled"] = named_cert.enabled;
      named_cert_node["hdr_profile"] = named_cert.hdr_profile;
      named_cert_node["display_mode"] = named_cert.display_mode;
      named_cert_node["output_name_override"] = named_cert.output_name_override;
      named_cert_node["virtual_display_mode"] = named_cert.virtual_display_mode_override;
      named_cert_node["virtual_display_layout"] = named_cert.virtual_display_layout_override;
      named_cert_node["always_use_virtual_display"] = named_cert.always_use_virtual_display;
      named_cert_node["prefer_10bit_sdr"] = named_cert.prefer_10bit_sdr;
      if (named_cert.last_seen.has_value()) {
        named_cert_node["last_seen"] = *named_cert.last_seen;
      }
      if (!named_cert.config_overrides.empty()) {
        nlohmann::json overrides = nlohmann::json::object();
        for (const auto &[k, v] : named_cert.config_overrides) {
          if (k.empty()) {
            continue;
          }
          try {
            overrides[k] = nlohmann::json::parse(v);
          } catch (...) {
            overrides[k] = v;
          }
        }
        named_cert_node["config_overrides"] = std::move(overrides);
      }

      bool connected = false;
      if (!connected_uuids.empty()) {
        for (auto it = connected_uuids.begin(); it != connected_uuids.end(); ++it) {
          if (*it == named_cert.uuid) {
            connected = true;
            connected_uuids.erase(it);
            break;
          }
        }
      }
      named_cert_node["connected"] = connected;
      named_cert_nodes.push_back(named_cert_node);
    }

    return named_cert_nodes;
  }

  nlohmann::json get_remote_display_layout() {
    const auto client = client_root_snapshot();
    try {
      return remote_display_topology::normalize_layout(nlohmann::json::parse(client.remote_display_layout_json));
    } catch (...) {
      return {{"version", remote_display_topology::layout_version}, {"placements", nlohmann::json::object()}};
    }
  }

  bool set_remote_display_layout(const nlohmann::json &layout, std::string &error) {
    std::vector<std::string> known_clients;
    {
      std::lock_guard lock(client_mutex);
      for (const auto &client : client_root.named_devices) known_clients.push_back(client.uuid);
    }
    if (!remote_display_topology::validate_layout(layout, known_clients, remote_display_topology::instance().physical_node_ids(), error)) return false;
    const auto canonical_layout = remote_display_topology::normalize_layout(layout);
    {
      std::lock_guard lock(client_mutex);
      client_root.remote_display_layout_json = canonical_layout.dump();
    }
    save_state();
    remote_display_topology::instance().set_layout(canonical_layout);
    return true;
  }

  void mark_client_last_seen(const std::string &uuid) {
    if (uuid.empty()) {
      return;
    }

    bool changed = false;
    {
      std::lock_guard<std::mutex> lock(client_mutex);
      for (auto &named_cert : client_root.named_devices) {
        if (named_cert.uuid != uuid) {
          continue;
        }

        const auto now = now_seconds();
        if (named_cert.last_seen.has_value() && *named_cert.last_seen == now) {
          return;
        }
        named_cert.last_seen = now;
        changed = true;
        break;
      }
    }
    if (changed && !config::sunshine.flags[config::flag::FRESH_STATE]) {
      save_state();
    }
  }

  void applist(resp_https_t response, req_https_t request) {
    print_req<SunshineHTTPS>(request);

    pt::ptree tree;

    auto g = util::fail_guard([&]() {
      std::ostringstream data;

      pt::write_xml(data, tree);
      SimpleWeb::CaseInsensitiveMultimap headers;
      headers.emplace("Cache-Control", "no-store, no-cache, must-revalidate");
      headers.emplace("Pragma", "no-cache");
      response->write(SimpleWeb::StatusCode::success_ok, data.str(), headers);
      response->close_connection_after_response = true;
    });

    auto &apps = tree.add_child("root", pt::ptree {});

    apps.put("<xmlattr>.status_code", 200);

#ifdef _WIN32
    const auto advertised_video = advertised_encoder_capabilities_for_http().advertised;
#else
    const auto advertised_video = video::advertised_encoder_capabilities(true);
#endif

    const auto configured_apps = proc::proc.get_apps();
    std::vector<remote_session::app_t> remote_configured_apps;
    remote_configured_apps.reserve(configured_apps.size());
    for (const auto &configured : configured_apps) {
      remote_configured_apps.push_back({static_cast<std::int32_t>(util::from_view(configured.id)), configured.uuid, configured.name, false});
    }

    const auto current_appid = proc::proc.running();
    const auto current_app = proc::proc.resolve_app(current_appid);
    const auto active_session = proc::proc.active_session_guard();
    const auto identity = resolve_client_identity_from_request(request);
    const remote_session::caller_t caller {
      .uuid = identity.uuid,
      .paired = !identity.uuid.empty(),
      .may_view = !identity.uuid.empty(),
      .may_launch = !identity.uuid.empty(),
      .may_terminate = !identity.uuid.empty(),
    };
    const remote_session::game_t game {
      .running = current_appid > 0,
      .owner_uuid = active_session.client_uuid,
      .generation = active_session_generation(active_session),
      .app = current_app ? remote_session::app_t {static_cast<std::int32_t>(util::from_view(current_app->id)), current_app->uuid, current_app->name, false} : remote_session::app_t {},
    };
    const auto remote_gate = remote_role_gate_snapshot_for_client(identity.uuid);
    const auto projection = remote_session::project(caller, game, remote_gate.owner, remote_configured_apps, remote_gate.active);

    for (const auto &entry : projection.catalogue) {
      pt::ptree app;

      app.put("IsHdrSupported"s, (advertised_video.hevc_mode == 3 || advertised_video.av1_mode == 3) ? 1 : 0);
      app.put("AppTitle"s, entry.title);
      app.put("UUID", entry.uuid);
      app.put("ID", entry.id);
      const auto configured = std::find_if(configured_apps.begin(), configured_apps.end(), [&entry](const auto &candidate) { return candidate.uuid == entry.uuid; });
      if (entry.synthetic) {
        if (remote_session::identify(entry.id, entry.uuid) == remote_session::control_e::running_game && current_app) {
          app.put("ArtVersion", current_app->art_version);
        } else {
          app.put("ArtVersion", configured == configured_apps.end() ? "remote-session-v6" : configured->art_version);
        }
      } else {
        app.put("ArtVersion", configured == configured_apps.end() ? "" : configured->art_version);
      }

      apps.push_back(std::make_pair("App", std::move(app)));
    }
  }

  void resume(bool &host_audio, resp_https_t response, req_https_t request, int current_appid, bool normal_app_transition = true, bool launched_from_applist = false);
  void cancel(resp_https_t response, req_https_t request);

  void launch(bool &host_audio, resp_https_t response, req_https_t request, int current_appid) {
    print_req<SunshineHTTPS>(request);

#ifdef _WIN32
    // Keep encoder probes blocked across the complete failure unwind: virtual
    // display removal, response publication, and any final helper restore.
    stream::session::cleanup_reservation_t cleanup_reservation;
#endif
    pt::ptree tree;
    bool revert_display_configuration {false};
    auto g = util::fail_guard([&]() {
      std::ostringstream data;

      if (tree.empty()) {
        BOOST_LOG(error) << EMPTY_PROPERTY_TREE_ERROR_MSG;
      }

      pt::write_xml(data, tree);
      response->write(data.str());
      response->close_connection_after_response = true;

      if (revert_display_configuration) {
        display_helper_integration::revert();
      }
    });

    auto args = request->parse_query_string();
    if (
      args.find("rikey"s) == std::end(args) ||
      args.find("rikeyid"s) == std::end(args) ||
      args.find("localAudioPlayMode"s) == std::end(args) ||
      (args.find("appid"s) == std::end(args) && args.find("appuuid"s) == std::end(args))
    ) {
      tree.put("root.resume", 0);
      tree.put("root.<xmlattr>.status_code", 400);
      tree.put("root.<xmlattr>.status_message", "Missing a required launch parameter");

      return;
    }

    const auto request_identity = resolve_client_identity_from_request(request);
    if (request_identity.uuid.empty()) {
      tree.put("root.resume", 0);
      tree.put("root.<xmlattr>.status_code", 403);
      tree.put("root.<xmlattr>.status_message", "A paired TLS client identity is required");
      return;
    }

    auto appid_str = get_arg(args, "appid", "0");
    auto appuuid_str = get_arg(args, "appuuid", "");
    // Synthetic controls are host actions, never configured applications. Do
    // this before resolve_app() so a stale/foreign control cannot collide with
    // an apps.json id and launch a real process.
    const auto synthetic_control = remote_session::identify(util::from_view(appid_str), appuuid_str, current_appid);
    // A secondary Moonlight client sees the running game in its projected
    // catalogue even though serverinfo is deliberately presented as free.
    // Launching that advertised entry is therefore a Resume request, not an
    // attempt to start the configured application again.
    if (synthetic_control == remote_session::control_e::none && current_appid > 0) {
      if (const auto active_app = proc::proc.resolve_app(current_appid)) {
        const bool requests_active_app =
          !appuuid_str.empty() ? appuuid_str == active_app->uuid :
                                 util::from_view(appid_str) == util::from_view(active_app->id);
        if (requests_active_app) {
          remote_session::clear_app_replacement_confirmation(request_identity.uuid);
          g.disable();
          resume(host_audio, std::move(response), std::move(request), current_appid, true, true);
          return;
        }
      }
    }
    if (synthetic_control != remote_session::control_e::none) {
      std::unique_lock remote_transition_lock {remote_http_control_transition_mutex};
      const auto &identity = request_identity;
      const auto active_session = proc::proc.active_session_guard();
      const auto active_app = proc::proc.resolve_app(current_appid);
      const remote_session::game_t game {
        .running = current_appid > 0,
        .owner_uuid = active_session.client_uuid,
        .generation = active_session_generation(active_session),
        .app = active_app ? remote_session::app_t {static_cast<std::int32_t>(util::from_view(active_app->id)), active_app->uuid, active_app->name, false} : remote_session::app_t {},
      };
      const remote_session::caller_t caller {
        .uuid = identity.uuid,
        .paired = !identity.uuid.empty(),
        .may_view = !identity.uuid.empty(),
        .may_launch = !identity.uuid.empty(),
        .may_terminate = !identity.uuid.empty(),
      };
      const auto owner = remote_role_gate_snapshot_for_client(identity.uuid).owner;
      const auto decision = remote_session::dispatch(caller, game, owner, synthetic_control);
      if (!decision.allowed) {
        tree.put("root.resume", 0);
        // Authentication and paired-client capabilities were already checked
        // above. A denial here is a state/role conflict, not an authorization
        // failure, so do not show Moonlight a false permission error.
        tree.put("root.<xmlattr>.status_code", 409);
        tree.put("root.<xmlattr>.status_message", "Remote session action conflicts with this client's current session state");
        return;
      }
      remote_session::clear_app_replacement_confirmation(identity.uuid);
      if (decision.resume && decision.resume_role == remote_session::role_e::game && current_appid > 0) {
        g.disable();
        resume(host_audio, std::move(response), std::move(request), current_appid, true, true);
        return;
      }
      if (decision.terminate) {
        const bool caller_owns_active_game = !game.owner_uuid.empty() && identity.uuid == game.owner_uuid;
        if (remote_session::requires_termination_confirmation(
              config::video.remote_monitor_terminate_on_first_request,
              caller_owns_active_game
            )) {
          const auto confirmation = remote_session::arm_or_confirm_termination(identity.uuid, game.generation, game.app.id);
          if (confirmation == remote_session::terminate_confirmation_e::prompt) {
            BOOST_LOG(info) << "Terminate confirmation armed for client " << identity.uuid
                            << " (app=" << game.app.id << ", generation=" << game.generation << ").";
            tree.put("root.resume", 0);
            tree.put("root.gamesession", 0);
            tree.put("root.<xmlattr>.status_code", 410);
            tree.put("root.<xmlattr>.status_message", std::string {remote_session::termination_confirmation_message()});
            return;
          }
          BOOST_LOG(info) << "Terminate confirmation accepted for client " << identity.uuid
                          << " (app=" << game.app.id << ", generation=" << game.generation << ").";
        } else {
          // Do not let a previous guarded request survive a configuration
          // change or an owner request and confirm a later extra-client launch.
          remote_session::clear_termination_confirmation(identity.uuid);
          BOOST_LOG(info) << "Terminate accepted on the first request for client " << identity.uuid
                          << (caller_owns_active_game ? " (active-game owner)." : " (configured first-request mode).");
        }
        const bool disconnected = rtsp_stream::disconnect_game_sessions(true);
        // Role-scoped transport teardown deliberately preserves Remote Monitor
        // and Remote Input, but it does not end the configured application.
        // Complete the same process/session lifecycle as /cancel while
        // transferring the stream-lifecycle lock already held by /launch.
        proc::proc.terminate(false, true);
        tree.put("root.resume", 0);
        tree.put("root.gamesession", 0);
        if (!disconnected) {
          BOOST_LOG(info) << "Terminate found no active game transport; closed the paused configured application lifecycle.";
        }
        const auto completion = *remote_session::successful_control_completion(synthetic_control);
        tree.put("root.<xmlattr>.status_code", completion.status_code);
        tree.put("root.<xmlattr>.status_message", std::string {completion.status_message});
        return;
      }
      if (synthetic_control == remote_session::control_e::disconnect_input ||
          synthetic_control == remote_session::control_e::disconnect_monitor) {
        if (decision.already_complete) {
          const auto completion = *remote_session::successful_control_completion(synthetic_control);
          tree.put("root.resume", 0);
          tree.put("root.gamesession", 0);
          tree.put("root.<xmlattr>.status_code", completion.status_code);
          tree.put("root.<xmlattr>.status_message", std::string {completion.status_message});
          return;
        }
        const auto role = synthetic_control == remote_session::control_e::disconnect_input ? remote_session::role_e::input : remote_session::role_e::monitor;
        const auto generation = remote_owner_generation(identity.uuid, role);
        if (!generation) {
          tree.put("root.resume", 0);
          tree.put("root.<xmlattr>.status_code", 409);
          tree.put("root.<xmlattr>.status_message", "Remote session generation is no longer owned by this caller");
          return;
        }
        (void) rtsp_stream::disconnect_remote_role_session(identity.uuid, role, *generation, true);
        if (role == remote_session::role_e::monitor) {
          // Stop exact-output capture before removing its owned VDD. The join
          // publishes transport loss first; this explicit generation-matched
          // release then removes only the caller's display.
          remote_session::release_monitor(identity.uuid, *generation, "Disconnect Monitor");
        }
        forget_remote_owner(identity.uuid, role, *generation);
#if defined(_WIN32) || defined(__linux__)
        if (role == remote_session::role_e::monitor) {
          cleanup_virtual_display_if_idle_locked();
        }
#endif
        const auto completion = *remote_session::successful_control_completion(synthetic_control);
        tree.put("root.resume", 0);
        tree.put("root.<xmlattr>.status_code", completion.status_code);
        tree.put("root.<xmlattr>.status_message", std::string {completion.status_message});
        tree.put("root.gamesession", 0);
        return;
      }
      if (synthetic_control == remote_session::control_e::resume ||
          synthetic_control == remote_session::control_e::running_game) {
        if (decision.resume_role == remote_session::role_e::game && current_appid > 0) {
          g.disable();
          resume(host_audio, std::move(response), std::move(request), current_appid, true, true);
          return;
        }
        if (decision.resume_role != remote_session::role_e::monitor ||
            !remote_owner_generation(identity.uuid, remote_session::role_e::monitor)) {
          tree.put("root.resume", 0);
          tree.put("root.<xmlattr>.status_code", 404);
          tree.put("root.<xmlattr>.status_message", "No running game or retained Remote Monitor belongs to this caller");
          return;
        }
      }

      if (synthetic_control != remote_session::control_e::input &&
          synthetic_control != remote_session::control_e::monitor &&
          synthetic_control != remote_session::control_e::resume &&
          synthetic_control != remote_session::control_e::running_game) {
        tree.put("root.resume", 0);
        tree.put("root.<xmlattr>.status_code", 400);
        tree.put("root.<xmlattr>.status_message", "Unsupported remote session control");
        return;
      }

      std::unique_lock normal_transition_lock {normal_http_app_transition_mutex};
      const bool no_active_sessions = !has_stream_session_activity();
      const auto runtime_app = proc::proc.resolve_app(
        "0",
        remote_session::synthetic(
          synthetic_control == remote_session::control_e::input ? remote_session::control_e::input : remote_session::control_e::monitor
        ).uuid
      );
      const auto client_settings = get_named_cert_by_uuid(identity.uuid);
      std::unordered_map<std::string, std::string> requested_runtime_overrides;
      if (runtime_app) {
        config::merge_config_overrides(requested_runtime_overrides, runtime_app->config_overrides);
      }
      if (client_settings) {
        config::merge_config_overrides(requested_runtime_overrides, client_settings->config_overrides);
      }
      if (!no_active_sessions &&
          !config::adapter_config_overrides_compatible_with_active(requested_runtime_overrides)) {
        tree.put("root.resume", 0);
        tree.put("root.<xmlattr>.status_code", 400);
        tree.put("root.<xmlattr>.status_message", "Another stream is active with a different capture adapter selection");
        return;
      }

      auto previous_runtime_overrides = config::runtime_config_overrides_snapshot();
      bool runtime_overrides_applied = false;
      bool keep_runtime_overrides = false;
      auto runtime_overrides_guard = util::fail_guard([&]() {
        if (!runtime_overrides_applied || keep_runtime_overrides) {
          return;
        }
        config::set_runtime_config_overrides(std::move(previous_runtime_overrides));
        if (!has_stream_session_activity()) {
          config::apply_config_now();
        } else {
          config::mark_deferred_reload();
        }
      });

      if (no_active_sessions) {
        try {
          auto overrides = requested_runtime_overrides;
#ifdef _WIN32
          if (client_settings &&
              !client_settings->hdr_profile.empty() &&
              !overrides.contains("rtx_hdr_peak_brightness")) {
            if (const auto profile_peak = VDISPLAY::hdr_profile_peak_luminance_nits(client_settings->hdr_profile)) {
              overrides.insert_or_assign("rtx_hdr_peak_brightness", std::to_string(std::clamp<std::uint32_t>(*profile_peak, 400, 2000)));
            }
          }
#endif
          config::set_runtime_config_overrides(std::move(overrides));
          runtime_overrides_applied = true;
          config::apply_config_now();
        } catch (...) {
          config::set_runtime_config_overrides(previous_runtime_overrides);
          config::apply_config_now();
          runtime_overrides_applied = false;
          throw;
        }
      }

      auto _hot_apply_gate = config::acquire_apply_read_gate();
      if (no_active_sessions) {
        config::record_active_adapter_config();
      }
      auto launch_session = make_launch_session(false, args, request, false, &identity);
      launch_session->role_generation = launch_session->id;
      launch_session->role = synthetic_control == remote_session::control_e::input ? remote_session::role_e::input : remote_session::role_e::monitor;
      launch_session->host_audio = remote_session::uses_host_audio(launch_session->role);
      launch_session->continuous_audio = false;
      if (launch_session->role == remote_session::role_e::monitor) {
        const auto mode = std::format("{}x{}@{}", launch_session->width, launch_session->height, launch_session->fps);
        const auto monitor = remote_session::activate_or_resume_monitor(
          identity.uuid,
          identity.name,
          mode,
          rtsp_stream::effective_hdr_requested(*launch_session),
          launch_session->role_generation
        );
        if (monitor.accepted) {
          // Publish retryable ownership as well as ready ownership. This makes
          // the reduced Resume/Disconnect Monitor catalogue reachable after a
          // failed apply, and the next Resume retries with a new generation.
          remember_remote_owner(identity.uuid, launch_session->role, launch_session->role_generation);
        }
        if (!monitor.ready || monitor.output.empty()) {
          tree.put("root.resume", 0);
          tree.put("root.<xmlattr>.status_code", monitor.retryable ? 503 : 500);
          tree.put("root.<xmlattr>.status_message", monitor.error.empty() ? "Remote Monitor exact capture target is not ready" : monitor.error);
          return;
        }
        launch_session->virtual_display_hdr_enabled = monitor.hdr_enabled;
        if (rtsp_stream::effective_hdr_requested(*launch_session) && !monitor.hdr_enabled) {
          launch_session->force_sdr = true;
        }
        launch_session->remote_capture_output = monitor.output;
        BOOST_LOG(info) << "Remote Monitor exact capture target for client '" << identity.uuid
                        << "' is '" << monitor.output << "'.";
      }
      stream::session::arm_shared_runtime_cleanup(launch_session->virtual_display_guid_bytes);
      if (!paired_client_uuid_enabled(launch_session->client_uuid)) {
        if (launch_session->role == remote_session::role_e::monitor) {
          remote_session::release_monitor(identity.uuid, launch_session->role_generation, "Paired client authorization revoked");
          forget_remote_owner(identity.uuid, launch_session->role, launch_session->role_generation);
#if defined(_WIN32) || defined(__linux__)
          cleanup_virtual_display_if_idle_locked();
#endif
        }
        tree.put("root.resume", 0);
        tree.put("root.<xmlattr>.status_code", 403);
        tree.put("root.<xmlattr>.status_message", "Paired client authorization was revoked before stream admission");
        return;
      }
      if (!rtsp_stream::launch_session_raise(launch_session)) {
        if (launch_session->role == remote_session::role_e::monitor) {
          remote_session::release_monitor(identity.uuid, launch_session->role_generation, "RTSP admission rejected");
          forget_remote_owner(identity.uuid, launch_session->role, launch_session->role_generation);
#if defined(_WIN32) || defined(__linux__)
          cleanup_virtual_display_if_idle_locked();
#endif
        }
        tree.put("root.resume", 0);
        tree.put("root.<xmlattr>.status_code", 409);
        tree.put("root.<xmlattr>.status_message", "RTSP pending session admission was rejected");
        return;
      }
      keep_runtime_overrides = true;
      if (launch_session->role != remote_session::role_e::monitor) {
        remember_remote_owner(identity.uuid, launch_session->role, launch_session->role_generation);
      }
      tree.put("root.<xmlattr>.status_code", 200);
      tree.put("root.sessionUrl0", std::format("{}{}:{}", launch_session->rtsp_url_scheme, net::addr_to_url_escaped_string(request->local_endpoint().address()), static_cast<int>(net::map_port(rtsp_stream::RTSP_SETUP_PORT))));
      tree.put("root.gamesession", 1);
      return;
    }
    std::unique_lock normal_transition_lock {normal_http_app_transition_mutex};
    auto requested_app = proc::proc.resolve_app(appid_str, appuuid_str);
    auto appid = requested_app ? util::from_view(requested_app->id) : util::from_view(appid_str);

    if (current_appid > 0) {
      if (remote_role_gate_snapshot_for_client(request_identity.uuid).active) {
        remote_session::clear_app_replacement_confirmation(request_identity.uuid);
        tree.put("root.resume", 0);
        tree.put("root.<xmlattr>.status_code", 409);
        tree.put(
          "root.<xmlattr>.status_message",
          "Remote Input or Remote Monitor is active; launch Terminate before starting a different app"
        );
        return;
      }
      if (!requested_app || appid <= 0) {
        tree.put("root.resume", 0);
        tree.put("root.<xmlattr>.status_code", 404);
        tree.put("root.<xmlattr>.status_message", "The requested replacement app was not found");
        return;
      }

      if (config::video.remote_monitor_confirm_app_replacement) {
        const auto active_session = proc::proc.active_session_guard();
        const auto confirmation = remote_session::arm_or_confirm_app_replacement(
          request_identity.uuid,
          active_session_generation(active_session),
          static_cast<std::int32_t>(appid)
        );
        if (confirmation == remote_session::app_replacement_confirmation_e::prompt) {
          BOOST_LOG(info) << "App replacement confirmation armed for client " << request_identity.uuid
                          << " (running_app=" << current_appid << ", requested_app=" << appid << ").";
          tree.put("root.resume", 0);
          tree.put("root.gamesession", 0);
          tree.put("root.<xmlattr>.status_code", 410);
          tree.put("root.<xmlattr>.status_message", std::string {remote_session::app_replacement_confirmation_message()});
          return;
        }
        BOOST_LOG(info) << "App replacement confirmation accepted for client " << request_identity.uuid
                        << " (running_app=" << current_appid << ", requested_app=" << appid << ").";
      } else {
        remote_session::clear_app_replacement_confirmation(request_identity.uuid);
      }

      BOOST_LOG(info) << "Replacing running app " << current_appid << " with app " << appid
                      << " at the request of paired client " << request_identity.uuid << ".";
      (void) rtsp_stream::disconnect_game_sessions(true);
      proc::proc.terminate(false, true);
    }

    host_audio = util::from_view(get_arg(args, "localAudioPlayMode"));

    const bool no_active_sessions = !has_stream_session_activity();
    const auto request_client_identity = resolve_client_identity_from_request(request);
    // Runtime overrides are global process state. Do not reapply them while
    // another RTSP session is active, otherwise a second client can mutate
    // active stream limits (e.g. fps/encoding-related settings) mid-session.
    const bool update_runtime_overrides = no_active_sessions;

    // Build the requested layer even when another stream owns the process-wide
    // runtime config. A shared capture may only be joined when its adapter pair
    // is compatible with the one that is already active.
    std::unordered_map<std::string, std::string> requested_runtime_overrides;
    if (requested_app) {
      config::merge_config_overrides(requested_runtime_overrides, requested_app->config_overrides);
    }

    std::string client_uuid = request_client_identity.uuid;
    const auto launch_client_uuid = resolve_known_client_uuid_from_launch_id(get_arg(args, "uniqueid", ""));
    if (!launch_client_uuid.empty() && launch_client_uuid != client_uuid) {
      BOOST_LOG(warning) << "Ignoring launch uniqueid for runtime overrides; TLS caller identity is authoritative.";
    }
    const auto client_settings = get_named_cert_by_uuid(client_uuid);
    if (client_settings) {
      config::merge_config_overrides(requested_runtime_overrides, client_settings->config_overrides);
    }

    if (!update_runtime_overrides &&
        !config::adapter_config_overrides_compatible_with_active(requested_runtime_overrides)) {
      BOOST_LOG(warning) << "Rejecting shared launch with a capture adapter selection that differs from the active stream.";
      tree.put("root.resume", 0);
      tree.put("root.<xmlattr>.status_code", 400);
      tree.put(
        "root.<xmlattr>.status_message",
        "Another stream is active with a different capture adapter selection"
      );
      return;
    }

    // Apply per-application runtime config overrides before we build session metadata or
    // prepare display/capture so the effective config is used everywhere.
    bool runtime_overrides_applied = false;
    bool keep_runtime_overrides = false;
    auto runtime_overrides_guard = util::fail_guard([&]() {
      if (!runtime_overrides_applied || keep_runtime_overrides) {
        return;
      }

      config::clear_runtime_config_overrides();

      // Restore global config immediately when safe; otherwise defer.
      if (!has_stream_session_activity()) {
        config::apply_config_now();
      } else {
        config::mark_deferred_reload();
      }
    });

    if (update_runtime_overrides) {
      try {
        auto overrides = requested_runtime_overrides;

#ifdef _WIN32
        // "Auto" client peak brightness follows the selected Windows HDR calibration
        // profile's MHC2 peak. An explicit app/client override remains authoritative.
        if (client_settings &&
            !client_settings->hdr_profile.empty() &&
            !overrides.contains("rtx_hdr_peak_brightness")) {
          if (const auto profile_peak = VDISPLAY::hdr_profile_peak_luminance_nits(client_settings->hdr_profile)) {
            const auto effective_peak = std::clamp<std::uint32_t>(*profile_peak, 400, 2000);
            overrides.insert_or_assign("rtx_hdr_peak_brightness", std::to_string(effective_peak));
            BOOST_LOG(info) << "HDR peak: using " << effective_peak << " nits from MHC2 profile '"
                            << client_settings->hdr_profile << "'"
                            << (*profile_peak == effective_peak ? "." : " (clamped to supported range).");
          } else {
            BOOST_LOG(warning) << "HDR peak: profile '" << client_settings->hdr_profile
                               << "' has no readable MHC2 peak; using the configured default.";
          }
        }
#endif

        config::set_runtime_config_overrides(std::move(overrides));
        runtime_overrides_applied = true;

        // Re-apply config so overrides take effect in config::video/config::input/etc.
        config::apply_config_now();
      } catch (...) {
        // If something goes wrong, fall back to global config only.
        config::clear_runtime_config_overrides();
        config::apply_config_now();
        runtime_overrides_applied = true;
      }
    } else {
      BOOST_LOG(debug) << "Launch while an RTSP/WebRTC session is already active; preserving current runtime overrides.";
    }

    // Prevent interleaving with hot-apply while we prep/start a session
    auto _hot_apply_gate = config::acquire_apply_read_gate();
    if (no_active_sessions) {
      config::record_active_adapter_config();
    }

#ifdef _WIN32
    const auto display_startup_deadline =
      std::chrono::steady_clock::now() +
      display_helper_integration::kStreamStartApplyVerificationTimeout;
    const auto display_startup_cancelled = [display_startup_deadline] {
      return std::chrono::steady_clock::now() >= display_startup_deadline;
    };
    // First step on stream start: stop any in-flight helper restore loop immediately.
    // This must happen before any other display helper work to prevent restore/crash loops on virtual displays.
    (void) display_helper_integration::disarm_pending_restore(
      display_startup_cancelled,
      display_startup_deadline
    );
#endif

    const bool allow_display_changes = true;
    auto launch_session = make_launch_session(host_audio, args, request, allow_display_changes, &request_client_identity);
    std::optional<std::string> pending_output_override;
    auto output_override_guard = util::fail_guard([&]() {
      if (pending_output_override) {
        config::set_runtime_output_name_override(std::nullopt);
      }
    });
    if (no_active_sessions) {
      config::set_runtime_output_name_override(std::nullopt);
#if defined(_WIN32) || defined(__linux__)
      stream::cancel_paused_display_cleanup();
#endif
    }

#ifdef _WIN32
    std::optional<video::encoder_probe_adapter_hint_lease_t> pending_adapter_hint;
    auto pending_adapter_hint_guard = util::fail_guard([&] {
      if (pending_adapter_hint) {
        (void) video::clear_pending_virtual_display_adapter_hint(*pending_adapter_hint);
      }
    });
    // Declare teardown before the identity guard so failure unwinds the
    // normal-role reservation first. Generic cleanup can then proceed if this
    // was the final managed identity, or remain pending for real peers.
    auto virtual_display_teardown_guard = util::fail_guard([&]() {
      stream::session::cleanup_reservation_t cleanup_reservation;
      if (has_stream_session_activity()) {
        return;
      }

      if (!launch_session->virtual_display) {
        return;
      }

      BOOST_LOG(info) << "Launch aborted before session start; removing virtual displays.";
      (void) platf::virtual_display_cleanup::run(
        "launch_aborted",
        config::video.dd.config_revert_on_disconnect
      );
    });
    auto normal_vdd_identity_guard = util::fail_guard([&] {
      if (launch_session->normal_vdd_identity_newly_reserved) {
        remote_display_topology::instance().rollback_normal_game_identity(
          launch_session->client_uuid,
          launch_session->normal_vdd_identity_token
        );
      }
    });
    prepare_virtual_display_for_session(
      launch_session,
      no_active_sessions,
      allow_display_changes,
      pending_output_override,
      pending_adapter_hint,
      display_startup_cancelled,
      display_startup_deadline
    );
    if (launch_session->normal_vdd_capacity_rejected) {
      tree.put("root.<xmlattr>.status_code", 409);
      tree.put("root.<xmlattr>.status_message", "Remote display capacity is four paired-client identities");
      tree.put("root.gamesession", 0);
      return;
    }
#elif defined(__linux__)
    const auto linux_private_display = platf::linux_private_display::prepare_session(
      *launch_session,
      no_active_sessions,
      allow_display_changes
    );
    if (!linux_private_display.active && linux_private_display.requested) {
      tree.put("root.<xmlattr>.status_code", 503);
      tree.put("root.<xmlattr>.status_message", linux_private_display.error);
      tree.put("root.gamesession", 0);
      return;
    }
    auto virtual_display_teardown_guard = util::fail_guard([&]() {
      if (!has_stream_session_activity() && launch_session->virtual_display) {
        if (remote_display_topology::instance().generic_virtual_display_cleanup_allowed()) {
          BOOST_LOG(info) << "Launch aborted before session start; restoring Linux private display state.";
          (void) platf::linux_private_display::revert();
        } else {
          BOOST_LOG(info) << "Launch aborted while another managed display identity remains; preserving its composed topology.";
        }
      }
    });
    auto normal_vdd_identity_guard = util::fail_guard([&] {
      rollback_linux_normal_display_identity(launch_session);
    });
    const auto normal_identity = reserve_linux_normal_display_identity(launch_session);
    if (normal_identity == linux_normal_identity_result_e::capacity_rejected) {
      tree.put("root.<xmlattr>.status_code", 409);
      tree.put("root.<xmlattr>.status_message", "Remote display capacity is four paired-client identities");
      tree.put("root.gamesession", 0);
      return;
    }
    if (normal_identity == linux_normal_identity_result_e::topology_failed) {
      tree.put("root.<xmlattr>.status_code", 503);
      tree.put("root.<xmlattr>.status_message", "Failed to compose the Linux private streaming displays");
      tree.put("root.gamesession", 0);
      return;
    }
    if (linux_private_display.active) {
      config::set_runtime_output_name_override(linux_private_display.output_name);
      pending_output_override = linux_private_display.output_name;
    }
#endif

    // The display should be restored in case something fails as there are no other sessions.
    if (no_active_sessions) {
      revert_display_configuration = true;

#ifdef _WIN32
      const bool helper_session_available = display_helper_session_available();
      (void) display_helper_integration::disarm_pending_restore(
        display_startup_cancelled,
        display_startup_deadline
      );
      auto request = display_helper_integration::helpers::build_request_from_session(config::video, *launch_session);
      if (!request) {
        BOOST_LOG(warning) << "Display helper: failed to build display configuration request; continuing with existing display.";
      }

      if (request) {
        display_helper_integration::ApplyVerificationTicket verification_ticket;
        const bool applied = display_helper_integration::apply(
          *request,
          &verification_ticket,
          display_startup_cancelled,
          display_helper_integration::ApplyRetryPolicy::StreamStart,
          display_startup_deadline);
        launch_session->display_config_preapplied = applied;
        if (!applied) {
          if (helper_session_available) {
            BOOST_LOG(warning) << "Display helper: failed to apply display configuration; continuing with existing display.";
          }
        } else {
          // Soft gate: capture start waits (bounded) for the helper's apply
          // verification; failures/timeouts log and proceed.
          auto gate_promise = std::make_shared<std::promise<rtsp_stream::launch_session_t::display_helper_gate_status_e>>();
          launch_session->display_helper_gate = gate_promise->get_future().share();
          BOOST_LOG(debug) << "Display helper: gating capture start on helper verification (non-blocking session start).";

          std::thread([gate_promise, verification_ticket]() {
            const auto status = display_helper_integration::wait_for_apply_verification(
              verification_ticket,
              display_helper_integration::kStreamStartApplyVerificationTimeout);
            rtsp_stream::launch_session_t::display_helper_gate_status_e gate_status =
              rtsp_stream::launch_session_t::display_helper_gate_status_e::proceed_gaveup;

            if (status == display_helper_integration::ApplyVerificationStatus::Verified) {
              gate_status = rtsp_stream::launch_session_t::display_helper_gate_status_e::proceed;
            } else if (status == display_helper_integration::ApplyVerificationStatus::Failed) {
              gate_status = rtsp_stream::launch_session_t::display_helper_gate_status_e::abort_failed;
            }

            try {
              gate_promise->set_value(gate_status);
            } catch (...) {
              // best-effort: ignore double-satisfaction
            }
          }).detach();
        }
      }

      // Apply a per-client HDR profile to physical displays (virtual displays are handled at creation time).
      const auto physical_hdr_profile_policy = display_helper_integration::request_policy::evaluate({
        .virtual_display = launch_session->virtual_display,
        .virtual_display_failed = launch_session->virtual_display_failed,
        .hdr_profile_selected = launch_session->hdr_profile && !launch_session->hdr_profile->empty(),
      });
      if (physical_hdr_profile_policy.apply_hdr_profile_to_physical) {
        const auto active_output = config::get_active_output_name();
        VDISPLAY::applyHdrProfileToOutput(
          launch_session->client_name.c_str(),
          launch_session->hdr_profile ? launch_session->hdr_profile->c_str() : nullptr,
          active_output.empty() ? nullptr : active_output.c_str()
        );
      }
#else
      display_helper_integration::DisplayApplyBuilder noop_builder;
      noop_builder.set_session(*launch_session);
      if (!display_helper_integration::apply(noop_builder.build())) {
        if (launch_session->virtual_display) {
          const std::string status_message = "Failed to activate the Linux private streaming display.";
          BOOST_LOG(error) << status_message;
          tree.put("root.<xmlattr>.status_code", 503);
          tree.put("root.<xmlattr>.status_message", status_message);
          tree.put("root.gamesession", 0);
          return;
        }
        BOOST_LOG(warning) << "Display helper: failed to apply display configuration; continuing with existing display.";
      }
#endif

      // Probe encoders again before streaming to ensure our chosen
      // encoder matches the active GPU (which could have changed
      // due to hotplugging, driver crash, primary monitor change,
      // or any number of other factors).
#ifdef _WIN32
      bool encoder_probe_failed = false;
      if (!video::has_successful_encoder_probe()) {
        {
          VDISPLAY::ensure_display_result ensure_result {};
          auto cleanup_probe_display = util::fail_guard([&ensure_result]() {
            VDISPLAY::cleanup_ensure_display(ensure_result);
          });
          if (VDISPLAY::policy::should_ensure_probe_display(launch_session->virtual_display)) {
            ensure_result = VDISPLAY::ensure_display();
            if (!ensure_result.owns_temporary_probe_request() && !ensure_result.ready_for_capture()) {
              BOOST_LOG(warning)
                << "Launch could not acquire a temporary-display lease; continuing with synthetic encoder validation.";
            }
          }

          encoder_probe_failed = video::probe_encoders();
        }
      } else {
        BOOST_LOG(debug) << "Launch encoder probe skipped (matching selected-GPU cache).";
      }
#else
      bool encoder_probe_failed = video::probe_encoders();
#endif

      if (encoder_probe_failed) {
        const std::string status_message = "Failed to initialize a video encoder on the selected adapter.";
        BOOST_LOG(error) << status_message;
        tree.put("root.<xmlattr>.status_code", 503);
        tree.put("root.<xmlattr>.status_message", status_message);
        tree.put("root.gamesession", 0);

        return;
      }
    }

    auto encryption_mode = net::encryption_mode_for_address(request->remote_endpoint().address());
    if (!launch_session->rtsp_cipher && encryption_mode == config::ENCRYPTION_MODE_MANDATORY) {
      BOOST_LOG(error) << "Rejecting client that cannot comply with mandatory encryption requirement"sv;

      tree.put("root.<xmlattr>.status_code", 403);
      tree.put("root.<xmlattr>.status_message", "Encryption is mandatory for this host but unsupported by the client");
      tree.put("root.gamesession", 0);

      return;
    }

#ifdef _WIN32
    auto pending_vulkan_hdr_layer_guard = util::fail_guard([]() {
      rtsp_stream::set_vulkan_hdr_layer_pending_stream(false);
    });
#endif
    if (appid > 0) {
#ifdef _WIN32
      rtsp_stream::set_vulkan_hdr_layer_pending_stream(rtsp_stream::effective_hdr_requested(*launch_session));
#endif
      auto err = proc::proc.execute((int) appid, launch_session);
      if (err) {
        tree.put("root.<xmlattr>.status_code", err);
        tree.put("root.<xmlattr>.status_message", "Failed to start the specified application");
        tree.put("root.gamesession", 0);

        return;
      }
    }

    // From this point forward, the app is considered started and runtime overrides (if any)
    // should remain active until the app terminates.
    keep_runtime_overrides = true;

    tree.put("root.<xmlattr>.status_code", 200);
    tree.put(
      "root.sessionUrl0",
      std::format(
        "{}{}:{}",
        launch_session->rtsp_url_scheme,
        net::addr_to_url_escaped_string(request->local_endpoint().address()),
        static_cast<int>(net::map_port(rtsp_stream::RTSP_SETUP_PORT))
      )
    );
    tree.put("root.gamesession", 1);
#ifdef _WIN32
    tree.put("root.VirtualDisplayDriverReady", proc::vDisplayDriverStatus.load(std::memory_order_acquire) == VDISPLAY::DRIVER_STATUS::OK);
#elif defined(__linux__)
    tree.put("root.VirtualDisplayDriverReady", platf::linux_private_display::ready());
#else
    tree.put("root.VirtualDisplayDriverReady", false);
#endif

    stream::session::arm_shared_runtime_cleanup(
      launch_session->virtual_display_guid_bytes
    );
    if (!paired_client_uuid_enabled(launch_session->client_uuid)) {
      if (appid > 0) {
        proc::proc.terminate(false, true);
      }
      tree.put("root.<xmlattr>.status_code", 403);
      tree.put("root.<xmlattr>.status_message", "Paired client authorization was revoked before stream admission");
      tree.put("root.gamesession", 0);
      return;
    }
    if (!rtsp_stream::launch_session_raise(launch_session)) {
      if (appid > 0) {
        proc::proc.terminate(false, true);
      }
      tree.put("root.<xmlattr>.status_code", 409);
      tree.put("root.<xmlattr>.status_message", "RTSP pending session admission was rejected");
      tree.put("root.gamesession", 0);
      return;
    }
#ifdef _WIN32
    pending_vulkan_hdr_layer_guard.disable();
#endif
#if defined(_WIN32) || defined(__linux__)
    virtual_display_teardown_guard.disable();
#endif
#if defined(_WIN32) || defined(__linux__)
    normal_vdd_identity_guard.disable();
#endif
    output_override_guard.disable();
    runtime_overrides_guard.disable();

    // Stream was started successfully, we will revert the config when the app or session terminates
    revert_display_configuration = false;
  }

  void resume(bool &host_audio, resp_https_t response, req_https_t request, int current_appid, const bool normal_app_transition, const bool launched_from_applist) {
    print_req<SunshineHTTPS>(request);

#ifdef _WIN32
    // See launch(): the response fail guard can restore through the helper
    // after the virtual-display teardown guard has already completed.
    stream::session::cleanup_reservation_t cleanup_reservation;
#endif
    pt::ptree tree;
    bool revert_display_configuration {false};
    auto g = util::fail_guard([&]() {
      std::ostringstream data;

      if (tree.empty()) {
        BOOST_LOG(error) << EMPTY_PROPERTY_TREE_ERROR_MSG;
      }

      pt::write_xml(data, tree);
      response->write(data.str());
      response->close_connection_after_response = true;

      if (revert_display_configuration) {
        display_helper_integration::revert();
      }
    });

    // proc_t::terminate() leaves the app id at -1 and nothing resets it to 0, so any
    // non-positive id means nothing is running. Comparing against 0 alone would let a
    // stale /resume run the whole resume path for an app that no longer exists.
    if (current_appid <= 0) {
      tree.put("root.resume", 0);
      tree.put("root.<xmlattr>.status_code", 503);
      tree.put("root.<xmlattr>.status_message", "No running app to resume");

      return;
    }

    auto args = request->parse_query_string();
    if (
      args.find("rikey"s) == std::end(args) ||
      args.find("rikeyid"s) == std::end(args)
    ) {
      tree.put("root.resume", 0);
      tree.put("root.<xmlattr>.status_code", 400);
      tree.put("root.<xmlattr>.status_message", "Missing a required resume parameter");

      return;
    }

    if (resolve_client_identity_from_request(request).uuid.empty()) {
      tree.put("root.resume", 0);
      tree.put("root.<xmlattr>.status_code", 403);
      tree.put("root.<xmlattr>.status_message", "A paired TLS client identity is required");
      return;
    }

    std::unique_lock normal_transition_lock {normal_http_app_transition_mutex, std::defer_lock};
    if (normal_app_transition) {
      normal_transition_lock.lock();
    }

    const bool no_active_sessions = !has_stream_session_activity();
    bool retained_game_output_ready = false;
    if (no_active_sessions) {
      if (const auto retained_output = config::runtime_output_name_override(); retained_output && !retained_output->empty()) {
        const auto capture_outputs = platf::display_names();
        retained_game_output_ready =
          std::find(capture_outputs.begin(), capture_outputs.end(), *retained_output) != capture_outputs.end();
        if (retained_game_output_ready) {
          BOOST_LOG(info) << "Resume will join the capture-ready retained game output '"
                          << *retained_output << "'.";
        }
      }
    }
    const bool joining_existing_game_output =
      remote_session::joins_existing_game_output(
        remote_session::role_e::game,
        !no_active_sessions,
        retained_game_output_ready
      );
    const auto request_client_identity = resolve_client_identity_from_request(request);

    std::unordered_map<std::string, std::string> requested_runtime_overrides;
    if (auto running_app = proc::proc.resolve_app(current_appid)) {
      config::merge_config_overrides(requested_runtime_overrides, running_app->config_overrides);
    }

    std::string client_uuid = request_client_identity.uuid;
    const auto resume_client_uuid = resolve_known_client_uuid_from_launch_id(get_arg(args, "uniqueid", ""));
    if (!resume_client_uuid.empty() && resume_client_uuid != client_uuid) {
      BOOST_LOG(warning) << "Ignoring resume uniqueid for runtime overrides; TLS caller identity is authoritative.";
    }
    if (const auto client_settings = get_named_cert_by_uuid(client_uuid)) {
      config::merge_config_overrides(requested_runtime_overrides, client_settings->config_overrides);
    }

    if (!no_active_sessions &&
        !config::adapter_config_overrides_compatible_with_active(requested_runtime_overrides)) {
      BOOST_LOG(warning) << "Rejecting shared resume with a capture adapter selection that differs from the active stream.";
      tree.put("root.resume", 0);
      tree.put("root.<xmlattr>.status_code", 400);
      tree.put(
        "root.<xmlattr>.status_message",
        "Another stream is active with a different capture adapter selection"
      );
      return;
    }

    bool runtime_overrides_reapplied = false;
    auto previous_runtime_overrides = config::runtime_config_overrides_snapshot();
    auto runtime_overrides_guard = util::fail_guard([&]() {
      if (!runtime_overrides_reapplied) {
        return;
      }
      config::set_runtime_config_overrides(std::move(previous_runtime_overrides));
      if (!has_stream_session_activity()) {
        config::apply_config_now();
      } else {
        config::mark_deferred_reload();
      }
    });

    if (no_active_sessions) {
      config::set_runtime_config_overrides(std::move(requested_runtime_overrides));
      config::apply_config_now();
      runtime_overrides_reapplied = true;
    }

    const bool allow_display_changes = config::video.dd.config_revert_on_disconnect;
    const bool allow_session_display_changes = allow_display_changes && !joining_existing_game_output;
    if (no_active_sessions && allow_display_changes) {
      config::set_runtime_output_name_override(std::nullopt);
    }
    if (no_active_sessions && args.find("localAudioPlayMode"s) != std::end(args)) {
      host_audio = util::from_view(get_arg(args, "localAudioPlayMode"));
    }
#if defined(_WIN32) || defined(__linux__)
    if (no_active_sessions) {
      stream::cancel_paused_display_cleanup();
    }
#endif
    // Prevent interleaving with hot-apply while we prep/resume a session
    auto _hot_apply_gate = config::acquire_apply_read_gate();
    if (no_active_sessions) {
      config::record_active_adapter_config();
    }

#ifdef _WIN32
    const auto display_startup_deadline =
      std::chrono::steady_clock::now() +
      display_helper_integration::kStreamStartApplyVerificationTimeout;
    const auto display_startup_cancelled = [display_startup_deadline] {
      return std::chrono::steady_clock::now() >= display_startup_deadline;
    };
    if (allow_session_display_changes) {
      // Stop any in-flight helper restore loop before resuming display changes.
      (void) display_helper_integration::disarm_pending_restore(
        display_startup_cancelled,
        display_startup_deadline
      );
    }
#endif
    const auto launch_session = make_launch_session(host_audio, args, request, allow_session_display_changes, &request_client_identity);
    if (joining_existing_game_output) {
      // A secondary game transport attaches to the display already owned by
      // the running app. Its per-client virtual-display preferences must not
      // create, reclaim, or retarget a display while another transport owns
      // the active capture source.
      launch_session->virtual_display = false;
      launch_session->client_requests_virtual_display = false;
      launch_session->client_virtual_display_override.reset();
      launch_session->virtual_display_mode_override = config::video_t::virtual_display_mode_e::disabled;
      launch_session->virtual_display_layout_override.reset();
      launch_session->dd_config_option_override.reset();
      launch_session->output_name_override.reset();
      launch_session->virtual_display_guid_bytes.fill(0);
      launch_session->virtual_display_device_id.clear();
      BOOST_LOG(info) << "Joining the running game's active capture output without preparing a per-client display.";
    }
    std::optional<std::string> pending_output_override;
    auto output_override_guard = util::fail_guard([&]() {
      if (pending_output_override) {
        config::set_runtime_output_name_override(std::nullopt);
      }
    });

#ifdef _WIN32
    std::optional<video::encoder_probe_adapter_hint_lease_t> pending_adapter_hint;
    auto pending_adapter_hint_guard = util::fail_guard([&] {
      if (pending_adapter_hint) {
        (void) video::clear_pending_virtual_display_adapter_hint(*pending_adapter_hint);
      }
    });
    auto virtual_display_teardown_guard = util::fail_guard([&]() {
      stream::session::cleanup_reservation_t cleanup_reservation;
      if (has_stream_session_activity()) {
        return;
      }

      if (!launch_session->virtual_display) {
        return;
      }

      BOOST_LOG(info) << "Resume aborted before session start; removing virtual displays.";
      (void) platf::virtual_display_cleanup::run(
        "resume_aborted",
        config::video.dd.config_revert_on_disconnect
      );
    });
    auto normal_vdd_identity_guard = util::fail_guard([&] {
      if (launch_session->normal_vdd_identity_newly_reserved) {
        remote_display_topology::instance().rollback_normal_game_identity(
          launch_session->client_uuid,
          launch_session->normal_vdd_identity_token
        );
      }
    });
    if (!joining_existing_game_output) {
      prepare_virtual_display_for_session(
        launch_session,
        no_active_sessions,
        allow_session_display_changes,
        pending_output_override,
        pending_adapter_hint,
        display_startup_cancelled,
        display_startup_deadline
      );
    }
    if (launch_session->normal_vdd_capacity_rejected) {
      tree.put("root.resume", 0);
      tree.put("root.<xmlattr>.status_code", 409);
      tree.put("root.<xmlattr>.status_message", "Remote display capacity is four paired-client identities");
      return;
    }
#elif defined(__linux__)
    const auto linux_private_display = platf::linux_private_display::prepare_session(
      *launch_session,
      no_active_sessions,
      allow_session_display_changes
    );
    if (!linux_private_display.active && linux_private_display.requested) {
      tree.put("root.resume", 0);
      tree.put("root.<xmlattr>.status_code", 503);
      tree.put("root.<xmlattr>.status_message", linux_private_display.error);
      return;
    }
    auto virtual_display_teardown_guard = util::fail_guard([&]() {
      if (!has_stream_session_activity() && launch_session->virtual_display) {
        if (remote_display_topology::instance().generic_virtual_display_cleanup_allowed()) {
          BOOST_LOG(info) << "Resume aborted before session start; restoring Linux private display state.";
          (void) platf::linux_private_display::revert();
        } else {
          BOOST_LOG(info) << "Resume aborted while another managed display identity remains; preserving its composed topology.";
        }
      }
    });
    auto normal_vdd_identity_guard = util::fail_guard([&] {
      rollback_linux_normal_display_identity(launch_session);
    });
    const auto normal_identity = reserve_linux_normal_display_identity(launch_session);
    if (normal_identity == linux_normal_identity_result_e::capacity_rejected) {
      tree.put("root.resume", 0);
      tree.put("root.<xmlattr>.status_code", 409);
      tree.put("root.<xmlattr>.status_message", "Remote display capacity is four paired-client identities");
      return;
    }
    if (normal_identity == linux_normal_identity_result_e::topology_failed) {
      tree.put("root.resume", 0);
      tree.put("root.<xmlattr>.status_code", 503);
      tree.put("root.<xmlattr>.status_message", "Failed to compose the Linux private streaming displays");
      return;
    }
    if (linux_private_display.active) {
      config::set_runtime_output_name_override(linux_private_display.output_name);
      pending_output_override = linux_private_display.output_name;
    }
#endif

    if (no_active_sessions) {
      // We want to prepare display only if there are no active sessions at
      // the moment. This should be done before probing encoders as it could
      // change the active displays.
      const bool should_apply_display_request =
#ifdef __linux__
        platf::linux_private_display::resume_policy::requires_session_apply(
          launch_session->virtual_display,
          allow_display_changes,
          launch_session->normal_vdd_identity_newly_reserved,
          launch_session->virtual_display_recreated_on_demand || launch_session->virtual_display_needs_resume_apply
        );
#else
        allow_display_changes ||
        launch_session->virtual_display_recreated_on_demand ||
        launch_session->virtual_display_needs_resume_apply;
#endif
      if (should_apply_display_request) {
        BOOST_LOG(debug) << "Display helper: applying session display request on "
                         << (allow_display_changes ? "normal start/resume" :
                                                       (launch_session->virtual_display_recreated_on_demand ?
                                                          "resume virtual-display recreation" :
                                                          "resume virtual-display refresh"))
                         << " for client '" << launch_session->client_name << "'.";
        revert_display_configuration = allow_display_changes || launch_session->virtual_display_failed;

#ifdef _WIN32
        const bool helper_session_available = display_helper_session_available();
        (void) display_helper_integration::disarm_pending_restore(
          display_startup_cancelled,
          display_startup_deadline
        );
        auto request = display_helper_integration::helpers::build_request_from_session(config::video, *launch_session);
        if (!request) {
          BOOST_LOG(warning) << "Display helper: failed to build display configuration request; continuing with existing display.";
        }

        if (request) {
          display_helper_integration::ApplyVerificationTicket verification_ticket;
          const bool applied = display_helper_integration::apply(
            *request,
            &verification_ticket,
            display_startup_cancelled,
            display_helper_integration::ApplyRetryPolicy::StreamStart,
            display_startup_deadline);
          if (!applied) {
            if (helper_session_available) {
              BOOST_LOG(warning) << "Display helper: failed to apply display configuration; continuing with existing display.";
            }
          } else {
            auto gate_promise = std::make_shared<std::promise<rtsp_stream::launch_session_t::display_helper_gate_status_e>>();
            launch_session->display_helper_gate = gate_promise->get_future().share();
            BOOST_LOG(debug) << "Display helper: gating capture start on helper verification (non-blocking session resume).";

            std::thread([gate_promise, verification_ticket]() {
              const auto status = display_helper_integration::wait_for_apply_verification(
                verification_ticket,
                display_helper_integration::kStreamStartApplyVerificationTimeout);
              rtsp_stream::launch_session_t::display_helper_gate_status_e gate_status =
                rtsp_stream::launch_session_t::display_helper_gate_status_e::proceed_gaveup;

              if (status == display_helper_integration::ApplyVerificationStatus::Verified) {
                gate_status = rtsp_stream::launch_session_t::display_helper_gate_status_e::proceed;
              } else if (status == display_helper_integration::ApplyVerificationStatus::Failed) {
                gate_status = rtsp_stream::launch_session_t::display_helper_gate_status_e::abort_failed;
              }

              try {
                gate_promise->set_value(gate_status);
              } catch (...) {
                // best-effort: ignore double-satisfaction
              }
            }).detach();
          }
        }

        // Apply a per-client HDR profile to physical displays (virtual displays are handled at creation time).
        const auto physical_hdr_profile_policy = display_helper_integration::request_policy::evaluate({
          .virtual_display = launch_session->virtual_display,
          .virtual_display_failed = launch_session->virtual_display_failed,
          .hdr_profile_selected = launch_session->hdr_profile && !launch_session->hdr_profile->empty(),
        });
        if (physical_hdr_profile_policy.apply_hdr_profile_to_physical) {
          const auto active_output = config::get_active_output_name();
          VDISPLAY::applyHdrProfileToOutput(
            launch_session->client_name.c_str(),
            launch_session->hdr_profile ? launch_session->hdr_profile->c_str() : nullptr,
            active_output.empty() ? nullptr : active_output.c_str()
          );
        }
#else
        display_helper_integration::DisplayApplyBuilder noop_builder;
        noop_builder.set_session(*launch_session);
        if (!display_helper_integration::apply(noop_builder.build())) {
          if (launch_session->virtual_display) {
            const std::string status_message = "Failed to activate the Linux private streaming display.";
            BOOST_LOG(error) << status_message;
            tree.put("root.resume", 0);
            tree.put("root.<xmlattr>.status_code", 503);
            tree.put("root.<xmlattr>.status_message", status_message);
            return;
          }
          BOOST_LOG(warning) << "Display helper: failed to apply display configuration; continuing with existing display.";
        }
#endif
      } else {
#ifdef _WIN32
        BOOST_LOG(debug) << "Display helper: skipping resume apply; only deferrals are allowed.";
#else
        BOOST_LOG(debug) << "Display helper: skipping resume apply; only deferrals are allowed.";
#endif
      }

      // Probe encoders again before streaming to ensure our chosen
      // encoder matches the active GPU (which could have changed
      // due to hotplugging, driver crash, primary monitor change,
      // or any number of other factors).
#ifdef _WIN32
      bool encoder_probe_failed = false;
      if (!video::has_successful_encoder_probe()) {
        {
          VDISPLAY::ensure_display_result ensure_result {};
          auto cleanup_probe_display = util::fail_guard([&ensure_result]() {
            VDISPLAY::cleanup_ensure_display(ensure_result);
          });
          if (VDISPLAY::policy::should_ensure_probe_display(launch_session->virtual_display)) {
            ensure_result = VDISPLAY::ensure_display();
            if (!ensure_result.owns_temporary_probe_request() && !ensure_result.ready_for_capture()) {
              BOOST_LOG(warning)
                << "Resume could not acquire a temporary-display lease; continuing with synthetic encoder validation.";
            }
          }

          encoder_probe_failed = video::probe_encoders();
        }
      } else {
        BOOST_LOG(debug) << "Resume encoder probe skipped (matching selected-GPU cache).";
      }
#else
      bool encoder_probe_failed = video::probe_encoders();
#endif

      if (encoder_probe_failed) {
        const std::string status_message = "Failed to initialize a video encoder on the selected adapter.";
        tree.put("root.resume", 0);
        tree.put("root.<xmlattr>.status_code", 503);
        tree.put("root.<xmlattr>.status_message", status_message);

        return;
      }
    }

    auto encryption_mode = net::encryption_mode_for_address(request->remote_endpoint().address());
    if (!launch_session->rtsp_cipher && encryption_mode == config::ENCRYPTION_MODE_MANDATORY) {
      BOOST_LOG(error) << "Rejecting client that cannot comply with mandatory encryption requirement"sv;

      tree.put("root.<xmlattr>.status_code", 403);
      tree.put("root.<xmlattr>.status_message", "Encryption is mandatory for this host but unsupported by the client");
      tree.put("root.gamesession", 0);

      return;
    }

    tree.put("root.<xmlattr>.status_code", 200);
    tree.put(
      "root.sessionUrl0",
      std::format(
        "{}{}:{}",
        launch_session->rtsp_url_scheme,
        net::addr_to_url_escaped_string(request->local_endpoint().address()),
        static_cast<int>(net::map_port(rtsp_stream::RTSP_SETUP_PORT))
      )
    );
    tree.put(std::string {"root."} + std::string {remote_session::stream_start_response_key(launched_from_applist)}, 1);
#ifdef _WIN32
    tree.put("root.VirtualDisplayDriverReady", proc::vDisplayDriverStatus.load(std::memory_order_acquire) == VDISPLAY::DRIVER_STATUS::OK);
#elif defined(__linux__)
    tree.put("root.VirtualDisplayDriverReady", platf::linux_private_display::ready());
#else
    tree.put("root.VirtualDisplayDriverReady", false);
#endif

    stream::session::arm_shared_runtime_cleanup(
      launch_session->virtual_display_guid_bytes
    );
    if (!paired_client_uuid_enabled(launch_session->client_uuid)) {
      tree.put("root.resume", 0);
      tree.put("root.<xmlattr>.status_code", 403);
      tree.put("root.<xmlattr>.status_message", "Paired client authorization was revoked before stream admission");
      return;
    }
    if (!rtsp_stream::launch_session_raise(launch_session)) {
      tree.put("root.resume", 0);
      tree.put("root.<xmlattr>.status_code", 409);
      tree.put("root.<xmlattr>.status_message", "RTSP pending session admission was rejected");
      return;
    }
#if defined(_WIN32) || defined(__linux__)
    virtual_display_teardown_guard.disable();
#endif
#if defined(_WIN32) || defined(__linux__)
    normal_vdd_identity_guard.disable();
#endif
    output_override_guard.disable();
    runtime_overrides_guard.disable();
    revert_display_configuration = false;
  }

  void cancel(resp_https_t response, req_https_t request) {
    print_req<SunshineHTTPS>(request);

#ifdef _WIN32
    // Reserve cleanup before sampling process/session state and retain it
    // through session teardown, app termination, and final display cleanup.
    stream::session::cleanup_reservation_t cleanup_reservation;
#endif

    pt::ptree tree;
    auto g = util::fail_guard([&]() {
      std::ostringstream data;

      pt::write_xml(data, tree);
      response->write(data.str());
      response->close_connection_after_response = true;
    });

    const auto identity = resolve_client_identity_from_request(request);
    if (identity.uuid.empty()) {
      tree.put("root.cancel", 0);
      tree.put("root.<xmlattr>.status_code", 403);
      tree.put("root.<xmlattr>.status_message", "A paired TLS client identity is required");
      return;
    }

    const bool has_running_app = proc::proc.running() > 0;
    const auto active_session = proc::proc.active_session_guard();
    const remote_session::caller_t caller {
      .uuid = identity.uuid,
      .paired = true,
      .may_terminate = true,
    };
    const remote_session::game_t game {
      .running = has_running_app,
      .owner_uuid = active_session.client_uuid,
      .generation = active_session_generation(active_session),
    };
    const bool remote_sessions_active = remote_role_gate_snapshot_for_client(identity.uuid).active;
    if (!remote_session::allows_normal_game_cancel(caller, game, remote_sessions_active)) {
      tree.put("root.cancel", 0);
      tree.put("root.<xmlattr>.status_code", 403);
      tree.put(
        "root.<xmlattr>.status_message",
        remote_sessions_active ?
          "Only the configured running-game owner may cancel this game while Remote Input or Remote Monitor is active" :
          "No running app is available to cancel"
      );
      return;
    }
    remote_session::clear_app_replacement_confirmation(identity.uuid);

    tree.put("root.cancel", 1);
    tree.put("root.<xmlattr>.status_code", 200);

#ifdef _WIN32
    const bool preserve_deferred_launch =
      has_running_app &&
      proc::proc.is_launch_deferred() &&
      rtsp_stream::session_count_no_cleanup() == 0;
    if (preserve_deferred_launch) {
      BOOST_LOG(info) << "Cancel requested while app launch is deferred; preserving deferred app and virtual display state.";
    }
#else
    constexpr bool preserve_deferred_launch = false;
#endif
    rtsp_stream::terminate_sessions(preserve_deferred_launch);

    if (has_running_app && !preserve_deferred_launch) {
      proc::proc.terminate();
    }
    // The config needs to be reverted regardless of whether "proc::proc.terminate()" was called or not.

#ifdef _WIN32
    if (!preserve_deferred_launch) {
      // RTSP session termination above is synchronous, so by the time we reach
      // this point the old session threads have already completed their joins.
      cleanup_virtual_display_if_idle();
    }
#endif
  }

  void appasset(resp_https_t response, req_https_t request) {
    print_req<SunshineHTTPS>(request);

    auto args = request->parse_query_string();
    const auto appid = get_arg(args, "appid", "0");
    const auto appuuid = get_arg(args, "appuuid", "");
    const auto current_appid = proc::proc.running();
    const auto synthetic_control = remote_session::identify(util::from_view(appid), appuuid, current_appid);
    auto app_ctx = proc::proc.resolve_app(appid, appuuid);
    std::string app_image;
    if (app_ctx) {
      app_image = proc::validate_app_image_path(app_ctx->image_path);
    } else if (synthetic_control == remote_session::control_e::running_game) {
      if (const auto running_app = proc::proc.resolve_app(current_appid)) {
        app_image = proc::validate_app_image_path(running_app->image_path);
      }
    } else if (const auto artwork = remote_session::synthetic_artwork_filename(synthetic_control)) {
      app_image = (fs::path {SUNSHINE_ASSETS_DIR} / "remote-session" / std::string {*artwork}).string();
    } else {
      app_image = proc::proc.get_app_image((int) util::from_view(appid));
    }

    auto image = proc::read_validated_app_image(app_image);
    if (!image) {
      image = proc::read_validated_app_image(DEFAULT_APP_IMAGE_PATH);
    }
    SimpleWeb::CaseInsensitiveMultimap headers;
    headers.emplace("Content-Type", "image/png");
    response->write(SimpleWeb::StatusCode::success_ok, image.value_or(std::string {}), headers);
    response->close_connection_after_response = true;
  }

  void setBitrate(resp_https_t response, req_https_t request) {
    print_req<SunshineHTTPS>(request);

    pt::ptree tree;
    auto g = util::fail_guard([&]() {
      std::ostringstream data;
      pt::write_xml(data, tree);
      response->write(data.str());
      response->close_connection_after_response = true;
    });

    const auto identity = resolve_client_identity_from_request(request);
    if (identity.uuid.empty()) {
      BOOST_LOG(warning) << "Bitrate change rejected: could not resolve a paired client identity"sv;
      tree.put("root.bitrate", 0);
      tree.put("root.<xmlattr>.status_code", 403);
      tree.put("root.<xmlattr>.status_message", "Client not recognized");
      return;
    }

    auto args = request->parse_query_string();
    const int requested = (int) util::from_view(get_arg(args, "bitrate", "0"));
    if (requested <= 0) {
      tree.put("root.bitrate", 0);
      tree.put("root.<xmlattr>.status_code", 400);
      tree.put("root.<xmlattr>.status_message", "Missing or invalid bitrate parameter");
      return;
    }

    // Clamp to the host bitrate ceiling. max_bitrate == 0 means "unlimited" in config, so also
    // enforce an absolute ceiling to keep the value sane and avoid overflow downstream
    // (bitrate_kbps * 1000 must fit the encoder's 32-bit rate-control fields).
    constexpr int absolute_max_bitrate_kbps = 500000;  // 500 Mbps
    int applied = requested;
    if (config::video.max_bitrate > 0 && applied > config::video.max_bitrate) {
      applied = config::video.max_bitrate;
    }
    if (applied > absolute_max_bitrate_kbps) {
      applied = absolute_max_bitrate_kbps;
    }
    if (applied != requested) {
      BOOST_LOG(info) << "Clamped requested bitrate "sv << requested << " kbps to "sv << applied << " kbps"sv;
    }

    const int updated = stream::set_bitrate_for_sessions(identity.uuid, applied);
    if (updated <= 0) {
      BOOST_LOG(warning) << "Bitrate change requested by ["sv << identity.name << "] but no matching active session was found"sv;
      tree.put("root.bitrate", 0);
      tree.put("root.<xmlattr>.status_code", 404);
      tree.put("root.<xmlattr>.status_message", "No active session for this client");
      return;
    }

    BOOST_LOG(info) << "Client ["sv << identity.name << "] set runtime bitrate to "sv << applied << " kbps ("sv << updated << " session(s))"sv;
    tree.put("root.bitrate", applied);
    tree.put("root.<xmlattr>.status_code", 200);
  }

  void getAbrCapabilities(resp_https_t response, req_https_t request) {
    print_req<SunshineHTTPS>(request);

    // Server-side adaptive bitrate decisioning is not implemented. Reporting it unsupported makes
    // Foundation-compatible clients (e.g. Moonlight V+) drive their own local ABR controller, which
    // applies decisions through the runtime /bitrate endpoint above.
    const std::string body = R"({"supported":false,"version":1,"features":["runtime_bitrate"]})";
    SimpleWeb::CaseInsensitiveMultimap headers;
    headers.emplace("Content-Type", "application/json");
    response->write(SimpleWeb::StatusCode::success_ok, body, headers);
    response->close_connection_after_response = true;
  }

  void setup(const std::string &pkey, const std::string &cert) {
    conf_intern.pkey = pkey;
    conf_intern.servercert = cert;
  }

  void start() {
    platf::set_thread_name("nvhttp");
#if defined(_WIN32) || defined(__linux__)
    // The listeners below can accept /launch as soon as they are started.
    // Install the concrete coordinator callbacks before exposing that route.
    register_remote_monitor_runtime();
#endif
    auto shutdown_event = mail::man->event<bool>(mail::shutdown);

    auto port_http = net::map_port(PORT_HTTP);
    auto port_https = net::map_port(PORT_HTTPS);
    auto address_family = net::af_from_enum_string(config::sunshine.address_family);

    bool clean_slate = config::sunshine.flags[config::flag::FRESH_STATE];

    if (!clean_slate) {
      if (!load_state()) {
        // Do not expose a newly generated uniqueid when durable pairing state
        // is unavailable. The controller will retry after the filesystem or
        // profile issue is repaired, while the recovery copy remains intact.
        BOOST_LOG(fatal) << "HTTP interface is stopping because durable pairing state could not be loaded."sv;
        shutdown_event->raise(true);
        return;
      }
    } else {
      // FRESH_STATE is an explicit, non-persistent test/reset mode. It is the
      // only path allowed to operate without a durable state snapshot.
      authorization_state_ready.store(true, std::memory_order_release);
    }

    auto pkey = file_handler::read_file(config::nvhttp.pkey.c_str());
    auto cert = file_handler::read_file(config::nvhttp.cert.c_str());
    setup(pkey, cert);

    auto add_cert = std::make_shared<safe::queue_t<crypto::x509_t>>(30);

    // resume doesn't always get the parameter "localAudioPlayMode"
    // launch will store it in host_audio
    bool host_audio {};

    https_server_t https_server {config::nvhttp.cert, config::nvhttp.pkey};
    http_server_t http_server;
    thread_pool_util::ThreadPool blocking_route_pool;
    blocking_route_pool.start(1);
    // Discovery routes are observation-only, so they must not queue behind the mutating
    // routes. A launch/resume/cancel handler can hold the lifecycle gate across unbounded
    // work, and on a single FIFO worker that made the host undiscoverable until restart.
    thread_pool_util::ThreadPool discovery_route_pool;
    discovery_route_pool.start(1);

    // Verify certificates after establishing connection
    https_server.verify = [add_cert](SSL *ssl, const boost::asio::ip::tcp::endpoint &remote_endpoint) {
      forget_tls_client_identity(remote_endpoint);

      crypto::x509_t x509_verify {
#if OPENSSL_VERSION_MAJOR >= 3
        SSL_get1_peer_certificate(ssl)
#else
        SSL_get_peer_certificate(ssl)
#endif
      };

      if (!x509_verify) {
        BOOST_LOG(info) << "unknown -- denied"sv;
        return 0;
      }

      int verified = 0;

      auto fg = util::fail_guard([&]() {
        const auto subject_name = cert_subject_name_for_log(x509_verify);

        BOOST_LOG(verbose) << subject_name << " -- "sv << (verified ? "verified"sv : "denied"sv);
      });

      const char *err_str = nullptr;
      std::optional<resolved_client_identity_t> identity;
      {
        std::lock_guard<std::mutex> lock(client_mutex);
        while (add_cert->peek()) {
          auto cert = add_cert->pop();
          const auto subject_name = cert_subject_name_for_log(cert);

          BOOST_LOG(verbose) << "Added cert ["sv << subject_name << ']';
          cert_chain.add(std::move(cert));
        }

        err_str = cert_chain.verify(x509_verify.get());
        if (!err_str) {
          identity = resolve_client_identity_from_peer_cert_locked(x509_verify);
          if (!identity) {
            err_str = "Client certificate is not one exact enabled pairing record";
          } else {
            // Keep the client record and endpoint cache coherent with disable
            // and unpair, which take the same client -> TLS-cache lock order.
            remember_tls_client_identity(remote_endpoint, *identity);
          }
        }
      }
      if (err_str) {
        BOOST_LOG(warning) << "SSL Verification error :: "sv << err_str;

        return verified;
      }

      verified = 1;
      return verified;
    };

    https_server.on_verify_failed = [](resp_https_t resp, req_https_t req) {
      pt::ptree tree;
      auto g = util::fail_guard([&]() {
        std::ostringstream data;

        pt::write_xml(data, tree);
        resp->write(data.str());
        resp->close_connection_after_response = true;
      });

      tree.put("root.<xmlattr>.status_code"s, 401);
      tree.put("root.<xmlattr>.query"s, req->path);
      tree.put("root.<xmlattr>.status_message"s, "The client is not authorized. Certificate verification failed."s);
    };

    auto run_on_blocking_pool = [](thread_pool_util::ThreadPool &pool, auto task) {
      pool.push([task = std::move(task)]() mutable {
        try {
          task();
        } catch (const std::exception &e) {
          BOOST_LOG(error) << "Blocking NVHTTP handler failed: " << e.what();
        } catch (...) {
          BOOST_LOG(error) << "Blocking NVHTTP handler failed with an unknown exception";
        }
      });
    };

    auto run_blocking_nvhttp = [&blocking_route_pool, run_on_blocking_pool](auto task) {
      run_on_blocking_pool(blocking_route_pool, std::move(task));
    };

    auto run_discovery_nvhttp = [&discovery_route_pool, run_on_blocking_pool](auto task) {
      run_on_blocking_pool(discovery_route_pool, std::move(task));
    };

    https_server.default_resource["GET"] = not_found<SunshineHTTPS>;
    https_server.default_resource["POST"] = not_found<SunshineHTTPS>;
    https_server.resource["^/serverinfo$"]["GET"] = [run_discovery_nvhttp](auto resp, auto req) {
      run_discovery_nvhttp([resp = std::move(resp), req = std::move(req)]() mutable {
        serverinfo<SunshineHTTPS>(std::move(resp), std::move(req));
      });
    };
    https_server.resource["^/pair/?$"]["GET"] = [&add_cert](auto resp, auto req) {
      pair<SunshineHTTPS>(add_cert, resp, req);
    };
    https_server.resource["^/pair/?$"]["POST"] = [&add_cert](auto resp, auto req) {
      pair<SunshineHTTPS>(add_cert, resp, req);
    };
    https_server.resource["^/unpair/?$"]["GET"] = unpair<SunshineHTTPS>;
    https_server.resource["^/unpair/?$"]["POST"] = unpair<SunshineHTTPS>;
    https_server.resource["^/applist$"]["GET"] = [run_discovery_nvhttp](auto resp, auto req) {
      run_discovery_nvhttp([resp = std::move(resp), req = std::move(req)]() mutable {
        applist(std::move(resp), std::move(req));
      });
    };
    https_server.resource["^/appasset$"]["GET"] = appasset;
    https_server.resource["^/launch$"]["GET"] = [&host_audio, run_blocking_nvhttp](auto resp, auto req) {
      run_blocking_nvhttp([&host_audio, resp = std::move(resp), req = std::move(req)]() mutable {
        (void) proc::proc.running();
        auto lifecycle_lock = acquire_stream_start_lifecycle_lock();
        const int current_appid = proc::proc.current_app_id();
        launch(host_audio, std::move(resp), std::move(req), current_appid);
      });
    };
    https_server.resource["^/resume$"]["GET"] = [&host_audio, run_blocking_nvhttp](auto resp, auto req) {
      run_blocking_nvhttp([&host_audio, resp = std::move(resp), req = std::move(req)]() mutable {
        (void) proc::proc.running();
        auto lifecycle_lock = acquire_stream_start_lifecycle_lock();
        const int current_appid = proc::proc.current_app_id();
        resume(host_audio, std::move(resp), std::move(req), current_appid);
      });
    };
    https_server.resource["^/cancel$"]["GET"] = [run_blocking_nvhttp](auto resp, auto req) {
      run_blocking_nvhttp([resp = std::move(resp), req = std::move(req)]() mutable {
        std::lock_guard lock {launch_request_mutex};
        cancel(std::move(resp), std::move(req));
      });
    };
    https_server.resource["^/bitrate$"]["GET"] = setBitrate;
    https_server.resource["^/api/abr/capabilities$"]["GET"] = getAbrCapabilities;

    https_server.config.reuse_address = true;
    https_server.config.max_request_streambuf_size = 256U * 1024U;
    https_server.config.address = net::get_bind_address(address_family);
    https_server.config.port = port_https;

    http_server.default_resource["GET"] = not_found<SimpleWeb::HTTP>;
    http_server.default_resource["POST"] = not_found<SimpleWeb::HTTP>;
    http_server.resource["^/serverinfo$"]["GET"] = [run_discovery_nvhttp](auto resp, auto req) {
      run_discovery_nvhttp([resp = std::move(resp), req = std::move(req)]() mutable {
        serverinfo<SimpleWeb::HTTP>(std::move(resp), std::move(req));
      });
    };
    http_server.resource["^/pair/?$"]["GET"] = [&add_cert](auto resp, auto req) {
      pair<SimpleWeb::HTTP>(add_cert, resp, req);
    };
    http_server.resource["^/pair/?$"]["POST"] = [&add_cert](auto resp, auto req) {
      pair<SimpleWeb::HTTP>(add_cert, resp, req);
    };
    http_server.resource["^/unpair/?$"]["GET"] = unpair<SimpleWeb::HTTP>;
    http_server.resource["^/unpair/?$"]["POST"] = unpair<SimpleWeb::HTTP>;

    http_server.config.reuse_address = true;
    http_server.config.max_request_streambuf_size = 256U * 1024U;
    http_server.config.address = net::get_bind_address(address_family);
    http_server.config.port = port_http;

    auto accept_and_run = [&](auto *http_server) {
      try {
        std::string name = "nvhttp::" + std::to_string(http_server->config.port);
        platf::set_thread_name(name);
        http_server->start();
      } catch (boost::system::system_error &err) {
        // It's possible the exception gets thrown after calling http_server->stop() from a different thread
        if (shutdown_event->peek()) {
          return;
        }

        BOOST_LOG(fatal) << "Couldn't start http server on ports ["sv << port_https << ", "sv << port_https << "]: "sv << err.what();
        shutdown_event->raise(true);
        return;
      }
    };
    std::thread ssl {accept_and_run, &https_server};
    std::thread tcp {accept_and_run, &http_server};
    std::jthread pairing_expiry_worker([](std::stop_token stop_token) {
      platf::set_thread_name("pair_expiry");
      while (!stop_token.stop_requested()) {
        for (int interval = 0; interval < 10 && !stop_token.stop_requested(); ++interval) {
          std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        if (stop_token.stop_requested()) {
          break;
        }
        std::lock_guard pairing_lock {pairing_sessions_mutex};
        expire_pairing_sessions_locked(std::chrono::steady_clock::now());
      }
    });

    // Wait for any event
    shutdown_event->view();

#ifdef __linux__
    if (const char *machine_host = std::getenv("VIBESHINE_MACHINE_HOST"); machine_host && machine_host[0] == '1' && machine_host[1] == '\0') {
      // Publish the handoff fence before any session destructor can recompose,
      // restore, or disconnect a compositor-owned output.
      platf::linux_private_display::request_process_shutdown_preserve();
    }
#endif

    pairing_expiry_worker.request_stop();
    pairing_expiry_worker.join();

    https_server.stop();
    http_server.stop();

    ssl.join();
    tcp.join();
    blocking_route_pool.stop();
    blocking_route_pool.join();
    discovery_route_pool.stop();
    discovery_route_pool.join();
    rtsp_stream::terminate_sessions(false);
    remote_session::notify_monitor_shutdown();
#if defined(_WIN32) || defined(__linux__)
    cleanup_virtual_display_if_idle();
#endif
    remote_session::register_monitor_runtime_hooks({});
  }

  bool erase_all_clients() {
    if (!config::sunshine.flags[config::flag::FRESH_STATE] &&
        !authorization_state_ready.load(std::memory_order_acquire)) {
      BOOST_LOG(error) << "Refusing to erase pairing state because durable state is unavailable."sv;
      return false;
    }

    std::vector<named_cert_t> clients;
    {
      std::lock_guard<std::mutex> lock(client_mutex);
      clients = std::move(client_root.named_devices);
      client_root = client_t {};
      cert_chain.clear();
    }
    clear_tls_client_identities();
    // Every launch/resume handler holds this gate from before identity lookup
    // through pending RTSP insertion. Clearing the authority first and then
    // crossing the gate is a non-blocking admission barrier: earlier handlers
    // finish, later handlers fail live revalidation, and teardown joins happen
    // only after the gate is released.
    {
      auto admission_barrier = acquire_stream_start_lifecycle_lock();
    }
    for (const auto &client : clients) {
      (void) rtsp_stream::disconnect_client_sessions(client.uuid);
      remote_session::notify_monitor_unpair(client.uuid);
      forget_remote_client(client.uuid);
    }
#if defined(_WIN32) || defined(__linux__)
    cleanup_virtual_display_if_idle();
#endif
    return save_state();
  }

  bool update_device_info(
    const std::string &uuid,
    const std::string &name,
    const std::string &display_mode,
    const std::string &output_name_override,
    const bool always_use_virtual_display,
    const std::string &virtual_display_mode,
    const std::string &virtual_display_layout,
    std::optional<std::unordered_map<std::string, std::string>> config_overrides,
    const bool prefer_10bit_sdr,
    const std::optional<std::string> hdr_profile
  ) {
    if (uuid.empty()) {
      return false;
    }

    const auto trimmed_name = boost::algorithm::trim_copy(name);
    if (trimmed_name.size() > pairing_policy::max_paired_client_name_length) {
      BOOST_LOG(warning) << "Refusing oversized paired client name for '" << uuid << "'.";
      return false;
    }
    if (is_placeholder_client_name(trimmed_name)) {
      BOOST_LOG(warning) << "Refusing to update paired client '" << uuid << "' to reserved name '" << trimmed_name << "'.";
      return false;
    }
    const auto trimmed_display_mode = boost::algorithm::trim_copy(display_mode);
    const auto trimmed_output_override = boost::algorithm::trim_copy(output_name_override);
    const auto trimmed_vd_mode = boost::algorithm::trim_copy(virtual_display_mode);
    const auto trimmed_vd_layout = boost::algorithm::trim_copy(virtual_display_layout);
    if (config_overrides) {
      std::unordered_map<std::string, std::string> normalized_overrides;
      config::merge_config_overrides(normalized_overrides, *config_overrides);
      config_overrides = std::move(normalized_overrides);
    }

    bool updated = false;
    {
      std::lock_guard<std::mutex> lock(client_mutex);
      for (auto &named_cert : client_root.named_devices) {
        if (named_cert.uuid != uuid) {
          continue;
        }

        named_cert.name = trimmed_name;
        named_cert.display_mode = trimmed_display_mode;
        named_cert.always_use_virtual_display = always_use_virtual_display;
        named_cert.output_name_override = always_use_virtual_display ? "" : trimmed_output_override;
        named_cert.virtual_display_mode_override = trimmed_vd_mode;
        named_cert.virtual_display_layout_override = trimmed_vd_layout;
        named_cert.prefer_10bit_sdr = prefer_10bit_sdr;
        if (config_overrides) {
          named_cert.config_overrides = std::move(*config_overrides);
        }
        if (hdr_profile.has_value()) {
          named_cert.hdr_profile = boost::algorithm::trim_copy(*hdr_profile);
        }
        updated = true;
        break;
      }
    }

    if (updated) {
      save_state();
    }
    return updated;
  }

  bool set_client_hdr_profile(const std::string &uuid, const std::string &hdr_profile) {
    if (uuid.empty()) {
      return false;
    }

    const auto trimmed_hdr_profile = boost::algorithm::trim_copy(hdr_profile);

    bool updated = false;
    {
      std::lock_guard<std::mutex> lock(client_mutex);
      for (auto &named_cert : client_root.named_devices) {
        if (named_cert.uuid != uuid) {
          continue;
        }

        named_cert.hdr_profile = trimmed_hdr_profile;
        updated = true;
        break;
      }
    }

    if (updated) {
      save_state();
    }
    return updated;
  }

  void revoke_paired_client_access(const std::string_view uuid) {
    forget_tls_client_identities_for_uuid(uuid);
    // The caller has already disabled or removed the live pairing record.
    // Cross the launch/resume gate before scanning sessions so any handler
    // that copied the old identity either fails its final live recheck or has
    // published a pending session that the scan below will remove. Never hold
    // this gate across the potentially blocking stream joins.
    {
      auto admission_barrier = acquire_stream_start_lifecycle_lock();
    }
    (void) rtsp_stream::disconnect_client_sessions(std::string {uuid});
    remote_session::notify_monitor_unpair(uuid);
    forget_remote_client(uuid);
#if defined(_WIN32) || defined(__linux__)
    cleanup_virtual_display_if_idle();
#endif
  }

  bool set_client_enabled(std::string_view uuid, bool enabled) {
    bool updated = false;
    bool previously_enabled = false;
    bool persisted = false;
    const bool fresh_state = config::sunshine.flags[config::flag::FRESH_STATE];
    if (!fresh_state && !authorization_state_ready.load(std::memory_order_acquire)) {
      BOOST_LOG(error) << "Refusing to change pairing authorization because durable state is unavailable."sv;
      return false;
    }
    if (!fresh_state) {
      statefile::migrate_recent_state_keys();
    }
    {
      std::lock_guard<std::mutex> state_lock(statefile::state_mutex());
      std::lock_guard<std::mutex> client_lock(client_mutex);
      for (auto &named_cert : client_root.named_devices) {
        if (named_cert.uuid == uuid) {
          previously_enabled = named_cert.enabled;
          named_cert.enabled = enabled;
          updated = true;
          break;
        }
      }
      if (!updated) {
        return false;
      }

      persisted = fresh_state || save_state_snapshot_locked(client_root);
      if (!persisted && enabled && !previously_enabled) {
        for (auto &named_cert : client_root.named_devices) {
          if (named_cert.uuid == uuid) {
            named_cert.enabled = false;
            break;
          }
        }
      }
    }
    if (!enabled || (!persisted && !previously_enabled)) {
      revoke_paired_client_access(uuid);
    }
    return persisted;
  }

  bool has_client_uuid(std::string_view uuid) {
    std::lock_guard<std::mutex> lock(client_mutex);
    for (const auto &named_cert : client_root.named_devices) {
      if (named_cert.uuid == uuid) {
        return true;
      }
    }
    return false;
  }

  std::string get_cert_by_uuid(std::string_view uuid) {
    std::lock_guard<std::mutex> lock(client_mutex);
    for (const auto &named_cert : client_root.named_devices) {
      if (named_cert.uuid == uuid) {
        return named_cert.cert;
      }
    }
    return {};
  }

  bool disconnect_client(const std::string &uuid) {
    // Capture the generation before stopping transport. The join path may
    // retain it for Resume, while a newer launch admitted after this point
    // must never be released by this disconnect request.
    const auto monitor_generation = config::video.remote_monitor_disconnect_on_client_disconnect ?
                                      remote_owner_generation(uuid, remote_session::role_e::monitor) :
                                      std::nullopt;
    const auto disconnect = rtsp_stream::disconnect_client_sessions_with_result(uuid);
    // The result is the pending-map removal linearization point. Never look
    // up the current Input owner here: a newer generation may have been
    // admitted after the RTSP critical section and must survive.
    std::vector<rtsp_stream::pending_policy::pending_owner_t> removed;
    for (std::size_t i = 0; i < disconnect.pending_roles.size(); ++i) {
      removed.push_back({.role = disconnect.pending_roles[i], .client_uuid = uuid, .generation = disconnect.pending_generations[i]});
    }
    for (const auto &owner : rtsp_stream::pending_policy::disconnect_input_owners_to_forget(removed)) {
      forget_remote_owner(owner.client_uuid, owner.role, owner.generation);
    }

    bool monitor_disconnected = false;
    if (monitor_generation) {
      std::unique_lock lifecycle_lock {stream_lifecycle_mutex()};
      // An active session may already have released this generation while it
      // joined above. Recheck under the lifecycle gate so this path handles
      // only retained/pending ownership and never repeats or reaches into a
      // newer Remote Monitor launch.
      if (remote_owner_generation(uuid, remote_session::role_e::monitor) != monitor_generation) {
        return disconnect.disconnected;
      }
      remote_session::release_monitor(uuid, *monitor_generation, "Paired client disconnected");
      forget_remote_owner(uuid, remote_session::role_e::monitor, *monitor_generation);
#if defined(_WIN32) || defined(__linux__)
      cleanup_virtual_display_if_idle_locked();
#endif
      monitor_disconnected = true;
    }
    return disconnect.disconnected || monitor_disconnected;
  }

  bool get_client_prefer_10bit_sdr(const std::string &uuid) {
    std::lock_guard<std::mutex> lock(client_mutex);
    for (const auto &named_cert : client_root.named_devices) {
      if (named_cert.uuid == uuid) {
        return named_cert.prefer_10bit_sdr;
      }
    }
    return false;
  }

  std::unordered_map<std::string, std::string> get_client_config_overrides(const std::string &uuid) {
    std::lock_guard<std::mutex> lock(client_mutex);
    for (const auto &named_cert : client_root.named_devices) {
      if (named_cert.uuid == uuid) {
        auto overrides = named_cert.config_overrides;
#ifdef _WIN32
        if (!named_cert.hdr_profile.empty() && !overrides.contains("rtx_hdr_peak_brightness")) {
          if (const auto profile_peak = VDISPLAY::hdr_profile_peak_luminance_nits(named_cert.hdr_profile)) {
            const auto effective_peak = std::clamp<std::uint32_t>(*profile_peak, 400, 2000);
            overrides.insert_or_assign("rtx_hdr_peak_brightness", std::to_string(effective_peak));
          }
        }
#endif
        return overrides;
      }
    }
    return {};
  }

  // (Windows-only) display_helper_integration is included above

  bool unpair_client(const std::string_view uuid) {
    if (!config::sunshine.flags[config::flag::FRESH_STATE] &&
        !authorization_state_ready.load(std::memory_order_acquire)) {
      BOOST_LOG(error) << "Refusing to remove pairing state because durable state is unavailable."sv;
      return false;
    }

    bool removed = false;
    {
      std::lock_guard<std::mutex> lock(client_mutex);
      for (auto it = client_root.named_devices.begin(); it != client_root.named_devices.end();) {
        if ((*it).uuid == uuid) {
          it = client_root.named_devices.erase(it);
          removed = true;
        } else {
          ++it;
        }
      }
      if (removed) {
        std::vector<std::string> certificate_identities;
        std::vector<pairing_policy::paired_client_record_view_t> records;
        std::vector<crypto::x509_t> parsed_certificates;
        cert_chain.clear();
        if (build_paired_client_records(client_root, certificate_identities, records, &parsed_certificates)) {
          for (auto &certificate : parsed_certificates) {
            cert_chain.add(std::move(certificate));
          }
        } else {
          BOOST_LOG(error) << "Pairing state became invalid while rebuilding authorization after unpair; denying all TLS clients.";
        }
      }
    }

    if (removed) {
      revoke_paired_client_access(uuid);
      const bool persisted = config::sunshine.flags[config::flag::FRESH_STATE] || save_state();
      return persisted;
    }
    return false;
  }
}  // namespace nvhttp

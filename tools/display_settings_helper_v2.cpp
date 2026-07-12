/**
 * @file tools/display_settings_helper_v2.cpp
 * @brief Display helper v2 engine: modern FSM-based engine with the legacy
 *        helper's battle-tested restore semantics.
 */
#ifdef _WIN32

  #include <algorithm>
  #include <atomic>
  #include <cctype>
  #include <chrono>
  #include <cstdint>
  #include <cstring>
  #include <filesystem>
  #include <fstream>
  #include <memory>
  #include <mutex>
  #include <optional>
  #include <set>
  #include <span>
  #include <string>
  #include <thread>
  #include <type_traits>
  #include <utility>
  #include <vector>

  #ifndef WIN32_LEAN_AND_MEAN
    #define WIN32_LEAN_AND_MEAN
  #endif
  #include "src/logging.h"
  #include "src/platform/windows/display_helper_v2/async_dispatcher.h"
  #include "src/platform/windows/display_helper_v2/golden_health.h"
  #include "src/platform/windows/display_helper_v2/operations.h"
  #include "src/platform/windows/display_helper_v2/runtime_support.h"
  #include "src/platform/windows/display_helper_v2/snapshot.h"
  #include "src/platform/windows/display_helper_v2/snapshot_codec.h"
  #include "src/platform/windows/display_helper_v2/state_machine.h"
  #include "src/platform/windows/display_helper_v2/win_display_settings.h"
  #include "src/platform/windows/display_helper_v2/win_event_pump.h"
  #include "src/platform/windows/display_helper_v2/win_platform_workarounds.h"
  #include "src/platform/windows/display_helper_v2/win_scheduled_task_manager.h"
  #include "src/platform/windows/display_helper_v2/win_virtual_display_driver.h"
  #include "src/platform/windows/ipc/display_settings_protocol.h"
  #include "src/platform/windows/ipc/pipes.h"
  #include "tools/display_helper_paths.h"

  #include <display_device/json.h>
  #include <display_device/logging.h>
  #include <nlohmann/json.hpp>
  #include <windows.h>
  #include <winsock2.h>

namespace {
  using MsgType = platf::display_helper_protocol::MsgType;
  using ResultStatus = platf::display_helper_protocol::ResultStatus;

  std::optional<int> parse_log_level_value(const char *value) {
    if (!value || *value == '\0') {
      return std::nullopt;
    }

    std::string lower(value);
    std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char ch) {
      return static_cast<char>(std::tolower(ch));
    });

    if (lower == "verbose") {
      return 0;
    }
    if (lower == "debug") {
      return 1;
    }
    if (lower == "info") {
      return 2;
    }
    if (lower == "warning") {
      return 3;
    }
    if (lower == "error") {
      return 4;
    }
    if (lower == "fatal") {
      return 5;
    }
    if (lower == "none") {
      return 6;
    }
    if (lower.size() == 1 && lower[0] >= '0' && lower[0] <= '6') {
      return lower[0] - '0';
    }

    return std::nullopt;
  }

  void send_framed_content(platf::dxgi::AsyncNamedPipe &pipe, MsgType type, std::span<const uint8_t> payload = {}) {
    std::vector<uint8_t> out;
    out.reserve(1 + payload.size());
    out.push_back(static_cast<uint8_t>(type));
    out.insert(out.end(), payload.begin(), payload.end());
    pipe.send(out);
  }

  std::vector<std::string> parse_snapshot_exclude_json_node(const nlohmann::json &node) {
    std::vector<std::string> ids;
    const nlohmann::json *arr = &node;
    nlohmann::json nested;
    if (node.is_object()) {
      if (node.contains("exclude_devices")) {
        nested = node["exclude_devices"];
        arr = &nested;
      } else if (node.contains("devices")) {
        nested = node["devices"];
        arr = &nested;
      }
    }

    if (!arr->is_array()) {
      return ids;
    }

    for (const auto &el : *arr) {
      if (el.is_string()) {
        ids.push_back(el.get<std::string>());
      } else if (el.is_object()) {
        if (el.contains("device_id") && el["device_id"].is_string()) {
          ids.push_back(el["device_id"].get<std::string>());
        } else if (el.contains("id") && el["id"].is_string()) {
          ids.push_back(el["id"].get<std::string>());
        }
      }
    }

    return ids;
  }

  std::optional<std::vector<std::string>> parse_snapshot_exclude_payload(std::span<const uint8_t> payload) {
    if (payload.empty()) {
      return std::nullopt;
    }

    try {
      std::string raw(reinterpret_cast<const char *>(payload.data()), payload.size());
      if (raw.empty()) {
        return std::vector<std::string> {};
      }
      auto j = nlohmann::json::parse(raw, nullptr, false);
      if (j.is_discarded()) {
        return std::nullopt;
      }
      return parse_snapshot_exclude_json_node(j);
    } catch (...) {
      return std::nullopt;
    }
  }

  /**
   * @brief Load snapshot exclusions from vibeshine_state.json: the user-configured
   *        exclusion list merged with all Sunshine-managed virtual display ids so
   *        they are never captured into (or restored from) display baselines (f3841ad8).
   */
  bool load_vibeshine_snapshot_exclusions(const std::filesystem::path &path, std::vector<std::string> &ids_out) {
    ids_out.clear();
    if (path.empty()) {
      return false;
    }

    std::error_code ec;
    if (!std::filesystem::exists(path, ec) || ec) {
      return false;
    }

    try {
      std::ifstream file(path, std::ios::binary);
      if (!file) {
        return false;
      }
      auto j = nlohmann::json::parse(file, nullptr, false);
      if (!j.is_object() || !j.contains("root") || !j["root"].is_object()) {
        return false;
      }
      const auto &root = j["root"];
      bool found = false;
      if (root.contains("snapshot_exclude_devices")) {
        ids_out = parse_snapshot_exclude_json_node(root["snapshot_exclude_devices"]);
        found = !ids_out.empty() || root["snapshot_exclude_devices"].is_array();
      }
      if (root.contains("virtual_display_devices")) {
        auto virtual_ids = parse_snapshot_exclude_json_node(root["virtual_display_devices"]);
        for (auto &id : virtual_ids) {
          if (std::find(ids_out.begin(), ids_out.end(), id) == ids_out.end()) {
            ids_out.push_back(std::move(id));
          }
        }
        found = found || !ids_out.empty();
      }
      return found;
    } catch (...) {
      return false;
    }
  }

  bool parse_apply_payload(
    std::span<const uint8_t> payload,
    display_helper::v2::ApplyRequest &out_request,
    std::optional<std::vector<std::string>> &snapshot_exclusions,
    std::string &error
  ) {
    std::string json(reinterpret_cast<const char *>(payload.data()), payload.size());
    std::string sanitized_json = json;

    try {
      auto j = nlohmann::json::parse(json, nullptr, false);
      if (j.is_object()) {
        if (j.contains("wa_hdr_toggle")) {
          out_request.hdr_blank = j["wa_hdr_toggle"].get<bool>();
          j.erase("wa_hdr_toggle");
        }
        if (j.contains("sunshine_virtual_layout") && j["sunshine_virtual_layout"].is_string()) {
          out_request.virtual_layout = j["sunshine_virtual_layout"].get<std::string>();
          j.erase("sunshine_virtual_layout");
        }
        if (j.contains("sunshine_monitor_positions") && j["sunshine_monitor_positions"].is_object()) {
          for (auto it = j["sunshine_monitor_positions"].begin(); it != j["sunshine_monitor_positions"].end(); ++it) {
            const auto &node = it.value();
            if (!node.is_object()) {
              continue;
            }
            auto x_it = node.find("x");
            auto y_it = node.find("y");
            if (x_it == node.end() || y_it == node.end() || !x_it->is_number_integer() || !y_it->is_number_integer()) {
              continue;
            }
            out_request.monitor_positions.emplace_back(
              it.key(),
              display_device::Point {x_it->get<int>(), y_it->get<int>()}
            );
          }
          j.erase("sunshine_monitor_positions");
        }
        if (j.contains("sunshine_snapshot_exclude_devices")) {
          snapshot_exclusions = parse_snapshot_exclude_json_node(j["sunshine_snapshot_exclude_devices"]);
          j.erase("sunshine_snapshot_exclude_devices");
        }
        if (j.contains("sunshine_topology") && j["sunshine_topology"].is_array()) {
          display_device::ActiveTopology topo;
          for (const auto &grp_node : j["sunshine_topology"]) {
            if (!grp_node.is_array()) {
              continue;
            }
            std::vector<std::string> group;
            for (const auto &id_node : grp_node) {
              if (id_node.is_string()) {
                group.push_back(id_node.get<std::string>());
              }
            }
            if (!group.empty()) {
              topo.push_back(std::move(group));
            }
          }
          if (!topo.empty()) {
            out_request.topology = std::move(topo);
          }
          j.erase("sunshine_topology");
        }
        if (j.contains("sunshine_always_restore_from_golden") && j["sunshine_always_restore_from_golden"].is_boolean()) {
          out_request.prefer_golden_first = j["sunshine_always_restore_from_golden"].get<bool>();
          j.erase("sunshine_always_restore_from_golden");
        }
        if (j.contains("sunshine_restore_on_disconnect") && j["sunshine_restore_on_disconnect"].is_boolean()) {
          out_request.restore_on_disconnect = j["sunshine_restore_on_disconnect"].get<bool>();
          j.erase("sunshine_restore_on_disconnect");
        } else {
          out_request.restore_on_disconnect = true;
        }
        if (j.contains("sunshine_device_refresh_rate_overrides") && j["sunshine_device_refresh_rate_overrides"].is_object()) {
          for (auto it = j["sunshine_device_refresh_rate_overrides"].begin(); it != j["sunshine_device_refresh_rate_overrides"].end(); ++it) {
            const auto &node = it.value();
            if (!node.is_object()) {
              continue;
            }
            auto num_it = node.find("num");
            auto den_it = node.find("den");
            if (num_it == node.end() || den_it == node.end() || !num_it->is_number_unsigned() || !den_it->is_number_unsigned()) {
              continue;
            }
            out_request.refresh_rate_overrides.emplace_back(
              it.key(),
              std::make_pair(num_it->get<unsigned int>(), den_it->get<unsigned int>())
            );
          }
          j.erase("sunshine_device_refresh_rate_overrides");
        }
        sanitized_json = j.dump();
      }
    } catch (...) {
    }

    display_device::SingleDisplayConfiguration cfg {};
    std::string parse_error;
    if (!display_device::fromJson(sanitized_json, cfg, &parse_error)) {
      error = parse_error;
      return false;
    }

    out_request.configuration = std::move(cfg);
    return true;
  }

  void parse_revert_payload(std::span<const uint8_t> payload, display_helper::v2::RevertCommand &out) {
    if (payload.empty()) {
      return;
    }

    try {
      std::string raw(reinterpret_cast<const char *>(payload.data()), payload.size());
      auto j = nlohmann::json::parse(raw, nullptr, false);
      if (!j.is_object()) {
        return;
      }

      auto it = j.find("sunshine_prefer_golden_if_current_missing");
      if (it != j.end() && it->is_boolean()) {
        out.prefer_golden_if_current_missing = it->get<bool>();
      }

      it = j.find("sunshine_always_restore_from_golden");
      if (it != j.end() && it->is_boolean()) {
        out.always_restore_from_golden = it->get<bool>();
      }
    } catch (...) {
    }
  }

  bool parse_frame(
    std::span<const uint8_t> frame,
    MsgType &type,
    std::span<const uint8_t> &payload
  ) {
    if (frame.empty()) {
      return false;
    }

    if (frame.size() >= 5) {
      uint32_t len = 0;
      std::memcpy(&len, frame.data(), sizeof(len));
      if (len > 0 && frame.size() >= 4u + len) {
        type = static_cast<MsgType>(frame[4]);
        if (len > 1) {
          payload = std::span<const uint8_t>(frame.data() + 5, len - 1);
        } else {
          payload = {};
        }
        return true;
      }
    }

    type = static_cast<MsgType>(frame[0]);
    payload = frame.subspan(1);
    return true;
  }

  class DisplayDeviceLogBridge {
  public:
    void install() {
      display_device::Logger::get().setCustomCallback(
        [](display_device::Logger::LogLevel level, std::string message) {
          const auto prefixed = std::string("display_device: ") + message;
          switch (level) {
            case display_device::Logger::LogLevel::verbose:
            case display_device::Logger::LogLevel::debug:
              BOOST_LOG(debug) << prefixed;
              break;
            case display_device::Logger::LogLevel::info:
              BOOST_LOG(info) << prefixed;
              break;
            case display_device::Logger::LogLevel::warning:
              BOOST_LOG(warning) << prefixed;
              break;
            case display_device::Logger::LogLevel::error:
              BOOST_LOG(error) << prefixed;
              break;
            case display_device::Logger::LogLevel::fatal:
              BOOST_LOG(fatal) << prefixed;
              break;
          }
        }
      );
    }
  };

  /// Validate a session snapshot file found in a search root; remove it when it
  /// has no usable restore payload (legacy validate_session_snapshot).
  bool validate_session_snapshot_file(const std::filesystem::path &path) {
    const auto text = display_helper::v2::codec::read_file_text(path);
    if (!text) {
      return false;
    }
    if (display_helper::v2::codec::snapshot_text_has_restore_payload(*text)) {
      return true;
    }

    BOOST_LOG(warning) << "Existing session snapshot is missing restore topology/mode data; removing path=" << path.string();
    std::error_code ec_rm;
    std::filesystem::remove(path, ec_rm);
    return false;
  }

  /// Copy validated snapshots from any search root into the active snapshot dir
  /// so SYSTEM/user contexts and old install layouts share one restore chain.
  void adopt_snapshots_from_search_roots(
    const std::vector<std::filesystem::path> &search_roots,
    const std::filesystem::path &active_current,
    const std::filesystem::path &active_previous
  ) {
    for (const auto &root : search_roots) {
      const auto paths = display_helper_paths::make_snapshot_paths(root);
      std::error_code ec_cur;
      if (std::filesystem::exists(paths.session_current, ec_cur) && !ec_cur) {
        if (validate_session_snapshot_file(paths.session_current)) {
          BOOST_LOG(info) << "Existing current session snapshot detected; will preserve until confirmed restore: "
                          << paths.session_current.string();
          if (paths.session_current != active_current) {
            std::error_code ec_copy;
            std::filesystem::create_directories(active_current.parent_path(), ec_copy);
            std::filesystem::copy_file(paths.session_current, active_current, std::filesystem::copy_options::overwrite_existing, ec_copy);
          }
          break;
        }
      }
    }
    for (const auto &root : search_roots) {
      const auto paths = display_helper_paths::make_snapshot_paths(root);
      std::error_code ec_prev;
      if (std::filesystem::exists(paths.session_previous, ec_prev) && !ec_prev) {
        if (validate_session_snapshot_file(paths.session_previous)) {
          if (paths.session_previous != active_previous) {
            std::error_code ec_copy;
            std::filesystem::create_directories(active_previous.parent_path(), ec_copy);
            std::filesystem::copy_file(paths.session_previous, active_previous, std::filesystem::copy_options::overwrite_existing, ec_copy);
          }
          break;
        }
      }
    }
  }
}  // namespace

int run_v2_helper(int argc, char *argv[]) {
  bool restore_mode = false;
  std::optional<int> log_level_override;
  constexpr const char *kLogLevelPrefix = "--log-level=";
  for (int i = 1; i < argc; ++i) {
    if (std::strcmp(argv[i], "--restore") == 0) {
      restore_mode = true;
    } else if (std::strcmp(argv[i], "--log-level") == 0 && (i + 1) < argc) {
      if (auto parsed = parse_log_level_value(argv[i + 1])) {
        log_level_override = *parsed;
      }
      ++i;
    } else if (std::strncmp(argv[i], kLogLevelPrefix, std::strlen(kLogLevelPrefix)) == 0) {
      if (auto parsed = parse_log_level_value(argv[i] + std::strlen(kLogLevelPrefix))) {
        log_level_override = *parsed;
      }
    }
  }

  if (restore_mode) {
    FreeConsole();
    display_helper_paths::hide_console_window();
  }

  // Initialize logging early so we can log singleton conflicts and other early exits
  const auto log_dir = display_helper_paths::compute_log_dir();
  const auto snapshot_dir = display_helper_paths::compute_snapshot_dir();
  const auto log_file = log_dir / L"sunshine_display_helper.log";
  const int min_log_level = log_level_override.value_or(2);
  auto log_guard = logging::init(min_log_level, log_file);

  BOOST_LOG(info) << "Display helper v2 starting up" << (restore_mode ? " (restore mode)" : "") << "...";

  HANDLE singleton = nullptr;
  if (!display_helper_paths::ensure_single_instance(singleton)) {
    BOOST_LOG(warning) << "Display helper: another instance is already running (singleton conflict). Exiting with code 3.";
    logging::log_flush();
    return 3;
  }

  DisplayDeviceLogBridge log_bridge;
  log_bridge.install();

  const auto active_snapshots = display_helper_paths::make_snapshot_paths(snapshot_dir);
  const auto search_roots = display_helper_paths::snapshot_search_roots();

  display_helper::v2::SystemClock clock;
  display_helper::v2::WinDisplaySettings display_settings;
  display_helper::v2::SnapshotService snapshot_service(display_settings);

  display_helper::v2::SnapshotPaths paths {
    .current = active_snapshots.session_current,
    .previous = active_snapshots.session_previous,
    .golden = active_snapshots.golden,
  };
  display_helper::v2::FileSnapshotStorage storage(paths);
  display_helper::v2::SnapshotPersistence persistence(storage);
  display_helper::v2::GoldenHealth golden_health(active_snapshots.golden_status);
  display_helper::v2::RestoreState restore_state;
  display_helper::v2::ApplyPolicy apply_policy(clock);
  display_helper::v2::WinVirtualDisplayDriver virtual_display;
  display_helper::v2::WinPlatformWorkarounds workarounds;
  display_helper::v2::WinScheduledTaskManager task_manager;
  display_helper::v2::HeartbeatMonitor heartbeat(clock);
  display_helper::v2::CancellationSource cancellation;
  display_helper::v2::SystemPorts system_ports(workarounds, task_manager, heartbeat, clock, cancellation);
  // Drive this from ApplyOperation's mutation boundary, rather than receipt
  // of an IPC frame. A disconnected client can leave parsed APPLY messages in
  // the queue that must never arm an autonomous restore.
  std::atomic<bool> apply_seen {false};
  display_helper::v2::ApplyOperation apply_operation(
    display_settings,
    clock,
    [&task_manager, &apply_seen]() {
      apply_seen.store(true, std::memory_order_release);
      (void) task_manager.create_restore_task(L"");
    }
  );
  display_helper::v2::VerificationOperation verification_operation(display_settings, clock);
  display_helper::v2::RecoveryOperation recovery_operation(display_settings, storage, golden_health, restore_state, clock);
  display_helper::v2::RecoveryValidationOperation recovery_validation(snapshot_service, clock);
  // Must outlive the dispatcher: outstanding worker completions enqueue here
  // while AsyncDispatcher's destructor drains/joins its worker.
  display_helper::v2::MessageQueue<display_helper::v2::Message> queue;
  display_helper::v2::AsyncDispatcher dispatcher(
    apply_operation,
    verification_operation,
    recovery_operation,
    recovery_validation,
    virtual_display,
    clock
  );

  std::atomic<bool> running {true};

  // Adopt snapshots written by other contexts (SYSTEM vs user) or the legacy engine.
  adopt_snapshots_from_search_roots(search_roots, paths.current, paths.previous);

  // Load snapshot exclusions (user-configured + Sunshine-managed virtual display ids).
  std::set<std::string> initial_blacklist;
  for (const auto &root : search_roots) {
    std::vector<std::string> exclusions;
    const auto state_file = root / L"vibeshine_state.json";
    if (load_vibeshine_snapshot_exclusions(state_file, exclusions)) {
      BOOST_LOG(info) << "Loaded snapshot exclusions from vibeshine_state.json (" << exclusions.size()
                      << ") at " << state_file.string();
      for (auto &id : exclusions) {
        if (!id.empty()) {
          initial_blacklist.insert(std::move(id));
        }
      }
      break;
    }
  }

  auto enqueue_message = [&](display_helper::v2::Message message) {
    queue.push(std::move(message));
  };
  display_helper::v2::ApplyPipeline apply_pipeline(dispatcher, apply_policy, system_ports, restore_state, enqueue_message);
  display_helper::v2::RecoveryPipeline recovery_pipeline(dispatcher, system_ports, enqueue_message);
  display_helper::v2::SnapshotLedger snapshot_ledger(snapshot_service, persistence, clock);

  display_helper::v2::StateMachine state_machine(
    apply_pipeline,
    recovery_pipeline,
    snapshot_ledger,
    system_ports,
    virtual_display,
    golden_health,
    restore_state
  );

  state_machine.set_snapshot_blacklist(std::move(initial_blacklist));

  // Connection epochs: ignore stale pipe callbacks and decide whether a confirmed
  // restore should exit the helper or keep it alive for a newer connection.
  std::atomic<uint64_t> connection_epoch {0};
  std::atomic<uint64_t> restore_origin_epoch {0};
  std::atomic<bool> client_connected {false};
  // Every liveness publication and the recovery-exit snapshot share this
  // mutex. An old pipe can therefore never overwrite a disconnect's false
  // liveness value after that disconnect advances the epoch.
  std::recursive_mutex client_command_epoch_mutex;

  int exit_code = 0;
  state_machine.set_exit_callback([&](int code) {
    std::lock_guard epoch_lock(client_command_epoch_mutex);
    const auto origin = restore_origin_epoch.load(std::memory_order_acquire);
    const auto current = connection_epoch.load(std::memory_order_acquire);
    if (code == 0 && origin != 0 && current > origin && client_connected.load(std::memory_order_acquire)) {
      BOOST_LOG(info) << "Restore confirmed while newer connection active; helper remains running.";
      restore_origin_epoch.store(0, std::memory_order_release);
      return;
    }
    exit_code = code;
    running.store(false, std::memory_order_release);
  });

  std::atomic<platf::dxgi::AsyncNamedPipe *> active_pipe {nullptr};
  std::mutex response_mutex;
  state_machine.set_apply_result_callback([&](display_helper::v2::ApplyStatus status, std::uint64_t request_id, std::uint64_t epoch) {
    std::lock_guard<std::mutex> lock(response_mutex);
    if (connection_epoch.load(std::memory_order_acquire) != epoch) {
      return;
    }
    auto *pipe = active_pipe.load(std::memory_order_acquire);
    if (!pipe) {
      return;
    }
    if (request_id != 0) {
      const auto payload = platf::display_helper_protocol::encode_correlated_result(
        request_id,
        status == display_helper::v2::ApplyStatus::Ok ?
          ResultStatus::Succeeded :
          (status == display_helper::v2::ApplyStatus::Expired ? ResultStatus::Expired : ResultStatus::Failed)
      );
      send_framed_content(*pipe, MsgType::ApplyResultCorrelated, payload);
    } else {
      const std::vector<uint8_t> payload {
        status == display_helper::v2::ApplyStatus::Ok ? std::uint8_t {1} : std::uint8_t {0}
      };
      send_framed_content(*pipe, MsgType::ApplyResult, payload);
    }
  });
  state_machine.set_verification_result_callback([&](bool success, std::uint64_t request_id, std::uint64_t epoch) {
    std::lock_guard<std::mutex> lock(response_mutex);
    if (connection_epoch.load(std::memory_order_acquire) != epoch) {
      return;
    }
    auto *pipe = active_pipe.load(std::memory_order_acquire);
    if (!pipe) {
      return;
    }
    if (request_id != 0) {
      const auto payload = platf::display_helper_protocol::encode_correlated_result(
        request_id,
        success ? ResultStatus::Succeeded : ResultStatus::Failed
      );
      send_framed_content(*pipe, MsgType::VerificationResultCorrelated, payload);
    } else {
      const std::vector<uint8_t> payload {success ? std::uint8_t {1} : std::uint8_t {0}};
      send_framed_content(*pipe, MsgType::VerificationResult, payload);
    }
  });
  state_machine.set_snapshot_result_callback([&](std::uint64_t request_id, std::uint64_t epoch, bool success) {
    std::lock_guard<std::mutex> lock(response_mutex);
    if (connection_epoch.load(std::memory_order_acquire) != epoch) {
      return;
    }
    auto *pipe = active_pipe.load(std::memory_order_acquire);
    if (!pipe) {
      return;
    }
    const auto payload = platf::display_helper_protocol::encode_correlated_result(
      request_id,
      success ? ResultStatus::Succeeded : ResultStatus::Failed
    );
    send_framed_content(*pipe, MsgType::SnapshotCurrentResult, payload);
  });
  state_machine.set_disarm_result_callback([&](std::uint64_t request_id, std::uint64_t epoch, bool accepted) {
    std::lock_guard<std::mutex> lock(response_mutex);
    if (connection_epoch.load(std::memory_order_acquire) != epoch) {
      return;
    }
    auto *pipe = active_pipe.load(std::memory_order_acquire);
    if (!pipe) {
      return;
    }
    const auto payload = platf::display_helper_protocol::encode_correlated_result(
      request_id,
      accepted ? ResultStatus::Succeeded : ResultStatus::Busy
    );
    send_framed_content(*pipe, MsgType::DisarmResult, payload);
  });

  display_helper::v2::DebouncedTrigger debouncer(std::chrono::milliseconds(500));
  std::mutex debounce_mutex;
  display_helper::v2::WinEventPump event_pump;
  event_pump.start([&](display_helper::v2::DisplayEvent) {
    std::lock_guard<std::mutex> lock(debounce_mutex);
    debouncer.notify(clock.now());
  });

  auto process_queue = [&]() {
    auto message = queue.wait_for(std::chrono::milliseconds(100));
    if (message) {
      const auto message_epoch = display_helper::v2::connection_bound_epoch(*message);
      if (message_epoch) {
        // Serialize the final epoch check with disconnect invalidation. A
        // command is therefore either accepted before the disconnect callback
        // or rejected after it; it cannot cross that boundary half-validated.
        std::lock_guard epoch_lock(client_command_epoch_mutex);
        const auto serialized_epoch = connection_epoch.load(std::memory_order_acquire);
        if (*message_epoch != serialized_epoch) {
          BOOST_LOG(info) << "Dropping stale client command from IPC epoch " << *message_epoch
                          << " (current=" << serialized_epoch << ").";
          return;
        }
        if (const auto *revert = std::get_if<display_helper::v2::RevertCommand>(&*message);
            revert && revert->client_connection_epoch) {
          // Only an accepted explicit client REVERT owns this origin. A stale
          // command must not poison a later internal disconnect restore.
          restore_origin_epoch.store(*revert->client_connection_epoch, std::memory_order_release);
        }
        state_machine.handle_message(*message);
        return;
      }
      state_machine.handle_message(*message);
      return;
    }

    if (heartbeat.check_timeout()) {
      queue.push(display_helper::v2::HelperEventMessage {display_helper::v2::HelperEvent::HeartbeatTimeout, cancellation.current_generation()});
    }

    bool fire = false;
    {
      std::lock_guard<std::mutex> lock(debounce_mutex);
      fire = debouncer.should_fire(clock.now());
    }
    if (fire) {
      queue.push(display_helper::v2::DisplayEventMessage {display_helper::v2::DisplayEvent::DisplayChange, cancellation.current_generation()});
    }

    state_machine.handle_tick();
  };

  if (restore_mode) {
    BOOST_LOG(info) << "Display helper v2 running in restore mode.";
    display_helper::v2::RevertCommand revert;
    revert.generation = cancellation.current_generation();
    revert.immediate = true;
    queue.push(revert);
    while (running.load(std::memory_order_acquire)) {
      process_queue();
    }
    event_pump.stop();
    BOOST_LOG(info) << "Display helper v2 restore mode completed with exit code " << exit_code << ".";
    logging::log_flush();
    return exit_code;
  }

  auto last_connect_wait_log = std::chrono::steady_clock::time_point::min();
  constexpr auto kReconnectLogInterval = std::chrono::hours(1);

  while (running.load(std::memory_order_acquire)) {
    platf::dxgi::FramedPipeFactory pipe_factory(std::make_unique<platf::dxgi::AnonymousPipeFactory>());
    auto server_pipe = pipe_factory.create_server("sunshine_display_helper");
    if (!server_pipe) {
      platf::dxgi::FramedPipeFactory fallback_factory(std::make_unique<platf::dxgi::NamedPipeFactory>());
      server_pipe = fallback_factory.create_server("sunshine_display_helper");
      if (!server_pipe) {
        BOOST_LOG(error) << "Failed to create control pipe; retrying in 500ms";
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        continue;
      }
    }

    platf::dxgi::AsyncNamedPipe async_pipe(std::move(server_pipe));

    // Wait for a client connection before starting the async worker. Without
    // this the cleanup path below would tear down the server pipe before
    // Sunshine ever had a chance to connect (28b048ac). Keep processing FSM
    // work (pending restores, ticks) while waiting.
    {
      const auto wait_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
      while (running.load(std::memory_order_acquire) && !async_pipe.is_connected() &&
             std::chrono::steady_clock::now() < wait_deadline) {
        async_pipe.wait_for_client_connection(100);
        process_queue();
      }
    }
    if (!running.load(std::memory_order_acquire)) {
      async_pipe.stop();
      break;
    }
    if (!async_pipe.is_connected()) {
      const auto now = std::chrono::steady_clock::now();
      if (now - last_connect_wait_log > kReconnectLogInterval) {
        BOOST_LOG(info) << "Waiting for Sunshine to connect to display helper IPC...";
        last_connect_wait_log = now;
      }
      async_pipe.stop();
      continue;
    }

    std::uint64_t epoch = 0;
    {
      // Publish connection liveness before the new epoch. The restore exit
      // callback uses the epoch as a newer-connection fence; seeing that
      // fence must also mean the connection is already live.
      std::lock_guard epoch_lock(client_command_epoch_mutex);
      client_connected.store(true, std::memory_order_release);
      epoch = connection_epoch.fetch_add(1, std::memory_order_acq_rel) + 1;
    }
    active_pipe.store(&async_pipe, std::memory_order_release);

    auto on_message = [&, epoch](std::span<const uint8_t> bytes) {
      if (connection_epoch.load(std::memory_order_acquire) != epoch) {
        return;
      }
      MsgType type {};
      std::span<const uint8_t> payload;
      if (!parse_frame(bytes, type, payload)) {
        return;
      }

      switch (type) {
        case MsgType::Apply:
        case MsgType::ApplyRequest:
          {
            std::uint64_t request_id = 0;
            std::optional<std::chrono::steady_clock::time_point> apply_expires_at;
            auto apply_payload = payload;
            if (type == MsgType::ApplyRequest) {
              const auto correlated = platf::display_helper_protocol::decode_correlated_request(payload);
              if (!correlated) {
                BOOST_LOG(warning) << "Ignoring malformed correlated APPLY request.";
                break;
              }
              request_id = correlated->request_id;
              apply_payload = correlated->body;
              const auto now_tick = static_cast<std::uint64_t>(::GetTickCount64());
              if (now_tick >= correlated->not_after_tick_ms) {
                const auto result_payload = platf::display_helper_protocol::encode_correlated_result(
                  request_id,
                  ResultStatus::Expired
                );
                std::lock_guard<std::mutex> lock(response_mutex);
                send_framed_content(async_pipe, MsgType::ApplyResultCorrelated, result_payload);
                break;
              }
              apply_expires_at = clock.now() +
                                 std::chrono::milliseconds(correlated->not_after_tick_ms - now_tick);
            }

            display_helper::v2::ApplyRequest request;
            std::optional<std::vector<std::string>> snapshot_exclusions;
            std::string parse_failure;
            if (!parse_apply_payload(apply_payload, request, snapshot_exclusions, parse_failure)) {
              BOOST_LOG(error) << "Failed to parse SingleDisplayConfiguration JSON: " << parse_failure;
              std::lock_guard<std::mutex> lock(response_mutex);
              if (request_id != 0) {
                const auto result_payload = platf::display_helper_protocol::encode_correlated_result(
                  request_id,
                  ResultStatus::Invalid
                );
                send_framed_content(async_pipe, MsgType::ApplyResultCorrelated, result_payload);
              } else {
                std::vector<uint8_t> result_payload;
                result_payload.push_back(0u);
                if (!parse_failure.empty()) {
                  result_payload.insert(result_payload.end(), parse_failure.begin(), parse_failure.end());
                }
                send_framed_content(async_pipe, MsgType::ApplyResult, result_payload);
              }
              return;
            }
            request.expires_at = apply_expires_at;
            std::optional<std::set<std::string>> apply_blacklist;
            if (snapshot_exclusions.has_value()) {
              std::set<std::string> blacklist;
              for (auto &id : *snapshot_exclusions) {
                if (!id.empty()) {
                  blacklist.insert(std::move(id));
                }
              }
              apply_blacklist = std::move(blacklist);
            }

            display_helper::v2::ApplyCommand command {request, cancellation.current_generation()};
            command.request_id = request_id;
            command.connection_epoch = epoch;
            command.snapshot_blacklist = std::move(apply_blacklist);
            queue.push(std::move(command));
            break;
          }
        case MsgType::Revert:
          {
            display_helper::v2::RevertCommand revert;
            revert.generation = cancellation.current_generation();
            parse_revert_payload(payload, revert);
            revert.client_connection_epoch = epoch;
            queue.push(revert);
            break;
          }
        case MsgType::Disarm:
          {
            display_helper::v2::DisarmCommand disarm {cancellation.current_generation()};
            disarm.connection_epoch = epoch;
            queue.push(disarm);
          }
          break;
        case MsgType::DisarmRequest:
          {
            const auto correlated = platf::display_helper_protocol::decode_correlated_request(payload);
            if (!correlated) {
              BOOST_LOG(warning) << "Ignoring malformed correlated DISARM request.";
              break;
            }
            const auto now_tick = static_cast<std::uint64_t>(::GetTickCount64());
            if (now_tick >= correlated->not_after_tick_ms) {
              const auto result_payload = platf::display_helper_protocol::encode_correlated_result(
                correlated->request_id,
                ResultStatus::Expired
              );
              std::lock_guard<std::mutex> lock(response_mutex);
              send_framed_content(async_pipe, MsgType::DisarmResult, result_payload);
              break;
            }
            display_helper::v2::DisarmCommand disarm {cancellation.current_generation()};
            disarm.request_id = correlated->request_id;
            disarm.connection_epoch = epoch;
            disarm.expires_at = clock.now() + std::chrono::milliseconds(correlated->not_after_tick_ms - now_tick);
            queue.push(disarm);
            break;
          }
        case MsgType::ExportGolden:
          {
            display_helper::v2::SnapshotCommandPayload payload_struct;
            if (auto parsed = parse_snapshot_exclude_payload(payload)) {
              payload_struct.exclude_devices = std::move(*parsed);
              payload_struct.update_exclusions = true;
            }
            display_helper::v2::ExportGoldenCommand command {payload_struct, cancellation.current_generation()};
            command.connection_epoch = epoch;
            queue.push(std::move(command));
            break;
          }
        case MsgType::SnapshotCurrent:
          {
            display_helper::v2::SnapshotCommandPayload payload_struct;
            if (auto parsed = parse_snapshot_exclude_payload(payload)) {
              payload_struct.exclude_devices = std::move(*parsed);
              payload_struct.update_exclusions = true;
            }
            display_helper::v2::SnapshotCurrentCommand snapshot {payload_struct, cancellation.current_generation()};
            snapshot.connection_epoch = epoch;
            queue.push(std::move(snapshot));
            break;
          }
        case MsgType::SnapshotCurrentRequest:
          {
            const auto correlated = platf::display_helper_protocol::decode_correlated_request(payload);
            if (!correlated) {
              BOOST_LOG(warning) << "Ignoring malformed correlated SNAPSHOT_CURRENT request.";
              break;
            }

            const auto now_tick = static_cast<std::uint64_t>(::GetTickCount64());
            if (now_tick >= correlated->not_after_tick_ms) {
              const auto result_payload = platf::display_helper_protocol::encode_correlated_result(
                correlated->request_id,
                ResultStatus::Expired
              );
              std::lock_guard<std::mutex> lock(response_mutex);
              send_framed_content(async_pipe, MsgType::SnapshotCurrentResult, result_payload);
              break;
            }

            display_helper::v2::SnapshotCommandPayload payload_struct;
            if (auto parsed = parse_snapshot_exclude_payload(correlated->body)) {
              payload_struct.exclude_devices = std::move(*parsed);
              payload_struct.update_exclusions = true;
            }
            display_helper::v2::SnapshotCurrentCommand snapshot {payload_struct, cancellation.current_generation()};
            snapshot.request_id = correlated->request_id;
            snapshot.connection_epoch = epoch;
            snapshot.expires_at = clock.now() + std::chrono::milliseconds(correlated->not_after_tick_ms - now_tick);
            queue.push(snapshot);
            break;
          }
        case MsgType::Reset:
          {
            display_helper::v2::ResetCommand reset {cancellation.current_generation()};
            reset.connection_epoch = epoch;
            queue.push(reset);
          }
          break;
        case MsgType::Ping: {
          {
            std::lock_guard<std::mutex> lock(response_mutex);
            send_framed_content(async_pipe, MsgType::Ping);
          }
          display_helper::v2::PingCommand ping {cancellation.current_generation()};
          ping.connection_epoch = epoch;
          queue.push(ping);
          break;
        }
        case MsgType::LogLevel:
          if (!payload.empty()) {
            // This changes process-global state. Serialize its final epoch
            // check with disconnect invalidation as we do for queued commands.
            std::lock_guard epoch_lock(client_command_epoch_mutex);
            if (connection_epoch.load(std::memory_order_acquire) == epoch) {
              const int level = std::clamp(static_cast<int>(payload.front()), 0, 6);
              logging::reconfigure_min_log_level(level);
              BOOST_LOG(info) << "Display helper log level updated to " << level;
            }
          }
          break;
        case MsgType::Stop:
          {
            display_helper::v2::StopCommand stop {cancellation.current_generation()};
            stop.connection_epoch = epoch;
            queue.push(stop);
          }
          break;
        default:
          BOOST_LOG(warning) << "Unknown message type: " << static_cast<int>(type);
          break;
      }
    };

    std::atomic<bool> broken {false};

    auto on_error = [&, epoch](const std::string &err) {
      std::lock_guard epoch_lock(client_command_epoch_mutex);
      auto expected_epoch = epoch;
      if (connection_epoch.load(std::memory_order_acquire) != epoch) {
        BOOST_LOG(info) << "Ignoring async pipe error from stale connection (epoch=" << epoch << ")";
        return;
      }
      // Publish disconnection before the invalidating epoch. An exit callback
      // that sees the newer epoch can therefore never mistake this stale pipe
      // for a live reconnect.
      client_connected.store(false, std::memory_order_release);
      if (!connection_epoch.compare_exchange_strong(
            expected_epoch,
            epoch + 1,
            std::memory_order_acq_rel
          )) {
        BOOST_LOG(info) << "Ignoring async pipe error from stale connection (epoch=" << epoch << ")";
        return;
      }
      client_connected.store(false, std::memory_order_release);
      BOOST_LOG(error) << "Async pipe error: " << err << "; handling disconnect and revert policy.";
      broken.store(true, std::memory_order_release);
    };

    auto on_broken = [&, epoch]() {
      std::lock_guard epoch_lock(client_command_epoch_mutex);
      auto expected_epoch = epoch;
      if (connection_epoch.load(std::memory_order_acquire) != epoch) {
        BOOST_LOG(info) << "Ignoring disconnect notification from stale connection (epoch=" << epoch << ")";
        return;
      }
      // See on_error: connection liveness must become false before its epoch
      // becomes observable as a newer connection fence.
      client_connected.store(false, std::memory_order_release);
      if (!connection_epoch.compare_exchange_strong(
            expected_epoch,
            epoch + 1,
            std::memory_order_acq_rel
          )) {
        BOOST_LOG(info) << "Ignoring disconnect notification from stale connection (epoch=" << epoch << ")";
        return;
      }
      client_connected.store(false, std::memory_order_release);
      BOOST_LOG(warning) << "Client disconnected; applying revert policy.";
      broken.store(true, std::memory_order_release);
    };

    async_pipe.start(on_message, on_error, on_broken);

    bool enqueue_disconnect_revert = false;
    while (running.load(std::memory_order_acquire)) {
      process_queue();

      bool connected = async_pipe.is_connected() && !broken.load(std::memory_order_acquire);
      {
        std::lock_guard epoch_lock(client_command_epoch_mutex);
        if (connection_epoch.load(std::memory_order_acquire) == epoch) {
          client_connected.store(connected, std::memory_order_release);
        } else {
          // on_error/on_broken already published false before invalidating the
          // epoch. Do not let this stale pipe revive it from an old
          // is_connected() snapshot.
          connected = false;
        }
      }
      if (!connected) {
        // Sunshine disconnected or crashed. Arm the autonomous restore now (the
        // FSM applies a 5s grace and the restore-on-disconnect policy; a fast
        // reconnect supersedes it via DISARM/APPLY like the legacy engine), but
        // only when this helper actually changed something, an accepted APPLY
        // still needs to drain, or a restore is already being worked on. An
        // APPLY worker can be queued between IPC acceptance and its mutation
        // boundary; route that case through the deferred disconnect REVERT so
        // it is canceled before it can mutate after the client is gone.
        const bool apply_worker_active =
          restore_state.apply_workers_active.load(std::memory_order_acquire) != 0;
        if (apply_seen.load(std::memory_order_acquire) ||
            apply_worker_active ||
            state_machine.restore_pending()) {
          BOOST_LOG(info) << "Client disconnected; applying revert policy and staying alive until successful.";
          enqueue_disconnect_revert = true;
        }
        break;
      }
    }

    {
      // Completion callbacks hold this lock from their epoch check through the
      // final send. Retiring the pointer under the same lock guarantees none
      // can retain it across AsyncNamedPipe destruction.
      std::lock_guard<std::mutex> lock(response_mutex);
      active_pipe.store(nullptr, std::memory_order_release);
    }
    {
      std::lock_guard epoch_lock(client_command_epoch_mutex);
      if (connection_epoch.load(std::memory_order_acquire) == epoch) {
        client_connected.store(false, std::memory_order_release);
      }
    }
    async_pipe.stop();

    // The pipe worker is now joined, so no more commands from this connection
    // can enter the queue. Remove abandoned client work while preserving
    // internal completions and recovery events that must drain generation and
    // apply-worker accounting. Explicit client REVERT is connection-bound;
    // internal safety/disconnect REVERT remains preserved.
    const auto purged = queue.erase_if(display_helper::v2::is_connection_bound_command);
    if (purged != 0) {
      BOOST_LOG(info) << "Discarded " << purged << " abandoned client command(s) from IPC epoch " << epoch << ".";
    }

    if (enqueue_disconnect_revert) {
      display_helper::v2::RevertCommand revert;
      revert.generation = cancellation.current_generation();
      revert.from_disconnect = true;
      {
        std::lock_guard epoch_lock(client_command_epoch_mutex);
        restore_origin_epoch.store(epoch, std::memory_order_release);
      }
      queue.push(revert);
    }
  }

  event_pump.stop();
  BOOST_LOG(info) << "Display helper v2 shutting down with exit code " << exit_code << ".";
  logging::log_flush();
  return exit_code;
}

#endif  // _WIN32

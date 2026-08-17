#include "terminal_session_runtime.h"
#include "rtsp.h"
#include "terminal_session_launch_codec.h"
#include "terminal_session_worker.h"
#include "terminal_session_service.h"
#include "steam_offline_policy.h"
#include "logging.h"

#include <algorithm>
#include <limits>
#include <utility>

namespace terminal_session {
  namespace {
    class unsupported_provider final: public seat_provider_t {
    public:
      provider_capability_t preflight() override {
        return {.supported = false, .concurrent_sessions = false, .remote_display = false, .audio_endpoint = false, .token_launch = false,
                .error = "No supported Windows concurrent-session provider, remote display, and seat-scoped audio endpoint are available."};
      }
      std::optional<provider_resource_t> allocate(const provider_request_t &, std::string &error) override {
        error = "Managed terminal seat provider is unavailable; console streaming is not a fallback.";
        return std::nullopt;
      }
      void release(const provider_resource_t &) noexcept override {}
    };

    class unsupported_worker final: public seat_worker_t {
    public:
      std::optional<route_t> start(const worker_request_t &, std::string &error) override {
        error = "Private Sunshine worker cannot be admitted without a supported seat provider.";
        return std::nullopt;
      }
      bool stop(const route_t &) noexcept override { return true; }
    };
  }

  runtime_t::runtime_t(std::unique_ptr<seat_provider_t> provider, std::unique_ptr<seat_worker_t> worker):
      provider_(std::move(provider)), worker_(std::move(worker)) {}

  runtime_t::runtime_t(std::unique_ptr<seat_provider_t> provider, worker_factory_t worker_factory):
      provider_(std::move(provider)), worker_factory_(std::move(worker_factory)) {}

  runtime_t::~runtime_t() { shutdown(); }

  route_t runtime_t::reject(bool retryable, std::string error) const {
    return {.accepted = false, .ready = false, .retryable = retryable, .error = std::move(error)};
  }

  worker_request_t runtime_t::make_worker_request(const request_t &request, const provider_request_t &provider_request,
                                                  const provider_resource_t &resource, std::string &error) {
    const auto ticket_operation = request.operation == operation_e::resume ? protocol::opcode::resume : protocol::opcode::prepare;
    const auto ticket = authority_.issue(provider_request.client_uuid, provider_request.generation, provider_request.launch_id,
                                         protocol::admission_authority::clock_t::now(), ticket_operation);
    if (!ticket) { error = "Terminal-session admission nonce generation failed."; return {}; }
    const auto contract = worker::make_contract(resource.seat_id, resource.rtsp_port, resource.control_port, resource.video_port, resource.audio_port);
    worker_request_t worker_request {
      provider_request, resource, *ticket, contract.config_root, contract.state_root, contract.log_root, {},
      steam_offline::enabled_for_terminal(request.launch_session->terminal_session_requested,
                                           request.launch_session->steam_offline_isolation)
    };
    try {
      worker_request.launch_payload = launch_codec::encode(request, error);
    } catch (...) {
      error = "Terminal launch serialization raised unexpectedly.";
    }
    worker_request.ticket_validator = [this](const protocol::request_t &candidate) {
      auto authenticated = candidate;
      authenticated.peer = {.pid = 1, .sid = "internal-service-runtime", .creation_time = 1, .authenticated = true};
      return !authority_.consume(authenticated).has_value();
    };
    return worker_request;
  }

  route_t runtime_t::prepare(request_t request) {
    if (!request.launch_session) return reject(false, "Terminal session launch material is missing.");
    const auto &launch = *request.launch_session;
    const std::uint64_t generation = launch.role_generation == 0 ? launch.id : launch.role_generation;
    if (generation == 0 || launch.id == 0 || launch.client_uuid.empty()) return reject(false, "Terminal session identity or launch generation is missing.");
    const auto dimension = [](const int value, const std::uint16_t fallback) {
      return value > 0 && value <= std::numeric_limits<std::uint16_t>::max() ? static_cast<std::uint16_t>(value) : fallback;
    };
    const provider_request_t provider_request {
      launch.client_uuid, generation, launch.id, dimension(launch.width, 1920), dimension(launch.height, 1080)
    };

    std::lock_guard lock {mutex_};
    auto existing = seats_.find(launch.client_uuid);
    if (existing != seats_.end()) {
      // Reconnect carries a new launch ID but the same role generation. It
      // may reuse the already-admitted worker route; a different generation
      // is a stale request and may never replay the original seat launch.
      if (existing->second.generation != generation) return reject(false, "Terminal session launch generation is stale.");
      if (request.operation == operation_e::resume &&
          (existing->second.state == seat_state_e::ready || existing->second.state == seat_state_e::running ||
           existing->second.state == seat_state_e::retained)) {
        std::string error;
        const bool reconnecting = existing->second.state == seat_state_e::retained;
        auto resource = existing->second.resource;
        if (reconnecting) {
          std::optional<provider_resource_t> connected;
          try {
            connected = provider_->allocate(provider_request, error);
          } catch (...) {
            error = "Terminal session provider reconnect failed.";
          }
          if (!connected) return reject(true, error.empty() ? "The retained managed seat could not reconnect." : error);
          if (connected->seat_id != resource.seat_id || connected->windows_session_id != resource.windows_session_id ||
              connected->rtsp_port != resource.rtsp_port || connected->control_port != resource.control_port ||
              connected->video_port != resource.video_port || connected->audio_port != resource.audio_port) {
            (void) provider_->release_checked(*connected, protocol::release_mode::retain);
            return reject(true, "The retained managed seat reconnected with a different worker route.");
          }
          resource = *connected;
        }
        auto worker_request = make_worker_request(request, provider_request, resource, error);
        if (worker_request.launch_payload.empty()) {
          if (reconnecting) (void) provider_->release_checked(resource, protocol::release_mode::retain);
          return reject(false, error.empty() ? "Terminal reconnect serialization failed." : error);
        }
        std::optional<route_t> resumed;
        try {
          resumed = existing->second.worker->resume(worker_request, error);
        } catch (...) {
          error = "Private worker raised during reconnect admission.";
        }
        if (!resumed || !resumed->accepted || !resumed->ready || resumed->rtsp_port != existing->second.route.rtsp_port ||
            resumed->control_port != existing->second.route.control_port || resumed->video_port != existing->second.route.video_port ||
            resumed->audio_port != existing->second.route.audio_port) {
          if (reconnecting) (void) provider_->release_checked(resource, protocol::release_mode::retain);
          return reject(true, error.empty() ? "Private worker rejected reconnect admission." : error);
        }
        existing->second.launch_id = launch.id;
        existing->second.resource = resource;
        existing->second.state = seat_state_e::running;
        existing->second.route = *resumed;
        existing->second.route.windows_session_id = existing->second.resource.windows_session_id;
        existing->second.route.seat_id = existing->second.resource.seat_id;
        return existing->second.route;
      }
      if (existing->second.launch_id != launch.id) return reject(false, "Terminal session launch ID is stale.");
      if (existing->second.state == seat_state_e::running || existing->second.state == seat_state_e::preparing) return reject(true, "Terminal session is already being prepared.");
      if (!release_locked(launch.client_uuid, "replaced launch", protocol::release_mode::abandon)) {
        return reject(true, "Previous terminal seat teardown did not complete; replacement is blocked.");
      }
    }

    provider_capability_t capability;
    try {
      capability = provider_->preflight();
    } catch (...) {
      return reject(true, "Terminal session provider preflight failed.");
    }
    if (!capability.supported || !capability.concurrent_sessions || !capability.remote_display || !capability.audio_endpoint || !capability.token_launch) {
      return reject(true, capability.error.empty() ? "Terminal session provider is not capable of a complete seat." : capability.error);
    }

    seat_t seat;
    seat.state = seat_state_e::preparing;
    seat.generation = generation;
    seat.launch_id = launch.id;
    if (worker_factory_) {
      try {
        seat.worker_owner = worker_factory_();
      } catch (...) {
        return reject(true, "Private seat worker factory failed.");
      }
      seat.worker = seat.worker_owner.get();
    } else {
      seat.worker = worker_.get();
      if (!seat.worker || std::any_of(seats_.begin(), seats_.end(), [this](const auto &entry) { return entry.second.worker == worker_.get(); })) {
        return reject(true, "The configured terminal worker cannot own another concurrent seat.");
      }
    }
    if (!seat.worker) return reject(true, "Private seat worker is unavailable.");
    seats_.emplace(launch.client_uuid, std::move(seat));

    std::string error;
    std::optional<provider_resource_t> resource;
    try {
      resource = provider_->allocate(provider_request, error);
    } catch (...) {
      error = "Terminal session provider allocation failed.";
    }
    if (!resource) {
      seats_.erase(launch.client_uuid);
      return reject(true, error.empty() ? "Terminal session provider allocation failed." : error);
    }

    auto worker_request = make_worker_request(request, provider_request, *resource, error);
    if (worker_request.launch_payload.empty()) {
      if (!provider_->release_checked(*resource, protocol::release_mode::abandon)) {
        auto &failed = seats_.at(launch.client_uuid);
        failed.state = seat_state_e::failed;
        failed.resource = *resource;
        return reject(true, "Terminal seat rollback failed after launch serialization was rejected.");
      }
      seats_.erase(launch.client_uuid);
      return reject(false, error.empty() ? "Terminal launch serialization failed." : error);
    }
    std::optional<route_t> route;
    try {
      route = seats_.at(launch.client_uuid).worker->start(worker_request, error);
    } catch (...) {
      error = "Private seat worker raised during admission.";
    }
    if (!route || !route->accepted || !route->ready || route->rtsp_port == 0 || route->control_port == 0 || route->video_port == 0 || route->audio_port == 0) {
      auto *seat_worker = seats_.at(launch.client_uuid).worker;
      bool worker_cleanup_failed = seat_worker->cleanup_needed();
      if (route || worker_cleanup_failed) {
        worker_cleanup_failed = !seat_worker->stop(route.value_or(route_t {})) || seat_worker->cleanup_needed();
      }
      if (worker_cleanup_failed) {
        auto &failed = seats_.at(launch.client_uuid);
        failed.state = seat_state_e::failed;
        failed.resource = *resource;
        failed.route = route.value_or(route_t {});
        return reject(true, "Private worker rollback failed; retry teardown before replacement.");
      }
      if (!provider_->release_checked(*resource, protocol::release_mode::abandon)) {
        auto &failed = seats_.at(launch.client_uuid);
        failed.state = seat_state_e::failed;
        failed.resource = *resource;
        failed.route = route.value_or(route_t {});
        return reject(true, "Terminal seat provider rollback failed; retry teardown before replacement.");
      }
      seats_.erase(launch.client_uuid);
      return reject(true, error.empty() ? "Private seat worker did not publish a complete route." : error);
    }
    route->windows_session_id = resource->windows_session_id;
    route->seat_id = resource->seat_id;
    auto &stored = seats_.at(launch.client_uuid);
    stored.state = seat_state_e::running;
    stored.route = *route;
    stored.resource = *resource;
    return stored.route;
  }

  state_t runtime_t::snapshot(std::string_view client_uuid) const {
    std::lock_guard lock {mutex_};
    const auto found = seats_.find(std::string {client_uuid});
    if (found == seats_.end()) return {};
    return {.exists = true, .ready = found->second.state == seat_state_e::ready || found->second.state == seat_state_e::running || found->second.state == seat_state_e::retained,
            .connected = found->second.state == seat_state_e::running, .windows_session_id = found->second.route.windows_session_id,
            .seat_id = found->second.route.seat_id};
  }

  bool runtime_t::release_locked(std::string_view client_uuid, std::string_view, const protocol::release_mode mode) {
    const auto found = seats_.find(std::string {client_uuid});
    if (found == seats_.end()) return true;
    if (mode == protocol::release_mode::retain && found->second.state == seat_state_e::retained) return true;
    const bool can_retain = mode == protocol::release_mode::retain &&
      (found->second.state == seat_state_e::ready || found->second.state == seat_state_e::running) &&
      found->second.worker;
    found->second.state = seat_state_e::stopping;
    if (can_retain) {
      if (!found->second.worker || !found->second.worker->park(found->second.route)) {
        found->second.state = seat_state_e::failed;
        return false;
      }
      if (!provider_->release_checked(found->second.resource, mode)) {
        found->second.state = seat_state_e::failed;
        return false;
      }
      found->second.resource.launch_token = 0;
      found->second.state = seat_state_e::retained;
      return true;
    }
    const bool worker_stopped = found->second.worker && found->second.worker->stop(found->second.route);
    if (!worker_stopped) { found->second.state = seat_state_e::failed; return false; }
    const auto teardown_mode = mode == protocol::release_mode::retain ? protocol::release_mode::abandon : mode;
    if (!provider_->release_checked(found->second.resource, teardown_mode)) { found->second.state = seat_state_e::failed; return false; }
    found->second.state = seat_state_e::idle;
    seats_.erase(found);
    return true;
  }

  bool runtime_t::disconnect(std::string_view client_uuid, std::string_view reason, const protocol::release_mode mode) {
    std::lock_guard lock {mutex_};
    return release_locked(client_uuid, reason, mode);
  }

  void runtime_t::unpair(std::string_view client_uuid) { std::lock_guard lock {mutex_}; (void) release_locked(client_uuid, "unpair", protocol::release_mode::abandon); }

  void runtime_t::shutdown() {
    std::lock_guard lock {mutex_};
    std::vector<std::string> clients;
    clients.reserve(seats_.size());
    for (const auto &[client, _] : seats_) clients.push_back(client);
    for (const auto &client : clients) (void) release_locked(client, "shutdown", protocol::release_mode::shutdown);
  }

  namespace {
    std::mutex production_mutex;
    std::unique_ptr<runtime_t> production_runtime;
#ifdef _WIN32
    class remote_runtime final {
    public:
      route_t prepare(request_t request) {
        if (!request.launch_session) return {.retryable = true, .error = "Terminal launch material is missing."};
        auto &launch = *request.launch_session;
        if (request.operation == operation_e::resume && !routes_.contains(launch.client_uuid)) {
          if (recover(launch.client_uuid) == snapshot_status_e::unavailable) {
            return {.retryable = true, .error = "Terminal broker seat status is unavailable."};
          }
        }
        if (launch.role_generation == 0) {
          if (request.operation == operation_e::resume) {
            const auto retained = routes_.find(launch.client_uuid);
            if (retained == routes_.end()) return {.retryable = false, .error = "No retained terminal seat exists for this paired client."};
            launch.role_generation = retained->second.generation;
          } else {
            launch.role_generation = launch.id;
          }
        }
        const std::uint64_t generation = launch.role_generation == 0 ? launch.id : launch.role_generation;
        protocol::request_t challenge {.operation = protocol::opcode::control_challenge, .client_uuid = launch.client_uuid,
                                       .generation = generation, .launch_id = launch.id};
        challenge.ticket.operation = protocol::opcode::control_prepare;
        auto challenge_response = service::pipe_client_t::transact(challenge);
        if (!challenge_response || !challenge_response->accepted || !challenge_response->ticket) return {.retryable = true, .error = "Terminal broker admission challenge failed."};
        std::string payload_error;
        auto payload = launch_codec::encode(request, payload_error);
        if (payload.empty()) return {.retryable = false, .error = payload_error.empty() ? "Terminal launch serialization failed." : payload_error};
        protocol::request_t control {.operation = protocol::opcode::control_prepare, .client_uuid = launch.client_uuid,
                                     .generation = generation, .launch_id = launch.id, .launch_payload = std::move(payload),
                                     .ticket = *challenge_response->ticket};
        // First admission may create an account, complete Winlogon/DWM/Remote
        // IDD startup, and then initialize a private encoder process.
        auto response = service::pipe_client_t::transact(control, 150000);
        if (!response) return {.retryable = true, .error = "Terminal broker service is unavailable."};
        route_t route {.accepted = response->accepted, .ready = response->accepted, .retryable = !response->accepted,
                       .rtsp_port = response->rtsp_port, .control_port = response->control_port,
                       .video_port = response->video_port, .audio_port = response->audio_port,
                       .windows_session_id = response->windows_session_id, .seat_id = response->seat_id,
                       .error = response->error};
        if (route.ready) {
          routes_[launch.client_uuid] = {
            .route = route,
            .generation = generation,
            .launch_id = launch.id,
            .connected = true,
            .app_id = launch.appid,
            .app_uuid = launch.app_metadata ? launch.app_metadata->uuid : std::string {},
            .app_name = launch.app_metadata ? launch.app_metadata->name : std::string {},
          };
        }
        return route;
      }
      snapshot_result_t snapshot(std::string_view uuid) {
        const std::string client {uuid};
        if (!routes_.contains(client)) {
          const auto recovered = recover(client);
          if (recovered != snapshot_status_e::present) return {.status = recovered};
        }
        const auto found = routes_.find(client);
        if (found == routes_.end()) {
          return {.status = snapshot_status_e::absent};
        }
        return {.status = snapshot_status_e::present,
                .state = {.exists = true, .ready = true, .connected = found->second.connected,
                          .app_id = found->second.app_id, .app_uuid = found->second.app_uuid, .app_name = found->second.app_name,
                          .windows_session_id = found->second.route.windows_session_id, .seat_id = found->second.route.seat_id}};
      }
      bool release(std::string_view uuid, const protocol::release_mode mode) {
        const std::string client {uuid};
        if (!routes_.contains(client) && recover(client) == snapshot_status_e::unavailable) return false;
        const auto found = routes_.find(client);
        if (found == routes_.end()) return true;
        protocol::request_t challenge {.operation = protocol::opcode::control_challenge, .release = mode,
                                       .client_uuid = std::string {uuid}, .generation = found->second.generation,
                                       .launch_id = found->second.launch_id};
        challenge.ticket.operation = protocol::opcode::control_release;
        challenge.ticket.release = mode;
        const auto challenge_response = service::pipe_client_t::transact(challenge);
        if (!challenge_response || !challenge_response->accepted || !challenge_response->ticket) return false;
        protocol::request_t control {.operation = protocol::opcode::control_release, .release = mode,
                                     .client_uuid = std::string {uuid}, .generation = found->second.generation,
                                     .launch_id = found->second.launch_id, .ticket = *challenge_response->ticket};
        const auto response = service::pipe_client_t::transact(control, 30000);
        if (!response || !response->accepted) return false;
        if (mode == protocol::release_mode::retain) {
          found->second.connected = false;
        } else {
          routes_.erase(found);
        }
        return true;
      }
      void forget() { routes_.clear(); }
    private:
      struct route_record {
        route_t route;
        std::uint64_t generation {};
        std::uint32_t launch_id {};
        bool connected {};
        std::int32_t app_id {};
        std::string app_uuid;
        std::string app_name;
      };

      snapshot_status_e recover(const std::string_view uuid) {
        if (uuid.empty()) return snapshot_status_e::unavailable;
        protocol::request_t challenge {
          .operation = protocol::opcode::control_challenge,
          .client_uuid = std::string {uuid},
          .generation = 1,
          .launch_id = 1,
        };
        challenge.ticket.operation = protocol::opcode::control_query;
        const auto issued = service::pipe_client_t::transact(challenge);
        if (!issued || !issued->accepted || !issued->ticket) {
          BOOST_LOG(warning) << "Terminal broker snapshot challenge was unavailable.";
          return snapshot_status_e::unavailable;
        }
        protocol::request_t query {
          .operation = protocol::opcode::control_query,
          .client_uuid = std::string {uuid},
          .generation = 1,
          .launch_id = 1,
          .ticket = *issued->ticket,
        };
        const auto response = service::pipe_client_t::transact(query);
        if (!response || !response->accepted) {
          BOOST_LOG(warning) << "Terminal broker snapshot query was unavailable.";
          return snapshot_status_e::unavailable;
        }
        if (!response->state_exists) return snapshot_status_e::absent;
        if (response->owner_generation == 0 || response->owner_launch_id == 0 || response->app_id <= 0 ||
            response->rtsp_port == 0 || response->control_port == 0 || response->video_port == 0 || response->audio_port == 0 ||
            response->windows_session_id == 0 || response->seat_id.empty()) return snapshot_status_e::unavailable;
        route_t route {
          .accepted = true,
          .ready = true,
          .rtsp_port = response->rtsp_port,
          .control_port = response->control_port,
          .video_port = response->video_port,
          .audio_port = response->audio_port,
          .windows_session_id = response->windows_session_id,
          .seat_id = response->seat_id,
        };
        routes_.insert_or_assign(std::string {uuid}, route_record {
          std::move(route), response->owner_generation, response->owner_launch_id, response->state_connected,
          response->app_id, response->app_uuid, response->app_name
        });
        return snapshot_status_e::present;
      }
      std::unordered_map<std::string, route_record> routes_;
    };
    std::unique_ptr<remote_runtime> production_remote;
#endif
  }

  void register_production_runtime() {
    std::lock_guard lock {production_mutex};
#ifdef _WIN32
    production_remote = std::make_unique<remote_runtime>();
    register_runtime_hooks({
      .prepare = [](request_t request) { std::lock_guard lock {production_mutex}; return production_remote ? production_remote->prepare(std::move(request)) : route_t {.retryable = true, .error = "Terminal broker service is unavailable."}; },
      .snapshot = [](std::string_view uuid) {
        std::lock_guard lock {production_mutex};
        return production_remote ? production_remote->snapshot(uuid) : snapshot_result_t {};
      },
      .disconnect = [](std::string_view uuid, std::string_view reason) {
        const auto mode = reason == "Terminal emulation disabled" ? protocol::release_mode::abandon : protocol::release_mode::retain;
        std::lock_guard lock {production_mutex};
        return production_remote && production_remote->release(uuid, mode);
      },
      .unpair = [](std::string_view uuid) { std::lock_guard lock {production_mutex}; if (production_remote) (void) production_remote->release(uuid, protocol::release_mode::abandon); },
      // The SCM service owns seat teardown. Main-process shutdown only forgets
      // its projection so ordinary restart can recover retained/connected seats.
      .shutdown = [] { std::lock_guard lock {production_mutex}; if (production_remote) production_remote->forget(); },
    });
#else
    production_runtime = std::make_unique<runtime_t>(std::make_unique<unsupported_provider>(), std::make_unique<unsupported_worker>());
    register_runtime_hooks({
      .prepare = [](request_t request) { std::lock_guard lock {production_mutex}; return production_runtime ? production_runtime->prepare(std::move(request)) : route_t {.retryable = true, .error = "Terminal session runtime is unavailable."}; },
      .snapshot = [](std::string_view uuid) {
        std::lock_guard lock {production_mutex};
        if (!production_runtime) return snapshot_result_t {};
        auto state = production_runtime->snapshot(uuid);
        return snapshot_result_t {.status = state.exists ? snapshot_status_e::present : snapshot_status_e::absent, .state = std::move(state)};
      },
      .disconnect = [](std::string_view uuid, std::string_view reason) { std::lock_guard lock {production_mutex}; return production_runtime && production_runtime->disconnect(uuid, reason); },
      .unpair = [](std::string_view uuid) { std::lock_guard lock {production_mutex}; if (production_runtime) production_runtime->unpair(uuid); },
      .shutdown = [] { std::lock_guard lock {production_mutex}; if (production_runtime) production_runtime->shutdown(); },
    });
#endif
  }

  void unregister_production_runtime() {
    std::lock_guard lock {production_mutex};
    if (production_runtime) production_runtime->shutdown();
    production_runtime.reset();
#ifdef _WIN32
    production_remote.reset();
#endif
    register_runtime_hooks({});
  }
} // namespace terminal_session

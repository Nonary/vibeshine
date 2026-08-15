#include "terminal_session_runtime.h"
#include "rtsp.h"
#include "terminal_session_worker.h"
#include "terminal_session_service.h"

#include <algorithm>
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

  runtime_t::~runtime_t() { shutdown(); }

  route_t runtime_t::reject(bool retryable, std::string error) const {
    return {.accepted = false, .ready = false, .retryable = retryable, .error = std::move(error)};
  }

  route_t runtime_t::prepare(request_t request) {
    if (!request.launch_session) return reject(false, "Terminal session launch material is missing.");
    const auto &launch = *request.launch_session;
    const std::uint64_t generation = launch.role_generation == 0 ? launch.id : launch.role_generation;
    if (generation == 0 || launch.id == 0 || launch.client_uuid.empty()) return reject(false, "Terminal session identity or launch generation is missing.");

    std::lock_guard lock {mutex_};
    auto existing = seats_.find(launch.client_uuid);
    if (existing != seats_.end()) {
      // Reconnect carries a new launch ID but the same role generation. It
      // may reuse the already-admitted worker route; a different generation
      // is a stale request and may never replay the original seat launch.
      if (existing->second.generation != generation) return reject(false, "Terminal session launch generation is stale.");
      if (request.operation == operation_e::resume && existing->second.state == seat_state_e::ready) return existing->second.route;
      if (existing->second.launch_id != launch.id) return reject(false, "Terminal session launch ID is stale.");
      if (existing->second.state == seat_state_e::running || existing->second.state == seat_state_e::preparing) return reject(true, "Terminal session is already being prepared.");
      if (!release_locked(launch.client_uuid, "replaced launch")) {
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
    seats_.emplace(launch.client_uuid, seat);

    const auto now = protocol::admission_authority::clock_t::now();
    auto ticket = authority_.issue(launch.client_uuid, generation, launch.id, now, protocol::opcode::prepare);
    if (!ticket) {
      seats_.erase(launch.client_uuid);
      return reject(true, "Terminal-session admission nonce generation failed.");
    }
    provider_request_t provider_request {launch.client_uuid, generation, launch.id};
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

    const auto worker_contract = worker::make_contract(resource->seat_id, resource->rtsp_port, resource->control_port, resource->video_port, resource->audio_port);
    worker_request_t worker_request {provider_request, *resource, *ticket, worker_contract.config_root, worker_contract.state_root, worker_contract.log_root};
    std::optional<route_t> route;
    try {
      route = worker_->start(worker_request, error);
    } catch (...) {
      error = "Private seat worker raised during admission.";
    }
    if (!route || !route->accepted || !route->ready || route->rtsp_port == 0 || route->control_port == 0 || route->video_port == 0 || route->audio_port == 0) {
      bool worker_cleanup_failed = worker_->cleanup_needed();
      if (route || worker_cleanup_failed) {
        worker_cleanup_failed = !worker_->stop(route.value_or(route_t {})) || worker_->cleanup_needed();
      }
      if (worker_cleanup_failed) {
        auto &failed = seats_.at(launch.client_uuid);
        failed.state = seat_state_e::failed;
        failed.resource = *resource;
        failed.route = route.value_or(route_t {});
        return reject(true, "Private worker rollback failed; retry teardown before replacement.");
      }
      if (!provider_->release_checked(*resource)) {
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
    stored.state = seat_state_e::ready;
    stored.route = *route;
    stored.resource = *resource;
    return stored.route;
  }

  state_t runtime_t::snapshot(std::string_view client_uuid) const {
    std::lock_guard lock {mutex_};
    const auto found = seats_.find(std::string {client_uuid});
    if (found == seats_.end()) return {};
    return {.exists = true, .ready = found->second.state == seat_state_e::ready || found->second.state == seat_state_e::running,
            .connected = found->second.state == seat_state_e::running, .windows_session_id = found->second.route.windows_session_id,
            .seat_id = found->second.route.seat_id};
  }

  bool runtime_t::release_locked(std::string_view client_uuid, std::string_view) {
    const auto found = seats_.find(std::string {client_uuid});
    if (found == seats_.end()) return true;
    found->second.state = seat_state_e::stopping;
    const bool worker_stopped = worker_->stop(found->second.route);
    if (!worker_stopped) { found->second.state = seat_state_e::failed; return false; }
    if (!provider_->release_checked(found->second.resource)) { found->second.state = seat_state_e::failed; return false; }
    found->second.state = seat_state_e::idle;
    seats_.erase(found);
    return true;
  }

  bool runtime_t::disconnect(std::string_view client_uuid, std::string_view reason) {
    std::lock_guard lock {mutex_};
    return release_locked(client_uuid, reason);
  }

  void runtime_t::unpair(std::string_view client_uuid) { std::lock_guard lock {mutex_}; (void) release_locked(client_uuid, "unpair"); }

  void runtime_t::shutdown() {
    std::lock_guard lock {mutex_};
    std::vector<std::string> clients;
    clients.reserve(seats_.size());
    for (const auto &[client, _] : seats_) clients.push_back(client);
    for (const auto &client : clients) (void) release_locked(client, "shutdown");
  }

  namespace {
    std::mutex production_mutex;
    std::unique_ptr<runtime_t> production_runtime;
#ifdef _WIN32
    class remote_runtime final {
    public:
      route_t prepare(request_t request) {
        if (!request.launch_session) return {.retryable = true, .error = "Terminal launch material is missing."};
        const auto &launch = *request.launch_session;
        const std::uint64_t generation = launch.role_generation == 0 ? launch.id : launch.role_generation;
        protocol::request_t challenge {.operation = protocol::opcode::control_challenge, .client_uuid = launch.client_uuid,
                                       .generation = generation, .launch_id = launch.id};
        challenge.ticket.operation = protocol::opcode::control_prepare;
        auto challenge_response = service::pipe_client_t::transact(challenge);
        if (!challenge_response || !challenge_response->accepted || !challenge_response->ticket) return {.retryable = true, .error = "Terminal broker admission challenge failed."};
        protocol::request_t control {.operation = protocol::opcode::control_prepare, .client_uuid = launch.client_uuid,
                                     .generation = generation, .launch_id = launch.id, .ticket = *challenge_response->ticket};
        auto response = service::pipe_client_t::transact(control);
        if (!response) return {.retryable = true, .error = "Terminal broker service is unavailable."};
        route_t route {.accepted = response->accepted, .ready = response->accepted, .retryable = !response->accepted,
                       .rtsp_port = response->rtsp_port, .control_port = response->control_port,
                       .video_port = response->video_port, .audio_port = response->audio_port,
                       .windows_session_id = response->windows_session_id, .seat_id = response->seat_id,
                       .error = response->error};
        if (route.ready) routes_[launch.client_uuid] = {.route = route, .generation = generation, .launch_id = launch.id};
        return route;
      }
      state_t snapshot(std::string_view uuid) const {
        const auto found = routes_.find(std::string {uuid});
        if (found == routes_.end()) return {};
        return {.exists = true, .ready = true, .connected = true, .windows_session_id = found->second.route.windows_session_id, .seat_id = found->second.route.seat_id};
      }
      bool release(std::string_view uuid) {
        const auto found = routes_.find(std::string {uuid});
        if (found == routes_.end()) return true;
        protocol::request_t challenge {.operation = protocol::opcode::control_challenge, .client_uuid = std::string {uuid},
                                       .generation = found->second.generation, .launch_id = found->second.launch_id};
        challenge.ticket.operation = protocol::opcode::control_release;
        const auto challenge_response = service::pipe_client_t::transact(challenge);
        if (!challenge_response || !challenge_response->accepted || !challenge_response->ticket) return false;
        protocol::request_t control {.operation = protocol::opcode::control_release, .client_uuid = std::string {uuid},
                                     .generation = found->second.generation, .launch_id = found->second.launch_id, .ticket = *challenge_response->ticket};
        const auto response = service::pipe_client_t::transact(control);
        if (!response || !response->accepted) return false;
        routes_.erase(found);
        return true;
      }
      void clear() { std::vector<std::string> clients; clients.reserve(routes_.size()); for (const auto &[uuid, _] : routes_) clients.push_back(uuid); for (const auto &uuid : clients) (void) release(uuid); }
    private:
      struct route_record { route_t route; std::uint64_t generation {}; std::uint32_t launch_id {}; };
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
      .snapshot = [](std::string_view uuid) { std::lock_guard lock {production_mutex}; return production_remote ? production_remote->snapshot(uuid) : state_t {}; },
      .disconnect = [](std::string_view uuid, std::string_view) { std::lock_guard lock {production_mutex}; return production_remote && production_remote->release(uuid); },
      .unpair = [](std::string_view uuid) { std::lock_guard lock {production_mutex}; if (production_remote) (void) production_remote->release(uuid); },
      .shutdown = [] { std::lock_guard lock {production_mutex}; if (production_remote) production_remote->clear(); },
    });
#else
    production_runtime = std::make_unique<runtime_t>(std::make_unique<unsupported_provider>(), std::make_unique<unsupported_worker>());
    register_runtime_hooks({
      .prepare = [](request_t request) { std::lock_guard lock {production_mutex}; return production_runtime ? production_runtime->prepare(std::move(request)) : route_t {.retryable = true, .error = "Terminal session runtime is unavailable."}; },
      .snapshot = [](std::string_view uuid) { std::lock_guard lock {production_mutex}; return production_runtime ? production_runtime->snapshot(uuid) : state_t {}; },
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
    if (production_remote) production_remote->clear();
    production_remote.reset();
#endif
    register_runtime_hooks({});
  }
} // namespace terminal_session

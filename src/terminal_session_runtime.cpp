#include "terminal_session_runtime.h"
#include "rtsp.h"
#include "terminal_session_worker.h"

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
      (void) release_locked(launch.client_uuid, "replaced launch");
      seats_.erase(launch.client_uuid);
    }

    const auto capability = provider_->preflight();
    if (!capability.supported || !capability.concurrent_sessions || !capability.remote_display || !capability.audio_endpoint || !capability.token_launch) {
      return reject(true, capability.error.empty() ? "Terminal session provider is not capable of a complete seat." : capability.error);
    }

    seat_t seat;
    seat.state = seat_state_e::preparing;
    seat.generation = generation;
    seat.launch_id = launch.id;
    seats_.emplace(launch.client_uuid, seat);

    const auto now = protocol::admission_authority::clock_t::now();
    auto ticket = authority_.issue(launch.client_uuid, generation, launch.id, now);
    provider_request_t provider_request {launch.client_uuid, generation, launch.id};
    std::string error;
    auto resource = provider_->allocate(provider_request, error);
    if (!resource) {
      seats_.erase(launch.client_uuid);
      return reject(true, error.empty() ? "Terminal session provider allocation failed." : error);
    }

    const auto worker_contract = worker::make_contract(resource->seat_id, resource->rtsp_port, resource->control_port, resource->video_port, resource->audio_port);
    worker_request_t worker_request {provider_request, *resource, ticket, worker_contract.config_root, worker_contract.state_root, worker_contract.log_root};
    auto route = worker_->start(worker_request, error);
    if (!route || !route->accepted || !route->ready || route->rtsp_port == 0 || route->control_port == 0 || route->video_port == 0 || route->audio_port == 0) {
      provider_->release(*resource);
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
    provider_->release(found->second.resource);
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
  }

  void register_production_runtime() {
    std::lock_guard lock {production_mutex};
    production_runtime = std::make_unique<runtime_t>(std::make_unique<unsupported_provider>(), std::make_unique<unsupported_worker>());
    register_runtime_hooks({
      .prepare = [](request_t request) { std::lock_guard lock {production_mutex}; return production_runtime ? production_runtime->prepare(std::move(request)) : route_t {.retryable = true, .error = "Terminal session runtime is unavailable."}; },
      .snapshot = [](std::string_view uuid) { std::lock_guard lock {production_mutex}; return production_runtime ? production_runtime->snapshot(uuid) : state_t {}; },
      .disconnect = [](std::string_view uuid, std::string_view reason) { std::lock_guard lock {production_mutex}; return production_runtime && production_runtime->disconnect(uuid, reason); },
      .unpair = [](std::string_view uuid) { std::lock_guard lock {production_mutex}; if (production_runtime) production_runtime->unpair(uuid); },
      .shutdown = [] { std::lock_guard lock {production_mutex}; if (production_runtime) production_runtime->shutdown(); },
    });
  }

  void unregister_production_runtime() {
    std::lock_guard lock {production_mutex};
    if (production_runtime) production_runtime->shutdown();
    production_runtime.reset();
    register_runtime_hooks({});
  }
} // namespace terminal_session

/**
 * @file tools/steam_ui_helper/controller.cpp
 * @brief Keeps Vibeshine's script injected into Steam's shared JS context and relays its messages.
 */
#include "controller.h"

#include "src/steam_big_picture_controls_protocol.h"

#include <exception>
#include <stdexcept>

namespace steam_ui_helper {
  namespace policy = platf::steam::big_picture::policy;
  namespace protocol = platf::steam::big_picture::protocol;

  namespace {
    // Steam reports its build date through SteamClient; a missing value only leaves the report blank.
    constexpr std::string_view steam_build_expression = "SteamClient.System.GetSystemInfo().then((i) => i.sSteamBuildDate).catch(() => '')";

    std::string exception_text(const json &details) {
      auto text = details.value("/exception/description"_json_pointer, details.value("text", std::string("script error")));
      if (const auto line = text.find('\n'); line != std::string::npos) {
        text.resize(line);
      }
      return text;
    }
  }  // namespace

  controller_t::controller_t(steam_link_t &link, controller_options_t options):
      link_(link),
      options_(std::move(options)) {
  }

  controller_t::~controller_t() {
    // The session's reader thread queues events into members below; stop it first.
    session_.reset();
  }

  void controller_t::set_status(json status) {
    {
      std::lock_guard lock(mutex_);
      status_ = std::move(status);
      ++status_version_;
    }
    changed_.notify_all();
  }

  void controller_t::queue_event(std::uint64_t generation, cdp_event_t event) {
    {
      std::lock_guard lock(mutex_);
      events_.emplace_back(generation, std::move(event));
    }
    changed_.notify_all();
  }

  void controller_t::step() {
    const auto now = options_.now();
    std::deque<std::pair<std::uint64_t, cdp_event_t>> events;
    {
      std::lock_guard lock(mutex_);
      events.swap(events_);
    }
    for (const auto &[generation, event] : events) {
      // Events from a probe or a dropped connection belong to an older generation.
      if (generation != generation_) {
        continue;
      }
      try {
        handle(event, now);
      } catch (const std::exception &error) {
        log("warning", std::string("ignored an unexpected event from Steam: ") + error.what());
      }
    }

    if (!session_) {
      if (next_connect_ && now < *next_connect_) {
        return;
      }
      connect(now);
      if (!session_) {
        return;
      }
    }
    if (!injected_) {
      if (script_refused_) {
        return;
      }
      if (tracker_.tripped()) {
        set_state(protocol::steam_state::breaker_open);
        return;
      }
      if (!tracker_.may_attempt(now)) {
        return;
      }
      inject(now);
    }
    if (injected_ && !script_disabled_) {
      push_status();
    }
  }

  void controller_t::run(std::stop_token stop) {
    while (!stop.stop_requested()) {
      try {
        step();
      } catch (const std::exception &error) {
        log("error", std::string("unexpected error: ") + error.what());
      }
      std::unique_lock lock(mutex_);
      changed_.wait_for(lock, stop, std::chrono::milliseconds(500), [this] {
        return !events_.empty() || (injected_ && !script_disabled_ && status_version_ != pushed_version_);
      });
    }
    unpatch();
  }

  json controller_t::check() {
    if (!session_) {
      connect(options_.now());
    }
    if (!session_) {
      throw std::runtime_error("Steam's debugger is not reachable (" + state_ + ")");
    }
    auto options = options_.inject_options;
    options["checkOnly"] = true;
    const auto result = evaluate(policy::inject_expression(options, options_.script));
    const auto report = policy::report_message(result.value("/result/value"_json_pointer, json {}), steam_build_);
    if (!report) {
      throw std::runtime_error("the Steam script speaks a different protocol version");
    }
    return *report;
  }

  void controller_t::unpatch() {
    if (!session_ || !injected_) {
      return;
    }
    try {
      evaluate(policy::unpatch_expression);
      injected_ = false;
    } catch (const std::exception &error) {
      log("warning", std::string("could not remove the Steam patches: ") + error.what());
    }
  }

  void controller_t::handle(const cdp_event_t &event, clock::time_point now) {
    if (event.method == closed_event) {
      log("info", "Steam connection closed");
      drop_session();
      on_reload(now);
      next_connect_ = now + options_.reconnect_delay;
      return;
    }
    if (event.method == "Runtime.executionContextsCleared") {
      contexts_cleared_ = true;
      return;
    }
    if (event.method == "Runtime.executionContextCreated") {
      const auto context = event.params.value("context", json::object());
      const auto aux = context.value("auxData", json::object());
      if (!aux.value("isDefault", false)) {
        return;
      }
      const auto frame = aux.value("frameId", std::string {});
      const auto id = context.value("id", std::int64_t {0});
      // The first default context is the page itself; later ones in the same frame mean Steam's UI
      // reloaded. Default contexts of other frames are ignored unless the page was cleared first.
      if (!main_frame_) {
        main_frame_ = frame;
        default_context_ = id;
        return;
      }
      if ((frame != *main_frame_ && !contexts_cleared_) || id == default_context_) {
        return;
      }
      main_frame_ = frame;
      default_context_ = id;
      contexts_cleared_ = false;
      on_reload(now);
      return;
    }
    if (event.method == "Runtime.bindingCalled") {
      if (event.params.value("name", std::string {}) != policy::bridge_binding) {
        return;
      }
      const auto message = policy::script_message(event.params.value("payload", std::string {}));
      if (!message) {
        log("warning", "ignored a malformed message from Steam");
        return;
      }
      if (message->value("type", std::string {}) == protocol::message::disabled) {
        script_disabled_ = true;
      }
      emit(*message);
    }
  }

  void controller_t::connect(clock::time_point now) {
    const auto verdict = policy::endpoint_verdict(link_.endpoint_facts());
    if (verdict != policy::endpoint_e::ok) {
      set_state(policy::endpoint_state_name(verdict));
      next_connect_ = now + options_.reconnect_delay;
      return;
    }
    set_state(protocol::steam_state::connecting);

    json targets;
    try {
      targets = link_.targets();
    } catch (const std::exception &error) {
      log("warning", std::string("could not list Steam's pages: ") + error.what());
      next_connect_ = now + options_.reconnect_delay;
      return;
    }
    for (const auto &candidate : policy::shared_context_candidates(targets)) {
      const auto generation = ++generation_;
      std::unique_ptr<cdp_session_t> session;
      try {
        session = link_.connect(candidate.ws_url, [this, generation](cdp_event_t event) {
          queue_event(generation, std::move(event));
        });
        const auto probe = session->call("Runtime.evaluate", {{"expression", policy::is_shared_context_expression}, {"returnByValue", true}});
        if (probe.value("/result/value"_json_pointer, false) != true) {
          continue;
        }
        session->call("Runtime.enable", json::object());
        session->call("Runtime.addBinding", {{"name", policy::bridge_binding}});
      } catch (const std::exception &) {
        continue;
      }
      session_ = std::move(session);
      main_frame_.reset();
      default_context_.reset();
      contexts_cleared_ = false;
      next_connect_.reset();
      try {
        steam_build_ = evaluate(steam_build_expression, true).value("/result/value"_json_pointer, std::string {});
      } catch (const std::exception &) {
        steam_build_.clear();
      }
      log("info", "connected to Steam's shared JS context (\"" + candidate.title + "\")");
      return;
    }
    // Steam is probably still starting. Leave the generation of the last probe so its close is ignored.
    ++generation_;
    next_connect_ = now + options_.reconnect_delay;
  }

  void controller_t::drop_session() {
    session_.reset();
    ++generation_;
  }

  void controller_t::inject(clock::time_point now) {
    try {
      const auto result = evaluate(policy::inject_expression(options_.inject_options, options_.script));
      const auto report = policy::report_message(result.value("/result/value"_json_pointer, json {}), steam_build_);
      if (!report) {
        script_refused_ = true;
        log("error", "the Steam script speaks a different protocol version; not injecting it");
        return;
      }
      injected_ = true;
      script_disabled_ = false;
      pushed_version_ = 0;
      tracker_.injected(now);
      emit(*report);
      set_state(protocol::steam_state::injected);
    } catch (const std::exception &error) {
      const auto delay = std::chrono::duration_cast<std::chrono::seconds>(tracker_.failed(now));
      log("warning", std::string("injection failed (") + error.what() + "); retrying in " + std::to_string(delay.count()) + " s");
    }
  }

  void controller_t::push_status() {
    json status;
    std::uint64_t version;
    {
      std::lock_guard lock(mutex_);
      if (!status_ || status_version_ == pushed_version_) {
        return;
      }
      status = *status_;
      version = status_version_;
    }
    try {
      evaluate(policy::set_status_expression(status));
      pushed_version_ = version;
    } catch (const std::exception &error) {
      log("warning", std::string("could not update the status in Steam: ") + error.what());
    }
  }

  void controller_t::on_reload(clock::time_point now) {
    if (!injected_) {
      return;
    }
    injected_ = false;
    script_disabled_ = false;
    pushed_version_ = 0;
    if (tracker_.reloaded(now)) {
      log("warning", "Steam's UI reloaded " + std::to_string(policy::injection_tracker_t::breaker_reloads) + " times right after injection; no longer injecting until the helper restarts");
      set_state(protocol::steam_state::breaker_open);
      return;
    }
    log("info", "Steam's UI reloaded; injecting again");
    set_state(protocol::steam_state::connecting);
  }

  json controller_t::evaluate(std::string_view expression, bool await_promise) {
    const auto result = session_->call("Runtime.evaluate", {{"expression", expression}, {"returnByValue", true}, {"awaitPromise", await_promise}});
    if (const auto details = result.find("exceptionDetails"); details != result.end()) {
      throw std::runtime_error(exception_text(*details));
    }
    return result;
  }

  void controller_t::set_state(std::string_view state, std::string detail) {
    if (state == state_) {
      return;
    }
    state_ = state;
    emit({{"type", protocol::message::state}, {"steam", state}, {"detail", std::move(detail)}});
  }

  void controller_t::log(std::string_view level, std::string message) {
    emit({{"type", protocol::message::log}, {"level", level}, {"message", std::move(message)}});
  }

  void controller_t::emit(const json &message) {
    if (options_.emit) {
      options_.emit(message);
    }
  }
}  // namespace steam_ui_helper

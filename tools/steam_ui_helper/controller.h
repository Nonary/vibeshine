/**
 * @file tools/steam_ui_helper/controller.h
 * @brief Keeps Vibeshine's script injected into Steam's shared JS context and relays its messages.
 *
 * Platform-independent: Steam's debugger is reached through steam_link_t, so tests drive the
 * controller with a fake Steam and a fake clock.
 */
#pragma once

#include "src/steam_big_picture_controls_policy.h"

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <nlohmann/json.hpp>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <utility>

namespace steam_ui_helper {
  using json = nlohmann::json;
  using clock = std::chrono::steady_clock;

  /** Pseudo event method delivered last, when a connection closes. */
  inline constexpr std::string_view closed_event = "<closed>";

  struct cdp_event_t {
    std::string method;
    json params;
  };

  /** One CDP connection to a Steam page. */
  class cdp_session_t {
  public:
    virtual ~cdp_session_t() = default;
    /** Send a command and wait for its result. Throws on a CDP error, a timeout or a closed connection. */
    virtual json call(std::string_view method, const json &params) = 0;
  };

  /** Steam's debugger, as seen from the helper. */
  class steam_link_t {
  public:
    virtual ~steam_link_t() = default;
    virtual platf::steam::big_picture::policy::endpoint_facts_t endpoint_facts() = 0;
    /** Steam's /json target list. */
    virtual json targets() = 0;
    /** Connect to a page. on_event may be called from another thread, and is called last with closed_event. */
    virtual std::unique_ptr<cdp_session_t> connect(const std::string &ws_url, std::function<void(cdp_event_t)> on_event) = 0;
  };

  struct controller_options_t {
    std::string script;  ///< Source of assets/steam/big_picture_controls.js
    json inject_options = json::object();
    std::function<void(const json &)> emit;  ///< Messages for the service, as in the protocol header
    std::function<clock::time_point()> now = [] {
      return clock::now();
    };
    clock::duration reconnect_delay = std::chrono::seconds(2);
  };

  class controller_t {
  public:
    controller_t(steam_link_t &link, controller_options_t options);
    ~controller_t();
    controller_t(const controller_t &) = delete;
    controller_t &operator=(const controller_t &) = delete;

    /** The status payload to show inside Steam. Thread-safe. */
    void set_status(json status);

    /** One pass: handle queued events, connect if needed, (re)inject, push status. Doesn't wait. */
    void step();

    /** step() until stop is requested, waking for events and status changes; removes the patches on exit. */
    void run(std::stop_token stop);

    /** Inject in check-only mode and return the "report" message. Throws when Steam can't be reached. */
    json check();

    /** Run check() on the next step and emit a "check_result" message with this id. Thread-safe. */
    void request_check(std::uint64_t id);

    /** Remove the script's patches from Steam, if injected (best effort). */
    void unpatch();

  private:
    void queue_event(std::uint64_t generation, cdp_event_t event);
    void handle(const cdp_event_t &event, clock::time_point now);
    void connect(clock::time_point now);
    void drop_session();
    void inject(clock::time_point now);
    void push_status();
    void on_reload(clock::time_point now);
    json evaluate(std::string_view expression, bool await_promise = false);
    void set_state(std::string_view state, std::string detail = {});
    void log(std::string_view level, std::string message);
    void emit(const json &message);

    steam_link_t &link_;
    controller_options_t options_;
    platf::steam::big_picture::policy::injection_tracker_t tracker_;

    // Owned by the thread calling step().
    std::unique_ptr<cdp_session_t> session_;
    std::uint64_t generation_ = 0;
    std::optional<clock::time_point> next_connect_;
    std::optional<std::string> main_frame_;
    std::optional<std::int64_t> default_context_;
    bool contexts_cleared_ = false;
    bool injected_ = false;
    bool script_disabled_ = false;
    bool script_refused_ = false;
    std::uint64_t pushed_version_ = 0;
    std::string steam_build_;
    std::string state_;

    // Shared with event and status producers.
    std::mutex mutex_;
    std::condition_variable_any changed_;
    std::deque<std::pair<std::uint64_t, cdp_event_t>> events_;
    std::deque<std::uint64_t> checks_;
    std::optional<json> status_;
    std::uint64_t status_version_ = 0;
  };
}  // namespace steam_ui_helper

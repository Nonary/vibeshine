/**
 * @file tests/unit/test_steam_ui_helper_controller.cpp
 * @brief Steam UI helper lifecycle against a fake Steam debugger and a fake clock.
 */
#include "tools/steam_ui_helper/controller.h"

#include <gtest/gtest.h>

namespace {
  using namespace std::chrono_literals;
  using steam_ui_helper::cdp_event_t;
  using steam_ui_helper::clock;
  using steam_ui_helper::json;
  namespace policy = platf::steam::big_picture::policy;

  constexpr std::string_view set_status_prefix = "window.VibeshineBPM?.setStatus(";

  struct fake_page_t {
    std::string title;
    std::string url;
    std::string ws_url;
    bool shared = false;
  };

  json default_context(std::int64_t id, std::string frame = "main") {
    return {{"context", {{"id", id}, {"auxData", {{"isDefault", true}, {"frameId", std::move(frame)}}}}}};
  }

  class fake_steam_t final: public steam_ui_helper::steam_link_t {
  public:
    policy::endpoint_facts_t facts {.steam_running = true, .listening = true, .owner_is_steam_webhelper = true, .owner_in_helper_session = true};
    std::vector<fake_page_t> pages {
      {"Steam", "https://steamloopback.host/routes/library/home", "ws://127.0.0.1:8080/devtools/page/MAIN", false},
      {"SharedJSContext", "https://steamloopback.host/routes/settings", "ws://127.0.0.1:8080/devtools/page/SHARED", true},
    };
    json report = {{"protocol", 1}, {"version", 1}, {"react", "19.1.1"}, {"enabled", true}, {"errors", 0}, {"features", {{"power_menu", "waiting for the Power menu to open"}}}};
    int failing_injections = 0;
    std::int64_t context_id = 1;

    int connects = 0;
    std::vector<std::string> probed;
    std::vector<std::string> injections;  // first line of each injection: the options
    std::vector<json> statuses;
    int unpatches = 0;
    std::function<void(cdp_event_t)> events;  // the latest connection to the shared context

    policy::endpoint_facts_t endpoint_facts() override {
      return facts;
    }

    json targets() override {
      json list = json::array();
      for (const auto &page : pages) {
        list.push_back({{"type", "page"}, {"title", page.title}, {"url", page.url}, {"webSocketDebuggerUrl", page.ws_url}});
      }
      return list;
    }

    std::unique_ptr<steam_ui_helper::cdp_session_t> connect(const std::string &ws_url, std::function<void(cdp_event_t)> on_event) override;

    void fire(std::string method, json params = json::object()) {
      events({std::move(method), std::move(params)});
    }

    void reload() {
      fire("Runtime.executionContextCreated", default_context(++context_id));
    }

    void binding(const json &payload) {
      fire("Runtime.bindingCalled", {{"name", "__vibeshineBridge"}, {"payload", payload.dump()}});
    }
  };

  class fake_session_t final: public steam_ui_helper::cdp_session_t {
  public:
    fake_session_t(fake_steam_t &steam, fake_page_t page, std::function<void(cdp_event_t)> on_event):
        steam_(steam),
        page_(std::move(page)),
        on_event_(std::move(on_event)) {
    }

    // Like the real connection, report closing when torn down.
    ~fake_session_t() override {
      on_event_({std::string(steam_ui_helper::closed_event), json::object()});
    }

    json call(std::string_view method, const json &params) override {
      if (method == "Runtime.enable") {
        // Like Chrome, report the page's existing context before answering.
        on_event_({"Runtime.executionContextCreated", default_context(steam_.context_id)});
        return json::object();
      }
      if (method != "Runtime.evaluate") {
        return json::object();
      }
      const auto expression = params.value("expression", std::string {});
      if (expression == policy::is_shared_context_expression) {
        steam_.probed.push_back(page_.title);
        return {{"result", {{"value", page_.shared}}}};
      }
      if (expression.starts_with("window.__vibeshineOptions")) {
        steam_.injections.push_back(expression.substr(0, expression.find('\n')));
        if (steam_.failing_injections > 0) {
          --steam_.failing_injections;
          return {{"exceptionDetails", {{"text", "Uncaught"}, {"exception", {{"description", "Error: React not found\n    at <anonymous>"}}}}}};
        }
        return {{"result", {{"value", steam_.report}}}};
      }
      if (expression.starts_with(set_status_prefix)) {
        const auto payload = expression.substr(set_status_prefix.size(), expression.rfind("), true") - set_status_prefix.size());
        steam_.statuses.push_back(json::parse(payload));
        return {{"result", {{"value", true}}}};
      }
      if (expression == policy::unpatch_expression) {
        ++steam_.unpatches;
        return {{"result", {{"value", true}}}};
      }
      return {{"result", {{"value", "Oct 22 2026 14:39:25"}}}};  // Steam's build date
    }

  private:
    fake_steam_t &steam_;
    fake_page_t page_;
    std::function<void(cdp_event_t)> on_event_;
  };

  std::unique_ptr<steam_ui_helper::cdp_session_t> fake_steam_t::connect(const std::string &ws_url, std::function<void(cdp_event_t)> on_event) {
    const auto page = std::find_if(pages.begin(), pages.end(), [&](const auto &candidate) {
      return candidate.ws_url == ws_url;
    });
    if (page == pages.end()) {
      throw std::runtime_error("no such page");
    }
    ++connects;
    if (page->shared) {
      events = on_event;
    }
    return std::make_unique<fake_session_t>(*this, *page, std::move(on_event));
  }

  class SteamUiHelperController: public ::testing::Test {
  protected:
    fake_steam_t steam;
    clock::time_point now = clock::time_point {} + 1h;
    std::vector<json> emitted;
    std::unique_ptr<steam_ui_helper::controller_t> controller;

    steam_ui_helper::controller_t &make(json inject_options = json::object()) {
      controller = std::make_unique<steam_ui_helper::controller_t>(steam, steam_ui_helper::controller_options_t {
                                                                            .script = "(() => ({}))();",
                                                                            .inject_options = std::move(inject_options),
                                                                            .emit = [this](const json &message) {
                                                                              emitted.push_back(message);
                                                                            },
                                                                            .now = [this] {
                                                                              return now;
                                                                            },
                                                                          });
      return *controller;
    }

    std::vector<json> of_type(std::string_view type) const {
      std::vector<json> found;
      for (const auto &message : emitted) {
        if (message["type"] == type) {
          found.push_back(message);
        }
      }
      return found;
    }

    std::vector<std::string> states() const {
      std::vector<std::string> found;
      for (const auto &message : of_type("state")) {
        found.push_back(message["steam"]);
      }
      return found;
    }
  };

  const json streaming = {{"version", 1}, {"appRunning", true}, {"clients", {{{"id", "deck"}, {"name", "Steam Deck"}, {"kind", "moonlight"}}}}};
  const json idle = {{"version", 1}, {"appRunning", false}, {"clients", json::array()}};
}  // namespace

TEST_F(SteamUiHelperController, ConnectsInjectsAndPushesStatus) {
  auto &helper = make();
  helper.set_status(streaming);
  helper.step();
  EXPECT_EQ(steam.injections.size(), 1U);
  EXPECT_EQ(steam.statuses, std::vector<json> {streaming});
  EXPECT_EQ(states(), (std::vector<std::string> {"connecting", "injected"}));
  const auto reports = of_type("report");
  ASSERT_EQ(reports.size(), 1U);
  EXPECT_EQ(reports[0]["steam_build"], "Oct 22 2026 14:39:25");
  EXPECT_EQ(reports[0]["features"]["power_menu"], "waiting for the Power menu to open");

  // Nothing changes until the status does; the context Steam reported while connecting isn't a reload.
  helper.step();
  EXPECT_EQ(steam.injections.size(), 1U);
  EXPECT_EQ(steam.statuses.size(), 1U);
  helper.set_status(idle);
  helper.step();
  EXPECT_EQ(steam.statuses, (std::vector<json> {streaming, idle}));
}

TEST_F(SteamUiHelperController, ProbesPagesUntilItFindsTheSharedContext) {
  steam.pages = {
    {"Steam", "https://steamloopback.host/routes/library/home", "ws://127.0.0.1:8080/devtools/page/MAIN", false},
    {"Remote", "https://steamloopback.host/", "ws://10.0.0.5:8080/devtools/page/REMOTE", true},
    {"Renamed shared context", "https://steamloopback.host/routes/", "ws://127.0.0.1:8080/devtools/page/SHARED", true},
  };
  auto &helper = make();
  helper.step();
  EXPECT_EQ(steam.probed, (std::vector<std::string> {"Steam", "Renamed shared context"}));
  EXPECT_EQ(steam.injections.size(), 1U);
  // The rejected probe's connection closing isn't mistaken for losing Steam.
  helper.step();
  ASSERT_EQ(of_type("log").size(), 1U);
  EXPECT_TRUE(of_type("log")[0]["message"].get<std::string>().starts_with("connected"));
  EXPECT_EQ(states().back(), "injected");
}

TEST_F(SteamUiHelperController, ReportsEndpointProblemsAndRetriesLater) {
  steam.facts = {.steam_running = true};
  auto &helper = make();
  helper.step();
  EXPECT_EQ(states(), std::vector<std::string> {"port_closed"});
  EXPECT_EQ(steam.connects, 0);

  steam.facts = {.steam_running = true, .listening = true, .owner_is_steam_webhelper = true, .owner_in_helper_session = true};
  helper.step();
  EXPECT_EQ(steam.connects, 0) << "waits for the reconnect delay";
  now += 2s;
  helper.step();
  EXPECT_EQ(steam.injections.size(), 1U);
  EXPECT_EQ(states(), (std::vector<std::string> {"port_closed", "connecting", "injected"}));
}

TEST_F(SteamUiHelperController, NeverConnectsToAPortSteamDoesNotOwn) {
  steam.facts.owner_is_steam_webhelper = false;
  auto &helper = make();
  for (int i = 0; i < 5; ++i) {
    helper.step();
    now += 10s;
  }
  EXPECT_EQ(states(), std::vector<std::string> {"port_foreign"});
  EXPECT_EQ(steam.connects, 0);
}

TEST_F(SteamUiHelperController, InjectsAgainAfterSteamReloads) {
  auto &helper = make();
  helper.set_status(streaming);
  helper.step();
  now += 60s;
  steam.reload();
  helper.step();
  EXPECT_EQ(steam.injections.size(), 2U);
  EXPECT_EQ(steam.statuses, (std::vector<json> {streaming, streaming}));
}

TEST_F(SteamUiHelperController, StopsInjectingAfterThreeQuickReloads) {
  auto &helper = make();
  helper.step();
  for (int i = 0; i < 3; ++i) {
    now += 5s;
    steam.reload();
    helper.step();
  }
  EXPECT_EQ(steam.injections.size(), 3U);
  EXPECT_EQ(states().back(), "breaker_open");
  now += 1h;
  helper.step();
  EXPECT_EQ(steam.injections.size(), 3U);
}

TEST_F(SteamUiHelperController, KeepsInjectingWhenReloadsAreSlow) {
  auto &helper = make();
  helper.step();
  for (int i = 0; i < 5; ++i) {
    now += 60s;
    steam.reload();
    helper.step();
  }
  EXPECT_EQ(steam.injections.size(), 6U);
  EXPECT_EQ(states().back(), "injected");
}

TEST_F(SteamUiHelperController, NeverCountsSteamRestartsTowardsTheBreaker) {
  const policy::endpoint_facts_t closing {.steam_running = true, .steam_instance = 1};
  steam.facts.steam_instance = 1;
  auto &helper = make();
  helper.step();
  for (int i = 0; i < 3; ++i) {
    now += 5s;
    steam.fire(std::string(steam_ui_helper::closed_event));
    helper.step();
    // Steam's port closes before steam.exe exits; nothing is decided until Steam is gone or back.
    steam.facts = closing;
    steam.facts.steam_instance = static_cast<std::uint64_t>(i + 1);
    now += 2s;
    helper.step();
    steam.facts = {};
    now += 2s;
    helper.step();
    steam.facts = {.steam_running = true, .listening = true, .owner_is_steam_webhelper = true, .owner_in_helper_session = true, .steam_instance = static_cast<std::uint64_t>(i + 2)};
    now += 2s;
    helper.step();
  }
  EXPECT_EQ(steam.injections.size(), 4U);
  EXPECT_EQ(states().back(), "injected");
}

TEST_F(SteamUiHelperController, CountsQuickWebHelperRestartsUnderTheSameSteam) {
  steam.facts.steam_instance = 1;
  auto &helper = make();
  helper.step();
  for (int i = 0; i < 3; ++i) {
    now += 5s;
    steam.fire(std::string(steam_ui_helper::closed_event));
    helper.step();
    now += 2s;
    helper.step();  // The same steam.exe opened its port again.
  }
  EXPECT_EQ(steam.injections.size(), 3U);
  EXPECT_EQ(states().back(), "breaker_open");
  now += 1h;
  helper.step();
  EXPECT_EQ(steam.injections.size(), 3U);
}

TEST_F(SteamUiHelperController, BacksOffWhileInjectionFails) {
  steam.failing_injections = 2;
  auto &helper = make();
  helper.step();  // fails, next try in 2 s
  helper.step();
  EXPECT_EQ(steam.injections.size(), 1U);
  now += 2s;
  helper.step();  // fails, next try in 4 s
  now += 2s;
  helper.step();
  EXPECT_EQ(steam.injections.size(), 2U);
  now += 2s;
  helper.step();
  EXPECT_EQ(steam.injections.size(), 3U);
  EXPECT_EQ(states().back(), "injected");
  EXPECT_NE(of_type("log")[1]["message"].get<std::string>().find("React not found"), std::string::npos);
}

TEST_F(SteamUiHelperController, RefusesAScriptOfAnotherProtocol) {
  steam.report["protocol"] = 2;
  auto &helper = make();
  for (int i = 0; i < 3; ++i) {
    helper.step();
    now += 1min;
  }
  EXPECT_EQ(steam.injections.size(), 1U);
  EXPECT_TRUE(of_type("report").empty());
  EXPECT_EQ(of_type("log").back()["level"], "error");
}

TEST_F(SteamUiHelperController, ForwardsValidScriptMessagesOnly) {
  auto &helper = make();
  helper.step();
  emitted.clear();
  steam.binding({{"type", "action"}, {"action", "disconnect"}, {"client", "deck"}});
  steam.binding({{"type", "action"}, {"action", "format_disk"}});
  steam.binding({{"type", "feature"}, {"name", "power_menu"}, {"value", "ok (menu parts: learned)"}});
  steam.fire("Runtime.bindingCalled", {{"name", "somethingElse"}, {"payload", R"({"type":"action","action":"end_stream"})"}});
  helper.step();
  EXPECT_EQ(of_type("action"), std::vector<json> {json({{"type", "action"}, {"action", "disconnect"}, {"client", "deck"}})});
  EXPECT_EQ(of_type("feature").size(), 1U);
  EXPECT_EQ(of_type("log").size(), 1U) << "one warning for the malformed action";
}

TEST_F(SteamUiHelperController, StopsPushingStatusWhileTheScriptIsOffUntilSteamReloads) {
  auto &helper = make();
  helper.set_status(streaming);
  helper.step();
  steam.binding({{"type", "disabled"}, {"reason", "8 errors"}});
  helper.step();
  helper.set_status(idle);
  helper.step();
  EXPECT_EQ(steam.statuses, std::vector<json> {streaming});
  EXPECT_EQ(of_type("disabled").size(), 1U);

  now += 60s;
  steam.reload();
  helper.step();
  EXPECT_EQ(steam.injections.size(), 2U);
  EXPECT_EQ(steam.statuses, (std::vector<json> {streaming, idle}));
}

TEST_F(SteamUiHelperController, ReconnectsWhenTheConnectionCloses) {
  auto &helper = make();
  helper.step();
  steam.fire(std::string(steam_ui_helper::closed_event));
  helper.step();
  EXPECT_EQ(steam.connects, 1) << "waits for the reconnect delay";
  const auto logs = of_type("log");
  EXPECT_TRUE(std::any_of(logs.begin(), logs.end(), [](const json &entry) {
    return entry["message"] == "lost the connection to Steam (Steam closed or restarted); reconnecting";
  }));
  now += 2s;
  helper.step();
  EXPECT_EQ(steam.connects, 2);
  EXPECT_EQ(steam.injections.size(), 2U);
  EXPECT_EQ(states(), (std::vector<std::string> {"connecting", "injected", "connecting", "injected"}));
}

TEST_F(SteamUiHelperController, IgnoresOtherFramesUnlessThePageWasCleared) {
  auto &helper = make();
  helper.step();
  now += 60s;
  steam.fire("Runtime.executionContextCreated", default_context(40, "iframe"));
  helper.step();
  EXPECT_EQ(steam.injections.size(), 1U);

  steam.fire("Runtime.executionContextsCleared");
  steam.fire("Runtime.executionContextCreated", default_context(41, "new-main"));
  helper.step();
  EXPECT_EQ(steam.injections.size(), 2U);
}

TEST_F(SteamUiHelperController, PassesInjectOptionsToTheScript) {
  make({{"simulateCrash", {"power-items"}}}).step();
  ASSERT_EQ(steam.injections.size(), 1U);
  EXPECT_EQ(steam.injections[0], R"(window.__vibeshineOptions = {"simulateCrash":["power-items"]};)");
}

TEST_F(SteamUiHelperController, CheckRunsTheScriptWithoutPatching) {
  auto &helper = make();
  const auto report = helper.check();
  EXPECT_EQ(report["type"], "report");
  ASSERT_EQ(steam.injections.size(), 1U);
  EXPECT_NE(steam.injections[0].find(R"("checkOnly":true)"), std::string::npos);
  EXPECT_TRUE(steam.statuses.empty());
  helper.unpatch();
  EXPECT_EQ(steam.unpatches, 0) << "nothing was patched";

  steam.facts = {};
  steam_ui_helper::controller_t unreachable(steam, {.script = "", .emit = {}, .now = [this] {
                                                       return now;
                                                     }});
  EXPECT_THROW(unreachable.check(), std::runtime_error);
}

TEST_F(SteamUiHelperController, AnswersCheckRequestsOnTheNextStep) {
  auto &helper = make();
  helper.step();
  helper.request_check(7);
  helper.step();
  const auto results = of_type("check_result");
  ASSERT_EQ(results.size(), 1U);
  EXPECT_EQ(results[0]["id"], 7);
  EXPECT_EQ(results[0]["report"]["type"], "report");
  EXPECT_EQ(steam.injections.size(), 2U) << "the real injection plus the check";
}

TEST_F(SteamUiHelperController, ReportsCheckRequestsItCannotRun) {
  steam.facts = {.steam_running = true};
  auto &helper = make();
  helper.request_check(8);
  helper.step();
  const auto results = of_type("check_result");
  ASSERT_EQ(results.size(), 1U);
  EXPECT_EQ(results[0]["id"], 8);
  EXPECT_NE(results[0]["error"].get<std::string>().find("port_closed"), std::string::npos);
}

TEST_F(SteamUiHelperController, UnpatchRemovesTheScriptsPatches) {
  auto &helper = make();
  helper.step();
  helper.unpatch();
  EXPECT_EQ(steam.unpatches, 1);
}

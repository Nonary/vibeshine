#include "src/platform/windows/playnite_sync.h"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

using namespace platf::playnite;
using namespace platf::playnite::sync;

static Game G(std::string id, std::string last, bool installed = true, std::vector<std::string> cats = {}) {
  Game g;
  g.id = id;
  g.name = id;
  g.last_played = last;
  g.installed = installed;
  g.categories = cats;
  return g;
}

TEST(PlayniteAutosync_Reconcile, AddsSelectedGamesToEmptyApps) {
  nlohmann::json root;
  root["apps"] = nlohmann::json::array();
  std::vector<Game> all {G("A", "2025-01-01T00:00:00Z", true), G("B", "2024-01-01T00:00:00Z", true, {"RPG"})};
  bool changed = false;
  std::size_t matched = 0;
  autosync_reconcile(root, all, /*recentN*/ 1, /*recentAgeDays*/ 0, /*delete_after_days*/ 0, /*require_repl*/ true,
                     /*categories*/ std::vector<std::string> {"RPG"},
                     /*exclude*/ {},
                     changed,
                     matched);
  EXPECT_TRUE(changed);
  ASSERT_EQ(root["apps"].size(), 2u);  // A from recent, B from category
  std::unordered_set<std::string> ids;
  for (auto &a : root["apps"]) {
    ids.insert(a["playnite-id"].get<std::string>());
  }
  EXPECT_TRUE(ids.contains("A"));
  EXPECT_TRUE(ids.contains("B"));
}

TEST(PlayniteAutosync_Reconcile, HonorsExcludeIds) {
  nlohmann::json root;
  root["apps"] = nlohmann::json::array();
  std::vector<Game> all {G("A", "2025-01-01T00:00:00Z", true)};
  bool changed = false;
  std::size_t matched = 0;
  autosync_reconcile(root, all, 1, 0, 0, true, {}, {"a"}, changed, matched);
  EXPECT_FALSE(changed);
  EXPECT_EQ(root["apps"].size(), 0u);
}

TEST(PlayniteAutosync_Reconcile, UpdatesExistingAndSetsManagedFields) {
  // Existing app matching by id gets annotated and not duplicated
  nlohmann::json root;
  root["apps"] = nlohmann::json::array();
  nlohmann::json a;
  a["playnite-id"] = "A";
  root["apps"].push_back(a);
  std::vector<Game> all {G("A", "2025-01-01T00:00:00Z", true)};
  bool changed = false;
  std::size_t matched = 0;
  autosync_reconcile(root, all, 1, 0, 0, true, {}, {}, changed, matched);
  EXPECT_TRUE(changed);
  ASSERT_EQ(root["apps"].size(), 1u);
  EXPECT_EQ(root["apps"][0]["playnite-id"], "A");
  EXPECT_EQ(root["apps"][0]["playnite-managed"], "auto");
}

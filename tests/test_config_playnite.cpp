#include <gtest/gtest.h>
#include <unordered_map>
#include <string>
#include <filesystem>

#include "src/config_playnite.h"
#include "src/platform/common.h"

namespace fs = std::filesystem;

struct PlayniteConfigFixture : public ::testing::Test {
  config::playnite_t saved;
  void SetUp() override { saved = config::playnite; config::playnite = config::playnite_t{}; }
  void TearDown() override { config::playnite = saved; }
};

TEST_F(PlayniteConfigFixture, Booleans_ParseCaseInsensitiveTruths) {
  std::unordered_map<std::string, std::string> vars{{"playnite_enabled","YeS"},{"playnite_auto_sync","on"},{"playnite_autosync_require_replacement","0"}};
  config::apply_playnite(vars);
  EXPECT_TRUE(config::playnite.enabled);
  EXPECT_TRUE(config::playnite.auto_sync);
  EXPECT_FALSE(config::playnite.autosync_require_replacement);
  EXPECT_TRUE(vars.empty());
}

TEST_F(PlayniteConfigFixture, Integers_ValidAndClampNegatives) {
  std::unordered_map<std::string, std::string> vars{{"playnite_recent_games","20"},{"playnite_recent_max_age_days","-5"},{"playnite_autosync_delete_after_days","7"},{"playnite_focus_attempts","0"},{"playnite_focus_timeout_secs","12"}};
  config::apply_playnite(vars);
  EXPECT_EQ(config::playnite.recent_games, 20);
  EXPECT_EQ(config::playnite.recent_max_age_days, 0); // clamped
  EXPECT_EQ(config::playnite.autosync_delete_after_days, 7);
  EXPECT_EQ(config::playnite.focus_attempts, 0);
  EXPECT_EQ(config::playnite.focus_timeout_secs, 12);
}

TEST_F(PlayniteConfigFixture, Integers_InvalidStringsAreIgnored) {
  // Leave defaults when invalid
  std::unordered_map<std::string, std::string> vars{{"playnite_recent_games","abc"},{"playnite_focus_attempts","-x"},{"playnite_focus_timeout_secs",""}};
  config::apply_playnite(vars);
  EXPECT_EQ(config::playnite.recent_games, 10);
  EXPECT_EQ(config::playnite.focus_attempts, 3);
  EXPECT_EQ(config::playnite.focus_timeout_secs, 15);
}

TEST_F(PlayniteConfigFixture, Lists_ParseJsonArrayAndCsv) {
  std::unordered_map<std::string, std::string> vars{{"playnite_sync_categories","[\"A\",\"B\"]"},{"playnite_exclude_games"," x , y, z "}};
  config::apply_playnite(vars);
  ASSERT_EQ(config::playnite.sync_categories.size(), 2u);
  EXPECT_EQ(config::playnite.sync_categories[0], "A");
  ASSERT_EQ(config::playnite.exclude_games.size(), 3u);
  EXPECT_EQ(config::playnite.exclude_games[0], "x");
  EXPECT_EQ(config::playnite.exclude_games[2], "z");
}

TEST_F(PlayniteConfigFixture, Paths_RelativeResolvedUnderAppdataAndDirectoriesCreated) {
  auto base = platf::appdata();
  fs::path rel = fs::path("ut_playnite_cfg") / "sub" / "target.txt";
  std::unordered_map<std::string, std::string> vars{{"playnite_install_dir", rel.generic_string()},{"playnite_extensions_dir", rel.generic_string()}};
  config::apply_playnite(vars);
  fs::path expect = base / rel;
  EXPECT_EQ(fs::path(config::playnite.install_dir), expect);
  EXPECT_EQ(fs::path(config::playnite.extensions_dir), expect);
  // Parent directories should exist per parse_path behavior
  EXPECT_TRUE(fs::exists(expect.parent_path()));
}

TEST_F(PlayniteConfigFixture, FocusExitOnFirst_ParsesBoolean) {
  std::unordered_map<std::string, std::string> vars{{"playnite_focus_exit_on_first","true"}};
  config::apply_playnite(vars);
  EXPECT_TRUE(config::playnite.focus_exit_on_first);
}


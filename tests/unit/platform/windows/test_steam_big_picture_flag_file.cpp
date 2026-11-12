/**
 * @file tests/unit/platform/windows/test_steam_big_picture_flag_file.cpp
 * @brief Steam's debug-port flag file in real folders: which file Vibeshine owns, and cleaning it up
 * when Steam's folder changes, when the controls are turned off and when Vibeshine is uninstalled.
 */
#include "src/platform/windows/steam_big_picture_flag_file.h"

#include <cstddef>
#include <fstream>
#include <gtest/gtest.h>
#include <vector>
#include <windows.h>
// Included after windows.h, which they need.
#include <aclapi.h>
#include <sddl.h>

namespace {
  namespace fs = std::filesystem;
  namespace flag_file = platf::steam::big_picture::flag_file;
  using flag_file::failure_e;
  using flag_file::removal_e;

  constexpr auto flag_name = L".cef-enable-remote-debugging";

  class SteamBigPictureFlagFile: public ::testing::Test {
  protected:
    void SetUp() override {
      base = fs::temp_directory_path() / (L"vibeshine_flag_file_test_" + std::to_wstring(GetCurrentProcessId()) + L"_" + std::to_wstring(GetTickCount64()));
      steam_a = base / L"SteamA";
      steam_b = base / L"SteamB";
      fs::create_directories(steam_a);
      fs::create_directories(steam_b);
    }

    void TearDown() override {
      if (user_token) {
        CloseHandle(user_token);
      }
      std::error_code error;
      fs::remove_all(base, error);
    }

    // The record lives in memory here; signed_in and save_works stand in for the console user and
    // Vibeshine's state file.
    flag_file::environment_t environment() {
      return {
        [this] {
          return record;
        },
        [this](const std::optional<flag_file::record_t> &next) {
          if (!save_works) {
            return false;
          }
          record = next;
          return true;
        },
        [this](const std::function<void()> &work) {
          if (!signed_in) {
            return false;
          }
          if (!user_token) {
            work();
            return true;
          }
          EXPECT_TRUE(ImpersonateLoggedOnUser(user_token));
          work();
          RevertToSelf();
          return true;
        },
        [this](const fs::path &path) {
          return id_unreadable && id_unreadable(path);
        },
      };
    }

    flag_file::set_result_t set(const fs::path &root, bool enabled) {
      return flag_file::set(root, enabled, environment());
    }

    flag_file::remove_result_t uninstall(const std::optional<fs::path> &steam_root = std::nullopt, bool may_remove_as_vibeshine = true) {
      return flag_file::remove_recorded(environment(), steam_root, [&](const fs::path &) {
        return may_remove_as_vibeshine;
      });
    }

    // Steam's folder A is moved or renamed, with the flag file inside: same file, same ID, new path.
    fs::path move_steam_a() {
      const auto moved = base / L"SteamMoved";
      fs::rename(steam_a, moved);
      return moved;
    }

    static void write_file(const fs::path &path) {
      std::ofstream(path).put('x');
    }

    // Another program deletes the flag file and adds its own at the same path. The old file is kept
    // under another name, so the new one can't get its file ID back.
    static void replace_file(const fs::path &path) {
      fs::rename(path, path.parent_path() / L"replaced-original");
      write_file(path);
    }

    // From now on the signed-in user is this test's account restricted to what Everyone may do, while
    // the test itself keeps the account's own access, as Vibeshine has more than the user.
    void restrict_user() {
      HANDLE token = nullptr;
      ASSERT_TRUE(OpenProcessToken(GetCurrentProcess(), TOKEN_DUPLICATE | TOKEN_QUERY | TOKEN_IMPERSONATE | TOKEN_ASSIGN_PRIMARY, &token));
      SID_IDENTIFIER_AUTHORITY world_authority = SECURITY_WORLD_SID_AUTHORITY;
      PSID everyone = nullptr;
      ASSERT_TRUE(AllocateAndInitializeSid(&world_authority, 1, SECURITY_WORLD_RID, 0, 0, 0, 0, 0, 0, 0, &everyone));
      SID_AND_ATTRIBUTES restricting {everyone, 0};
      const bool made = CreateRestrictedToken(token, 0, 0, nullptr, 0, nullptr, 1, &restricting, &user_token);
      FreeSid(everyone);
      CloseHandle(token);
      ASSERT_TRUE(made);
    }

    // Sets path's access to sddl, a DACL in which %USER% stands for this test's account.
    static void set_access(const fs::path &path, std::wstring sddl) {
      HANDLE token = nullptr;
      ASSERT_TRUE(OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token));
      std::vector<std::byte> buffer(256);
      DWORD size = 0;
      const bool read = GetTokenInformation(token, TokenUser, buffer.data(), static_cast<DWORD>(buffer.size()), &size);
      CloseHandle(token);
      ASSERT_TRUE(read);
      wchar_t *user = nullptr;
      ASSERT_TRUE(ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER *>(buffer.data())->User.Sid, &user));
      for (auto at = sddl.find(L"%USER%"); at != std::wstring::npos; at = sddl.find(L"%USER%")) {
        sddl.replace(at, 6, user);
      }
      LocalFree(user);
      PSECURITY_DESCRIPTOR descriptor = nullptr;
      ASSERT_TRUE(ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(), SDDL_REVISION_1, &descriptor, nullptr));
      BOOL present = FALSE;
      BOOL defaulted = FALSE;
      PACL dacl = nullptr;
      GetSecurityDescriptorDacl(descriptor, &present, &dacl, &defaulted);
      const auto applied = SetNamedSecurityInfoW(const_cast<wchar_t *>(path.c_str()), SE_FILE_OBJECT, DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION, nullptr, nullptr, dacl, nullptr);
      LocalFree(descriptor);
      ASSERT_EQ(applied, ERROR_SUCCESS);
    }

    // A drive letter with no drive, as after unplugging one.
    static fs::path missing_drive() {
      const DWORD drives = GetLogicalDrives();
      for (wchar_t letter = L'Z'; letter >= L'D'; --letter) {
        if (!(drives & (1u << (letter - L'A')))) {
          return fs::path(std::wstring {letter, L':', L'\\'});
        }
      }
      return {};
    }

    // The recorded path is in folder, and the ID is the one of the file there now.
    void expect_recorded(const fs::path &folder) {
      ASSERT_TRUE(record);
      EXPECT_EQ(record->path, folder / flag_name);
      EXPECT_FALSE(record->file_id.empty());
      EXPECT_EQ(flag_file::file_id(folder / flag_name), record->file_id);
    }

    fs::path base;
    fs::path steam_a;
    fs::path steam_b;
    std::optional<flag_file::record_t> record;
    bool signed_in = true;
    bool save_works = true;
    // Where Windows fails to read a file ID, as it can.
    std::function<bool(const fs::path &)> id_unreadable;
    // The signed-in user's token, when it has less access than the test (see restrict_user).
    HANDLE user_token = nullptr;
  };

  TEST_F(SteamBigPictureFlagFile, CreatesAndRecordsTheFlagFile) {
    const auto result = set(steam_a, true);
    EXPECT_EQ(result.failure, failure_e::none);
    EXPECT_TRUE(result.created);
    EXPECT_EQ(result.path, steam_a / flag_name);
    EXPECT_TRUE(fs::exists(steam_a / flag_name));
    expect_recorded(steam_a);

    // Turning on again keeps the same file and record.
    const auto id = record->file_id;
    EXPECT_FALSE(set(steam_a, true).created);
    EXPECT_EQ(record->file_id, id);
  }

  TEST_F(SteamBigPictureFlagFile, TurningOffDeletesTheFileItCreated) {
    set(steam_a, true);
    const auto result = set(steam_a, false);
    EXPECT_EQ(result.failure, failure_e::none);
    EXPECT_EQ(result.removed, removal_e::deleted);
    EXPECT_FALSE(fs::exists(steam_a / flag_name));
    EXPECT_FALSE(record);
  }

  TEST_F(SteamBigPictureFlagFile, LeavesAFlagFileItDidNotCreate) {
    write_file(steam_a / flag_name);
    const auto on = set(steam_a, true);
    EXPECT_EQ(on.failure, failure_e::none);
    EXPECT_FALSE(on.created);
    EXPECT_FALSE(record);

    const auto off = set(steam_a, false);
    EXPECT_EQ(off.failure, failure_e::none);
    EXPECT_TRUE(off.left_alone);
    EXPECT_TRUE(fs::exists(steam_a / flag_name));
  }

  TEST_F(SteamBigPictureFlagFile, LeavesAFileRecreatedAtTheSamePath) {
    set(steam_a, true);
    replace_file(steam_a / flag_name);
    const auto result = set(steam_a, false);
    EXPECT_EQ(result.failure, failure_e::none);
    EXPECT_EQ(result.removed, removal_e::replaced);
    EXPECT_TRUE(fs::exists(steam_a / flag_name));
    EXPECT_FALSE(record);
  }

  TEST_F(SteamBigPictureFlagFile, TurningOnInAnotherFolderDeletesTheOldFileFirst) {
    set(steam_a, true);
    const auto result = set(steam_b, true);
    EXPECT_EQ(result.failure, failure_e::none);
    EXPECT_EQ(result.retired, removal_e::deleted);
    EXPECT_EQ(result.retired_path, steam_a / flag_name);
    EXPECT_TRUE(result.created);
    EXPECT_FALSE(fs::exists(steam_a / flag_name));
    EXPECT_TRUE(fs::exists(steam_b / flag_name));
    expect_recorded(steam_b);
  }

  TEST_F(SteamBigPictureFlagFile, TwoFoldersThenTurningOffLeavesNoFlagFile) {
    set(steam_a, true);
    set(steam_b, true);
    const auto result = set(steam_b, false);
    EXPECT_EQ(result.failure, failure_e::none);
    EXPECT_EQ(result.removed, removal_e::deleted);
    EXPECT_FALSE(fs::exists(steam_a / flag_name));
    EXPECT_FALSE(fs::exists(steam_b / flag_name));
    EXPECT_FALSE(record);
  }

  TEST_F(SteamBigPictureFlagFile, TwoFoldersThenUninstallingLeavesNoFlagFile) {
    set(steam_a, true);
    set(steam_b, true);
    const auto result = uninstall();
    EXPECT_EQ(result.removal, removal_e::deleted);
    EXPECT_FALSE(fs::exists(steam_a / flag_name));
    EXPECT_FALSE(fs::exists(steam_b / flag_name));
    EXPECT_FALSE(record);
  }

  TEST_F(SteamBigPictureFlagFile, KeepsAReplacementInTheOldFolder) {
    set(steam_a, true);
    replace_file(steam_a / flag_name);
    const auto result = set(steam_b, true);
    EXPECT_EQ(result.failure, failure_e::none);
    EXPECT_EQ(result.retired, removal_e::replaced);
    EXPECT_TRUE(fs::exists(steam_a / flag_name));
    expect_recorded(steam_b);
  }

  TEST_F(SteamBigPictureFlagFile, ChangesNothingWhenTheOldFileCannotBeDeleted) {
    set(steam_a, true);
    const auto recorded = record;
    // Another program holds the old file open without letting it be deleted.
    HANDLE held = CreateFileW((steam_a / flag_name).c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
    ASSERT_NE(held, INVALID_HANDLE_VALUE);
    const auto result = set(steam_b, true);
    CloseHandle(held);
    EXPECT_EQ(result.failure, failure_e::remove);
    EXPECT_EQ(result.retired, removal_e::failed);
    EXPECT_EQ(result.failed_path, steam_a / flag_name);
    EXPECT_TRUE(result.error);
    EXPECT_TRUE(fs::exists(steam_a / flag_name));
    EXPECT_FALSE(fs::exists(steam_b / flag_name));
    ASSERT_TRUE(record);
    EXPECT_EQ(record->path, recorded->path);
    EXPECT_EQ(record->file_id, recorded->file_id);

    // Once the old file can go, turning on works.
    EXPECT_EQ(set(steam_b, true).failure, failure_e::none);
    EXPECT_FALSE(fs::exists(steam_a / flag_name));
    expect_recorded(steam_b);
  }

  TEST_F(SteamBigPictureFlagFile, TurningOffInAnotherFolderDeletesTheOldFile) {
    set(steam_a, true);
    write_file(steam_b / flag_name);  // Added by the user or another Steam mod.
    const auto result = set(steam_b, false);
    EXPECT_EQ(result.failure, failure_e::none);
    EXPECT_EQ(result.retired, removal_e::deleted);
    EXPECT_TRUE(result.left_alone);
    EXPECT_FALSE(fs::exists(steam_a / flag_name));
    EXPECT_TRUE(fs::exists(steam_b / flag_name));
    EXPECT_FALSE(record);
  }

  TEST_F(SteamBigPictureFlagFile, TurningOffInAnotherFolderFailsWhenTheOldFileStays) {
    set(steam_a, true);
    HANDLE held = CreateFileW((steam_a / flag_name).c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
    ASSERT_NE(held, INVALID_HANDLE_VALUE);
    const auto result = set(steam_b, false);
    CloseHandle(held);
    EXPECT_EQ(result.failure, failure_e::remove);
    EXPECT_EQ(result.failed_path, steam_a / flag_name);
    EXPECT_TRUE(fs::exists(steam_a / flag_name));
    expect_recorded(steam_a);
  }

  TEST_F(SteamBigPictureFlagFile, NeverDeletesAnythingButAFlagFile) {
    const auto other = steam_a / L"other.txt";
    write_file(other);
    // Even with that file's own ID.
    const flag_file::record_t other_record {other, *flag_file::file_id(other)};
    record = other_record;
    EXPECT_EQ(set(steam_b, true).failure, failure_e::none);
    EXPECT_TRUE(fs::exists(other));
    expect_recorded(steam_b);

    record = other_record;
    const auto result = uninstall();
    EXPECT_FALSE(result.removal);
    EXPECT_TRUE(fs::exists(other));
    EXPECT_FALSE(record);
  }

  TEST_F(SteamBigPictureFlagFile, NeverDeletesAFileItCannotIdentify) {
    // A record without a file ID, as early development builds saved, and another program's file at
    // its path: nothing proves which file the record meant, so nothing is deleted on its word.
    write_file(steam_a / flag_name);
    const flag_file::record_t unproven {steam_a / flag_name, ""};

    record = unproven;
    const auto off = set(steam_a, false);
    EXPECT_EQ(off.failure, failure_e::none);
    EXPECT_EQ(off.unproven, steam_a / flag_name);
    EXPECT_FALSE(off.removed);
    EXPECT_TRUE(off.left_alone);
    EXPECT_TRUE(fs::exists(steam_a / flag_name));
    EXPECT_FALSE(record);

    // Turning on in another folder doesn't delete it either.
    record = unproven;
    const auto on = set(steam_b, true);
    EXPECT_EQ(on.failure, failure_e::none);
    EXPECT_FALSE(on.retired);
    EXPECT_TRUE(fs::exists(steam_a / flag_name));
    expect_recorded(steam_b);

    // Nor does uninstalling.
    record = unproven;
    const auto removed = uninstall();
    EXPECT_TRUE(removed.unproven);
    EXPECT_FALSE(removed.removal);
    EXPECT_TRUE(fs::exists(steam_a / flag_name));
    EXPECT_FALSE(record);
  }

  TEST_F(SteamBigPictureFlagFile, TakesTheFileBackWhenItsIdCannotBeRead) {
    // A file Vibeshine can't identify couldn't be told apart from one another program adds later.
    id_unreadable = [](const fs::path &) {
      return true;
    };
    const auto result = set(steam_a, true);
    EXPECT_EQ(result.failure, failure_e::identify);
    EXPECT_EQ(result.undo, removal_e::deleted);
    EXPECT_FALSE(result.created);
    EXPECT_FALSE(fs::exists(steam_a / flag_name));
    EXPECT_FALSE(record);
  }

  TEST_F(SteamBigPictureFlagFile, KeepsTheRecordWhenTheIdCannotBeRead) {
    set(steam_a, true);
    const auto recorded = record;
    id_unreadable = [](const fs::path &) {
      return true;
    };
    // Turning on again doesn't forget the file.
    EXPECT_EQ(set(steam_a, true).failure, failure_e::none);
    ASSERT_TRUE(record);
    EXPECT_EQ(record->file_id, recorded->file_id);
    // Turning off can't prove it is the file, so it stays, and so does the record.
    const auto off = set(steam_a, false);
    EXPECT_EQ(off.failure, failure_e::remove);
    EXPECT_TRUE(fs::exists(steam_a / flag_name));
    ASSERT_TRUE(record);
    EXPECT_EQ(record->file_id, recorded->file_id);

    id_unreadable = nullptr;
    EXPECT_EQ(set(steam_a, false).removed, removal_e::deleted);
    EXPECT_FALSE(fs::exists(steam_a / flag_name));
    EXPECT_FALSE(record);
  }

  TEST_F(SteamBigPictureFlagFile, KeepsTheRecordOfAMovedFileWhoseIdCannotBeRead) {
    set(steam_a, true);
    const auto recorded = record;
    const auto moved = move_steam_a();
    // Only the file in Steam's new folder can't be identified.
    id_unreadable = [&](const fs::path &path) {
      return path == moved / flag_name;
    };
    const auto on = set(moved, true);
    EXPECT_EQ(on.failure, failure_e::none);
    EXPECT_FALSE(on.moved_from);
    ASSERT_TRUE(record);
    EXPECT_EQ(record->path, recorded->path);
    EXPECT_EQ(record->file_id, recorded->file_id);

    const auto off = set(moved, false);
    EXPECT_EQ(off.failure, failure_e::identify);
    EXPECT_EQ(off.failed_path, moved / flag_name);
    EXPECT_TRUE(off.error);
    EXPECT_TRUE(fs::exists(moved / flag_name));
    ASSERT_TRUE(record);
    EXPECT_EQ(record->file_id, recorded->file_id);

    // Once its ID can be read, it is found again and deleted.
    id_unreadable = nullptr;
    const auto again = set(moved, false);
    EXPECT_EQ(again.failure, failure_e::none);
    EXPECT_TRUE(again.moved_from);
    EXPECT_EQ(again.removed, removal_e::deleted);
    EXPECT_FALSE(fs::exists(moved / flag_name));
    EXPECT_FALSE(record);
  }

  TEST_F(SteamBigPictureFlagFile, TakesTheFileBackWhenItCannotBeRecorded) {
    save_works = false;
    const auto result = set(steam_a, true);
    EXPECT_EQ(result.failure, failure_e::record);
    EXPECT_EQ(result.undo, removal_e::deleted);
    EXPECT_FALSE(fs::exists(steam_a / flag_name));
    EXPECT_FALSE(record);
  }

  TEST_F(SteamBigPictureFlagFile, DoesNothingWithNobodySignedIn) {
    signed_in = false;
    EXPECT_EQ(set(steam_a, true).failure, failure_e::no_user);
    EXPECT_FALSE(fs::exists(steam_a / flag_name));
    EXPECT_FALSE(record);
  }

  TEST_F(SteamBigPictureFlagFile, UninstallKeepsAReplacement) {
    set(steam_a, true);
    replace_file(steam_a / flag_name);
    const auto result = uninstall();
    EXPECT_EQ(result.removal, removal_e::replaced);
    EXPECT_TRUE(fs::exists(steam_a / flag_name));
    EXPECT_FALSE(record);
  }

  TEST_F(SteamBigPictureFlagFile, UninstallWithNobodySignedInOnlyDeletesInAcceptedFolders) {
    set(steam_a, true);
    signed_in = false;
    const auto refused = uninstall(std::nullopt, false);
    EXPECT_TRUE(refused.left_outside_steam);
    EXPECT_FALSE(refused.removal);
    EXPECT_TRUE(fs::exists(steam_a / flag_name));
    expect_recorded(steam_a);

    const auto accepted = uninstall(std::nullopt, true);
    EXPECT_EQ(accepted.removal, removal_e::deleted);
    EXPECT_FALSE(fs::exists(steam_a / flag_name));
    EXPECT_FALSE(record);
  }

  TEST_F(SteamBigPictureFlagFile, FollowsTheFileWhenSteamsFolderMoves) {
    set(steam_a, true);
    const auto id = record->file_id;
    const auto moved = move_steam_a();
    const auto result = set(moved, true);
    EXPECT_EQ(result.failure, failure_e::none);
    EXPECT_FALSE(result.created);
    EXPECT_FALSE(result.retired);
    ASSERT_TRUE(result.moved_from);
    EXPECT_EQ(*result.moved_from, steam_a / flag_name);
    // The record follows the file, which keeps its ID.
    expect_recorded(moved);
    EXPECT_EQ(record->file_id, id);

    EXPECT_EQ(set(moved, false).removed, removal_e::deleted);
    EXPECT_FALSE(fs::exists(moved / flag_name));
    EXPECT_FALSE(record);
  }

  TEST_F(SteamBigPictureFlagFile, TurningOffAfterSteamsFolderMovedDeletesTheFile) {
    set(steam_a, true);
    const auto moved = move_steam_a();
    const auto result = set(moved, false);
    EXPECT_EQ(result.failure, failure_e::none);
    EXPECT_TRUE(result.moved_from);
    EXPECT_EQ(result.removed, removal_e::deleted);
    EXPECT_FALSE(result.left_alone);
    EXPECT_FALSE(fs::exists(moved / flag_name));
    EXPECT_FALSE(record);
  }

  TEST_F(SteamBigPictureFlagFile, UninstallFindsTheFileAfterSteamsFolderMoved) {
    set(steam_a, true);
    const auto moved = move_steam_a();
    const auto result = uninstall(moved);
    EXPECT_EQ(result.removal, removal_e::deleted);
    ASSERT_TRUE(result.moved_to);
    EXPECT_EQ(*result.moved_to, moved / flag_name);
    EXPECT_FALSE(fs::exists(moved / flag_name));
    EXPECT_FALSE(record);
  }

  TEST_F(SteamBigPictureFlagFile, UninstallLeavesAnotherFileInSteamsFolder) {
    set(steam_a, true);
    fs::remove(steam_a / flag_name);
    write_file(steam_b / flag_name);  // Added by the user or another Steam mod.
    const auto result = uninstall(steam_b);
    EXPECT_EQ(result.removal, removal_e::missing);
    EXPECT_FALSE(result.moved_to);
    EXPECT_TRUE(fs::exists(steam_b / flag_name));
    EXPECT_FALSE(record);
  }

  TEST_F(SteamBigPictureFlagFile, ACopyInANewFolderIsNotVibeshines) {
    set(steam_a, true);
    // Steam was copied to another drive and the old folder deleted: the copy is a new file.
    fs::copy_file(steam_a / flag_name, steam_b / flag_name);
    fs::remove_all(steam_a);
    const auto result = set(steam_b, false);
    EXPECT_EQ(result.failure, failure_e::none);
    EXPECT_FALSE(result.moved_from);
    EXPECT_EQ(result.retired, removal_e::missing);
    EXPECT_TRUE(result.left_alone);
    EXPECT_TRUE(fs::exists(steam_b / flag_name));
    EXPECT_FALSE(record);
  }

  TEST_F(SteamBigPictureFlagFile, ReportsWhetherTheRecordedFileIsStillThere) {
    set(steam_a, true);
    EXPECT_TRUE(flag_file::recorded_file_exists(*record));
    const auto recorded = *record;
    replace_file(steam_a / flag_name);
    EXPECT_FALSE(flag_file::recorded_file_exists(recorded));
    fs::remove(steam_a / flag_name);
    EXPECT_FALSE(flag_file::recorded_file_exists(recorded));
    // Only ever a flag file, whatever the record says.
    write_file(steam_a / L"other.txt");
    EXPECT_FALSE(flag_file::recorded_file_exists({steam_a / L"other.txt", *flag_file::file_id(steam_a / L"other.txt")}));
    // And only a recorded ID proves it is Vibeshine's.
    write_file(steam_a / flag_name);
    EXPECT_FALSE(flag_file::recorded_file_exists({steam_a / flag_name, ""}));
  }

  TEST_F(SteamBigPictureFlagFile, UninstallKeepsTheRecordWhenDeletingFails) {
    set(steam_a, true);
    HANDLE held = CreateFileW((steam_a / flag_name).c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
    ASSERT_NE(held, INVALID_HANDLE_VALUE);
    const auto result = uninstall();
    CloseHandle(held);
    EXPECT_EQ(result.removal, removal_e::failed);
    EXPECT_TRUE(result.error);
    expect_recorded(steam_a);
  }

  TEST_F(SteamBigPictureFlagFile, DeletesTheOldFileItselfWhenTheUserMayNotOpenIt) {
    set(steam_a, true);
    // Folder A is now another Windows user's: only the test's own account may open the file, while
    // the signed-in user may only use Steam's new folder B.
    set_access(steam_a / flag_name, L"D:P(A;;FA;;;%USER%)");
    set_access(steam_b, L"D:P(A;OICI;FA;;;WD)(A;OICI;FA;;;%USER%)");
    restrict_user();
    const auto result = set(steam_b, true);
    EXPECT_EQ(result.failure, failure_e::none);
    EXPECT_EQ(result.retired, removal_e::deleted);
    EXPECT_TRUE(result.retired_as_vibeshine);
    EXPECT_TRUE(result.created);
    EXPECT_FALSE(fs::exists(steam_a / flag_name));
    expect_recorded(steam_b);
  }

  TEST_F(SteamBigPictureFlagFile, KeepsAReplacementTheUserMayNotOpen) {
    set(steam_a, true);
    replace_file(steam_a / flag_name);
    set_access(steam_a / flag_name, L"D:P(A;;FA;;;%USER%)");
    set_access(steam_b, L"D:P(A;OICI;FA;;;WD)(A;OICI;FA;;;%USER%)");
    restrict_user();
    // Vibeshine's own access never reaches past the file ID check.
    const auto result = set(steam_b, true);
    EXPECT_EQ(result.failure, failure_e::none);
    EXPECT_EQ(result.retired, removal_e::replaced);
    EXPECT_TRUE(result.retired_as_vibeshine);
    EXPECT_TRUE(fs::exists(steam_a / flag_name));
    expect_recorded(steam_b);
  }

  TEST_F(SteamBigPictureFlagFile, UninstallDeletesItItselfWhenTheUserMayNotOpenIt) {
    set(steam_a, true);
    set_access(steam_a / flag_name, L"D:P(A;;FA;;;%USER%)");
    restrict_user();
    const auto result = uninstall();
    EXPECT_EQ(result.removal, removal_e::deleted);
    EXPECT_TRUE(result.removed_as_vibeshine);
    EXPECT_FALSE(fs::exists(steam_a / flag_name));
    EXPECT_FALSE(record);
  }

  TEST_F(SteamBigPictureFlagFile, ForgetsAFileOnADriveThatIsGone) {
    const auto drive = missing_drive();
    if (drive.empty()) {
      GTEST_SKIP() << "every drive letter is in use";
    }
    // Steam's previous folder was on a drive that is unplugged now. Turning on in folder B can't wait
    // for it, so that file stays there, forgotten.
    const flag_file::record_t unplugged {drive / L"Steam" / flag_name, "7:00ab"};
    record = unplugged;
    const auto on = set(steam_b, true);
    EXPECT_EQ(on.failure, failure_e::none);
    EXPECT_EQ(on.retired, removal_e::unavailable);
    EXPECT_TRUE(on.created);
    expect_recorded(steam_b);

    record = unplugged;
    const auto removed = uninstall();
    EXPECT_EQ(removed.removal, removal_e::unavailable);
    EXPECT_FALSE(record);
  }
}  // namespace

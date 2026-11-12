/**
 * @file src/platform/windows/steam_big_picture_flag_file.cpp
 * @brief Steam's debug-port flag file, and the record of the one Vibeshine created.
 */
#ifdef _WIN32
  #include "steam_big_picture_flag_file.h"

  #include "src/steam_big_picture_controls_policy.h"
  #include "src/utility.h"

  #include <string_view>
  #include <windows.h>

namespace platf::steam::big_picture::flag_file {
  namespace {
    namespace fs = std::filesystem;
    namespace policy = platf::steam::big_picture::policy;

    std::error_code last_error() {
      return {static_cast<int>(GetLastError()), std::system_category()};
    }

    // The volume and file ID of an open file. Deleting a file and creating it again changes the ID.
    std::optional<std::string> file_id(HANDLE file) {
      FILE_ID_INFO info {};
      if (!GetFileInformationByHandleEx(file, FileIdInfo, &info, sizeof(info))) {
        return std::nullopt;
      }
      constexpr std::string_view digits = "0123456789abcdef";
      std::string text = std::to_string(info.VolumeSerialNumber) + ':';
      for (const auto byte : info.FileId.Identifier) {
        text += digits[byte >> 4];
        text += digits[byte & 0xF];
      }
      return text;
    }

    // file_id, unless a test makes reading it fail. Either way, a failure leaves its reason in
    // GetLastError.
    std::optional<std::string> read_id(const environment_t &environment, const fs::path &path, HANDLE file) {
      if (environment.id_unreadable && environment.id_unreadable(path)) {
        SetLastError(ERROR_NOT_SUPPORTED);
        return std::nullopt;
      }
      return file_id(file);
    }

    // Opens a link itself, never its target, so only this name in Steam's folder is ever touched.
    HANDLE open_flag_file(const fs::path &path, DWORD access) {
      return CreateFileW(path.c_str(), access, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    }

    bool not_found(DWORD code) {
      return code == ERROR_FILE_NOT_FOUND || code == ERROR_PATH_NOT_FOUND;
    }

    // Opening path failed with code because its drive or network share isn't there: unplugged, an
    // empty card reader, another drive letter now, or a share that is offline.
    bool unavailable(const fs::path &path, DWORD code) {
      switch (code) {
        case ERROR_NOT_READY:
        case ERROR_INVALID_DRIVE:
        case ERROR_DEV_NOT_EXIST:
        case ERROR_UNRECOGNIZED_VOLUME:
        case ERROR_BAD_NETPATH:
        case ERROR_BAD_NET_NAME:
        case ERROR_NETNAME_DELETED:
        case ERROR_NETWORK_UNREACHABLE:
        case ERROR_HOST_UNREACHABLE:
          return true;
        case ERROR_PATH_NOT_FOUND:
          {
            // A folder that is gone, unless the drive itself is.
            const auto root = path.root_path();
            return !root.empty() && GetFileAttributesW(root.c_str()) == INVALID_FILE_ATTRIBUTES;
          }
        default:
          return false;
      }
    }

    // The file is no longer where it was recorded, as far as anyone can tell from here.
    bool gone(removal_e removal) {
      return removal == removal_e::missing || removal == removal_e::unavailable;
    }

    bool access_denied(const std::error_code &error) {
      return error == std::error_code(ERROR_ACCESS_DENIED, std::system_category());
    }

    // Whatever the record says, never delete anything but a flag file.
    bool names_flag_file(const fs::path &path) {
      return path.filename() == fs::path(policy::debug_port_flag_file);
    }

    struct lookup_t {
      bool exists = false;  ///< Something is there, or it couldn't be opened to find out.
      std::optional<std::string> id;
      std::error_code error;  ///< Why exists is true but id is empty.
    };

    // What is at path: nothing, a file with this ID, or a file whose ID can't be read.
    lookup_t lookup(const environment_t &environment, const fs::path &path) {
      HANDLE file = open_flag_file(path, FILE_READ_ATTRIBUTES);
      if (file == INVALID_HANDLE_VALUE) {
        const DWORD code = GetLastError();
        if (not_found(code)) {
          return {};
        }
        return {true, std::nullopt, {static_cast<int>(code), std::system_category()}};
      }
      auto close_file = util::fail_guard([&] {
        CloseHandle(file);
      });
      lookup_t result {true, read_id(environment, path, file)};
      if (!result.id) {
        result.error = last_error();
      }
      return result;
    }

    // Delete the flag file at path if it is still the one with expected_id. A file whose ID can't be
    // read, or a record without an ID, proves nothing: the file stays.
    removal_e remove(const environment_t &environment, const fs::path &path, const std::string &expected_id, std::error_code &error) {
      if (expected_id.empty()) {
        return removal_e::replaced;  // Callers forget such records first; never delete on their word.
      }
      HANDLE file = open_flag_file(path, DELETE | FILE_READ_ATTRIBUTES);
      if (file == INVALID_HANDLE_VALUE) {
        const DWORD code = GetLastError();
        if (unavailable(path, code)) {
          return removal_e::unavailable;
        }
        if (not_found(code)) {
          return removal_e::missing;
        }
        error = {static_cast<int>(code), std::system_category()};
        return removal_e::failed;
      }
      auto close_file = util::fail_guard([&] {
        CloseHandle(file);
      });
      // Checked on the handle that deletes it, so the file can't be swapped in between. That is also
      // what makes deleting with Vibeshine's own access safe: only the very file it created can go.
      const auto id = read_id(environment, path, file);
      if (!id) {
        // An ID that can't be read proves nothing either way, so the file stays and so does its
        // record, for the next try.
        error = last_error();
        return removal_e::failed;
      }
      if (*id != expected_id) {
        return removal_e::replaced;
      }
      FILE_DISPOSITION_INFO disposition {TRUE};
      if (!SetFileInformationByHandle(file, FileDispositionInfo, &disposition, sizeof(disposition))) {
        error = last_error();
        return removal_e::failed;
      }
      return removal_e::deleted;
    }

    // Delete the file this handle created, through the same file, so no other file can be hit.
    removal_e take_back(HANDLE created, std::error_code &error) {
      HANDLE file = ReOpenFile(created, DELETE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, 0);
      if (file == INVALID_HANDLE_VALUE) {
        error = last_error();
        return removal_e::failed;
      }
      auto close_file = util::fail_guard([&] {
        CloseHandle(file);
      });
      FILE_DISPOSITION_INFO disposition {TRUE};
      if (!SetFileInformationByHandle(file, FileDispositionInfo, &disposition, sizeof(disposition))) {
        error = last_error();
        return removal_e::failed;
      }
      return removal_e::deleted;
    }

    // A record proves which file Vibeshine created only with that file's ID, and only for a flag
    // file. Anything else is never acted on.
    bool trusted(const record_t &record) {
      return !record.file_id.empty() && names_flag_file(record.path);
    }
  }  // namespace

  std::optional<std::string> file_id(const fs::path &path) {
    HANDLE file = open_flag_file(path, FILE_READ_ATTRIBUTES);
    if (file == INVALID_HANDLE_VALUE) {
      return std::nullopt;
    }
    auto close_file = util::fail_guard([&] {
      CloseHandle(file);
    });
    return file_id(file);
  }

  set_result_t set(const fs::path &steam_root, bool enabled, const environment_t &environment) {
    set_result_t result;
    result.path = steam_root / policy::debug_port_flag_file;
    auto remembered = environment.load();
    bool forget = false;  // The record names a file that is gone, was replaced, or can't be trusted.
    if (remembered && !trusted(*remembered)) {
      if (names_flag_file(remembered->path)) {
        result.unproven = remembered->path;
      }
      remembered.reset();
      forget = true;
    }
    // First, where the recorded file is. The record names the file in Steam's folder when the path
    // matches, or when that file has the recorded ID because Steam's folder was moved or renamed with
    // it inside. Otherwise Steam runs from another folder now, and nothing would ever look at the old
    // one again, so the file Vibeshine created there goes first, before its record can be replaced.
    bool here = false;
    bool unidentified = false;  // Steam's folder has a file whose ID can't be read: maybe the recorded one, moved there.
    std::error_code unidentified_error;
    if (!environment.as_user([&] {
          const bool same_path = remembered && policy::same_windows_path(remembered->path.wstring(), result.path.wstring());
          lookup_t current;
          if (remembered && !same_path) {
            current = lookup(environment, result.path);
          }
          here = same_path || (current.id && *current.id == remembered->file_id);
          unidentified = !here && current.exists && !current.id;
          unidentified_error = current.error;
          if (here && !same_path) {
            result.moved_from = remembered->path;
          }
          if (remembered && !here) {
            result.retired_path = remembered->path;
            result.retired = remove(environment, remembered->path, remembered->file_id, result.error);
          }
        })) {
      result.failure = failure_e::no_user;
      return result;
    }
    if (result.retired == removal_e::failed && access_denied(result.error)) {
      // The console user may not open that folder, as when it is another Windows user's. Vibeshine
      // created the file there, and the ID check means it can only ever delete that very file.
      result.error.clear();
      result.retired = remove(environment, remembered->path, remembered->file_id, result.error);
      result.retired_as_vibeshine = true;
    }
    if (result.retired == removal_e::failed) {
      result.failure = failure_e::remove;
      result.failed_path = remembered->path;
      return result;
    }
    if (result.retired) {
      if (gone(*result.retired) && unidentified) {
        // Gone from the recorded path, and the file in Steam's folder can't be identified: keep the
        // record until its ID can be read. Turning on doesn't need to know; turning off does.
        if (!enabled) {
          result.failure = failure_e::identify;
          result.failed_path = result.path;
          result.error = unidentified_error;
          return result;
        }
      } else {
        forget = true;
      }
    }

    // Then the file in Steam's folder.
    std::optional<std::string> created;  // File ID of a file this call created.
    bool relocate = false;  // The recorded file is in Steam's folder under a new path.
    const bool signed_in = environment.as_user([&] {
      if (enabled) {
        // Created only if missing, so a file that is already there is never opened for writing or
        // deleting. Shared for deleting, so the new file can still be taken back below.
        HANDLE file = CreateFileW(result.path.c_str(), GENERIC_WRITE | FILE_READ_ATTRIBUTES, FILE_SHARE_DELETE, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
        bool existed = false;
        if (file == INVALID_HANDLE_VALUE && (GetLastError() == ERROR_FILE_EXISTS || GetLastError() == ERROR_ALREADY_EXISTS)) {
          existed = true;
          file = open_flag_file(result.path, FILE_READ_ATTRIBUTES);
        }
        if (file == INVALID_HANDLE_VALUE) {
          result.failure = failure_e::create;
          result.failed_path = result.path;
          result.error = last_error();
          return;
        }
        auto close_file = util::fail_guard([&] {
          CloseHandle(file);
        });
        const auto id = read_id(environment, result.path, file);
        if (!existed) {
          if (!id) {
            // Without its ID the file couldn't be told apart from one another program adds later, so
            // it isn't kept.
            result.failure = failure_e::identify;
            result.failed_path = result.path;
            result.error = last_error();
            std::error_code undo_error;
            result.undo = take_back(file, undo_error);
            if (*result.undo == removal_e::failed) {
              result.error = undo_error;
            }
            return;
          }
          created = *id;
          result.created = true;
        } else if (here) {
          switch (policy::match_recorded_flag_file(remembered->file_id, id)) {
            case policy::record_match_e::same:
              relocate = result.moved_from.has_value();
              break;
            case policy::record_match_e::different:
              forget = true;
              break;
            case policy::record_match_e::unknown:
              // An ID that can't be read proves nothing either way, so the record stays for the next try.
              break;
          }
        }
      } else if (here) {
        result.removed = remove(environment, result.path, remembered->file_id, result.error);
        if (*result.removed == removal_e::failed) {
          result.failure = failure_e::remove;
          result.failed_path = result.path;
          return;
        }
        forget = true;
      } else {
        // The user or another Steam mod added it, so it stays even when the controls are turned off.
        std::error_code error;
        result.left_alone = fs::exists(result.path, error);
      }
    });
    if (!signed_in) {
      // Signed out in between: the old folder's file may be gone already, and so must its record.
      result.failure = failure_e::no_user;
      if (forget) {
        result.save_failed = !environment.save(std::nullopt);
      }
      return result;
    }
    if (created) {
      // Replaces the record of a file retired above.
      if (environment.save(record_t {result.path, *created})) {
        return result;
      }
      // Without the record the file would look like another program's, and neither turning the
      // controls off nor uninstalling would remove it. Take it back instead.
      result.failure = failure_e::record;
      result.failed_path = result.path;
      std::error_code error;
      environment.as_user([&] {
        result.undo = remove(environment, result.path, *created, error);
      });
      result.error = error;
      return result;
    }
    if (relocate) {
      // Otherwise the file would look like another program's from now on. If saving fails, the ID
      // still finds it next time.
      result.save_failed = !environment.save(record_t {result.path, remembered->file_id});
    } else if (forget) {
      result.save_failed = !environment.save(std::nullopt);
    }
    return result;
  }

  bool recorded_file_exists(const record_t &record) {
    return trusted(record) && file_id(record.path) == record.file_id;
  }

  remove_result_t remove_recorded(const environment_t &environment, const std::optional<fs::path> &steam_root, const std::function<bool(const fs::path &folder)> &may_remove_as_vibeshine) {
    remove_result_t result;
    result.record = environment.load();
    if (!result.record) {
      return result;
    }
    const auto &record = *result.record;
    if (!trusted(record)) {
      result.unproven = names_flag_file(record.path);
      environment.save(std::nullopt);
      return result;
    }
    const bool signed_in = environment.as_user([&] {
      result.removal = remove(environment, record.path, record.file_id, result.error);
      // Gone from where it was made, but Steam's flag file has its ID: Steam's folder was moved or
      // renamed with it inside, and it must not stay behind once Vibeshine is gone.
      if (gone(*result.removal) && steam_root) {
        const auto moved = *steam_root / policy::debug_port_flag_file;
        if (lookup(environment, moved).id == record.file_id) {
          result.moved_to = moved;
          result.removal = remove(environment, moved, record.file_id, result.error);
        }
      }
    });
    if (signed_in && result.removal == removal_e::failed && access_denied(result.error)) {
      // The console user may not open its folder, as when it is another Windows user's. As in set(),
      // the ID check means Vibeshine can only ever delete the very file it created.
      result.error.clear();
      result.removal = remove(environment, result.moved_to.value_or(record.path), record.file_id, result.error);
      result.removed_as_vibeshine = true;
    }
    if (!signed_in) {
      if (!may_remove_as_vibeshine(record.path.parent_path())) {
        result.left_outside_steam = true;
        return result;
      }
      result.removal = remove(environment, record.path, record.file_id, result.error);
    }
    if (*result.removal != removal_e::failed) {
      environment.save(std::nullopt);
    }
    return result;
  }
}  // namespace platf::steam::big_picture::flag_file
#endif

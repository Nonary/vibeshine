/**
 * @file src/platform/windows/steam_big_picture_flag_file.h
 * @brief Steam's debug-port flag file, and the record of the one Vibeshine created.
 *
 * Vibeshine owns one flag file at most, and recognises it only by its file ID: a file it can't
 * identify is never deleted, and one it creates but can't identify is not kept. When Steam's folder
 * was moved or renamed with the file inside, the ID still finds it there. When Steam runs from
 * another folder, the file in the old one is deleted before anything else changes, so turning the
 * controls off or uninstalling never leaves one behind in a folder Vibeshine no longer looks at.
 */
#pragma once

#ifdef _WIN32
  #include <filesystem>
  #include <functional>
  #include <optional>
  #include <string>
  #include <system_error>

namespace platf::steam::big_picture::flag_file {
  /// The flag file Vibeshine created.
  struct record_t {
    std::filesystem::path path;
    std::string file_id;  ///< Its volume and file ID, so a file made again at the same path doesn't match. A record without one (early development builds saved those) proves nothing and is forgotten.
  };

  /// Where the record is kept and how file work runs. Vibeshine keeps the record in its state file
  /// and touches files as the console user; tests use memory and their own account.
  struct environment_t {
    std::function<std::optional<record_t>()> load;
    /// Saves the record, or forgets it for nullopt. False if that couldn't be saved.
    std::function<bool(const std::optional<record_t> &)> save;
    /// Runs work as the console user. False, without running it, when nobody is signed in. load and
    /// save are never called from inside the work.
    std::function<bool(const std::function<void()> &)> as_user;
    /// Tests only: whether reading the file ID of the file at this path fails, as it can in Windows.
    /// Empty in Vibeshine.
    std::function<bool(const std::filesystem::path &)> id_unreadable;
  };

  enum class removal_e {
    deleted,
    missing,
    unavailable,  ///< Its drive or network share isn't there. Like missing, its record is forgotten; the file stays on that drive.
    replaced,  ///< Another file is there now, and was left alone.
    failed,
  };

  /// The volume and file ID of the file at path, when it exists and can be read.
  std::optional<std::string> file_id(const std::filesystem::path &path);

  /// Whether the recorded file is still at its recorded path: a flag file there with the recorded ID.
  /// Runs as the calling thread's user.
  bool recorded_file_exists(const record_t &record);

  enum class failure_e {
    none,
    no_user,  ///< Nobody is signed in.
    create,  ///< failed_path could not be created.
    remove,  ///< failed_path, a flag file Vibeshine created, could not be deleted. Nothing else changed.
    record,  ///< The new file could not be recorded; undo says what happened when it was taken back.
    /// failed_path's file ID could not be read. For a file this call created, undo says what happened
    /// when it was taken back. Otherwise it is in Steam's folder and may be the recorded file, moved
    /// there with Steam's folder, so nothing changed.
    identify,
  };

  struct set_result_t {
    std::filesystem::path path;  ///< The flag file in Steam's folder.
    failure_e failure = failure_e::none;
    std::filesystem::path failed_path;
    std::error_code error;
    std::optional<removal_e> undo;  ///< For a record or identify failure: deleting the new file again. Empty if nobody was signed in.
    std::optional<std::filesystem::path> unproven;  ///< A record without a file ID named this path. It was forgotten, and nothing was deleted on its word.
    std::optional<removal_e> retired;  ///< The file Vibeshine created in another folder, deleted first.
    std::filesystem::path retired_path;
    bool retired_as_vibeshine = false;  ///< The console user may not open that folder, so Vibeshine deleted it with its own access.
    std::optional<removal_e> removed;  ///< Turning off: the file Vibeshine created in Steam's folder.
    std::optional<std::filesystem::path> moved_from;  ///< The recorded path, when Steam's folder was moved or renamed with the file inside.
    bool created = false;  ///< Turning on created the file in Steam's folder.
    bool left_alone = false;  ///< Turning off kept a flag file Vibeshine didn't create.
    bool save_failed = false;  ///< The record could not be cleared, or moved to the file's new path.
  };

  /// Create (enabled) or delete the flag file in steam_root. A file this creates is recorded, or
  /// deleted again if it can't be recorded or identified. Only the recorded file is ever deleted: one
  /// the user or another Steam mod added stays. The recorded file is recognised by its ID even after
  /// Steam's folder was moved or renamed with it inside, and its record then follows it. A recorded
  /// file in another folder is deleted first, with Vibeshine's own access when the console user may
  /// not open that folder, and if that fails nothing changes. A file ID that can't be read never
  /// makes Vibeshine forget its file. Everything else runs as the console user.
  set_result_t set(const std::filesystem::path &steam_root, bool enabled, const environment_t &environment);

  struct remove_result_t {
    std::optional<record_t> record;  ///< The recorded file, if any.
    std::optional<removal_e> removal;  ///< Empty when nothing was deleted.
    std::optional<std::filesystem::path> moved_to;  ///< Where the file was found after Steam's folder moved with it inside.
    bool left_outside_steam = false;  ///< Nobody is signed in and its folder isn't accepted, so it stays recorded.
    bool removed_as_vibeshine = false;  ///< The console user may not open its folder, so Vibeshine tried with its own access.
    bool unproven = false;  ///< The record has no file ID. It was forgotten, and nothing was deleted.
    std::error_code error;
  };

  /// For the uninstaller: delete the recorded file if it is still the one Vibeshine created, and
  /// forget it unless deleting failed. When it is gone from its recorded path but steam_root's flag
  /// file has its ID, Steam's folder was moved with it inside, and that file is deleted. A record
  /// naming any other file, or without a file ID, is forgotten without touching anything. When the
  /// console user may not open its folder, Vibeshine deletes it with its own access. With nobody
  /// signed in, only the recorded path is tried, with Vibeshine's own access, and only when
  /// may_remove_as_vibeshine accepts its folder.
  remove_result_t remove_recorded(const environment_t &environment, const std::optional<std::filesystem::path> &steam_root, const std::function<bool(const std::filesystem::path &folder)> &may_remove_as_vibeshine);
}  // namespace platf::steam::big_picture::flag_file
#endif

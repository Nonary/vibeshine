#pragma once

/**
 * @file tools/display_helper_paths.h
 * @brief Shared path/singleton helpers for the display helper engines.
 *
 * Both the legacy engine and the v2 engine must agree on snapshot file locations,
 * the search order for state files, and the single-instance mutex so that either
 * engine can pick up the other's persisted state (drop-in engine switching).
 * Bodies are ported verbatim from tools/display_settings_helper.cpp.
 */

#ifdef _WIN32

  #include <algorithm>
  #include <cwchar>
  #include <filesystem>
  #include <optional>
  #include <string>
  #include <string_view>
  #include <vector>

  #ifndef WIN32_LEAN_AND_MEAN
    #define WIN32_LEAN_AND_MEAN
  #endif
  #include <winsock2.h>
  #include <windows.h>
  #include <AclAPI.h>
  #include <sddl.h>
  #include <shlobj.h>
  #include <shellapi.h>

  #include "src/platform/windows/display_restore_policy.h"

  #include "src/platform/windows/ipc/misc_utils.h"

namespace display_helper_paths {
  inline bool is_private_system_dacl(PACL dacl, PSID system_sid, PSID administrators_sid, bool require_inheritance) {
    if (!dacl || !IsValidAcl(dacl)) {
      return false;
    }
    ACL_SIZE_INFORMATION info {};
    if (!GetAclInformation(dacl, &info, sizeof(info), AclSizeInformation)) {
      return false;
    }
    bool system_full = false;
    bool administrators_full = false;
    for (DWORD index = 0; index < info.AceCount; ++index) {
      void *raw_ace = nullptr;
      if (!GetAce(dacl, index, &raw_ace) || !raw_ace) {
        return false;
      }
      const auto *header = static_cast<const ACE_HEADER *>(raw_ace);
      if (header->AceType != ACCESS_ALLOWED_ACE_TYPE) {
        return false;
      }
      const auto *ace = static_cast<const ACCESS_ALLOWED_ACE *>(raw_ace);
      PSID sid = const_cast<DWORD *>(&ace->SidStart);
      const bool full_access = (ace->Mask & FILE_ALL_ACCESS) == FILE_ALL_ACCESS;
      const bool inherited_by_children = !require_inheritance ||
                                         (header->AceFlags & (OBJECT_INHERIT_ACE | CONTAINER_INHERIT_ACE)) ==
                                           (OBJECT_INHERIT_ACE | CONTAINER_INHERIT_ACE);
      if (!full_access || !inherited_by_children) {
        return false;
      }
      if (EqualSid(sid, system_sid)) {
        system_full = true;
      } else if (EqualSid(sid, administrators_sid)) {
        administrators_full = true;
      } else {
        return false;
      }
    }
    return system_full && administrators_full;
  }

  inline bool is_trusted_system_object(const std::filesystem::path &path, const bool require_protected_dacl) {
    const DWORD attributes = GetFileAttributesW(path.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES) {
      return false;
    }
    const DWORD flags = FILE_FLAG_OPEN_REPARSE_POINT |
                        ((attributes & FILE_ATTRIBUTE_DIRECTORY) ? FILE_FLAG_BACKUP_SEMANTICS : 0);
    HANDLE handle = CreateFileW(
      path.c_str(),
      READ_CONTROL,
      FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
      nullptr,
      OPEN_EXISTING,
      flags,
      nullptr
    );
    if (handle == INVALID_HANDLE_VALUE) {
      return false;
    }

    FILE_ATTRIBUTE_TAG_INFO tag_info {};
    const bool is_reparse = !GetFileInformationByHandleEx(handle, FileAttributeTagInfo, &tag_info, sizeof(tag_info)) ||
                            (tag_info.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;

    PSID owner = nullptr;
    PACL dacl = nullptr;
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    const DWORD security_status = GetSecurityInfo(
      handle,
      SE_FILE_OBJECT,
      OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION,
      &owner,
      nullptr,
      &dacl,
      nullptr,
      &descriptor
    );
    CloseHandle(handle);

    bool trusted = !is_reparse && security_status == ERROR_SUCCESS && owner && descriptor;
    BYTE system_sid_storage[SECURITY_MAX_SID_SIZE] {};
    BYTE administrators_sid_storage[SECURITY_MAX_SID_SIZE] {};
    DWORD system_sid_size = sizeof(system_sid_storage);
    DWORD administrators_sid_size = sizeof(administrators_sid_storage);
    trusted = trusted &&
              CreateWellKnownSid(WinLocalSystemSid, nullptr, system_sid_storage, &system_sid_size) &&
              CreateWellKnownSid(WinBuiltinAdministratorsSid, nullptr, administrators_sid_storage, &administrators_sid_size) &&
              (EqualSid(owner, system_sid_storage) || EqualSid(owner, administrators_sid_storage));
    if (trusted && require_protected_dacl) {
      SECURITY_DESCRIPTOR_CONTROL control = 0;
      DWORD revision = 0;
      trusted = GetSecurityDescriptorControl(descriptor, &control, &revision) &&
                (control & SE_DACL_PROTECTED) != 0;
    }
    if (trusted) {
      trusted = is_private_system_dacl(
        dacl,
        system_sid_storage,
        administrators_sid_storage,
        (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0
      );
    }
    if (descriptor) {
      LocalFree(descriptor);
    }
    return trusted;
  }

  inline bool ensure_private_system_directory(const std::filesystem::path &path) {
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
          L"O:SYG:SYD:P(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)",
          SDDL_REVISION_1,
          &descriptor,
          nullptr)) {
      return false;
    }
    SECURITY_ATTRIBUTES attributes {sizeof(attributes), descriptor, FALSE};
    const BOOL created = CreateDirectoryW(path.c_str(), &attributes);
    const DWORD create_error = created ? ERROR_SUCCESS : GetLastError();
    if (created) {
      LocalFree(descriptor);
      return is_trusted_system_object(path, true);
    }
    if (create_error != ERROR_ALREADY_EXISTS) {
      LocalFree(descriptor);
      return false;
    }

    if (is_trusted_system_object(path, true)) {
      LocalFree(descriptor);
      return true;
    }

    // Never take ownership of or repair an untrusted directory in place. An
    // attacker can retain a mutation/rename handle acquired before the DACL
    // change. Move that object away, then create a distinct protected object at
    // the canonical name. If a retained non-delete-shared handle blocks the
    // quarantine, fail closed without traversing the directory.
    bool quarantined = false;
    const auto nonce = std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64());
    for (unsigned int attempt = 0; attempt < 32 && !quarantined; ++attempt) {
      auto quarantine = path;
      quarantine += L".untrusted-" + nonce + L"-" + std::to_wstring(attempt);
      quarantined = MoveFileExW(path.c_str(), quarantine.c_str(), MOVEFILE_WRITE_THROUGH) != FALSE;
      if (!quarantined && GetLastError() != ERROR_ALREADY_EXISTS && GetLastError() != ERROR_FILE_EXISTS) {
        break;
      }
    }
    if (!quarantined) {
      LocalFree(descriptor);
      return false;
    }

    const BOOL recreated = CreateDirectoryW(path.c_str(), &attributes);
    LocalFree(descriptor);
    return recreated && is_trusted_system_object(path, true);
  }

  inline bool remove_untrusted_snapshot_objects(const std::filesystem::path &root) {
    constexpr std::wstring_view names[] = {
      L"display_golden_restore.json",
      L"display_golden_restore_status.json",
      L"display_session_current.json",
      L"display_session_previous.json",
      L"vibeshine_state.json",
    };
    for (const auto name : names) {
      const auto path = root / name;
      const DWORD attributes = GetFileAttributesW(path.c_str());
      if (attributes == INVALID_FILE_ATTRIBUTES) {
        if (GetLastError() == ERROR_FILE_NOT_FOUND || GetLastError() == ERROR_PATH_NOT_FOUND) {
          continue;
        }
        return false;
      }
      if (!is_trusted_system_object(path, false)) {
        if (!DeleteFileW(path.c_str())) {
          return false;
        }
      }
    }
    return true;
  }

  inline std::optional<std::filesystem::path> private_system_snapshot_dir() {
    std::wstring program_data(MAX_PATH, L'\0');
    if (FAILED(SHGetFolderPathW(nullptr, CSIDL_COMMON_APPDATA, nullptr, SHGFP_TYPE_CURRENT, program_data.data()))) {
      return std::nullopt;
    }
    program_data.resize(wcslen(program_data.c_str()));
    const auto path = std::filesystem::path(program_data) / L"Sunshine";
    if (!ensure_private_system_directory(path) || !remove_untrusted_snapshot_objects(path)) {
      return std::nullopt;
    }
    return path;
  }

  inline std::optional<std::filesystem::path> restore_state_file_from_command_line() {
    int argc = 0;
    auto argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (!argv) return std::nullopt;
    std::optional<std::filesystem::path> path;
    constexpr std::wstring_view prefix = L"--restore-state-file=";
    for (int i = 1; i < argc; ++i) {
      const std::wstring_view argument(argv[i]);
      if (argument.starts_with(prefix) && argument.size() > prefix.size()) {
        path = std::filesystem::path(argument.substr(prefix.size()));
      }
    }
    LocalFree(argv);
    return path;
  }

  inline std::wstring restore_task_arguments() {
    std::wstring arguments = L"--restore";
    if (const auto state_file = restore_state_file_from_command_line()) {
      arguments += L" " + display_helper::restore_state_file_argument(*state_file);
    }
    return arguments;
  }

  inline HANDLE make_named_mutex(const wchar_t *name) {
    SECURITY_ATTRIBUTES sa {};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = FALSE;
    return CreateMutexW(&sa, FALSE, name);
  }

  inline bool ensure_single_instance(HANDLE &out_handle) {
    out_handle = make_named_mutex(L"Global\\SunshineDisplayHelper");
    if (!out_handle && GetLastError() == ERROR_ACCESS_DENIED) {
      out_handle = make_named_mutex(L"Local\\SunshineDisplayHelper");
    }
    if (!out_handle) {
      return true;  // continue; best-effort singleton failed
    }
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
      return false;  // another instance running
    }
    return true;
  }

  inline void hide_console_window() {
    if (HWND console = GetConsoleWindow()) {
      ShowWindow(console, SW_HIDE);
    }
  }

  inline std::filesystem::path compute_log_dir() {
    // Try roaming AppData first
    std::wstring appdataW;
    appdataW.resize(MAX_PATH);
    if (SUCCEEDED(SHGetFolderPathW(nullptr, CSIDL_APPDATA, nullptr, SHGFP_TYPE_CURRENT, appdataW.data()))) {
      appdataW.resize(wcslen(appdataW.c_str()));
      auto path = std::filesystem::path(appdataW) / L"Sunshine";
      std::error_code ec;
      std::filesystem::create_directories(path, ec);
      return path;
    }

    // Next, %APPDATA%
    std::wstring envAppData;
    DWORD needed = GetEnvironmentVariableW(L"APPDATA", nullptr, 0);
    if (needed > 0) {
      envAppData.resize(needed);
      DWORD written = GetEnvironmentVariableW(L"APPDATA", envAppData.data(), needed);
      if (written > 0) {
        envAppData.resize(written);
        auto path = std::filesystem::path(envAppData) / L"Sunshine";
        std::error_code ec;
        std::filesystem::create_directories(path, ec);
        return path;
      }
    }

    // Fallback: temp directory or current dir
    std::wstring tempW;
    tempW.resize(MAX_PATH);
    DWORD tlen = GetTempPathW(MAX_PATH, tempW.data());
    if (tlen > 0 && tlen < MAX_PATH) {
      tempW.resize(tlen);
      auto path = std::filesystem::path(tempW) / L"Sunshine";
      std::error_code ec;
      std::filesystem::create_directories(path, ec);
      return path;
    }
    auto path = std::filesystem::path(L".") / L"Sunshine";
    std::error_code ec;
    std::filesystem::create_directories(path, ec);
    return path;
  }

  inline std::filesystem::path compute_snapshot_dir() {
    // When running as SYSTEM, prefer a shared ProgramData location for snapshots.
    if (platf::dxgi::is_running_as_system()) {
      if (const auto path = private_system_snapshot_dir()) {
        return *path;
      }
      return {};
    }

    // Default to per-user roaming AppData (or fallback locations inside compute_log_dir).
    return compute_log_dir();
  }

  struct SnapshotPaths {
    std::filesystem::path golden;
    std::filesystem::path golden_status;
    std::filesystem::path session_current;
    std::filesystem::path session_previous;
    std::filesystem::path vibeshine_state;
  };

  inline SnapshotPaths make_snapshot_paths(const std::filesystem::path &root) {
    return SnapshotPaths {
      .golden = root / L"display_golden_restore.json",
      .golden_status = root / L"display_golden_restore_status.json",
      .session_current = root / L"display_session_current.json",
      .session_previous = root / L"display_session_previous.json",
      .vibeshine_state = root / L"vibeshine_state.json",
    };
  }

  inline std::vector<std::filesystem::path> executable_config_search_roots() {
    std::vector<std::filesystem::path> roots;
    wchar_t exe_path[MAX_PATH] = {};
    if (!GetModuleFileNameW(nullptr, exe_path, MAX_PATH)) {
      return roots;
    }

    const auto module_path = std::filesystem::path(exe_path);
    const auto module_dir = module_path.parent_path();
    if (module_dir.empty()) {
      return roots;
    }

    roots.push_back(module_dir / L"config");
    roots.push_back(module_dir.parent_path() / L"config");
    roots.push_back(module_dir.parent_path());
    return roots;
  }

  inline std::vector<std::filesystem::path> snapshot_search_roots() {
    std::vector<std::filesystem::path> roots;
    const auto user_root = compute_log_dir();
    if (!user_root.empty()) {
      roots.push_back(user_root);
    }
    if (platf::dxgi::is_running_as_system()) {
      if (const auto path = private_system_snapshot_dir()) {
        roots.push_back(*path);
      }
    }
    for (const auto &root : executable_config_search_roots()) {
      if (!root.empty()) {
        roots.push_back(root);
      }
    }
    // De-duplicate while preserving order.
    std::vector<std::filesystem::path> uniq;
    for (const auto &root : roots) {
      if (root.empty()) {
        continue;
      }
      if (std::find(uniq.begin(), uniq.end(), root) == uniq.end()) {
        uniq.push_back(root);
      }
    }
    return uniq;
  }
}  // namespace display_helper_paths

#endif  // _WIN32

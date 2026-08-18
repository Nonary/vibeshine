#define WIN32_LEAN_AND_MEAN
#define NOMINMAX

#include <windows.h>
#include <aclapi.h>
#include <bcrypt.h>
#include <msiquery.h>
#include <sddl.h>

#include "terminal_isolation_ca_config.h"

#include <array>
#include <optional>
#include <string>
#include <vector>

namespace {
  constexpr UINT ca_failure = ERROR_INSTALL_FAILURE;
  constexpr DWORD writable_ace_mask = FILE_WRITE_DATA | FILE_APPEND_DATA | FILE_WRITE_EA | FILE_WRITE_ATTRIBUTES |
    DELETE | FILE_DELETE_CHILD | WRITE_DAC | WRITE_OWNER | GENERIC_WRITE | GENERIC_ALL;

  struct handle_t {
    HANDLE value {INVALID_HANDLE_VALUE};
    handle_t() = default;
    explicit handle_t(HANDLE handle): value {handle} {}
    static bool valid(HANDLE handle) { return handle != nullptr && handle != INVALID_HANDLE_VALUE; }
    ~handle_t() { if (valid(value)) CloseHandle(value); }
    handle_t(const handle_t &) = delete;
    handle_t &operator=(const handle_t &) = delete;
    handle_t(handle_t &&other) noexcept: value {other.value} { other.value = INVALID_HANDLE_VALUE; }
    handle_t &operator=(handle_t &&other) noexcept {
      if (this != &other) { if (valid(value)) CloseHandle(value); value = other.value; other.value = INVALID_HANDLE_VALUE; }
      return *this;
    }
    explicit operator bool() const { return valid(value); }
  };

  std::wstring full_path(const std::wstring &path) {
    std::vector<wchar_t> buffer(MAX_PATH);
    for (;;) {
      const DWORD length = GetFullPathNameW(path.c_str(), static_cast<DWORD>(buffer.size()), buffer.data(), nullptr);
      if (!length) return {};
      if (length < buffer.size()) return {buffer.data(), length};
      buffer.resize(length + 1);
    }
  }

  std::wstring trim_slash(std::wstring path) {
    while (path.size() > 3 && (path.back() == L'\\' || path.back() == L'/')) path.pop_back();
    return path;
  }

  bool equal_path(const std::wstring &left, const std::wstring &right) {
    return _wcsicmp(trim_slash(left).c_str(), trim_slash(right).c_str()) == 0;
  }

  bool descendant(const std::wstring &parent, const std::wstring &child) {
    const auto base = trim_slash(parent);
    const auto target = trim_slash(child);
    return target.size() > base.size() && _wcsnicmp(target.c_str(), base.c_str(), base.size()) == 0 && target[base.size()] == L'\\';
  }

  bool trusted_sid(PSID sid) {
    if (!sid || !IsValidSid(sid)) return false;
    constexpr const wchar_t *trusted[] = {
      L"S-1-5-18", // LocalSystem
      L"S-1-5-32-544", // Built-in Administrators
      L"S-1-5-80-956008885-3418522649-1831038044-1853292631-2271478464" // TrustedInstaller
    };
    for (const auto text : trusted) {
      PSID candidate = nullptr;
      if (ConvertStringSidToSidW(text, &candidate)) {
        const bool equal = EqualSid(sid, candidate) != FALSE;
        LocalFree(candidate);
        if (equal) return true;
      }
    }
    return false;
  }

  bool trusted_security(HANDLE file) {
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    PSID owner = nullptr;
    PACL dacl = nullptr;
    const DWORD result = GetSecurityInfo(file, SE_FILE_OBJECT, OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION,
      &owner, nullptr, &dacl, nullptr, &descriptor);
    if (result != ERROR_SUCCESS || !owner || !trusted_sid(owner) || !dacl) {
      if (descriptor) LocalFree(descriptor);
      return false;
    }
    ACL_SIZE_INFORMATION info {};
    if (!GetAclInformation(dacl, &info, sizeof(info), AclSizeInformation)) {
      LocalFree(descriptor);
      return false;
    }
    for (DWORD index = 0; index < info.AceCount; ++index) {
      void *raw = nullptr;
      if (!GetAce(dacl, index, &raw)) { LocalFree(descriptor); return false; }
      const auto *header = static_cast<const ACE_HEADER *>(raw);
      if ((header->AceFlags & INHERIT_ONLY_ACE) != 0) continue;
      if (header->AceType == ACCESS_ALLOWED_ACE_TYPE) {
        const auto *ace = static_cast<const ACCESS_ALLOWED_ACE *>(raw);
        const auto sid = const_cast<PSID>(reinterpret_cast<const SID *>(reinterpret_cast<const BYTE *>(&ace->SidStart)));
        if ((ace->Mask & writable_ace_mask) != 0 && !trusted_sid(sid)) {
          LocalFree(descriptor);
          return false;
        }
      } else if (header->AceType == ACCESS_ALLOWED_OBJECT_ACE_TYPE || header->AceType == ACCESS_ALLOWED_CALLBACK_ACE_TYPE ||
                 header->AceType == ACCESS_ALLOWED_CALLBACK_OBJECT_ACE_TYPE || header->AceType == ACCESS_ALLOWED_COMPOUND_ACE_TYPE) {
        // Do not attempt to reason about conditional/object/compound allow
        // masks. Failing closed prevents a non-obvious write grant from
        // reaching the PowerShell boundary.
        LocalFree(descriptor);
        return false;
      }
    }
    LocalFree(descriptor);
    return true;
  }

  std::wstring final_path(HANDLE file) {
    std::vector<wchar_t> buffer(MAX_PATH);
    for (;;) {
      const DWORD length = GetFinalPathNameByHandleW(file, buffer.data(), static_cast<DWORD>(buffer.size()), FILE_NAME_NORMALIZED);
      if (!length) return {};
      if (length < buffer.size()) {
        std::wstring result {buffer.data(), length};
        if (result.rfind(L"\\\\?\\", 0) == 0) result.erase(0, 4);
        return result;
      }
      buffer.resize(length + 1);
    }
  }

  handle_t open_checked(const std::wstring &path, bool directory, bool enforce_security = true) {
    const auto expected = full_path(path);
    if (expected.empty()) return {};
    const DWORD flags = (directory ? FILE_FLAG_BACKUP_SEMANTICS : 0) | FILE_FLAG_OPEN_REPARSE_POINT;
    const DWORD access = directory ? (FILE_READ_ATTRIBUTES | READ_CONTROL) : (GENERIC_READ | READ_CONTROL);
    handle_t file {CreateFileW(expected.c_str(), access, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
      nullptr, OPEN_EXISTING, flags, nullptr)};
    if (!file) return {};
    FILE_ATTRIBUTE_TAG_INFO tag {};
    if (!GetFileInformationByHandleEx(file.value, FileAttributeTagInfo, &tag, sizeof(tag)) || tag.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT ||
        !equal_path(final_path(file.value), expected) || (enforce_security && !trusted_security(file.value))) return {};
    return file;
  }

  bool checked_directory(const std::wstring &path, bool enforce_security = true) {
    const auto expected = trim_slash(full_path(path));
    if (expected.size() < 3 || expected[1] != L':' || expected[2] != L'\\') return false;
    // The root directory's inherited ACL commonly grants Authenticated Users
    // append/inherit-only rights.  It is checked for canonical identity and
    // reparse safety only; ownership/write admission begins at Program Files.
    if (!open_checked(expected.substr(0, 3), true, false)) return false;
    if (expected.size() == 3) return true;
    for (std::size_t end = 3;;) {
      const auto slash = expected.find(L'\\', end);
      const auto component = slash == std::wstring::npos ? expected : expected.substr(0, slash);
      if (!open_checked(component, true, enforce_security)) return false;
      if (slash == std::wstring::npos) break;
      end = slash + 1;
    }
    return true;
  }

  bool checked_file(const std::wstring &path, handle_t &result) {
    result = open_checked(path, false);
    return static_cast<bool>(result);
  }

  bool sha256_file(HANDLE file, std::array<unsigned char, 32> &result) {
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    DWORD object_length = 0, returned = 0;
    bool success = BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) == 0 &&
      BCryptGetProperty(algorithm, BCRYPT_OBJECT_LENGTH, reinterpret_cast<PUCHAR>(&object_length), sizeof(object_length), &returned, 0) == 0;
    if (!success) { if (hash) BCryptDestroyHash(hash); if (algorithm) BCryptCloseAlgorithmProvider(algorithm, 0); return false; }
    std::vector<unsigned char> object(object_length);
    success = BCryptCreateHash(algorithm, &hash, object.data(), object_length, nullptr, 0, 0) == 0;
    std::array<unsigned char, 64 * 1024> buffer {};
    DWORD read = 0;
    while (success) {
      if (!ReadFile(file, buffer.data(), static_cast<DWORD>(buffer.size()), &read, nullptr)) {
        success = false;
        break;
      }
      if (read == 0) break;
      success = BCryptHashData(hash, buffer.data(), read, 0) == 0;
    }
    DWORD hash_length = 0;
    success = success && BCryptGetProperty(algorithm, BCRYPT_HASH_LENGTH, reinterpret_cast<PUCHAR>(&hash_length), sizeof(hash_length), &returned, 0) == 0 && hash_length == result.size();
    success = success && BCryptFinishHash(hash, result.data(), static_cast<ULONG>(result.size()), 0) == 0;
    BCryptDestroyHash(hash);
    BCryptCloseAlgorithmProvider(algorithm, 0);
    return success;
  }

  bool pinned_file(const std::wstring &path, const char *expected) {
    handle_t file;
    if (!checked_file(path, file)) return false;
    std::array<unsigned char, 32> digest {};
    if (!sha256_file(file.value, digest)) return false;
    constexpr char hex[] = "0123456789abcdef";
    std::string actual;
    actual.reserve(64);
    for (const auto value : digest) { actual.push_back(hex[value >> 4]); actual.push_back(hex[value & 0xf]); }
    return actual == expected;
  }

  bool pinned_script(const std::wstring &path) {
    return pinned_file(path, VIBESHINE_TERMINAL_ISOLATION_CA_SCRIPT_SHA256);
  }

  bool get_data(MSIHANDLE install, std::wstring &root, const wchar_t *expected_action) {
    DWORD size = 512;
    std::wstring data;
    for (;;) {
      std::vector<wchar_t> buffer(size + 1, L'\0');
      DWORD requested = size;
      const UINT result = MsiGetPropertyW(install, L"CustomActionData", buffer.data(), &requested);
      if (result == ERROR_MORE_DATA) { size = requested + 1; continue; }
      if (result != ERROR_SUCCESS || requested == 0) return false;
      data.assign(buffer.data(), requested);
      break;
    }
    const auto separator = data.find(L'|');
    if (separator == std::wstring::npos || data.find(L'|', separator + 1) != std::wstring::npos) return false;
    root = data.substr(0, separator);
    const auto action = data.substr(separator + 1);
    if (root.empty() || root.size() > 32768 || action != expected_action) return false;
    for (const auto character : root) {
      if (character == L'"' || character == L'|' || character == L'\r' || character == L'\n' || character == L'\0') return false;
    }
    return true;
  }

  bool validate_tree(const std::wstring &root, std::wstring &script) {
    const auto install = trim_slash(full_path(root));
    const auto program_files = trim_slash(full_path([] {
      std::vector<wchar_t> buffer(MAX_PATH);
      const DWORD length = GetEnvironmentVariableW(L"ProgramFiles", buffer.data(), static_cast<DWORD>(buffer.size()));
      return length && length < buffer.size() ? std::wstring {buffer.data(), length} : std::wstring {};
    }()));
    if (install.empty() || program_files.empty() || !descendant(program_files, install) || !checked_directory(program_files) || !checked_directory(install)) return false;
    const auto terminal = install + L"\\terminal-isolation";
    script = terminal + L"\\terminal-isolation.ps1";
    if (!checked_directory(terminal)) return false;
    std::vector<wchar_t> windows(MAX_PATH);
    const DWORD windows_length = GetWindowsDirectoryW(windows.data(), static_cast<DWORD>(windows.size()));
    if (!windows_length || windows_length >= windows.size()) return false;
    const auto native = std::wstring {windows.data(), windows_length} + L"\\System32\\termsrv.dll";
    return pinned_file(native, VIBESHINE_TERMINAL_ISOLATION_CA_TERMSRV_SHA256) && pinned_script(script);
  }

  std::wstring quote(const std::wstring &value) {
    std::wstring result = L"\"";
    std::size_t slashes = 0;
    for (const auto character : value) {
      if (character == L'\\') {
        ++slashes;
        continue;
      }
      if (character == L'\"') {
        result.append(slashes * 2 + 1, L'\\');
        result.push_back(character);
        slashes = 0;
        continue;
      }
      result.append(slashes, L'\\');
      result.push_back(character);
      slashes = 0;
    }
    result.append(slashes * 2, L'\\');
    result.push_back(L'\"');
    return result;
  }

  constexpr DWORD process_timeout_ms = 120000;
  constexpr DWORD termination_wait_ms = 10000;

  bool terminate_job(handle_t &job, HANDLE process) {
    if (job) TerminateJobObject(job.value, ca_failure);
    if (WaitForSingleObject(process, termination_wait_ms) == WAIT_OBJECT_0) return true;
    // Assignment can fail before the process enters the job.  Terminate the
    // suspended/resumed child directly as a final containment fallback.
    TerminateProcess(process, ca_failure);
    return WaitForSingleObject(process, termination_wait_ms) == WAIT_OBJECT_0;
  }

  std::optional<bool> status_requires_reboot() {
    std::vector<wchar_t> buffer(MAX_PATH);
    const DWORD length = GetEnvironmentVariableW(L"ProgramData", buffer.data(), static_cast<DWORD>(buffer.size()));
    if (!length || length >= buffer.size()) return std::nullopt;
    const std::wstring state_root = std::wstring {buffer.data(), length} + L"\\Vibeshine\\TerminalIsolation";
    // ProgramData itself is intentionally user-appendable on normal Windows
    // hosts.  Validate every component for final identity/reparse safety and
    // enforce the protected ACL on the status file itself.
    if (GetFileAttributesW(state_root.c_str()) == INVALID_FILE_ATTRIBUTES && GetLastError() == ERROR_PATH_NOT_FOUND) return false;
    if (!checked_directory(state_root, false)) return std::nullopt;
    handle_t status_file;
    const auto status_path = state_root + L"\\status.txt";
    if (GetFileAttributesW(status_path.c_str()) == INVALID_FILE_ATTRIBUTES && GetLastError() == ERROR_FILE_NOT_FOUND) return false;
    if (!checked_file(status_path, status_file)) return std::nullopt;
    std::array<char, 64> status {};
    DWORD read = 0;
    if (!ReadFile(status_file.value, status.data(), static_cast<DWORD>(status.size() - 1), &read, nullptr)) return std::nullopt;
    const std::string value(status.data(), read);
    return value == "pending-restart" || value == "pending-native-restart";
  }

  UINT run(MSIHANDLE install, const wchar_t *action) {
    std::wstring root, script;
    if (!get_data(install, root, action) || !validate_tree(root, script)) return ca_failure;
    std::vector<wchar_t> windows(MAX_PATH);
    const DWORD length = GetWindowsDirectoryW(windows.data(), static_cast<DWORD>(windows.size()));
    if (!length || length >= windows.size()) return ca_failure;
    const std::wstring powershell = std::wstring {windows.data(), length} + L"\\System32\\WindowsPowerShell\\v1.0\\powershell.exe";
    handle_t powershell_file;
    if (!checked_file(powershell, powershell_file)) return ca_failure;
    std::wstring command = quote(powershell) + L" -NoLogo -NonInteractive -NoProfile -ExecutionPolicy Bypass -File " + quote(script) +
      L" -Action " + action + L" -InstallRoot " + quote(trim_slash(full_path(root)));
    std::vector<wchar_t> mutable_command(command.begin(), command.end());
    mutable_command.push_back(L'\0');
    STARTUPINFOW startup {};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process {};
    if (!CreateProcessW(powershell.c_str(), mutable_command.data(), nullptr, nullptr, FALSE,
      CREATE_UNICODE_ENVIRONMENT | CREATE_SUSPENDED, nullptr, nullptr, &startup, &process)) return ca_failure;
    handle_t job {CreateJobObjectW(nullptr, nullptr)};
    if (!job) {
      TerminateProcess(process.hProcess, ca_failure);
      WaitForSingleObject(process.hProcess, termination_wait_ms);
      CloseHandle(process.hThread);
      CloseHandle(process.hProcess);
      return ca_failure;
    }
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits {};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (!SetInformationJobObject(job.value, JobObjectExtendedLimitInformation, &limits, sizeof(limits)) ||
        !AssignProcessToJobObject(job.value, process.hProcess)) {
      terminate_job(job, process.hProcess);
      CloseHandle(process.hThread);
      CloseHandle(process.hProcess);
      return ca_failure;
    }
    handle_t process_handle {process.hProcess};
    handle_t thread_handle {process.hThread};
    if (ResumeThread(thread_handle.value) == static_cast<DWORD>(-1)) {
      terminate_job(job, process_handle.value);
      return ca_failure;
    }
    const DWORD wait = WaitForSingleObject(process_handle.value, process_timeout_ms);
    if (wait == WAIT_TIMEOUT) {
      terminate_job(job, process_handle.value);
      return ca_failure;
    }
    if (wait != WAIT_OBJECT_0) {
      terminate_job(job, process_handle.value);
      return ca_failure;
    }
    DWORD exit_code = ca_failure;
    if (!GetExitCodeProcess(process_handle.value, &exit_code)) return ca_failure;
    if (exit_code != ERROR_SUCCESS) return ca_failure;
    return ERROR_SUCCESS;
  }

  UINT set_reboot_property(MSIHANDLE install) {
    std::wstring root;
    if (!get_data(install, root, L"RebootProbe")) return ca_failure;
    const auto reboot = status_requires_reboot();
    if (!reboot.has_value()) return ca_failure;
    return MsiSetPropertyW(install, L"VIBESHINE_TERMINAL_REBOOT", *reboot ? L"1" : L"0") == ERROR_SUCCESS ? ERROR_SUCCESS : ca_failure;
  }
}

extern "C" __declspec(dllexport) UINT __stdcall TerminalIsolationInstall(MSIHANDLE install) {
  return run(install, L"Install");
}

extern "C" __declspec(dllexport) UINT __stdcall TerminalIsolationMsiRollback(MSIHANDLE install) {
  return run(install, L"Rollback");
}

extern "C" __declspec(dllexport) UINT __stdcall TerminalIsolationUninstall(MSIHANDLE install) {
  return run(install, L"Rollback");
}

extern "C" __declspec(dllexport) UINT __stdcall TerminalIsolationUninstallCompensation(MSIHANDLE install) {
  return run(install, L"Rearm");
}

extern "C" __declspec(dllexport) UINT __stdcall TerminalIsolationRebootProbe(MSIHANDLE install) {
  return set_reboot_property(install);
}

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID) {
  if (reason == DLL_PROCESS_ATTACH) DisableThreadLibraryCalls(instance);
  return TRUE;
}

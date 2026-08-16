#include "terminal_session_worker_process.h"
#include "terminal_session_display_protocol.h"

#ifdef _WIN32
  #include "terminal_session_service.h"
  #include "terminal_session_hdr_policy.h"
  #include "terminal_session_worker.h"
  #include "platform/windows/ipc/pipes.h"
  #include <openssl/crypto.h>
  #include <openssl/evp.h>
  #include <openssl/hmac.h>
  #include <openssl/rand.h>
  #include <Windows.h>
  #include <Aclapi.h>
  #include <Sddl.h>
  #include <ShlObj.h>
  #include <UserEnv.h>
  #include <algorithm>
  #include <array>
  #include <chrono>
  #include <cstddef>
  #include <cstring>
  #include <cwctype>
  #include <filesystem>
  #include <limits>
  #include <span>
  #include <sstream>
  #include <string_view>
  #include <mutex>
  #include <utility>
  #include <vector>
  #include <virtual_display/driver/windows_control_client.h>
#endif

namespace terminal_session::worker {
#ifdef _WIN32
  namespace {
    struct handle_closer {
      void operator()(void *value) const noexcept {
        if (value && value != INVALID_HANDLE_VALUE) CloseHandle(value);
      }
    };
    using unique_handle = std::unique_ptr<void, handle_closer>;

    struct poisoned_worker_t {
      HANDLE process {};
      HANDLE job {};
      std::unique_ptr<platf::dxgi::INamedPipe> pipe;
      steam_offline::registration_t registration;
    };

    /* A failed teardown remains contained and registered for the lifetime of
     * the service.  The heap allocation deliberately survives static
     * destruction so registration_t cannot release a live worker's filter. */
    std::mutex &poisoned_workers_mutex() {
      static auto *mutex = new std::mutex;
      return *mutex;
    }
    std::vector<poisoned_worker_t> &poisoned_workers() {
      static auto *workers = new std::vector<poisoned_worker_t>;
      return *workers;
    }

    struct local_free {
      void operator()(void *value) const noexcept {
        if (value) LocalFree(value);
      }
    };
    using unique_local_memory = std::unique_ptr<void, local_free>;

#pragma pack(push, 1)
    struct snapshot_auth_data {
      std::uint32_t session_id {};
      std::uint64_t generation {};
      std::uint64_t display_id {};
      std::uint32_t tier {};
      std::uint64_t sequence {};
      std::array<std::uint8_t, 32> digest {};
    };
#pragma pack(pop)

    static_assert(sizeof(snapshot_auth_data) == 64);

    bool snapshot_auth_tag(
      const std::array<std::uint8_t, 32> &key,
      const snapshot_auth_data &data,
      std::array<std::uint8_t, 32> &tag) {
      unsigned int output_size = 0;
      return HMAC(
               EVP_sha256(), key.data(), static_cast<int>(key.size()),
               reinterpret_cast<const unsigned char *>(&data), sizeof(data),
               tag.data(), &output_size) != nullptr && output_size == tag.size();
    }

    bool local_system_sid(PSID sid) {
      std::array<std::byte, SECURITY_MAX_SID_SIZE> storage {};
      DWORD size = static_cast<DWORD>(storage.size());
      return sid && IsValidSid(sid) && CreateWellKnownSid(WinLocalSystemSid, nullptr, storage.data(), &size) &&
             EqualSid(sid, storage.data());
    }

    unique_local_memory worker_security_descriptor() {
      PSECURITY_DESCRIPTOR raw = nullptr;
      if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(worker_process_security_sddl.data(), SDDL_REVISION_1, &raw, nullptr) ||
          !raw || !IsValidSecurityDescriptor(raw)) {
        if (raw) LocalFree(raw);
        return {};
      }
      PSID owner = nullptr;
      BOOL owner_defaulted = FALSE;
      BOOL dacl_present = FALSE;
      BOOL dacl_defaulted = FALSE;
      PACL dacl = nullptr;
      PVOID ace_raw = nullptr;
      if (!GetSecurityDescriptorOwner(raw, &owner, &owner_defaulted) || !local_system_sid(owner) ||
          !GetSecurityDescriptorDacl(raw, &dacl_present, &dacl, &dacl_defaulted) || !dacl_present || !dacl ||
          owner_defaulted || dacl_defaulted || dacl->AceCount != 1 || !GetAce(dacl, 0, &ace_raw) || !ace_raw) {
        LocalFree(raw);
        return {};
      }
      const auto *ace = static_cast<const ACCESS_ALLOWED_ACE *>(ace_raw);
      if (ace->Header.AceType != ACCESS_ALLOWED_ACE_TYPE || ace->Mask != GENERIC_ALL ||
          !local_system_sid(reinterpret_cast<PSID>(const_cast<DWORD *>(&ace->SidStart)))) {
        LocalFree(raw);
        return {};
      }
      return unique_local_memory {raw};
    }

    std::string unique_pipe(const std::string_view prefix = "VibeshineTerminalWorker-") {
      std::array<unsigned char, 16> bytes {};
      if (RAND_bytes(bytes.data(), static_cast<int>(bytes.size())) != 1) return {};
      constexpr char hex[] = "0123456789abcdef";
      std::string result {prefix};
      for (const auto value : bytes) { result.push_back(hex[value >> 4]); result.push_back(hex[value & 0xf]); }
      OPENSSL_cleanse(bytes.data(), bytes.size());
      return result;
    }

    std::filesystem::path service_directory() {
      std::wstring path(32768, L'\0');
      const DWORD size = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
      if (!size || size >= path.size()) return {};
      path.resize(size);
      return std::filesystem::path(path).parent_path();
    }

    std::wstring wide(std::string_view value);

    struct hdr_activation_result_t {
      bool accepted {};
      DWORD exit_code {};
      std::string error;
    };

    hdr_activation_result_t activate_hdr_helper(HANDLE launch_token, HANDLE worker_process, HANDLE worker_job,
                                                const provider_resource_t &resource,
                                                const terminal_session::hdr::target_binding_t &bound_target,
                                                terminal_session::hdr::activation_capability_t capability,
                                                bool &worker_poisoned) {
      if (!launch_token || !worker_process || !worker_job || capability.magic != terminal_session::hdr::activation_capability_magic ||
          capability.size != sizeof(capability) || capability.version != terminal_session::hdr::activation_capability_version ||
          capability.parent_pid != 0 || capability.reserved != 0 || capability.session_id != resource.windows_session_id) {
        return {.error = "The worker HDR target did not match the broker-owned seat."};
      }
      if (terminal_session::hdr::binding_from_capability(capability) != bound_target) {
        return {.error = "The worker HDR target changed after broker attestation."};
      }
      capability.parent_pid = GetCurrentProcessId();
      const auto helper = service_directory() / L"vibeshine_terminal_hdr_activator.exe";
      if (!std::filesystem::is_regular_file(helper)) return {.error = "The installed native HDR activator is missing."};

      const auto pipe_leaf = unique_pipe("VibeshineTerminalHdr-");
      if (pipe_leaf.empty()) return {.error = "The broker could not generate the native HDR endpoint."};
      const auto pipe_name = L"\\\\.\\pipe\\" + wide(pipe_leaf);
      const auto pipe_sddl = L"D:P(A;;GA;;;SY)(A;;GR;;;" + wide(resource.user_sid) + L")";
      PSECURITY_DESCRIPTOR pipe_descriptor = nullptr;
      if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(pipe_sddl.c_str(), SDDL_REVISION_1,
                                                                 &pipe_descriptor, nullptr)) {
        return {.error = "The broker could not secure the native HDR endpoint."};
      }
      SECURITY_ATTRIBUTES pipe_security {.nLength = sizeof(SECURITY_ATTRIBUTES), .lpSecurityDescriptor = pipe_descriptor};
      HANDLE raw_pipe = CreateNamedPipeW(pipe_name.c_str(),
                                         PIPE_ACCESS_OUTBOUND | FILE_FLAG_FIRST_PIPE_INSTANCE | FILE_FLAG_OVERLAPPED,
                                         PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
                                         1, sizeof(capability), 0, 0, &pipe_security);
      LocalFree(pipe_descriptor);
      if (!raw_pipe || raw_pipe == INVALID_HANDLE_VALUE) {
        return {.error = "The broker could not create the native HDR endpoint."};
      }
      unique_handle capability_pipe {raw_pipe};

      STARTUPINFOW startup {.cb = sizeof(STARTUPINFOW)};
      std::wstring desktop = wide(resource.desktop_name);
      startup.lpDesktop = desktop.data();

      std::wstring command = L"\"" + helper.wstring() + L"\" --activate-hdr --capability-pipe=" + pipe_name;
      PROCESS_INFORMATION process {};
      const BOOL created = CreateProcessAsUserW(launch_token, helper.c_str(), command.data(), nullptr, nullptr, FALSE,
                                                CREATE_NO_WINDOW | CREATE_SUSPENDED,
                                                nullptr, helper.parent_path().c_str(), &startup, &process);
      if (!created) return {.error = "The broker could not start the native HDR activator."};
      unique_handle process_handle {process.hProcess};
      unique_handle thread_handle {process.hThread};
      const auto abort_helper = [&](const DWORD exit_code, const bool force_job = false) {
        if (!process_handle) return false;
        const bool terminated = TerminateProcess(process_handle.get(), exit_code) != FALSE;
        const bool exited = WaitForSingleObject(process_handle.get(), 2000) == WAIT_OBJECT_0;
        if (!force_job && terminated && exited) return true;
        worker_poisoned = true;
        if (!TerminateJobObject(worker_job, exit_code)) return false;
        const bool helper_exited = WaitForSingleObject(process_handle.get(), INFINITE) == WAIT_OBJECT_0;
        const bool worker_exited = !worker_process || WaitForSingleObject(worker_process, INFINITE) == WAIT_OBJECT_0;
        return helper_exited && worker_exited;
      };
      if (!AssignProcessToJobObject(worker_job, process_handle.get())) {
        (void) abort_helper(ERROR_PROCESS_ABORTED);
        return {.error = "The broker could not contain the native HDR activator in the seat job."};
      }
      if (ResumeThread(thread_handle.get()) == static_cast<DWORD>(-1)) {
        (void) abort_helper(ERROR_PROCESS_ABORTED);
        return {.error = "The broker could not resume the native HDR activator."};
      }
      thread_handle.reset();

      unique_handle connected_event {CreateEventW(nullptr, TRUE, FALSE, nullptr)};
      if (!connected_event) {
        (void) abort_helper(ERROR_PROCESS_ABORTED);
        return {.error = "The broker could not create the native HDR connection event."};
      }
      const auto cancel_overlapped = [&](OVERLAPPED &operation) {
        (void) CancelIoEx(capability_pipe.get(), &operation);
        DWORD transferred = 0;
        // Keep the OVERLAPPED storage alive until cancellation has completed;
        // it is stack-owned and must not outlive the pipe I/O request.
        (void) GetOverlappedResult(capability_pipe.get(), &operation, &transferred, TRUE);
      };
      OVERLAPPED connected_overlapped {.hEvent = connected_event.get()};
      bool connect_pending = false;
      bool connected = ConnectNamedPipe(capability_pipe.get(), &connected_overlapped) != FALSE;
      if (!connected) {
        const DWORD connect_error = GetLastError();
        if (connect_error == ERROR_PIPE_CONNECTED) {
          connected = true;
        } else if (connect_error != ERROR_IO_PENDING) {
          (void) abort_helper(ERROR_PIPE_NOT_CONNECTED);
          return {.error = "The native HDR activator could not connect to its broker endpoint."};
        } else {
          connect_pending = true;
        }
      }
      if (!connected) {
        const HANDLE waits[] = {connected_event.get(), process_handle.get()};
        const DWORD waited = WaitForMultipleObjects(2, waits, FALSE, 5000);
        DWORD transferred = 0;
        if (waited == WAIT_OBJECT_0) {
          connect_pending = false;
          connected = GetOverlappedResult(capability_pipe.get(), &connected_overlapped, &transferred, FALSE) != FALSE;
        }
      }
      ULONG client_pid = 0;
      if (!connected || !GetNamedPipeClientProcessId(capability_pipe.get(), &client_pid) || client_pid != process.dwProcessId) {
        if (connect_pending) cancel_overlapped(connected_overlapped);
        (void) abort_helper(ERROR_PIPE_NOT_CONNECTED);
        return {.error = "The native HDR endpoint peer was not the broker-created helper."};
      }
      unique_handle write_event {CreateEventW(nullptr, TRUE, FALSE, nullptr)};
      if (!write_event) {
        (void) abort_helper(ERROR_PROCESS_ABORTED);
        return {.error = "The broker could not create the native HDR transfer event."};
      }
      OVERLAPPED write_overlapped {.hEvent = write_event.get()};
      DWORD written = 0;
      bool write_pending = false;
      bool write_complete = WriteFile(capability_pipe.get(), &capability, sizeof(capability), &written, &write_overlapped) != FALSE;
      if (!write_complete && GetLastError() == ERROR_IO_PENDING) {
        write_pending = true;
        if (WaitForSingleObject(write_event.get(), 2000) == WAIT_OBJECT_0) {
          write_pending = false;
          write_complete = GetOverlappedResult(capability_pipe.get(), &write_overlapped, &written, FALSE) != FALSE;
        }
      }
      if (!write_complete || written != sizeof(capability)) {
        if (write_pending) cancel_overlapped(write_overlapped);
        (void) abort_helper(ERROR_WRITE_FAULT);
        return {.error = "The broker could not transfer the native HDR one-shot capability."};
      }
      if (WaitForSingleObject(process_handle.get(), 15000) != WAIT_OBJECT_0) {
        (void) abort_helper(ERROR_TIMEOUT, true);
        return {.error = "Native HDR activation exceeded its 15-second broker watchdog."};
      }
      DWORD exit_code = ERROR_GEN_FAILURE;
      if (!GetExitCodeProcess(process_handle.get(), &exit_code)) {
        return {.error = "The broker could not read the native HDR activator result."};
      }
      if (exit_code != 0) return {.exit_code = exit_code, .error = "The native HDR activator rejected the broker-owned target."};
      return {.accepted = true};
    }

    std::wstring wide(const std::string_view value) {
      if (value.empty()) return {};
      const int size = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), nullptr, 0);
      if (size <= 0) return {};
      std::wstring result(size, L'\0');
      if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), result.data(), size) != size) return {};
      return result;
    }

    bool token_matches_resource(HANDLE token, const provider_resource_t &resource) {
      DWORD session = 0, size = sizeof(session);
      if (!GetTokenInformation(token, TokenSessionId, &session, size, &size) || session != resource.windows_session_id || resource.user_sid.empty()) return false;
      GetTokenInformation(token, TokenUser, nullptr, 0, &size);
      if (!size) return false;
      auto user = std::make_unique<std::uint8_t[]>(size);
      if (!GetTokenInformation(token, TokenUser, user.get(), size, &size)) return false;
      LPWSTR sid = nullptr;
      if (!ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER *>(user.get())->User.Sid, &sid) || !sid) return false;
      const std::wstring expected = wide(resource.user_sid);
      const bool matches = _wcsicmp(sid, expected.c_str()) == 0;
      LocalFree(sid);
      return matches;
    }

    bool process_matches_resource(HANDLE process, const provider_resource_t &resource) {
      HANDLE raw_token = nullptr;
      if (!OpenProcessToken(process, TOKEN_QUERY, &raw_token)) return false;
      const bool matches = token_matches_resource(raw_token, resource);
      CloseHandle(raw_token);
      return matches;
    }

    std::uint64_t process_creation_time(HANDLE process) {
      FILETIME created {}, exited {}, kernel {}, user {};
      if (!GetProcessTimes(process, &created, &exited, &kernel, &user)) return 0;
      ULARGE_INTEGER value {};
      value.LowPart = created.dwLowDateTime;
      value.HighPart = created.dwHighDateTime;
      return value.QuadPart;
    }

    std::wstring normalized_path(const std::filesystem::path &path) {
      const DWORD needed = GetFullPathNameW(path.c_str(), 0, nullptr, nullptr);
      if (!needed) return {};
      std::wstring result(needed, L'\0');
      const DWORD written = GetFullPathNameW(path.c_str(), needed, result.data(), nullptr);
      if (!written || written >= needed) return {};
      result.resize(written);
      while (result.size() > 3 && (result.back() == L'\\' || result.back() == L'/')) result.pop_back();
      return result;
    }

    std::wstring final_path(HANDLE directory) {
      const DWORD needed = GetFinalPathNameByHandleW(directory, nullptr, 0, FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
      if (!needed) return {};
      std::wstring result(needed, L'\0');
      const DWORD written = GetFinalPathNameByHandleW(directory, result.data(), needed, FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
      if (!written || written >= needed) return {};
      result.resize(written);
      constexpr std::wstring_view prefix = L"\\\\?\\";
      if (result.starts_with(prefix)) result.erase(0, prefix.size());
      return normalized_path(result);
    }

    bool equal_path(const std::filesystem::path &left, const std::filesystem::path &right) {
      const auto lhs = normalized_path(left);
      const auto rhs = normalized_path(right);
      return !lhs.empty() && !rhs.empty() && _wcsicmp(lhs.c_str(), rhs.c_str()) == 0;
    }

    bool well_known_sid(const PSID sid, const WELL_KNOWN_SID_TYPE type) {
      return sid && IsWellKnownSid(sid, type) != FALSE;
    }

    bool trusted_directory_security(HANDLE directory, const bool require_system_owner, PSID allowed_user = nullptr,
                                    const bool allow_inherited_creation_rights = false) {
      PSID owner = nullptr;
      PACL dacl = nullptr;
      PSECURITY_DESCRIPTOR descriptor = nullptr;
      const DWORD status = GetSecurityInfo(directory, SE_FILE_OBJECT, OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION,
                                           &owner, nullptr, &dacl, nullptr, &descriptor);
      if (status != ERROR_SUCCESS || !descriptor || !owner || !dacl) {
        if (descriptor) LocalFree(descriptor);
        return false;
      }
      bool trusted = well_known_sid(owner, WinLocalSystemSid) ||
                     (!require_system_owner && well_known_sid(owner, WinBuiltinAdministratorsSid));
      ACL_SIZE_INFORMATION information {};
      if (trusted && !GetAclInformation(dacl, &information, sizeof(information), AclSizeInformation)) trusted = false;
      constexpr ACCESS_MASK identity_dangerous = DELETE | FILE_DELETE_CHILD | WRITE_DAC | WRITE_OWNER | GENERIC_ALL;
      constexpr ACCESS_MASK creation_dangerous = GENERIC_WRITE | FILE_ADD_FILE | FILE_ADD_SUBDIRECTORY | FILE_WRITE_EA | FILE_WRITE_ATTRIBUTES;
      const ACCESS_MASK dangerous = identity_dangerous | (allow_inherited_creation_rights ? 0 : creation_dangerous);
      for (DWORD index = 0; trusted && index < information.AceCount; ++index) {
        void *raw_ace = nullptr;
        if (!GetAce(dacl, index, &raw_ace) || !raw_ace) { trusted = false; break; }
        const auto *header = static_cast<ACE_HEADER *>(raw_ace);
        if ((header->AceFlags & INHERIT_ONLY_ACE) || header->AceType == ACCESS_DENIED_ACE_TYPE) continue;
        if (header->AceType != ACCESS_ALLOWED_ACE_TYPE) { trusted = false; break; }
        const auto *ace = static_cast<ACCESS_ALLOWED_ACE *>(raw_ace);
        const auto sid = const_cast<DWORD *>(&ace->SidStart);
        if ((ace->Mask & dangerous) && !well_known_sid(sid, WinLocalSystemSid) &&
            !well_known_sid(sid, WinBuiltinAdministratorsSid) && (!allowed_user || !EqualSid(sid, allowed_user))) {
          trusted = false;
        }
      }
      LocalFree(descriptor);
      return trusted;
    }

    unique_handle open_safe_directory(const std::filesystem::path &path, const bool require_system_owner, PSID allowed_user = nullptr,
                                      const bool allow_inherited_creation_rights = false) {
      HANDLE raw = CreateFileW(path.c_str(), FILE_READ_ATTRIBUTES | READ_CONTROL | WRITE_DAC | WRITE_OWNER,
                               FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                               FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
      if (raw == INVALID_HANDLE_VALUE) return {};
      unique_handle directory {raw};
      FILE_ATTRIBUTE_TAG_INFO attributes {};
      if (!GetFileInformationByHandleEx(directory.get(), FileAttributeTagInfo, &attributes, sizeof(attributes)) ||
          !(attributes.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) || (attributes.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) ||
          !equal_path(final_path(directory.get()), path) ||
          !trusted_directory_security(directory.get(), require_system_owner, allowed_user, allow_inherited_creation_rights)) {
        return {};
      }
      return directory;
    }

    bool create_protected_system_directory(const std::filesystem::path &path, bool &created) {
      std::array<std::uint8_t, SECURITY_MAX_SID_SIZE> system_buffer {}, admin_buffer {};
      DWORD system_size = static_cast<DWORD>(system_buffer.size()), admin_size = static_cast<DWORD>(admin_buffer.size());
      if (!CreateWellKnownSid(WinLocalSystemSid, nullptr, system_buffer.data(), &system_size) ||
          !CreateWellKnownSid(WinBuiltinAdministratorsSid, nullptr, admin_buffer.data(), &admin_size)) return false;
      std::array<EXPLICIT_ACCESSW, 2> entries {};
      for (std::size_t index = 0; index < entries.size(); ++index) {
        entries[index].grfAccessPermissions = FILE_ALL_ACCESS;
        entries[index].grfAccessMode = SET_ACCESS;
        entries[index].grfInheritance = SUB_CONTAINERS_AND_OBJECTS_INHERIT;
        entries[index].Trustee.TrusteeForm = TRUSTEE_IS_SID;
        entries[index].Trustee.TrusteeType = TRUSTEE_IS_WELL_KNOWN_GROUP;
        entries[index].Trustee.ptstrName = static_cast<LPWSTR>(index == 0 ? static_cast<void *>(system_buffer.data()) : static_cast<void *>(admin_buffer.data()));
      }
      PACL acl = nullptr;
      if (SetEntriesInAclW(static_cast<ULONG>(entries.size()), entries.data(), nullptr, &acl) != ERROR_SUCCESS || !acl) {
        if (acl) LocalFree(acl);
        return false;
      }
      SECURITY_DESCRIPTOR descriptor {};
      const bool descriptor_ready = InitializeSecurityDescriptor(&descriptor, SECURITY_DESCRIPTOR_REVISION) &&
        SetSecurityDescriptorOwner(&descriptor, system_buffer.data(), FALSE) &&
        SetSecurityDescriptorDacl(&descriptor, TRUE, acl, FALSE) &&
        SetSecurityDescriptorControl(&descriptor, SE_DACL_PROTECTED, SE_DACL_PROTECTED);
      SECURITY_ATTRIBUTES attributes {sizeof(attributes), descriptor_ready ? &descriptor : nullptr, FALSE};
      created = descriptor_ready && CreateDirectoryW(path.c_str(), &attributes) != FALSE;
      const DWORD error = GetLastError();
      LocalFree(acl);
      return descriptor_ready && (created || error == ERROR_ALREADY_EXISTS);
    }

    unique_handle create_safe_directory(const std::filesystem::path &path, const bool require_system_owner, PSID allowed_user = nullptr) {
      bool created = false;
      if (!create_protected_system_directory(path, created)) return {};
      return open_safe_directory(path, require_system_owner, allowed_user);
    }

    bool set_managed_directory_acl(HANDLE directory, PSID user, const ACCESS_MASK user_access, const bool inherit_user) {
      std::array<std::uint8_t, SECURITY_MAX_SID_SIZE> system_buffer {}, admin_buffer {};
      DWORD system_size = static_cast<DWORD>(system_buffer.size()), admin_size = static_cast<DWORD>(admin_buffer.size());
      if (!CreateWellKnownSid(WinLocalSystemSid, nullptr, system_buffer.data(), &system_size) ||
          !CreateWellKnownSid(WinBuiltinAdministratorsSid, nullptr, admin_buffer.data(), &admin_size)) return false;
      std::array<EXPLICIT_ACCESSW, 3> entries {};
      const auto initialize = [](EXPLICIT_ACCESSW &entry, PSID sid, const ACCESS_MASK access, const DWORD inheritance) {
        entry.grfAccessPermissions = access;
        entry.grfAccessMode = SET_ACCESS;
        entry.grfInheritance = inheritance;
        entry.Trustee.TrusteeForm = TRUSTEE_IS_SID;
        entry.Trustee.TrusteeType = TRUSTEE_IS_WELL_KNOWN_GROUP;
        entry.Trustee.ptstrName = static_cast<LPWSTR>(sid);
      };
      initialize(entries[0], system_buffer.data(), FILE_ALL_ACCESS, SUB_CONTAINERS_AND_OBJECTS_INHERIT);
      initialize(entries[1], admin_buffer.data(), FILE_ALL_ACCESS, SUB_CONTAINERS_AND_OBJECTS_INHERIT);
      initialize(entries[2], user, user_access, inherit_user ? SUB_CONTAINERS_AND_OBJECTS_INHERIT : NO_INHERITANCE);
      entries[2].Trustee.TrusteeType = TRUSTEE_IS_USER;
      PACL acl = nullptr;
      const DWORD built = SetEntriesInAclW(static_cast<ULONG>(entries.size()), entries.data(), nullptr, &acl);
      if (built != ERROR_SUCCESS || !acl) { if (acl) LocalFree(acl); return false; }
      const DWORD applied = SetSecurityInfo(directory, SE_FILE_OBJECT,
        DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION, nullptr, nullptr, acl, nullptr);
      LocalFree(acl);
      return applied == ERROR_SUCCESS;
    }

    std::optional<std::filesystem::path> program_data_directory() {
      PWSTR raw = nullptr;
      if (FAILED(SHGetKnownFolderPath(FOLDERID_ProgramData, KF_FLAG_DEFAULT, nullptr, &raw)) || !raw) return std::nullopt;
      std::filesystem::path result {raw};
      CoTaskMemFree(raw);
      return result;
    }

    bool grant_directory(const std::filesystem::path &path, const std::wstring &sid_text) {
      const auto program_data = program_data_directory();
      PSID user = nullptr;
      if (!program_data || !ConvertStringSidToSidW(sid_text.c_str(), &user) || !user) return false;
      const auto user_guard = std::unique_ptr<void, decltype(&LocalFree)> {user, LocalFree};
      const auto seat_root = path.parent_path();
      const auto seats_root = seat_root.parent_path();
      const auto leaf = path.filename().wstring();
      const auto seat_name = seat_root.filename().wstring();
      const auto root_name = seats_root.filename().wstring();
      constexpr std::wstring_view root_prefix = L"VibeshineTerminalSeats-";
      const bool valid_leaf = leaf == L"config" || leaf == L"state" || leaf == L"logs";
      const bool valid_seat = !seat_name.empty() && std::all_of(seat_name.begin(), seat_name.end(), [](const wchar_t ch) {
        return ch >= L'0' && ch <= L'9';
      });
      const bool valid_root = root_name.size() == root_prefix.size() + 32 && root_name.starts_with(root_prefix) &&
        std::all_of(root_name.begin() + static_cast<std::ptrdiff_t>(root_prefix.size()), root_name.end(), [](const wchar_t ch) {
          return (ch >= L'0' && ch <= L'9') || (ch >= L'a' && ch <= L'f');
        });
      if (!valid_leaf || !valid_seat || !valid_root || !equal_path(seats_root.parent_path(), *program_data)) return false;

      // ProgramData is the OS-selected, SYSTEM-owned anchor. Standard users may
      // create direct children there, so precreation is handled by the exact
      // SYSTEM-owner check on the randomized VibeshineTerminalSeats-* root
      // rather than trusting an inherited DACL.
      auto known_root = open_safe_directory(*program_data, false, nullptr, true);
      if (!known_root) return false;
      auto seats = create_safe_directory(seats_root, true);
      if (!seats || !set_managed_directory_acl(seats.get(), user, FILE_TRAVERSE | SYNCHRONIZE, false)) return false;
      auto seat_directory = create_safe_directory(seat_root, true);
      if (!seat_directory || !set_managed_directory_acl(seat_directory.get(), user, FILE_TRAVERSE | SYNCHRONIZE, false)) return false;
      auto leaf_directory = create_safe_directory(path, true, user);
      return leaf_directory && set_managed_directory_acl(leaf_directory.get(), user, FILE_ALL_ACCESS, true);
    }

    std::vector<wchar_t> worker_environment(HANDLE token, const std::string &pipe_name,
                                            const std::string &display_pipe_name,
                                            const provider_resource_t &resource, const std::uint64_t generation,
                                            const std::string &helper_capability,
                                            const bool steam_offline_isolation) {
      LPVOID raw = nullptr;
      if (!CreateEnvironmentBlock(&raw, token, FALSE) || !raw) return {};
      std::vector<std::wstring> entries;
      const auto *cursor = static_cast<const wchar_t *>(raw);
      while (*cursor) { entries.emplace_back(cursor); cursor += entries.back().size() + 1; }
      DestroyEnvironmentBlock(raw);
      const auto replace = [&](std::wstring name, std::wstring value) {
        const std::wstring prefix = name + L"=";
        entries.erase(std::remove_if(entries.begin(), entries.end(), [&](const auto &entry) {
          return entry.size() >= prefix.size() && _wcsnicmp(entry.c_str(), prefix.c_str(), prefix.size()) == 0;
        }), entries.end());
        entries.push_back(prefix + std::move(value));
      };
      replace(L"VIBESHINE_TERMINAL_WORKER_PIPE", wide(pipe_name));
      replace(L"VIBESHINE_TERMINAL_BROKER_PID", std::to_wstring(GetCurrentProcessId()));
      replace(L"VIBESHINE_TERMINAL_DISPLAY_PIPE", wide(display_pipe_name));
      // These are immutable admission facts inherited by the worker and its
      // helper. The helper must never reclassify itself from WTS/console state;
      // a per-launch capability also prevents same-account sibling sessions
      // from guessing the endpoint name.
      replace(L"VIBESHINE_TERMINAL_SESSION_ID", std::to_wstring(resource.windows_session_id));
      replace(L"VIBESHINE_TERMINAL_GENERATION", std::to_wstring(generation));
      replace(L"VIBESHINE_TERMINAL_HELPER_CAPABILITY", wide(helper_capability));
      replace(L"VIBESHINE_STEAM_OFFLINE_ISOLATION", steam_offline_isolation ? L"1" : L"0");
      replace(L"VIBESHINE_STEAM_OFFLINE_SEAT_ID", wide(resource.seat_id));
      std::sort(entries.begin(), entries.end(), [](const auto &left, const auto &right) { return _wcsicmp(left.c_str(), right.c_str()) < 0; });
      std::size_t chars = 1;
      for (const auto &entry : entries) chars += entry.size() + 1;
      std::vector<wchar_t> result(chars, L'\0');
      auto *out = result.data();
      for (const auto &entry : entries) { std::copy(entry.begin(), entry.end(), out); out += entry.size(); *out++ = L'\0'; }
      *out = L'\0';
      return result;
    }

    protocol::request_t admission_request(const worker_request_t &request) {
      return {.operation = request.ticket.operation, .client_uuid = request.admission.client_uuid,
              .generation = request.admission.generation, .launch_id = request.admission.launch_id,
              .launch_payload = request.launch_payload, .ticket = request.ticket};
    }

    std::optional<route_t> transact_admission(platf::dxgi::INamedPipe &pipe, const worker_request_t &request,
                                              HANDLE worker_process, HANDLE worker_job,
                                              std::optional<terminal_session::hdr::target_binding_t> &target_binding,
                                              bool &worker_poisoned, const int timeout_ms, std::string &error) {
      auto admission = admission_request(request);
      if (!request.ticket_validator || !request.ticket_validator(admission)) {
        error = "Worker admission ticket validator is unavailable or rejected the ticket.";
        return std::nullopt;
      }
      const auto bytes = protocol::encode(admission);
      if (bytes.empty() || !pipe.send(bytes, 2000)) { error = "Worker admission transfer failed."; return std::nullopt; }
      std::array<std::uint8_t, protocol::max_message_size> response_bytes {};
      while (true) {
        std::size_t size = 0;
        if (pipe.receive(response_bytes, size, timeout_ms) != platf::dxgi::PipeResult::Success) {
          error = "Worker readiness timed out.";
          return std::nullopt;
        }
        const auto frame = std::span<const std::uint8_t> {response_bytes.data(), size};
        if (const auto response = protocol::decode_response(frame)) {
          if (!response->accepted || response->client_uuid != request.admission.client_uuid ||
              response->generation != request.admission.generation || response->launch_id != request.admission.launch_id ||
              response->rtsp_port != request.resource.rtsp_port || response->control_port != request.resource.control_port ||
              response->video_port != request.resource.video_port || response->audio_port != request.resource.audio_port) {
            error = !response->error.empty() ? response->error : "Worker did not prove the reserved route.";
            return std::nullopt;
          }
          return route_t {.accepted = true, .ready = true, .rtsp_port = response->rtsp_port, .control_port = response->control_port,
                          .video_port = response->video_port, .audio_port = response->audio_port};
        }

        const auto hdr_request = protocol::decode_request(frame);
        const bool ticket_matches = hdr_request && hdr_request->ticket.operation == request.ticket.operation &&
          hdr_request->ticket.release == request.ticket.release && hdr_request->ticket.client_uuid == request.ticket.client_uuid &&
          hdr_request->ticket.generation == request.ticket.generation && hdr_request->ticket.launch_id == request.ticket.launch_id &&
          hdr_request->ticket.nonce == request.ticket.nonce;
        if (!hdr_request || hdr_request->operation != protocol::opcode::worker_hdr_activate ||
            hdr_request->client_uuid != request.admission.client_uuid || hdr_request->generation != request.admission.generation ||
            hdr_request->launch_id != request.admission.launch_id || !ticket_matches ||
            hdr_request->launch_payload.size() != sizeof(terminal_session::hdr::activation_capability_t)) {
          error = "Worker returned an unexpected frame before route readiness.";
          return std::nullopt;
        }
        terminal_session::hdr::activation_capability_t capability;
        std::memcpy(&capability, hdr_request->launch_payload.data(), sizeof(capability));
        if (capability.magic != terminal_session::hdr::activation_capability_magic ||
            capability.size != sizeof(capability) || capability.version != terminal_session::hdr::activation_capability_version ||
            capability.parent_pid != 0 || capability.reserved != 0 || capability.session_id != request.resource.windows_session_id) {
          error = "The worker HDR capability was malformed or not bound to its seat.";
          return std::nullopt;
        }
        const auto presented_target = terminal_session::hdr::binding_from_capability(capability);
        if (!target_binding) {
          target_binding = presented_target;
        } else if (*target_binding != presented_target) {
          error = "The worker HDR target changed after broker attestation.";
          return std::nullopt;
        }
        const auto activated = activate_hdr_helper(reinterpret_cast<HANDLE>(request.resource.launch_token), worker_process, worker_job,
                                                   request.resource, *target_binding, capability, worker_poisoned);
        protocol::response_t hdr_response {
          .accepted = activated.accepted,
          .reason = activated.accepted ? protocol::reject_reason::malformed : protocol::reject_reason::worker_unavailable,
          .error = activated.error,
          .client_uuid = request.admission.client_uuid,
          .generation = request.admission.generation,
          .launch_id = request.admission.launch_id,
          .owner_launch_id = activated.exit_code,
        };
        const auto hdr_bytes = protocol::encode(hdr_response);
        if (hdr_bytes.empty() || !pipe.send(hdr_bytes, 2000)) {
          error = "The broker could not return the native HDR activation result.";
          return std::nullopt;
        }
      }
    }
  }
#endif

#ifdef _WIN32
  bool process_t::validate_display_client(platf::dxgi::INamedPipe &pipe) const {
    if (!process_ || !job_ || !process_matches_resource(static_cast<HANDLE>(process_), resource_)) return false;
    DWORD pid = 0;
    std::uint64_t creation = 0;
    std::wstring sid;
    if (!pipe.get_client_process_id(pid) || !pipe.get_client_process_creation_time(creation) ||
        !pipe.get_client_user_sid_string(sid) || pid == 0 || creation == 0 || sid.empty()) return false;
    if (std::string {sid.begin(), sid.end()} != resource_.user_sid) return false;
    DWORD session = 0;
    if (!ProcessIdToSessionId(pid, &session) || session != resource_.windows_session_id) return false;
    winrt::handle client {OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid)};
    if (!client || process_creation_time(client.get()) != creation) return false;
    BOOL in_job = FALSE;
    if (!IsProcessInJob(client.get(), static_cast<HANDLE>(job_), &in_job) || !in_job) return false;
    wchar_t image[MAX_PATH] {};
    DWORD length = _countof(image);
    if (!QueryFullProcessImageNameW(client.get(), 0, image, &length) || length == 0) return false;
    const std::wstring canonical = canonical_image(std::wstring {image, length});
    const auto expected = canonical_image((service_directory() / L"sunshine_display_helper.exe").wstring());
    return canonical == expected;
  }

  void process_t::run_display_broker() {
    std::uint64_t last_request_id = 0;
    DWORD bound_client_pid = 0;
    std::uint64_t bound_client_creation = 0;
    auto named_factory = std::make_unique<platf::dxgi::NamedPipeFactory>();
    const std::wstring user_sid = wide(resource_.user_sid);
    named_factory->set_security_descriptor_builder([user_sid](SECURITY_DESCRIPTOR &desc, PACL *out_pacl) {
      const std::wstring sddl = L"D:P(A;;GA;;;SY)(A;;GRGW;;;" + user_sid + L")";
      PSECURITY_DESCRIPTOR raw = nullptr;
      if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(), SDDL_REVISION_1, &raw, nullptr) || !raw) return false;
      BOOL dacl_present = FALSE;
      BOOL dacl_defaulted = FALSE;
      PACL dacl = nullptr;
      if (!GetSecurityDescriptorDacl(raw, &dacl_present, &dacl, &dacl_defaulted) || !dacl_present || !dacl ||
          !InitializeSecurityDescriptor(&desc, SECURITY_DESCRIPTOR_REVISION) ||
          !SetSecurityDescriptorDacl(&desc, TRUE, dacl, FALSE)) {
        LocalFree(raw);
        return false;
      }
      *out_pacl = reinterpret_cast<PACL>(raw);
      return true;
    });
    platf::dxgi::FramedPipeFactory factory {std::move(named_factory)};
    while (display_broker_running_.load(std::memory_order_acquire)) {
      auto pipe = factory.create_server(display_pipe_name_);
      if (!pipe) break;
      pipe->wait_for_client_connection(250);
      if (!pipe->is_connected()) continue;
      DWORD client_pid = 0;
      std::uint64_t client_creation = 0;
      if (!validate_display_client(*pipe) || !pipe->get_client_process_id(client_pid) ||
          !pipe->get_client_process_creation_time(client_creation)) {
        pipe->disconnect();
        continue;
      }
      if (client_pid != bound_client_pid || client_creation != bound_client_creation) {
        bound_client_pid = client_pid;
        bound_client_creation = client_creation;
        last_request_id = 0;
      }
      std::array<std::uint8_t, terminal_session::display::max_message_size> bytes {};
      std::size_t size = 0;
      if (pipe->receive(bytes, size, 5000) != platf::dxgi::PipeResult::Success) {
        pipe->disconnect();
        continue;
      }
      terminal_session::display::request_t request {};
      terminal_session::display::response_t response {};
      if (!terminal_session::display::decode(bytes.data(), size, request) ||
          !terminal_session::display::valid_request(request)) {
        response.result = static_cast<std::uint8_t>(terminal_session::display::result::malformed);
        (void) pipe->send(std::span<const std::uint8_t> {
          reinterpret_cast<const std::uint8_t *>(&response), sizeof(response)}, 1000);
        pipe->disconnect();
        continue;
      }
      response.operation = request.operation;
      response.generation = request.generation;
      response.request_id = request.request_id;
      if (request.generation != generation_ || request.request_id <= last_request_id) {
        response.result = static_cast<std::uint8_t>(request.generation != generation_ ?
          terminal_session::display::result::stale : terminal_session::display::result::stale);
      } else {
        last_request_id = request.request_id;
        auto opened = virtual_display::driver::open_remote_control_device_for_session(resource_.windows_session_id);
        if (!opened.ok()) {
          response.result = static_cast<std::uint8_t>(terminal_session::display::result::unavailable);
          response.native_error = opened.native_error;
        } else {
          virtual_display::driver::ControlClient client {*opened.transport};
          const auto queried = client.query_display_state();
          if (!queried.ok() || queried.value.entry_count != 1 || queried.value.entries[0].display_id == 0 ||
              queried.value.entries[0].kind != virtual_display::driver::kDisplayStateKindTemporary) {
            response.result = static_cast<std::uint8_t>(queried.ok() ? terminal_session::display::result::invalid : terminal_session::display::result::unavailable);
            response.native_error = queried.native_error;
          } else {
            const auto &state = queried.value.entries[0];
            response.width = state.width;
            response.height = state.height;
            response.refresh_rate_millihz = state.refresh_rate_millihz;
            response.hdr_enabled = (state.flags & virtual_display::driver::kDisplayStateFlagHdrEnabled) != 0 ? 1u : 0u;
            response.display_id = state.display_id;
            if (request.operation == static_cast<std::uint8_t>(terminal_session::display::operation::set_mode)) {
              const auto changed = client.set_display_mode(virtual_display::driver::SetDisplayModeRequest {
                .display_id = state.display_id,
                .width = request.width,
                .height = request.height,
                .refresh_rate_millihz = request.refresh_rate_millihz,
              });
              response.result = static_cast<std::uint8_t>(changed.ok() ? terminal_session::display::result::success : terminal_session::display::result::unavailable);
              response.native_error = changed.native_error;
            } else if (request.operation == static_cast<std::uint8_t>(terminal_session::display::operation::set_hdr)) {
              const auto changed = client.set_display_hdr_state(virtual_display::driver::SetDisplayHdrStateRequest {
                .display_id = state.display_id,
                .enabled = request.hdr_enabled,
              });
              response.result = static_cast<std::uint8_t>(changed.ok() ? terminal_session::display::result::success : terminal_session::display::result::unavailable);
              response.native_error = changed.native_error;
            } else if (request.operation == static_cast<std::uint8_t>(terminal_session::display::operation::seal_snapshot) ||
                       request.operation == static_cast<std::uint8_t>(terminal_session::display::operation::commit_snapshot) ||
                       request.operation == static_cast<std::uint8_t>(terminal_session::display::operation::verify_snapshot)) {
              auto &tier = snapshot_tiers_[request.snapshot_tier];
              const auto operation = static_cast<terminal_session::display::operation>(request.operation);
              const auto make_auth_data = [&](const std::uint64_t sequence, const std::array<std::uint8_t, 32> &digest) {
                return snapshot_auth_data {
                  .session_id = resource_.windows_session_id,
                  .generation = generation_,
                  .display_id = state.display_id,
                  .tier = request.snapshot_tier,
                  .sequence = sequence,
                  .digest = digest,
                };
              };
              const auto tag_for = [&](const std::uint64_t sequence,
                                       const std::array<std::uint8_t, 32> &digest,
                                       std::array<std::uint8_t, 32> &tag) {
                const auto auth_data = make_auth_data(sequence, digest);
                return snapshot_auth_tag(snapshot_auth_key_, auth_data, tag);
              };
              const auto matches = [&](const process_t::snapshot_record &record) {
                std::array<std::uint8_t, 32> expected_tag {};
                return record.display_id == state.display_id && record.sequence != 0 &&
                       record.sequence == request.snapshot_sequence && record.digest == request.snapshot_digest &&
                       CRYPTO_memcmp(record.tag.data(), request.snapshot_tag.data(), record.tag.size()) == 0 &&
                       tag_for(record.sequence, record.digest, expected_tag) &&
                       CRYPTO_memcmp(expected_tag.data(), record.tag.data(), expected_tag.size()) == 0;
              };
              if (operation == terminal_session::display::operation::seal_snapshot) {
                if (tier.next_sequence == 0 || tier.next_sequence == (std::numeric_limits<std::uint64_t>::max)()) {
                  response.result = static_cast<std::uint8_t>(terminal_session::display::result::unavailable);
                } else {
                  const auto sequence = tier.next_sequence++;
                  std::array<std::uint8_t, 32> tag {};
                  if (!tag_for(sequence, request.snapshot_digest, tag)) {
                    response.result = static_cast<std::uint8_t>(terminal_session::display::result::unavailable);
                  } else {
                    tier.pending = process_t::snapshot_record {
                      .sequence = sequence,
                      .display_id = state.display_id,
                      .digest = request.snapshot_digest,
                      .tag = tag,
                    };
                    response.snapshot_sequence = sequence;
                    response.snapshot_tag = tag;
                    response.result = static_cast<std::uint8_t>(terminal_session::display::result::success);
                  }
                }
              } else if (operation == terminal_session::display::operation::commit_snapshot) {
                if (!tier.pending || !matches(*tier.pending)) {
                  response.result = static_cast<std::uint8_t>(terminal_session::display::result::stale);
                } else {
                  tier.committed = tier.pending;
                  tier.pending.reset();
                  response.snapshot_sequence = tier.committed->sequence;
                  response.snapshot_tag = tier.committed->tag;
                  response.result = static_cast<std::uint8_t>(terminal_session::display::result::success);
                }
              } else {
                const snapshot_record *verified = nullptr;
                if (tier.committed && matches(*tier.committed)) {
                  verified = &*tier.committed;
                } else if (tier.pending && matches(*tier.pending)) {
                  // A published pending envelope may be recovered after a
                  // helper restart only when it is byte-for-byte the broker's
                  // pending record. Promote that exact record; never bless
                  // caller data.
                  tier.committed = tier.pending;
                  tier.pending.reset();
                  verified = &*tier.committed;
                }
                if (!verified) {
                  response.result = static_cast<std::uint8_t>(terminal_session::display::result::stale);
                } else {
                  response.snapshot_sequence = verified->sequence;
                  response.snapshot_tag = verified->tag;
                  response.result = static_cast<std::uint8_t>(terminal_session::display::result::success);
                }
              }
            } else {
              response.result = static_cast<std::uint8_t>(terminal_session::display::result::success);
            }
          }
        }
      }
      (void) pipe->send(std::span<const std::uint8_t> {
        reinterpret_cast<const std::uint8_t *>(&response), sizeof(response)}, 1000);
      pipe->disconnect();
    }
  }
#endif

  process_t::~process_t() {
#ifdef _WIN32
    if (!stop({})) {
      std::lock_guard lock {poisoned_workers_mutex()};
      poisoned_workers().push_back({
        reinterpret_cast<HANDLE>(process_),
        reinterpret_cast<HANDLE>(job_),
        std::move(pipe_),
        std::move(steam_offline_registration_)});
      process_ = nullptr;
      job_ = nullptr;
      pid_ = 0;
      cleanup_pending_ = true;
    }
#else
    (void) stop({});
#endif
  }

  bool process_t::cleanup_needed() const noexcept {
#ifdef _WIN32
    return process_ != nullptr || job_ != nullptr || pipe_ != nullptr || display_broker_running_.load() ||
           steam_offline_registration_.active() || cleanup_pending_;
#else
    return false;
#endif
  }

  std::optional<route_t> process_t::start(const worker_request_t &request, std::string &error) {
#ifndef _WIN32
    error = "Private worker process is Windows-only.";
    return std::nullopt;
#else
    if (cleanup_needed()) { error = "A previous private worker or Steam offline registration still owns resources; teardown must complete first."; return std::nullopt; }
    pipe_name_ = unique_pipe();
    const auto helper_capability = platf::dxgi::generate_guid();
    display_pipe_name_ = helper_capability.empty() ? std::string {} : "VibeshineTerminalDisplay-" + helper_capability;
    if (pipe_name_.empty() || display_pipe_name_.empty() || request.resource.launch_token == 0 || request.resource.windows_session_id == 0 ||
        request.resource.desktop_name.empty() || request.resource.user_sid.empty()) {
      error = "Provider did not supply a complete token/session/user/desktop launch contract.";
      return std::nullopt;
    }
    HANDLE launch_token = reinterpret_cast<HANDLE>(request.resource.launch_token);
    if (!token_matches_resource(launch_token, request.resource)) { error = "Provider launch token does not match the admitted session/user."; return std::nullopt; }

    const auto contract = make_contract(request.resource.seat_id, request.resource.rtsp_port, request.resource.control_port,
                                        request.resource.video_port, request.resource.audio_port);
    if (!contract.base_port || !grant_directory(std::filesystem::path {wide(contract.config_root)}, wide(request.resource.user_sid)) ||
        !grant_directory(std::filesystem::path {wide(contract.state_root)}, wide(request.resource.user_sid)) ||
        !grant_directory(std::filesystem::path {wide(contract.log_root)}, wide(request.resource.user_sid))) {
      error = "Private worker storage could not be created with the console-user ACL.";
      return std::nullopt;
    }

    platf::dxgi::FramedPipeFactory factory {std::make_unique<platf::dxgi::NamedPipeFactory>()};
    auto pipe = factory.create_server(pipe_name_);
    if (!pipe) { error = "The service could not create the worker first-instance pipe."; return std::nullopt; }
    HANDLE job = CreateJobObjectW(nullptr, nullptr);
    if (!job) { error = "Worker job creation failed."; return std::nullopt; }
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits {};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (!SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limits, sizeof(limits))) {
      CloseHandle(job); error = "Worker job configuration failed."; return std::nullopt;
    }

    const auto tools = service_directory();
    const auto root = tools.parent_path();
    const auto executable = root / L"Sunshine.exe";
    const auto config = root / L"config" / L"sunshine.conf";
    const auto apps = root / L"config" / L"apps.json";
    if (!std::filesystem::is_regular_file(executable) || !std::filesystem::is_regular_file(config) || !std::filesystem::is_regular_file(apps)) {
      CloseHandle(job); error = "The installed Sunshine executable/config/apps contract is incomplete."; return std::nullopt;
    }
    const auto quote = [](const std::wstring &value) { return L"\"" + value + L"\""; };
    std::wstring command = quote(executable.wstring()) + L" " + quote(config.wstring());
    for (const auto &argument : command_line(contract)) command += L" " + quote(wide(argument));
    command += L" " + quote(L"file_apps=" + apps.wstring());
    auto environment = worker_environment(
      launch_token,
      pipe_name_,
      display_pipe_name_,
      request.resource,
      request.admission.generation,
      helper_capability,
      request.steam_offline_isolation);
    if (environment.empty()) { CloseHandle(job); error = "Private worker environment creation failed."; return std::nullopt; }
    auto worker_descriptor = worker_security_descriptor();
    if (!worker_descriptor) {
      CloseHandle(job);
      error = "The broker could not validate the LocalSystem-only worker security descriptor.";
      return std::nullopt;
    }
    SECURITY_ATTRIBUTES worker_process_security {
      .nLength = sizeof(SECURITY_ATTRIBUTES), .lpSecurityDescriptor = worker_descriptor.get(), .bInheritHandle = FALSE
    };
    SECURITY_ATTRIBUTES worker_thread_security {
      .nLength = sizeof(SECURITY_ATTRIBUTES), .lpSecurityDescriptor = worker_descriptor.get(), .bInheritHandle = FALSE
    };
    STARTUPINFOW startup {.cb = sizeof(STARTUPINFOW)};
    std::wstring desktop = wide(request.resource.desktop_name);
    startup.lpDesktop = desktop.data();
    PROCESS_INFORMATION info {};
    if (!CreateProcessAsUserW(launch_token, executable.c_str(), command.data(), &worker_process_security, &worker_thread_security, FALSE,
                              CREATE_UNICODE_ENVIRONMENT | CREATE_NO_WINDOW | CREATE_SUSPENDED,
                              environment.data(), root.c_str(), &startup, &info)) {
      CloseHandle(job); error = "Private Sunshine worker process launch failed."; return std::nullopt;
    }
    process_ = info.hProcess;
    job_ = job;
    pid_ = info.dwProcessId;
    resource_ = request.resource;
    auto fail_start = [&](std::string message) -> std::optional<route_t> {
      if (info.hThread) { CloseHandle(info.hThread); info.hThread = nullptr; }
      if (!stop({})) message += " Cleanup remains owned and must be retried.";
      error = std::move(message);
      return std::nullopt;
    };
    if (!process_matches_resource(info.hProcess, request.resource)) return fail_start("Worker token/session identity did not match provider ownership.");
    if (!AssignProcessToJobObject(job, info.hProcess)) return fail_start("Worker job assignment failed.");
    const auto creation_time = process_creation_time(info.hProcess);
    if (!creation_time) return fail_start("Worker process creation identity could not be queried.");
    worker_generation_ = request.admission.generation;
    worker_creation_time_ = creation_time;
    if (request.steam_offline_isolation) {
      if (!steam_offline_registration_.register_root(info.dwProcessId, creation_time, request.admission.generation,
                                                     request.resource.seat_id, error)) {
        if (error.empty()) error = "Steam offline isolation driver registration failed.";
        return fail_start(error);
      }
      steam_offline_isolation_ = true;
      steam_offline_monitor_stop_.store(false, std::memory_order_release);
      steam_offline_poisoned_.store(false, std::memory_order_release);
      try {
        steam_offline_monitor_ = std::thread([this] {
          while (!steam_offline_monitor_stop_.load(std::memory_order_acquire)) {
            std::string health_error;
            if (!steam_offline_registration_.healthy(health_error)) {
              steam_offline_poisoned_.store(true, std::memory_order_release);
              const auto process = static_cast<HANDLE>(process_);
              const auto job = static_cast<HANDLE>(job_);
              if (!process || !job || !TerminateJobObject(job, ERROR_NETWORK_NOT_AVAILABLE) ||
                  WaitForSingleObject(process, 5000) != WAIT_OBJECT_0) {
                /* stop() retains both exact handles and the registration in
                 * the poisoned quarantine if containment is not proven. */
                return;
              }
              return;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(250));
          }
        });
      } catch (...) {
        return fail_start("Steam offline readiness monitor could not be created.");
      }
    }
    if (steam_offline_isolation_) {
      std::string readiness_error;
      if (!steam_offline_registration_.healthy(readiness_error)) {
        return fail_start(readiness_error.empty()
          ? "Steam offline isolation readiness changed before worker resume."
          : readiness_error);
      }
    }
    if (ResumeThread(info.hThread) == static_cast<DWORD>(-1)) return fail_start("Worker resume failed.");
    CloseHandle(info.hThread); info.hThread = nullptr;

    pipe->wait_for_client_connection(10000);
    DWORD client_pid = 0;
    if (!pipe->is_connected() || !pipe->get_client_process_id(client_pid) || client_pid != info.dwProcessId) {
      return fail_start("Worker pipe peer identity mismatch.");
    }
    pipe_ = std::move(pipe);
    bool worker_poisoned = false;
    auto route = transact_admission(*pipe_, request, static_cast<HANDLE>(process_), job, hdr_target_binding_, worker_poisoned, 60000, error);
    if (steam_offline_poisoned_.load(std::memory_order_acquire)) {
      return fail_start("Steam offline isolation driver lost BFE/WFP readiness.");
    }
    if (!route) return fail_start(error.empty() ? "Private worker did not become ready." : error);
    generation_ = request.admission.generation;
    if (RAND_bytes(snapshot_auth_key_.data(), static_cast<int>(snapshot_auth_key_.size())) != 1) {
      return fail_start("Private worker snapshot authentication key generation failed.");
    }
    display_broker_running_.store(true, std::memory_order_release);
    display_broker_thread_ = std::jthread([this](std::stop_token) { run_display_broker(); });
    return route;
#endif
  }

  std::optional<route_t> process_t::resume(const worker_request_t &request, std::string &error) {
#ifndef _WIN32
    error = "Private worker process is Windows-only.";
    return std::nullopt;
#else
    if (cleanup_pending_ || steam_offline_poisoned_.load(std::memory_order_acquire) ||
        steam_offline_isolation_ != steam_offline_registration_.active()) {
      error = "Steam offline registration cleanup or ownership state is not settled; reconnect is blocked.";
      return std::nullopt;
    }
    if (!process_ || !pipe_ || request.admission.generation != generation_ ||
        request.admission.generation != worker_generation_ ||
        request.resource.windows_session_id != resource_.windows_session_id ||
        request.resource.seat_id != resource_.seat_id || request.resource.rtsp_port != resource_.rtsp_port ||
        request.resource.control_port != resource_.control_port || request.resource.video_port != resource_.video_port ||
        request.resource.audio_port != resource_.audio_port || request.resource.launch_token == 0 ||
        !token_matches_resource(reinterpret_cast<HANDLE>(request.resource.launch_token), request.resource) ||
        request.steam_offline_isolation != steam_offline_isolation_ || process_creation_time(static_cast<HANDLE>(process_)) != worker_creation_time_ ||
        !process_matches_resource(static_cast<HANDLE>(process_), request.resource)) {
      error = "Reconnect does not match the running private worker resource.";
      return std::nullopt;
    }
    DWORD exit = 0;
    if (!GetExitCodeProcess(static_cast<HANDLE>(process_), &exit) || exit != STILL_ACTIVE) {
      error = "The private worker exited before reconnect admission.";
      return std::nullopt;
    }
    bool worker_poisoned = false;
    auto route = transact_admission(*pipe_, request, static_cast<HANDLE>(process_), static_cast<HANDLE>(job_),
                                    hdr_target_binding_, worker_poisoned, 20000, error);
    if (!route && !worker_poisoned) {
      DWORD post_failure_exit = STILL_ACTIVE;
      if (!GetExitCodeProcess(static_cast<HANDLE>(process_), &post_failure_exit) || post_failure_exit != STILL_ACTIVE) {
        worker_poisoned = true;
      }
    }
    if (!route && worker_poisoned && !stop({})) {
      error += " Worker containment failed and cleanup remains pending.";
    }
    if (steam_offline_poisoned_.load(std::memory_order_acquire)) {
      if (route) (void) stop({});
      error = "Steam offline isolation driver lost BFE/WFP readiness.";
      return std::nullopt;
    }
    if (route) resource_ = request.resource;
    return route;
#endif
  }

  bool process_t::park(const route_t &) noexcept {
#ifdef _WIN32
    if (!process_ || !pipe_) return false;
    DWORD exit = 0;
    return GetExitCodeProcess(static_cast<HANDLE>(process_), &exit) && exit == STILL_ACTIVE;
#else
    return false;
#endif
  }

  bool process_t::stop(const route_t &) noexcept {
#ifdef _WIN32
    bool stopped = true;
    display_broker_running_.store(false, std::memory_order_release);
    if (display_broker_thread_.joinable()) display_broker_thread_.request_stop();
    if (display_broker_thread_.joinable()) display_broker_thread_.join();
    steam_offline_monitor_stop_.store(true, std::memory_order_release);
    if (steam_offline_monitor_.joinable()) steam_offline_monitor_.join();
    if (pipe_) {
      const std::array<std::uint8_t, 1> stop {1};
      (void) pipe_->send(stop, 1000);
    }
    if (process_) {
      const auto process = static_cast<HANDLE>(process_);
      DWORD exit = STILL_ACTIVE;
      if (!GetExitCodeProcess(process, &exit)) {
        stopped = false;
      } else if (exit == STILL_ACTIVE && WaitForSingleObject(process, 10000) != WAIT_OBJECT_0) {
        stopped = TerminateProcess(process, ERROR_PROCESS_ABORTED) != FALSE &&
          WaitForSingleObject(process, 5000) == WAIT_OBJECT_0;
        if (!stopped && job_ && TerminateJobObject(static_cast<HANDLE>(job_), ERROR_PROCESS_ABORTED)) {
          stopped = WaitForSingleObject(process, 5000) == WAIT_OBJECT_0;
        }
      }
      if (stopped) { CloseHandle(process); process_ = nullptr; }
    }
    if (stopped) pipe_.reset();
    if (stopped && job_) { CloseHandle(static_cast<HANDLE>(job_)); job_ = nullptr; }
    if (stopped && steam_offline_registration_.active()) {
      std::string registration_error;
      if (!steam_offline_registration_.release(registration_error)) { stopped = false; cleanup_pending_ = true; }
      else cleanup_pending_ = false;
    }
    if (!stopped) return false;
    pid_ = 0; pipe_name_.clear(); display_pipe_name_.clear(); generation_ = 0; resource_ = {};
    hdr_target_binding_.reset();
    OPENSSL_cleanse(snapshot_auth_key_.data(), snapshot_auth_key_.size());
    snapshot_tiers_ = {};
    steam_offline_isolation_ = false;
    cleanup_pending_ = false;
    steam_offline_poisoned_.store(false, std::memory_order_release);
    worker_generation_ = 0; worker_creation_time_ = 0;
#endif
    return true;
  }
}

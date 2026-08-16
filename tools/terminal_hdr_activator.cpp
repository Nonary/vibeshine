/**
 * Seat-local native HDR activation helper.
 *
 * This is deliberately a disposable process. The SYSTEM broker launches it
 * under the managed seat and applies a watchdog, so a graphics-stack stall
 * cannot block the broker or a Terminal Services callback. It uses documented D3D11, DXGI,
 * DisplayConfig, and D3DKMT entry points and never patches a Windows binary.
 */

#include "src/terminal_session_hdr_policy.h"

#include <Windows.h>
#include <Aclapi.h>
#include <Sddl.h>
#include <TlHelp32.h>
#include <WtsApi32.h>
#include <d3d11.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <type_traits>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace {
  enum class exit_code_e: DWORD {
    success = 0,
    bad_arguments = 10,
    display_query_failed = 11,
    ambiguous_display = 12,
    source_mode_missing = 13,
    hdr_not_eligible = 14,
    dxgi_factory_failed = 15,
    adapter_failed = 16,
    d3d_device_failed = 17,
    texture_failed = 18,
    shared_handle_failed = 19,
    kmt_api_missing = 20,
    kmt_adapter_failed = 21,
    kmt_device_failed = 22,
    resource_query_failed = 23,
    resource_open_failed = 24,
    allocation_contract_failed = 25,
    primary_clone_failed = 26,
    source_owner_failed = 27,
    set_display_mode_failed = 28,
    hdr_did_not_activate = 29,
    hdr_did_not_persist = 30,
    cleanup_failed = 31,
    target_mismatch = 32,
    console_session_rejected = 33,
    parent_rejected = 34,
    capability_rejected = 35,
  };

  template<class T, class Deleter>
  using unique_resource = std::unique_ptr<T, Deleter>;

  struct handle_closer {
    void operator()(void *handle) const noexcept {
      if (handle && handle != INVALID_HANDLE_VALUE) CloseHandle(handle);
    }
  };
  using unique_handle = unique_resource<void, handle_closer>;

  struct local_memory_closer {
    void operator()(void *memory) const noexcept {
      if (memory) LocalFree(memory);
    }
  };
  using unique_local_memory = unique_resource<void, local_memory_closer>;

  struct module_closer {
    void operator()(std::remove_pointer_t<HMODULE> *module) const noexcept {
      if (module) FreeLibrary(module);
    }
  };
  using unique_module = unique_resource<std::remove_pointer_t<HMODULE>, module_closer>;

  struct active_path_t {
    DISPLAYCONFIG_PATH_INFO path {};
    DISPLAYCONFIG_SOURCE_MODE source_mode {};
    terminal_session::hdr::display_state_t color;
  };

  // MinGW exposes the Win11 device-info opcode but hides the matching record
  // behind NTDDI_VERSION. Keep the documented ABI local so this helper can
  // query the running OS without raising the product's Windows 10 baseline.
  struct advanced_color_info_v2_t {
    DISPLAYCONFIG_DEVICE_INFO_HEADER header {};
    UINT32 value {};
    DISPLAYCONFIG_COLOR_ENCODING color_encoding {};
    UINT32 bits_per_color_channel {};
    UINT32 active_color_mode {};
  };
  static_assert(sizeof(advanced_color_info_v2_t) == 36);

  constexpr UINT32 advanced_color_active = 1u << 1;
  constexpr UINT32 hdr_supported = 1u << 4;
  constexpr UINT32 hdr_user_enabled = 1u << 5;
  constexpr UINT32 advanced_color_mode_hdr = 2;

  struct expected_target_t {
    DWORD session_id {};
    LUID source_adapter {};
    UINT32 source_id {};
    LUID target_adapter {};
    UINT32 target_id {};
  };

  bool same_target(const active_path_t &active, const expected_target_t &expected) {
    return active.path.sourceInfo.adapterId.HighPart == expected.source_adapter.HighPart &&
           active.path.sourceInfo.adapterId.LowPart == expected.source_adapter.LowPart &&
           active.path.sourceInfo.id == expected.source_id &&
           active.path.targetInfo.adapterId.HighPart == expected.target_adapter.HighPart &&
           active.path.targetInfo.adapterId.LowPart == expected.target_adapter.LowPart &&
           active.path.targetInfo.id == expected.target_id;
  }

  bool managed_seat_session(const DWORD session_id) {
    LPWSTR raw = nullptr;
    DWORD bytes = 0;
    if (!WTSQuerySessionInformationW(WTS_CURRENT_SERVER_HANDLE, session_id, WTSUserName, &raw, &bytes) || !raw) return false;
    const auto memory = std::unique_ptr<wchar_t, decltype(&WTSFreeMemory)> {raw, WTSFreeMemory};
    std::wstring_view name {raw};
    constexpr std::wstring_view prefix = L"VibeshineSeat";
    return name.size() == prefix.size() + 2 && _wcsnicmp(name.data(), prefix.data(), prefix.size()) == 0 &&
           name[prefix.size()] >= L'0' && name[prefix.size()] <= L'9' &&
           name[prefix.size() + 1] >= L'0' && name[prefix.size() + 1] <= L'9';
  }

  unique_handle open_path(const std::filesystem::path &path, const bool directory) {
    return unique_handle {CreateFileW(path.c_str(), FILE_READ_ATTRIBUTES | READ_CONTROL,
                                      FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                      nullptr, OPEN_EXISTING,
                                      directory ? FILE_FLAG_BACKUP_SEMANTICS : FILE_ATTRIBUTE_NORMAL, nullptr)};
  }

  std::optional<std::wstring> final_path(const HANDLE handle) {
    const DWORD flags = FILE_NAME_NORMALIZED | VOLUME_NAME_DOS;
    const DWORD required = GetFinalPathNameByHandleW(handle, nullptr, 0, flags);
    if (!required) return std::nullopt;
    std::wstring result(required + 1, L'\0');
    const DWORD written = GetFinalPathNameByHandleW(handle, result.data(), static_cast<DWORD>(result.size()), flags);
    if (!written || written >= result.size()) return std::nullopt;
    result.resize(written);
    return result;
  }

  bool same_file(const HANDLE left, const HANDLE right) {
    BY_HANDLE_FILE_INFORMATION left_info {};
    BY_HANDLE_FILE_INFORMATION right_info {};
    return GetFileInformationByHandle(left, &left_info) && GetFileInformationByHandle(right, &right_info) &&
           left_info.dwVolumeSerialNumber == right_info.dwVolumeSerialNumber &&
           left_info.nFileIndexHigh == right_info.nFileIndexHigh && left_info.nFileIndexLow == right_info.nFileIndexLow;
  }

  bool trusted_owner(const PSID owner) {
    std::array<std::byte, SECURITY_MAX_SID_SIZE> system_storage {};
    std::array<std::byte, SECURITY_MAX_SID_SIZE> administrators_storage {};
    DWORD system_size = static_cast<DWORD>(system_storage.size());
    DWORD administrators_size = static_cast<DWORD>(administrators_storage.size());
    if (!CreateWellKnownSid(WinLocalSystemSid, nullptr, system_storage.data(), &system_size) ||
        !CreateWellKnownSid(WinBuiltinAdministratorsSid, nullptr, administrators_storage.data(), &administrators_size)) {
      return false;
    }
    PSID trusted_installer_raw = nullptr;
    if (!ConvertStringSidToSidW(
          L"S-1-5-80-956008885-3418522649-1831038044-1853292631-2271478464", &trusted_installer_raw)) {
      return false;
    }
    unique_local_memory trusted_installer {trusted_installer_raw};
    return owner && IsValidSid(owner) &&
           (EqualSid(owner, system_storage.data()) || EqualSid(owner, administrators_storage.data()) ||
            EqualSid(owner, trusted_installer.get()));
  }

  bool protected_from_current_user(const HANDLE object, const bool directory) {
    PSID owner = nullptr;
    PSECURITY_DESCRIPTOR raw_descriptor = nullptr;
    if (GetSecurityInfo(object, SE_FILE_OBJECT, OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION,
                        &owner, nullptr, nullptr, nullptr, &raw_descriptor) != ERROR_SUCCESS || !raw_descriptor) {
      return false;
    }
    unique_local_memory descriptor {raw_descriptor};
    if (!trusted_owner(owner)) return false;

    HANDLE raw_token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY | TOKEN_DUPLICATE, &raw_token)) return false;
    unique_handle token {raw_token};
    HANDLE raw_impersonation = nullptr;
    if (!DuplicateToken(token.get(), SecurityImpersonation, &raw_impersonation)) return false;
    unique_handle impersonation {raw_impersonation};
    GENERIC_MAPPING mapping {FILE_GENERIC_READ, FILE_GENERIC_WRITE, FILE_GENERIC_EXECUTE, FILE_ALL_ACCESS};
    std::array<std::byte, 1024> privilege_storage {};
    auto *privileges = reinterpret_cast<PRIVILEGE_SET *>(privilege_storage.data());
    DWORD privilege_size = static_cast<DWORD>(privilege_storage.size());
    DWORD granted = 0;
    BOOL access = FALSE;
    if (!AccessCheck(raw_descriptor, impersonation.get(), MAXIMUM_ALLOWED, &mapping, privileges,
                     &privilege_size, &granted, &access) || !access) {
      return false;
    }
    const DWORD shared_dangerous = FILE_WRITE_ATTRIBUTES | FILE_WRITE_EA | DELETE | WRITE_DAC | WRITE_OWNER;
    const DWORD dangerous = shared_dangerous |
      (directory ? (FILE_ADD_FILE | FILE_ADD_SUBDIRECTORY | FILE_DELETE_CHILD) : (FILE_WRITE_DATA | FILE_APPEND_DATA));
    return (granted & dangerous) == 0;
  }

  bool local_system_process(const HANDLE process) {
    HANDLE raw_token = nullptr;
    if (!OpenProcessToken(process, TOKEN_QUERY, &raw_token)) return false;
    unique_handle token {raw_token};
    DWORD bytes = 0;
    (void) GetTokenInformation(token.get(), TokenUser, nullptr, 0, &bytes);
    if (!bytes) return false;
    std::vector<std::byte> token_storage;
    try {
      token_storage.resize(bytes);
    } catch (...) {
      return false;
    }
    if (!GetTokenInformation(token.get(), TokenUser, token_storage.data(), bytes, &bytes)) return false;
    std::array<std::byte, SECURITY_MAX_SID_SIZE> system_storage {};
    DWORD system_size = static_cast<DWORD>(system_storage.size());
    return CreateWellKnownSid(WinLocalSystemSid, nullptr, system_storage.data(), &system_size) &&
           EqualSid(reinterpret_cast<TOKEN_USER *>(token_storage.data())->User.Sid, system_storage.data());
  }

  bool expected_broker_parent(const DWORD parent_pid) {
    unique_handle snapshot {CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0)};
    if (!snapshot || snapshot.get() == INVALID_HANDLE_VALUE) return false;
    PROCESSENTRY32W entry {.dwSize = sizeof(PROCESSENTRY32W)};
    bool parent_matches = false;
    if (Process32FirstW(snapshot.get(), &entry)) {
      do {
        if (entry.th32ProcessID == GetCurrentProcessId()) {
          parent_matches = entry.th32ParentProcessID == parent_pid;
          break;
        }
      } while (Process32NextW(snapshot.get(), &entry));
    }
    if (!parent_matches) return false;
    unique_handle parent {OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, parent_pid)};
    DWORD parent_session = 0xffffffffu;
    if (!parent || !ProcessIdToSessionId(parent_pid, &parent_session) || parent_session != 0 ||
        !local_system_process(parent.get())) return false;
    std::array<wchar_t, 32768> parent_image {};
    DWORD parent_chars = static_cast<DWORD>(parent_image.size());
    if (!QueryFullProcessImageNameW(parent.get(), 0, parent_image.data(), &parent_chars) || !parent_chars) return false;
    std::array<wchar_t, 32768> own_image {};
    const DWORD own_chars = GetModuleFileNameW(nullptr, own_image.data(), static_cast<DWORD>(own_image.size()));
    if (!own_chars || own_chars >= own_image.size()) return false;
    auto own_file = open_path(std::filesystem::path {std::wstring_view {own_image.data(), own_chars}}, false);
    auto parent_file = open_path(std::filesystem::path {std::wstring_view {parent_image.data(), parent_chars}}, false);
    if (!own_file || own_file.get() == INVALID_HANDLE_VALUE || !parent_file || parent_file.get() == INVALID_HANDLE_VALUE) return false;
    const auto own_final = final_path(own_file.get());
    if (!own_final) return false;
    const std::filesystem::path own_path {*own_final};
    if (_wcsicmp(own_path.filename().c_str(), L"vibeshine_terminal_hdr_activator.exe") != 0 ||
        _wcsicmp(own_path.parent_path().filename().c_str(), L"tools") != 0) {
      return false;
    }
    const auto tools_path = own_path.parent_path();
    const auto root_path = tools_path.parent_path();
    const auto expected_parent_path = tools_path / L"sunshinesvc.exe";
    auto expected_parent = open_path(expected_parent_path, false);
    auto tools_directory = open_path(tools_path, true);
    auto root_directory = open_path(root_path, true);
    if (!expected_parent || expected_parent.get() == INVALID_HANDLE_VALUE ||
        !tools_directory || tools_directory.get() == INVALID_HANDLE_VALUE ||
        !root_directory || root_directory.get() == INVALID_HANDLE_VALUE ||
        !same_file(parent_file.get(), expected_parent.get())) {
      return false;
    }
    const auto expected_parent_final = final_path(expected_parent.get());
    if (!expected_parent_final || _wcsicmp(expected_parent_final->c_str(), expected_parent_path.c_str()) != 0) return false;
    return protected_from_current_user(root_directory.get(), true) &&
           protected_from_current_user(tools_directory.get(), true) &&
           protected_from_current_user(own_file.get(), false) &&
           protected_from_current_user(expected_parent.get(), false);
  }

  std::optional<terminal_session::hdr::activation_capability_t> read_capability(const std::string_view argument) {
    constexpr std::string_view argument_prefix = "--capability-pipe=";
    constexpr std::string_view pipe_prefix = "\\\\.\\pipe\\VibeshineTerminalHdr-";
    if (!argument.starts_with(argument_prefix)) return std::nullopt;
    const auto pipe_name = argument.substr(argument_prefix.size());
    if (!pipe_name.starts_with(pipe_prefix) || pipe_name.size() != pipe_prefix.size() + 32 ||
        !std::all_of(pipe_name.begin() + static_cast<std::ptrdiff_t>(pipe_prefix.size()), pipe_name.end(), [](const char value) {
          return (value >= '0' && value <= '9') || (value >= 'a' && value <= 'f');
        })) {
      return std::nullopt;
    }
    const std::wstring wide_name(pipe_name.begin(), pipe_name.end());
    unique_handle pipe;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds {5};
    do {
      pipe.reset(CreateFileW(wide_name.c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING, 0, nullptr));
      if (pipe && pipe.get() != INVALID_HANDLE_VALUE) break;
      pipe.release();
      if (GetLastError() != ERROR_PIPE_BUSY || !WaitNamedPipeW(wide_name.c_str(), 100)) return std::nullopt;
    } while (std::chrono::steady_clock::now() < deadline);
    if (!pipe || pipe.get() == INVALID_HANDLE_VALUE || GetFileType(pipe.get()) != FILE_TYPE_PIPE) return std::nullopt;
    ULONG server_pid = 0;
    if (!GetNamedPipeServerProcessId(pipe.get(), &server_pid) || !server_pid) return std::nullopt;
    terminal_session::hdr::activation_capability_t capability;
    std::size_t offset = 0;
    while (offset < sizeof(capability)) {
      DWORD read = 0;
      if (!ReadFile(pipe.get(), reinterpret_cast<std::byte *>(&capability) + offset,
                    static_cast<DWORD>(sizeof(capability) - offset), &read, nullptr) || !read) {
        return std::nullopt;
      }
      offset += read;
    }
    if (capability.magic != terminal_session::hdr::activation_capability_magic ||
        capability.size != sizeof(capability) || capability.version != terminal_session::hdr::activation_capability_version ||
        capability.reserved != 0 || capability.parent_pid != server_pid) {
      return std::nullopt;
    }
    return capability;
  }

  std::optional<active_path_t> query_active_path(exit_code_e &error) {
    constexpr UINT32 flags = QDC_ONLY_ACTIVE_PATHS | QDC_VIRTUAL_MODE_AWARE;
    for (int attempt = 0; attempt < 2; ++attempt) {
      UINT32 path_count = 0;
      UINT32 mode_count = 0;
      if (GetDisplayConfigBufferSizes(flags, &path_count, &mode_count) != ERROR_SUCCESS) {
        error = exit_code_e::display_query_failed;
        return std::nullopt;
      }
      std::vector<DISPLAYCONFIG_PATH_INFO> paths(path_count);
      std::vector<DISPLAYCONFIG_MODE_INFO> modes(mode_count);
      const LONG queried = QueryDisplayConfig(flags, &path_count, paths.data(), &mode_count, modes.data(), nullptr);
      if (queried == ERROR_INSUFFICIENT_BUFFER) continue;
      if (queried != ERROR_SUCCESS) {
        error = exit_code_e::display_query_failed;
        return std::nullopt;
      }
      if (path_count != 1) {
        error = exit_code_e::ambiguous_display;
        return std::nullopt;
      }
      const auto &path = paths.front();
      const UINT32 source_index = path.sourceInfo.sourceModeInfoIdx;
      if (source_index == DISPLAYCONFIG_PATH_SOURCE_MODE_IDX_INVALID || source_index >= mode_count ||
          modes[source_index].infoType != DISPLAYCONFIG_MODE_INFO_TYPE_SOURCE) {
        error = exit_code_e::source_mode_missing;
        return std::nullopt;
      }

      advanced_color_info_v2_t color {};
      color.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_ADVANCED_COLOR_INFO_2;
      color.header.size = sizeof(color);
      color.header.adapterId = path.targetInfo.adapterId;
      color.header.id = path.targetInfo.id;
      if (DisplayConfigGetDeviceInfo(&color.header) != ERROR_SUCCESS) {
        error = exit_code_e::display_query_failed;
        return std::nullopt;
      }
      return active_path_t {
        .path = path,
        .source_mode = modes[source_index].sourceMode,
        .color = {
          .found = true,
          .supported = (color.value & hdr_supported) != 0,
          .user_enabled = (color.value & hdr_user_enabled) != 0,
          .active = (color.value & advanced_color_active) != 0,
          .hdr_color_mode = color.active_color_mode == advanced_color_mode_hdr,
          .bits_per_color_channel = color.bits_per_color_channel,
        },
      };
    }
    error = exit_code_e::display_query_failed;
    return std::nullopt;
  }

  namespace kmt {
    using status_t = LONG;
    using handle_t = UINT32;
    using source_id_t = UINT32;

    // MinGW does not ship d3dkmthk.h. These x64/ARM64 ABI replicas contain
    // only the public fields used by the documented D3DKMT calls below.
    struct open_adapter_t { LUID luid; handle_t adapter; };
    struct close_adapter_t { handle_t adapter; };
    struct create_device_t {
      union { handle_t adapter; void *adapter_pointer; };
      UINT32 flags;
      handle_t device;
      void *command_buffer;
      UINT32 command_buffer_size;
      void *allocation_list;
      UINT32 allocation_list_size;
      void *patch_location_list;
      UINT32 patch_location_list_size;
    };
    struct destroy_device_t { handle_t device; };
    struct query_resource_t {
      handle_t device;
      HANDLE nt_handle;
      void *private_runtime_data;
      UINT32 private_runtime_data_size;
      UINT32 total_private_driver_data_size;
      UINT32 resource_private_driver_data_size;
      UINT32 allocation_count;
    };
    struct open_allocation_info_t {
      handle_t allocation;
      const void *private_driver_data;
      UINT32 private_driver_data_size;
      UINT64 gpu_virtual_address;
      ULONG_PTR reserved[6];
    };
    struct open_resource_t {
      handle_t device;
      HANDLE nt_handle;
      UINT32 allocation_count;
      open_allocation_info_t *allocation_info;
      UINT32 private_runtime_data_size;
      void *private_runtime_data;
      UINT32 resource_private_driver_data_size;
      void *resource_private_driver_data;
      UINT32 total_private_driver_data_size;
      void *total_private_driver_data;
      handle_t resource;
      handle_t keyed_mutex;
      void *keyed_mutex_private_runtime_data;
      UINT32 keyed_mutex_private_runtime_data_size;
      handle_t sync_object;
    };
    struct allocation_info_t {
      handle_t allocation;
      union { HANDLE section; const void *system_memory; };
      void *private_driver_data;
      UINT32 private_driver_data_size;
      source_id_t source_id;
      UINT32 flags;
      UINT64 gpu_virtual_address;
      ULONG_PTR unused;
      ULONG_PTR reserved[5];
    };
    struct create_allocation_t {
      handle_t device;
      handle_t resource;
      handle_t global_share;
      const void *private_runtime_data;
      UINT32 private_runtime_data_size;
      const void *private_driver_data;
      UINT32 private_driver_data_size;
      UINT32 allocation_count;
      allocation_info_t *allocation_info;
      UINT32 flags;
      HANDLE private_runtime_resource_handle;
    };
    struct destroy_allocation_t {
      handle_t device;
      handle_t resource;
      const handle_t *allocation_list;
      UINT32 allocation_count;
    };
    struct set_owner_t {
      handle_t device;
      const UINT32 *types;
      const source_id_t *source_ids;
      UINT32 source_count;
    };
    struct set_display_mode_t {
      handle_t device;
      handle_t primary_allocation;
      UINT32 scan_line_ordering;
      UINT32 orientation;
      UINT32 private_driver_format_attribute;
      UINT32 flags;
    };

    static_assert(sizeof(void *) == 8, "The terminal HDR activator requires a 64-bit target.");
    static_assert(sizeof(create_device_t) == 64);
    static_assert(sizeof(query_resource_t) == 40);
    static_assert(sizeof(open_allocation_info_t) == 80);
    static_assert(sizeof(open_resource_t) == 104);
    static_assert(sizeof(allocation_info_t) == 96);
    static_assert(sizeof(create_allocation_t) == 72);
    static_assert(sizeof(destroy_allocation_t) == 24);
    static_assert(sizeof(set_owner_t) == 32);
    static_assert(sizeof(set_display_mode_t) == 24);

    template<class T>
    T load(HMODULE module, const char *name) {
      return reinterpret_cast<T>(GetProcAddress(module, name));
    }

    struct api_t {
      using open_adapter_fn = status_t (WINAPI *)(open_adapter_t *);
      using close_adapter_fn = status_t (WINAPI *)(const close_adapter_t *);
      using create_device_fn = status_t (WINAPI *)(create_device_t *);
      using destroy_device_fn = status_t (WINAPI *)(const destroy_device_t *);
      using query_resource_fn = status_t (WINAPI *)(query_resource_t *);
      using open_resource_fn = status_t (WINAPI *)(open_resource_t *);
      using create_allocation_fn = status_t (WINAPI *)(create_allocation_t *);
      using destroy_allocation_fn = status_t (WINAPI *)(const destroy_allocation_t *);
      using set_owner_fn = status_t (WINAPI *)(const set_owner_t *);
      using set_display_mode_fn = status_t (WINAPI *)(const set_display_mode_t *);
      using release_owners_fn = status_t (WINAPI *)(HANDLE);

      unique_module module {LoadLibraryW(L"gdi32.dll")};
      open_adapter_fn open_adapter {};
      close_adapter_fn close_adapter {};
      create_device_fn create_device {};
      destroy_device_fn destroy_device {};
      query_resource_fn query_resource {};
      open_resource_fn open_resource {};
      create_allocation_fn create_allocation {};
      destroy_allocation_fn destroy_allocation {};
      set_owner_fn set_owner {};
      set_display_mode_fn set_display_mode {};
      release_owners_fn release_owners {};

      api_t() {
        if (!module) return;
        open_adapter = load<open_adapter_fn>(module.get(), "D3DKMTOpenAdapterFromLuid");
        close_adapter = load<close_adapter_fn>(module.get(), "D3DKMTCloseAdapter");
        create_device = load<create_device_fn>(module.get(), "D3DKMTCreateDevice");
        destroy_device = load<destroy_device_fn>(module.get(), "D3DKMTDestroyDevice");
        query_resource = load<query_resource_fn>(module.get(), "D3DKMTQueryResourceInfoFromNtHandle");
        open_resource = load<open_resource_fn>(module.get(), "D3DKMTOpenResourceFromNtHandle");
        create_allocation = load<create_allocation_fn>(module.get(), "D3DKMTCreateAllocation");
        destroy_allocation = load<destroy_allocation_fn>(module.get(), "D3DKMTDestroyAllocation");
        set_owner = load<set_owner_fn>(module.get(), "D3DKMTSetVidPnSourceOwner");
        set_display_mode = load<set_display_mode_fn>(module.get(), "D3DKMTSetDisplayMode");
        release_owners = load<release_owners_fn>(module.get(), "D3DKMTReleaseProcessVidPnSourceOwners");
      }

      explicit operator bool() const noexcept {
        return module && open_adapter && close_adapter && create_device && destroy_device && query_resource &&
               open_resource && create_allocation && destroy_allocation && set_owner && set_display_mode && release_owners;
      }
    };

    struct context_t {
      api_t &api;
      handle_t adapter {};
      handle_t device {};
      handle_t opened_resource {};
      handle_t primary_resource {};
      handle_t primary_allocation {};
      bool owner_claimed {};

      bool cleanup() noexcept {
        bool complete = true;
        if (owner_claimed) {
          if (api.release_owners(GetCurrentProcess()) >= 0) owner_claimed = false;
          else complete = false;
        }
        if (device && primary_resource) {
          const destroy_allocation_t destroy {device, primary_resource, nullptr, 0};
          if (api.destroy_allocation(&destroy) >= 0) {
            primary_resource = 0;
            primary_allocation = 0;
          } else complete = false;
        }
        if (device && opened_resource) {
          const destroy_allocation_t destroy {device, opened_resource, nullptr, 0};
          if (api.destroy_allocation(&destroy) >= 0) opened_resource = 0;
          else complete = false;
        }
        if (device && !primary_resource && !opened_resource) {
          const destroy_device_t destroy {device};
          if (api.destroy_device(&destroy) >= 0) device = 0;
          else complete = false;
        }
        if (adapter && !device) {
          const close_adapter_t close {adapter};
          if (api.close_adapter(&close) >= 0) adapter = 0;
          else complete = false;
        }
        return complete && !owner_claimed && !primary_resource && !opened_resource && !device && !adapter;
      }

      ~context_t() { (void) cleanup(); }
    };
  }

  bool wait_for_hdr(const expected_target_t &expected, const std::chrono::milliseconds timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    do {
      exit_code_e ignored {};
      if (const auto active = query_active_path(ignored);
          active && same_target(*active, expected) && active->color.active && active->color.hdr_color_mode) return true;
      std::this_thread::sleep_for(std::chrono::milliseconds {50});
    } while (std::chrono::steady_clock::now() < deadline);
    return false;
  }

  exit_code_e activate_hdr(const expected_target_t &expected, const DWORD parent_pid) {
    DWORD process_session = 0xffffffffu;
    if (!ProcessIdToSessionId(GetCurrentProcessId(), &process_session) || process_session == 0 ||
        process_session == WTSGetActiveConsoleSessionId() || process_session != expected.session_id ||
        !managed_seat_session(process_session)) {
      return exit_code_e::console_session_rejected;
    }
    if (!expected_broker_parent(parent_pid)) return exit_code_e::parent_rejected;
    std::optional<active_path_t> active;
    const auto eligibility_deadline = std::chrono::steady_clock::now() + std::chrono::seconds {5};
    do {
      exit_code_e query_error {};
      active = query_active_path(query_error);
      if (active) {
        if (!same_target(*active, expected)) return exit_code_e::target_mismatch;
        switch (terminal_session::hdr::decide(true, active->color)) {
          case terminal_session::hdr::action_e::accept_active:
            return exit_code_e::success;
          case terminal_session::hdr::action_e::activate:
            break;
          default:
            active.reset();
            break;
        }
        if (active) break;
      } else if (query_error == exit_code_e::ambiguous_display) {
        return query_error;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds {100});
    } while (std::chrono::steady_clock::now() < eligibility_deadline);
    if (!active) return exit_code_e::hdr_not_eligible;

    if (active->source_mode.width == 0 || active->source_mode.height == 0) {
      return exit_code_e::source_mode_missing;
    }

    ComPtr<IDXGIFactory6> factory;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(factory.GetAddressOf())))) return exit_code_e::dxgi_factory_failed;
    ComPtr<IDXGIAdapter1> adapter;
    if (FAILED(factory->EnumAdapterByLuid(active->path.sourceInfo.adapterId, IID_PPV_ARGS(adapter.GetAddressOf())))) return exit_code_e::adapter_failed;
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> device_context;
    if (FAILED(D3D11CreateDevice(adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT,
                                 nullptr, 0, D3D11_SDK_VERSION, device.GetAddressOf(), nullptr, device_context.GetAddressOf()))) {
      return exit_code_e::d3d_device_failed;
    }

    D3D11_TEXTURE2D_DESC texture_desc {};
    texture_desc.Width = active->source_mode.width;
    texture_desc.Height = active->source_mode.height;
    texture_desc.MipLevels = 1;
    texture_desc.ArraySize = 1;
    texture_desc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    texture_desc.SampleDesc.Count = 1;
    texture_desc.Usage = D3D11_USAGE_DEFAULT;
    texture_desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    texture_desc.MiscFlags = D3D11_RESOURCE_MISC_SHARED | D3D11_RESOURCE_MISC_SHARED_NTHANDLE |
                             D3D11_RESOURCE_MISC_SHARED_DISPLAYABLE | D3D11_RESOURCE_MISC_SHARED_EXCLUSIVE_WRITER;
    ComPtr<ID3D11Texture2D> texture;
    if (FAILED(device->CreateTexture2D(&texture_desc, nullptr, texture.GetAddressOf()))) return exit_code_e::texture_failed;
    ComPtr<IDXGIResource1> shared_resource;
    if (FAILED(texture.As(&shared_resource))) return exit_code_e::shared_handle_failed;
    HANDLE raw_shared_handle = nullptr;
    if (FAILED(shared_resource->CreateSharedHandle(nullptr, DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE,
                                                   nullptr, &raw_shared_handle)) || !raw_shared_handle) {
      return exit_code_e::shared_handle_failed;
    }
    unique_handle shared_handle {raw_shared_handle};

    kmt::api_t api;
    if (!api) return exit_code_e::kmt_api_missing;
    kmt::context_t kmt_context {api};
    kmt::open_adapter_t open {active->path.sourceInfo.adapterId, 0};
    if (api.open_adapter(&open) < 0) return exit_code_e::kmt_adapter_failed;
    kmt_context.adapter = open.adapter;
    kmt::create_device_t create_device {};
    create_device.adapter = open.adapter;
    if (api.create_device(&create_device) < 0) return exit_code_e::kmt_device_failed;
    kmt_context.device = create_device.device;

    kmt::query_resource_t resource_info {};
    resource_info.device = create_device.device;
    resource_info.nt_handle = raw_shared_handle;
    if (api.query_resource(&resource_info) < 0 || resource_info.allocation_count != 1) return exit_code_e::resource_query_failed;
    constexpr std::size_t max_private_buffer = 4 * 1024 * 1024;
    constexpr std::size_t max_private_aggregate = 8 * 1024 * 1024;
    const std::size_t aggregate = static_cast<std::size_t>(resource_info.private_runtime_data_size) +
                                  resource_info.resource_private_driver_data_size + resource_info.total_private_driver_data_size;
    if (resource_info.private_runtime_data_size > max_private_buffer ||
        resource_info.resource_private_driver_data_size > max_private_buffer ||
        resource_info.total_private_driver_data_size > max_private_buffer || aggregate > max_private_aggregate) {
      return exit_code_e::allocation_contract_failed;
    }
    std::vector<std::uint8_t> runtime_data;
    std::vector<std::uint8_t> resource_driver_data;
    std::vector<std::uint8_t> total_driver_data;
    std::vector<kmt::open_allocation_info_t> allocation_info;
    try {
      runtime_data.resize(resource_info.private_runtime_data_size);
      resource_driver_data.resize(resource_info.resource_private_driver_data_size);
      total_driver_data.resize(resource_info.total_private_driver_data_size);
      allocation_info.resize(resource_info.allocation_count);
    } catch (...) {
      return exit_code_e::allocation_contract_failed;
    }
    kmt::open_resource_t open_resource {};
    open_resource.device = create_device.device;
    open_resource.nt_handle = raw_shared_handle;
    open_resource.allocation_count = resource_info.allocation_count;
    open_resource.allocation_info = allocation_info.data();
    open_resource.private_runtime_data_size = resource_info.private_runtime_data_size;
    open_resource.private_runtime_data = runtime_data.empty() ? nullptr : runtime_data.data();
    open_resource.resource_private_driver_data_size = resource_info.resource_private_driver_data_size;
    open_resource.resource_private_driver_data = resource_driver_data.empty() ? nullptr : resource_driver_data.data();
    open_resource.total_private_driver_data_size = resource_info.total_private_driver_data_size;
    open_resource.total_private_driver_data = total_driver_data.empty() ? nullptr : total_driver_data.data();
    if (api.open_resource(&open_resource) < 0 || !open_resource.resource) return exit_code_e::resource_open_failed;
    kmt_context.opened_resource = open_resource.resource;
    const auto driver_slice_is_capped = [](const void *pointer, const std::size_t size,
                                           const std::vector<std::uint8_t> &buffer) {
      if (!pointer || !size || buffer.empty()) return false;
      const auto begin = reinterpret_cast<std::uintptr_t>(buffer.data());
      const auto value = reinterpret_cast<std::uintptr_t>(pointer);
      if (value < begin) return false;
      const auto offset = value - begin;
      return offset <= buffer.size() && size <= buffer.size() - offset;
    };
    if (!allocation_info.front().private_driver_data || allocation_info.front().private_driver_data_size == 0 ||
        (!driver_slice_is_capped(allocation_info.front().private_driver_data,
                                 allocation_info.front().private_driver_data_size, total_driver_data) &&
         !driver_slice_is_capped(allocation_info.front().private_driver_data,
                                 allocation_info.front().private_driver_data_size, resource_driver_data))) {
      return exit_code_e::allocation_contract_failed;
    }

    kmt::allocation_info_t primary {};
    primary.private_driver_data = const_cast<void *>(allocation_info.front().private_driver_data);
    primary.private_driver_data_size = allocation_info.front().private_driver_data_size;
    primary.source_id = active->path.sourceInfo.id;
    primary.flags = 1;  // D3DDDI_ALLOCATIONINFO2::Flags.Primary
    kmt::create_allocation_t create_primary {};
    create_primary.device = create_device.device;
    create_primary.private_runtime_data = runtime_data.empty() ? nullptr : runtime_data.data();
    create_primary.private_runtime_data_size = static_cast<UINT32>(runtime_data.size());
    create_primary.private_driver_data = resource_driver_data.empty() ? nullptr : resource_driver_data.data();
    create_primary.private_driver_data_size = static_cast<UINT32>(resource_driver_data.size());
    create_primary.allocation_count = 1;
    create_primary.allocation_info = &primary;
    create_primary.flags = 1;  // D3DKMT_CREATEALLOCATIONFLAGS::CreateResource
    if (api.create_allocation(&create_primary) < 0 || !create_primary.resource || !primary.allocation) {
      return exit_code_e::primary_clone_failed;
    }
    kmt_context.primary_resource = create_primary.resource;
    kmt_context.primary_allocation = primary.allocation;

    exit_code_e target_error {};
    const auto before_owner = query_active_path(target_error);
    if (!before_owner || !same_target(*before_owner, expected)) return exit_code_e::target_mismatch;
    constexpr UINT32 exclusive_owner = 2;  // D3DKMT_VIDPNSOURCEOWNER_EXCLUSIVE
    const kmt::source_id_t source_id = expected.source_id;
    const kmt::set_owner_t owner {create_device.device, &exclusive_owner, &source_id, 1};
    if (api.set_owner(&owner) < 0) return exit_code_e::source_owner_failed;
    kmt_context.owner_claimed = true;

    kmt::set_display_mode_t set_mode {};
    set_mode.device = create_device.device;
    set_mode.primary_allocation = primary.allocation;
    set_mode.scan_line_ordering = 1;  // D3DDDI_VSSLO_PROGRESSIVE
    set_mode.orientation = 1;  // D3DDDI_ROTATION_IDENTITY
    set_mode.flags = 0;  // PreserveVidPn=FALSE is the causal HDR transition.
    kmt::status_t status {};
    constexpr kmt::status_t present_mode_changed = static_cast<kmt::status_t>(0xC01E0005u);
    for (int attempt = 0; attempt < 5; ++attempt) {
      const auto before_mode = query_active_path(target_error);
      if (!before_mode || !same_target(*before_mode, expected)) return exit_code_e::target_mismatch;
      status = api.set_display_mode(&set_mode);
      if (status != present_mode_changed) break;
      std::this_thread::sleep_for(std::chrono::milliseconds {250});
    }
    if (status < 0) return exit_code_e::set_display_mode_failed;
    if (!wait_for_hdr(expected, std::chrono::seconds {2})) return exit_code_e::hdr_did_not_activate;

    // Prove that Windows retained the HDR state after every temporary source
    // owner, D3DKMT allocation/device, shared handle, and D3D object is gone.
    if (!kmt_context.cleanup()) return exit_code_e::cleanup_failed;
    shared_handle.reset();
    shared_resource.Reset();
    texture.Reset();
    device_context.Reset();
    device.Reset();
    adapter.Reset();
    factory.Reset();
    if (!wait_for_hdr(expected, std::chrono::seconds {2})) return exit_code_e::hdr_did_not_persist;
    return exit_code_e::success;
  }

}

int main(const int argc, char **argv) {
  if (argc != 3 || std::strcmp(argv[1], "--activate-hdr") != 0) {
    return static_cast<int>(exit_code_e::bad_arguments);
  }
  const auto capability = read_capability(argv[2]);
  if (!capability) return static_cast<int>(exit_code_e::capability_rejected);
  expected_target_t expected;
  expected.session_id = capability->session_id;
  expected.source_adapter.LowPart = capability->source_adapter_low;
  expected.source_id = capability->source_id;
  expected.target_adapter.LowPart = capability->target_adapter_low;
  expected.target_adapter.HighPart = std::bit_cast<LONG>(capability->target_adapter_high);
  expected.target_id = capability->target_id;
  static_assert(sizeof(LONG) == sizeof(UINT32));
  expected.source_adapter.HighPart = std::bit_cast<LONG>(capability->source_adapter_high);
  return static_cast<int>(activate_hdr(expected, capability->parent_pid));
}

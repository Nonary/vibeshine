#include "steam_offline_manager.h"
#include "steam_offline_policy.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cwctype>
#include <cstring>
#include <fstream>
#include <limits>
#include <memory>
#include <optional>
#include <ranges>
#include <sstream>
#include <stdexcept>
#include <utility>

#include <nlohmann/json.hpp>

#ifdef _WIN32
  #include <Windows.h>
  #include <fwpmu.h>
  #include <sddl.h>
  #include <Aclapi.h>
  #include <ShlObj.h>
  #pragma comment(lib, "fwpuclnt.lib")
#endif

namespace steam_offline {
  namespace {
    constexpr std::size_t max_files = 8192;
    constexpr std::uintmax_t max_file_bytes = 1ULL << 31;
    constexpr std::uintmax_t max_tree_bytes = 8ULL << 30;
    constexpr std::size_t max_depth = 16;
    constexpr std::size_t max_path_chars = 32768;
    constexpr std::wstring_view volatile_directory_names[] = {
      L"steamapps", L"userdata", L"htmlcache", L"logs", L"dumps", L"confightml", L"package"};

    bool valid_seat(std::string_view value) {
      return !value.empty() && value.size() <= 64 && std::ranges::all_of(value, [](unsigned char ch) {
        return std::isalnum(ch) || ch == '-' || ch == '_' || ch == '.';
      });
    }

#ifdef _WIN32
    struct handle_closer { void operator()(HANDLE handle) const noexcept { if (handle && handle != INVALID_HANDLE_VALUE) CloseHandle(handle); } };
    using unique_handle = std::unique_ptr<void, handle_closer>;

    struct impersonation_scope {
      bool active {};
      explicit impersonation_scope(HANDLE token) : active(token && SetThreadToken(nullptr, token) != FALSE) {}
      ~impersonation_scope() { if (active) RevertToSelf(); }
      impersonation_scope(const impersonation_scope &) = delete;
      impersonation_scope &operator=(const impersonation_scope &) = delete;
    };

    bool token_matches_sid(HANDLE token, std::wstring_view expected_sid) {
      PSID expected = nullptr;
      std::wstring sid {expected_sid};
      if (!ConvertStringSidToSidW(sid.c_str(), &expected) || !expected) return false;
      const auto expected_guard = std::unique_ptr<void, decltype(&LocalFree)> {expected, LocalFree};
      DWORD size = 0;
      GetTokenInformation(token, TokenUser, nullptr, 0, &size);
      if (!size) return false;
      std::vector<std::byte> storage(size);
      if (!GetTokenInformation(token, TokenUser, storage.data(), size, &size)) return false;
      return EqualSid(static_cast<TOKEN_USER *>(static_cast<void *>(storage.data()))->User.Sid, expected) != FALSE;
    }

    bool secure_directory(HANDLE handle, const bool require_system_owner, PSID allowed_user = nullptr) {
      if (!handle || handle == INVALID_HANDLE_VALUE) return false;
      FILE_ATTRIBUTE_TAG_INFO tag {};
      if (!GetFileInformationByHandleEx(handle, FileAttributeTagInfo, &tag, sizeof(tag)) ||
          !(tag.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) || (tag.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) return false;
      FILE_ID_INFO identity {};
      if (!GetFileInformationByHandleEx(handle, FileIdInfo, &identity, sizeof(identity))) return false;
      PSECURITY_DESCRIPTOR descriptor = nullptr;
      if (GetSecurityInfo(handle, SE_FILE_OBJECT, OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION,
                          nullptr, nullptr, nullptr, nullptr, &descriptor) != ERROR_SUCCESS || !descriptor) return false;
      PSID owner = nullptr; BOOL present = FALSE, defaulted = FALSE; PACL dacl = nullptr;
      const bool got = GetSecurityDescriptorOwner(descriptor, &owner, &defaulted) &&
        GetSecurityDescriptorDacl(descriptor, &present, &dacl, &defaulted);
      std::array<std::uint8_t, SECURITY_MAX_SID_SIZE> system_buffer {};
      DWORD system_size = static_cast<DWORD>(system_buffer.size());
      const bool system = CreateWellKnownSid(WinLocalSystemSid, nullptr, system_buffer.data(), &system_size);
      SECURITY_DESCRIPTOR_CONTROL control {}; DWORD revision = 0;
      const bool protected_dacl = GetSecurityDescriptorControl(descriptor, &control, &revision) &&
        (control & SE_DACL_PROTECTED) != 0;
      const bool owner_ok = !require_system_owner || (system && owner && EqualSid(owner, system_buffer.data()));
      const bool user_ok = !allowed_user || (present && dacl && IsValidSid(allowed_user));
      // Protected roots/leaves are authored by make_protected_directory and
      // contain only the SYSTEM/Admin full-control entries until an explicit
      // seat leaf ACL is applied. Reject a pre-created root with extra ACEs.
      const bool root_acl_ok = !require_system_owner || allowed_user || (dacl && dacl->AceCount == 2);
      LocalFree(descriptor);
      return got && present && dacl && protected_dacl && owner_ok && user_ok && root_acl_ok;
    }

    unique_handle open_secure_directory(const std::filesystem::path &path, const bool require_system_owner,
                                        PSID allowed_user = nullptr) {
      HANDLE raw = CreateFileW(path.c_str(), FILE_READ_ATTRIBUTES | READ_CONTROL | WRITE_DAC | WRITE_OWNER,
                               FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                               FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
      if (raw == INVALID_HANDLE_VALUE) return {};
      unique_handle result {raw};
      if (!secure_directory(result.get(), require_system_owner, allowed_user)) return {};
      return result;
    }

    unique_handle open_no_reparse_directory(const std::filesystem::path &path) {
      HANDLE raw = CreateFileW(path.c_str(), FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
      if (raw == INVALID_HANDLE_VALUE) return {};
      unique_handle result {raw}; FILE_ATTRIBUTE_TAG_INFO tag {};
      return GetFileInformationByHandleEx(result.get(), FileAttributeTagInfo, &tag, sizeof(tag)) &&
        (tag.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) && !(tag.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) ? std::move(result) : unique_handle {};
    }

    bool make_protected_directory(const std::filesystem::path &path, bool &created) {
      std::array<std::uint8_t, SECURITY_MAX_SID_SIZE> system_buffer {}, admin_buffer {};
      DWORD system_size = static_cast<DWORD>(system_buffer.size()), admin_size = static_cast<DWORD>(admin_buffer.size());
      if (!CreateWellKnownSid(WinLocalSystemSid, nullptr, system_buffer.data(), &system_size) ||
          !CreateWellKnownSid(WinBuiltinAdministratorsSid, nullptr, admin_buffer.data(), &admin_size)) return false;
      std::array<EXPLICIT_ACCESSW, 2> entries {};
      for (std::size_t i = 0; i != entries.size(); ++i) {
        entries[i].grfAccessPermissions = FILE_ALL_ACCESS;
        entries[i].grfAccessMode = SET_ACCESS;
        entries[i].grfInheritance = SUB_CONTAINERS_AND_OBJECTS_INHERIT;
        entries[i].Trustee.TrusteeForm = TRUSTEE_IS_SID;
        entries[i].Trustee.TrusteeType = TRUSTEE_IS_WELL_KNOWN_GROUP;
        entries[i].Trustee.ptstrName = static_cast<LPWSTR>(i ? static_cast<void *>(admin_buffer.data()) : static_cast<void *>(system_buffer.data()));
      }
      PACL acl = nullptr;
      if (SetEntriesInAclW(static_cast<ULONG>(entries.size()), entries.data(), nullptr, &acl) != ERROR_SUCCESS || !acl) return false;
      SECURITY_DESCRIPTOR descriptor {};
      const bool ready = InitializeSecurityDescriptor(&descriptor, SECURITY_DESCRIPTOR_REVISION) &&
        SetSecurityDescriptorOwner(&descriptor, system_buffer.data(), FALSE) &&
        SetSecurityDescriptorDacl(&descriptor, TRUE, acl, FALSE) &&
        SetSecurityDescriptorControl(&descriptor, SE_DACL_PROTECTED, SE_DACL_PROTECTED);
      SECURITY_ATTRIBUTES security {sizeof(security), ready ? &descriptor : nullptr, FALSE};
      SetLastError(ERROR_SUCCESS);
      created = ready && CreateDirectoryW(path.c_str(), &security) != FALSE;
      const DWORD last = GetLastError();
      LocalFree(acl);
      return ready && (created || last == ERROR_ALREADY_EXISTS);
    }

    unique_handle ensure_protected_directory(const std::filesystem::path &path) {
      bool created = false;
      if (!make_protected_directory(path, created)) return {};
      return open_secure_directory(path, true);
    }

    bool set_leaf_acl(HANDLE directory, PSID user, const ACCESS_MASK user_access, const bool inherit_user = true) {
      if (!directory || !user) return false;
      std::array<std::uint8_t, SECURITY_MAX_SID_SIZE> system_buffer {}, admin_buffer {};
      DWORD system_size = static_cast<DWORD>(system_buffer.size()), admin_size = static_cast<DWORD>(admin_buffer.size());
      if (!CreateWellKnownSid(WinLocalSystemSid, nullptr, system_buffer.data(), &system_size) ||
          !CreateWellKnownSid(WinBuiltinAdministratorsSid, nullptr, admin_buffer.data(), &admin_size)) return false;
      std::array<EXPLICIT_ACCESSW, 3> entries {};
      const auto init = [](EXPLICIT_ACCESSW &entry, PSID sid, ACCESS_MASK access, DWORD inherit) {
        entry.grfAccessPermissions = access; entry.grfAccessMode = SET_ACCESS; entry.grfInheritance = inherit;
        entry.Trustee.TrusteeForm = TRUSTEE_IS_SID; entry.Trustee.TrusteeType = TRUSTEE_IS_USER; entry.Trustee.ptstrName = static_cast<LPWSTR>(sid);
      };
      init(entries[0], system_buffer.data(), FILE_ALL_ACCESS, SUB_CONTAINERS_AND_OBJECTS_INHERIT);
      init(entries[1], admin_buffer.data(), FILE_ALL_ACCESS, SUB_CONTAINERS_AND_OBJECTS_INHERIT);
      init(entries[2], user, user_access, inherit_user ? SUB_CONTAINERS_AND_OBJECTS_INHERIT : NO_INHERITANCE);
      PACL acl = nullptr;
      if (SetEntriesInAclW(static_cast<ULONG>(entries.size()), entries.data(), nullptr, &acl) != ERROR_SUCCESS || !acl) return false;
      const DWORD result = SetSecurityInfo(directory, SE_FILE_OBJECT,
        DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION, nullptr, nullptr, acl, nullptr);
      LocalFree(acl);
      return result == ERROR_SUCCESS;
    }

    bool verify_protected_user_acl(HANDLE object, PSID user, ACCESS_MASK expected) {
      PSECURITY_DESCRIPTOR descriptor = nullptr;
      if (GetSecurityInfo(object, SE_FILE_OBJECT, DACL_SECURITY_INFORMATION, nullptr, nullptr, nullptr, nullptr, &descriptor) != ERROR_SUCCESS || !descriptor) return false;
      BOOL present = FALSE, defaulted = FALSE; PACL dacl = nullptr;
      SECURITY_DESCRIPTOR_CONTROL control {}; DWORD revision = 0;
      std::array<std::uint8_t, SECURITY_MAX_SID_SIZE> system_sid {}, admin_sid {};
      DWORD system_size = static_cast<DWORD>(system_sid.size()), admin_size = static_cast<DWORD>(admin_sid.size());
      const bool known_sids = CreateWellKnownSid(WinLocalSystemSid, nullptr, system_sid.data(), &system_size) &&
        CreateWellKnownSid(WinBuiltinAdministratorsSid, nullptr, admin_sid.data(), &admin_size);
      bool user_ok = false, system_ok = false, admin_ok = false;
      if (GetSecurityDescriptorDacl(descriptor, &present, &dacl, &defaulted) && present && dacl && user) {
        for (DWORD index = 0; index < dacl->AceCount; ++index) {
          void *raw_ace = nullptr;
          if (!GetAce(dacl, index, &raw_ace) || !raw_ace) continue;
          const auto *ace = static_cast<const ACCESS_ALLOWED_ACE *>(raw_ace);
          if (ace->Header.AceType == ACCESS_ALLOWED_ACE_TYPE && known_sids && EqualSid(&ace->SidStart, system_sid.data())) {
            system_ok = (ace->Mask & FILE_ALL_ACCESS) == FILE_ALL_ACCESS;
          }
          if (ace->Header.AceType == ACCESS_ALLOWED_ACE_TYPE && known_sids && EqualSid(&ace->SidStart, admin_sid.data())) {
            admin_ok = (ace->Mask & FILE_ALL_ACCESS) == FILE_ALL_ACCESS;
          }
          if (ace->Header.AceType == ACCESS_ALLOWED_ACE_TYPE && EqualSid(&ace->SidStart, user)) {
            constexpr ACCESS_MASK forbidden = WRITE_DAC | WRITE_OWNER;
            user_ok = (ace->Mask & expected) == expected && (ace->Mask & forbidden) == 0;
            break;
          }
        }
      }
      const bool valid = present && dacl && dacl->AceCount == 3 && system_ok && admin_ok && user_ok &&
        GetSecurityDescriptorControl(descriptor, &control, &revision) && (control & SE_DACL_PROTECTED);
      LocalFree(descriptor);
      return valid;
    }

    bool apply_client_acl_tree(const std::filesystem::path &root, PSID user, const ACCESS_MASK access) {
      HANDLE raw = CreateFileW(root.c_str(), READ_CONTROL | WRITE_DAC | FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
      if (raw == INVALID_HANDLE_VALUE) return false;
      unique_handle opened {raw}; FILE_ATTRIBUTE_TAG_INFO tag {};
      if (!GetFileInformationByHandleEx(opened.get(), FileAttributeTagInfo, &tag, sizeof(tag)) ||
          (tag.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) || !set_leaf_acl(opened.get(), user, access, false) ||
          !verify_protected_user_acl(opened.get(), user, access)) return false;
      if (!(tag.FileAttributes & FILE_ATTRIBUTE_DIRECTORY)) return true;
      std::error_code ec;
      for (std::filesystem::directory_iterator it(root, std::filesystem::directory_options::none, ec), end; it != end && !ec; it.increment(ec)) {
        if (!apply_client_acl_tree(it->path(), user, access)) return false;
      }
      return !ec;
    }

    std::optional<std::filesystem::path> program_data() {
      PWSTR raw = nullptr;
      if (FAILED(SHGetKnownFolderPath(FOLDERID_ProgramData, KF_FLAG_DEFAULT, nullptr, &raw)) || !raw) return std::nullopt;
      std::filesystem::path path {raw}; CoTaskMemFree(raw); return path;
    }

    bool copy_regular_file(HANDLE token, const std::filesystem::path &source, const std::filesystem::path &destination,
                           const std::uintmax_t remaining_budget, std::uintmax_t &copied_size) {
      copied_size = 0;
      HANDLE raw = INVALID_HANDLE_VALUE;
      LARGE_INTEGER source_size {};
      FILE_ID_INFO source_identity {};
      {
        impersonation_scope impersonating {token};
        if (!impersonating.active) return false;
        raw = CreateFileW(source.c_str(), FILE_READ_ATTRIBUTES | FILE_READ_DATA,
          FILE_SHARE_READ, nullptr, OPEN_EXISTING,
          FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
        if (raw == INVALID_HANDLE_VALUE) return false;
        unique_handle source_handle {raw}; FILE_ATTRIBUTE_TAG_INFO tag {};
        if (!GetFileInformationByHandleEx(source_handle.get(), FileAttributeTagInfo, &tag, sizeof(tag)) ||
            (tag.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) return false;
        if (!GetFileInformationByHandleEx(source_handle.get(), FileIdInfo, &source_identity, sizeof(source_identity)) ||
            !GetFileSizeEx(source_handle.get(), &source_size) || source_size.QuadPart < 0 ||
            static_cast<std::uintmax_t>(source_size.QuadPart) > max_file_bytes ||
            static_cast<std::uintmax_t>(source_size.QuadPart) > remaining_budget) return false;
        raw = source_handle.release();
      }
      unique_handle source_handle {raw};
      HANDLE destination_raw = CreateFileW(destination.c_str(), GENERIC_WRITE | FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
      if (destination_raw == INVALID_HANDLE_VALUE) return false;
      unique_handle destination_handle {destination_raw}; FILE_ATTRIBUTE_TAG_INFO destination_tag {};
      if (!GetFileInformationByHandleEx(destination_handle.get(), FileAttributeTagInfo, &destination_tag, sizeof(destination_tag)) ||
          (destination_tag.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) return false;
      std::array<std::byte, 1 << 16> buffer {};
      std::uintmax_t copied = 0;
      while (copied < static_cast<std::uintmax_t>(source_size.QuadPart)) {
        const DWORD request = static_cast<DWORD>(std::min<std::uintmax_t>(buffer.size(), static_cast<std::uintmax_t>(source_size.QuadPart) - copied));
        DWORD read = 0, written = 0;
        if (!ReadFile(source_handle.get(), buffer.data(), request, &read, nullptr) || read == 0 ||
            !WriteFile(destination_handle.get(), buffer.data(), read, &written, nullptr) || written != read) return false;
        copied += written;
      }
      FILE_ID_INFO after_identity {};
      LARGE_INTEGER final_size {};
      const bool stable = GetFileInformationByHandleEx(source_handle.get(), FileIdInfo, &after_identity, sizeof(after_identity)) &&
        GetFileSizeEx(source_handle.get(), &final_size) && final_size.QuadPart == source_size.QuadPart &&
        std::memcmp(&source_identity, &after_identity, sizeof(source_identity)) == 0;
      if (!stable || copied != static_cast<std::uintmax_t>(source_size.QuadPart)) return false;
      copied_size = copied;
      return true;
    }

    bool create_owned_path(const std::filesystem::path &root, const std::filesystem::path &target) {
      const auto relative = target.lexically_relative(root);
      if (relative.empty() || relative.native().starts_with(L"..")) return false;
      auto current = root;
      for (const auto &part : relative) {
        current /= part;
        bool created = false;
        if (!make_protected_directory(current, created) || !open_secure_directory(current, true)) return false;
      }
      return true;
    }

    bool remove_owned_tree(const std::filesystem::path &path) {
      // Deletion is only called for a SYSTEM-owned path below the protected
      // ProgramData root. Every entry is opened with OPEN_REPARSE_POINT; a
      // reparse entry is rejected instead of followed or recursively removed.
      std::error_code ec;
      if (!std::filesystem::exists(path, ec)) return true;
      HANDLE raw = CreateFileW(path.c_str(), FILE_READ_ATTRIBUTES | DELETE,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
      if (raw == INVALID_HANDLE_VALUE) return false;
      unique_handle opened {raw}; FILE_ATTRIBUTE_TAG_INFO tag {};
      if (!GetFileInformationByHandleEx(opened.get(), FileAttributeTagInfo, &tag, sizeof(tag)) ||
          (tag.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) return false;
      if (tag.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
        for (std::filesystem::directory_iterator it(path, std::filesystem::directory_options::none, ec), end; it != end && !ec; it.increment(ec)) {
          if (!remove_owned_tree(it->path())) return false;
        }
        return !ec && RemoveDirectoryW(path.c_str()) != FALSE;
      }
      return !ec && DeleteFileW(path.c_str()) != FALSE;
    }

    bool reconcile_generation_root(const std::filesystem::path &root, std::string &error) {
      auto opened = open_secure_directory(root, true);
      if (!opened) {
        // The caller pins the protected ProgramData/owned/seat parents and
        // creates this component immediately before reconciliation.  A
        // missing generation is therefore an empty, safe admission state;
        // any existing component must pass handle validation above.
        return true;
      }
      std::error_code ec;
      // A generation with any client/cache/staging content may still have a
      // live clone from an earlier service lifetime. Keep it blocked and force
      // a fresh generation rather than deleting an unproven live tree.
      for (std::filesystem::directory_iterator it(root, std::filesystem::directory_options::none, ec), end; it != end && !ec; it.increment(ec)) {
        if (!it->is_directory(ec) || it->is_symlink(ec)) { error = "An existing Steam generation contains an unsafe entry."; return false; }
        error = "An occupied Steam generation root remains pending reconciliation.";
        return false;
      }
      return !ec;
    }
#endif

    std::string narrow(const std::filesystem::path &path) {
      try {
        const auto value = path.u8string();
        return {reinterpret_cast<const char *>(value.data()), value.size()};
      } catch (...) {
        return {};
      }
    }

    bool under_root(const std::filesystem::path &path, const std::filesystem::path &root) {
      const auto p = path.lexically_normal();
      const auto r = root.lexically_normal();
      auto pit = p.begin();
      for (auto rit = r.begin(); rit != r.end(); ++rit, ++pit) {
        if (pit == p.end()) return false;
        auto lhs = pit->wstring(); auto rhs = rit->wstring();
        std::ranges::transform(lhs, lhs.begin(), [](wchar_t ch) { return static_cast<wchar_t>(std::towlower(ch)); });
        std::ranges::transform(rhs, rhs.begin(), [](wchar_t ch) { return static_cast<wchar_t>(std::towlower(ch)); });
        if (lhs != rhs) return false;
      }
      return true;
    }

    bool excluded_directory(const std::filesystem::path &relative) {
      return std::ranges::any_of(relative, [](const auto &part) {
        auto value = part.wstring();
        std::ranges::transform(value, value.begin(), [](wchar_t ch) {
          return static_cast<wchar_t>(std::towlower(ch));
        });
        return std::ranges::any_of(volatile_directory_names, [&](std::wstring_view name) {
          return value == name;
        });
      });
    }

    std::string digest_manifest(const std::vector<std::filesystem::path> &files) {
      // This is an ownership/change token, not a cryptographic authorization
      // token.  WFP's canonical app IDs are the security boundary.
      std::uint64_t hash = 1469598103934665603ULL;
      for (const auto &path : files) {
        for (const auto ch : narrow(path)) {
          hash ^= static_cast<unsigned char>(ch);
          hash *= 1099511628211ULL;
        }
      }
      std::ostringstream out;
      out << std::hex << hash;
      return out.str();
    }

#ifdef _WIN32
    bool local_system() {
      HANDLE token = nullptr;
      if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return false;
      PSID sid = nullptr;
      DWORD bytes = 0;
      GetTokenInformation(token, TokenUser, nullptr, 0, &bytes);
      std::vector<std::byte> storage(bytes);
      const bool ok = GetTokenInformation(token, TokenUser, storage.data(), bytes, &bytes);
      if (ok) sid = static_cast<TOKEN_USER *>(static_cast<void *>(storage.data()))->User.Sid;
      std::array<std::byte, SECURITY_MAX_SID_SIZE> system_storage {};
      DWORD system_bytes = static_cast<DWORD>(system_storage.size());
      const bool system_ok = CreateWellKnownSid(WinLocalSystemSid, nullptr, system_storage.data(), &system_bytes);
      const bool result = ok && system_ok && EqualSid(sid, system_storage.data()) != FALSE;
      CloseHandle(token);
      return result;
    }

    std::array<std::uint8_t, 16> key_for(std::string_view value) {
      std::array<std::uint8_t, 16> key {};
      std::uint64_t a = 1469598103934665603ULL;
      std::uint64_t b = 1099511628211ULL;
      for (const auto ch : value) {
        a ^= static_cast<unsigned char>(ch); a *= 1099511628211ULL;
        b ^= static_cast<unsigned char>(ch + 17); b *= 1469598103934665603ULL;
      }
      std::memcpy(key.data(), &a, sizeof(a));
      std::memcpy(key.data() + sizeof(a), &b, sizeof(b));
      key[8] = static_cast<std::uint8_t>((key[8] & 0x0f) | 0x40);
      key[10] = static_cast<std::uint8_t>((key[10] & 0x3f) | 0x80);
      return key;
    }

    GUID guid(const std::array<std::uint8_t, 16> &key) {
      GUID result {};
      std::memcpy(&result, key.data(), sizeof(result));
      return result;
    }

    constexpr std::array<std::uint8_t, 16> provider_key_bytes {
      0x8c, 0x8d, 0x1d, 0x8e, 0x40, 0x57, 0x4e, 0x45, 0xa8, 0x2c, 0x17, 0xb7, 0x4f, 0x50, 0x44, 0x5a};
    constexpr std::array<std::uint8_t, 16> sublayer_key_bytes {
      0x41, 0x0d, 0x69, 0x1f, 0xe4, 0x11, 0x45, 0x2c, 0x9d, 0xe7, 0x60, 0x1b, 0xae, 0xc4, 0x5f, 0x22, 0x09};

    std::wstring service_epoch() {
      static const std::wstring epoch = [] {
        GUID value {};
        if (CoCreateGuid(&value) != S_OK) return std::wstring {L"epoch-unavailable"};
        wchar_t text[64] {};
        StringFromGUID2(value, text, ARRAYSIZE(text));
        return std::wstring {text};
      }();
      return epoch;
    }

    bool add_filter(HANDLE engine, const GUID &filter_key, const GUID &provider_key,
                    const GUID &sublayer_key, const GUID &layer_key, PBYTE app_id, UINT32 app_id_size,
                    std::wstring_view owner, std::string &error) {
      const std::wstring description {owner};
      FWPM_FILTER0 filter {};
      filter.filterKey = filter_key;
      filter.displayData.name = const_cast<wchar_t *>(L"Vibeshine Steam seat client block");
      filter.displayData.description = const_cast<wchar_t *>(description.c_str());
      filter.providerKey = const_cast<GUID *>(&provider_key);
      filter.layerKey = layer_key;
      filter.subLayerKey = sublayer_key;
      filter.weight.type = FWP_EMPTY;
      filter.flags = FWPM_FILTER_FLAG_PERSISTENT;
      FWPM_FILTER_CONDITION0 condition {};
      condition.fieldKey = FWPM_CONDITION_ALE_APP_ID;
      condition.matchType = FWP_MATCH_EQUAL;
      condition.conditionValue.type = FWP_BYTE_BLOB_TYPE;
      condition.conditionValue.byteBlob = new FWP_BYTE_BLOB {app_id_size, app_id};
      filter.numFilterConditions = 1;
      filter.filterCondition = &condition;
      filter.action.type = FWP_ACTION_BLOCK;
      const auto status = FwpmFilterAdd0(engine, &filter, nullptr, nullptr);
      delete condition.conditionValue.byteBlob;
      if (status != ERROR_SUCCESS) {
        error = "WFP filter creation failed (" + std::to_string(status) + ").";
        return false;
      }
      return true;
    }

    struct filter_owner { std::wstring seat; std::wstring epoch; std::uint64_t generation {}; };

    bool parse_filter_owner(const wchar_t *description, filter_owner &owner) {
      if (!description) return false;
      constexpr std::wstring_view prefix = L"VibeshineSteamSeat|seat=";
      const std::wstring_view value {description};
      if (!value.starts_with(prefix)) return false;
      const auto epoch_tag = value.find(L"|epoch=");
      const auto generation_tag = value.find(L"|generation=");
      if (epoch_tag == std::wstring_view::npos || generation_tag == std::wstring_view::npos ||
          epoch_tag <= prefix.size() || generation_tag <= epoch_tag + 7) return false;
      owner.seat.assign(value.substr(prefix.size(), epoch_tag - prefix.size()));
      owner.epoch.assign(value.substr(epoch_tag + 7, generation_tag - epoch_tag - 7));
      if (owner.epoch.empty()) return false;
      const auto number = value.substr(generation_tag + 12);
      if (number.empty() || !std::ranges::all_of(number, [](wchar_t ch) { return ch >= L'0' && ch <= L'9'; })) return false;
      try { owner.generation = std::stoull(std::wstring {number}); } catch (...) { return false; }
      std::string seat; seat.reserve(owner.seat.size());
      for (const wchar_t ch : owner.seat) {
        if (ch > 0x7f) return false;
        seat.push_back(static_cast<char>(ch));
      }
      return valid_seat(seat);
    }

    steam_offline::protected_generation_state protected_component_state(const std::filesystem::path &path) {
      SetLastError(ERROR_SUCCESS);
      HANDLE raw = CreateFileW(path.c_str(), FILE_READ_ATTRIBUTES | READ_CONTROL,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
      if (raw == INVALID_HANDLE_VALUE) {
        const auto last = GetLastError();
        return last == ERROR_FILE_NOT_FOUND || last == ERROR_PATH_NOT_FOUND
          ? steam_offline::protected_generation_state::absent
          : steam_offline::protected_generation_state::unknown;
      }
      unique_handle opened {raw};
      return secure_directory(opened.get(), true)
        ? steam_offline::protected_generation_state::present
        : steam_offline::protected_generation_state::unknown;
    }

    steam_offline::protected_generation_state generation_root_state(const filter_owner &owner) {
      if (owner.epoch.size() != 38 || owner.epoch.front() != L'{' || owner.epoch.back() != L'}' ||
          owner.epoch.find_first_of(L"\\/") != std::wstring::npos)
        return steam_offline::protected_generation_state::unknown;
      const auto root = program_data();
      if (!root || !open_no_reparse_directory(*root)) return steam_offline::protected_generation_state::unknown;
      const auto owned = *root / L"VibeshineSteamSeats";
      const auto seat = owned / owner.seat;
      const auto epoch = seat / (L"epoch-" + owner.epoch);
      const auto generation = epoch / (L"generation-" + std::to_wstring(owner.generation));
      for (const auto &component : {owned, seat, epoch}) {
        const auto state = protected_component_state(component);
        if (state != steam_offline::protected_generation_state::present)
          return state == steam_offline::protected_generation_state::absent
            ? steam_offline::protected_generation_state::absent
            : steam_offline::protected_generation_state::unknown;
      }
      // A successfully opened and securely validated generation is PRESENT,
      // regardless of whether its contents can be enumerated here.
      return protected_component_state(generation);
    }

    bool remove_stale_filters(HANDLE engine, std::wstring_view seat, std::uint64_t generation, std::string &error) {
      const auto provider = guid(provider_key_bytes);
      const auto sublayer = guid(sublayer_key_bytes);
      FWPM_FILTER_ENUM_TEMPLATE0 templ {};
      templ.providerKey = const_cast<GUID *>(&provider);
      HANDLE enumeration = nullptr;
      auto status = FwpmFilterCreateEnumHandle0(engine, &templ, &enumeration);
      if (status != ERROR_SUCCESS || !enumeration) { error = "WFP stale-filter enumeration could not start (" + std::to_string(status) + ")."; return false; }
      constexpr UINT32 page_size = 128, max_pages = 128, max_objects = page_size * max_pages;
      status = FwpmTransactionBegin0(engine, 0);
      UINT32 pages = 0, objects = 0; bool complete = false;
      bool ok = status == ERROR_SUCCESS;
      while (ok && pages <= max_pages) {
        UINT32 count = 0; FWPM_FILTER0 **filters = nullptr;
        status = FwpmFilterEnum0(engine, enumeration, page_size, &filters, &count);
        if (status != ERROR_SUCCESS) { ok = false; break; }
        if (count == 0) { complete = true; if (filters) FwpmFreeMemory0(reinterpret_cast<void **>(&filters)); break; }
        ++pages;
        if (pages > max_pages) { ok = false; if (filters) FwpmFreeMemory0(reinterpret_cast<void **>(&filters)); break; }
        objects += count;
        if (objects > max_objects) { ok = false; if (filters) FwpmFreeMemory0(reinterpret_cast<void **>(&filters)); break; }
        for (UINT32 i = 0; i < count && ok; ++i) {
          filter_owner parsed;
          if (!filters[i] || !filters[i]->subLayerKey || std::memcmp(filters[i]->subLayerKey, &sublayer, sizeof(sublayer)) != 0) continue;
          if (!parse_filter_owner(filters[i]->displayData.description, parsed)) { ok = false; break; }
          if (parsed.seat == seat && parsed.generation != generation &&
              steam_offline::stale_filters_removal_allowed(generation_root_state(parsed))) {
            status = FwpmFilterDeleteByKey0(engine, &filters[i]->filterKey);
            ok = status == ERROR_SUCCESS || status == FWP_E_FILTER_NOT_FOUND;
          }
        }
        if (filters) FwpmFreeMemory0(reinterpret_cast<void **>(&filters));
      }
      if (!complete && ok) ok = false;
      if (ok) status = FwpmTransactionCommit0(engine);
      if (!ok || status != ERROR_SUCCESS) {
        (void)FwpmTransactionAbort0(engine);
        error = "WFP stale-filter reconciliation failed; cleanup remains pending (" + std::to_string(status) + ").";
      }
      (void)FwpmFilterDestroyEnumHandle0(engine, enumeration);
      return ok && status == ERROR_SUCCESS;
    }

    bool validate_owned_schema(HANDLE engine, const GUID &provider_key, const GUID &sublayer_key, std::string &error) {
      FWPM_PROVIDER0 *provider = nullptr; FWPM_SUBLAYER0 *sublayer = nullptr;
      auto status = FwpmProviderGetByKey0(engine, &provider_key, &provider);
      const bool provider_ok = status == ERROR_SUCCESS && provider && provider->flags & FWPM_PROVIDER_FLAG_PERSISTENT &&
        provider->displayData.name && std::wstring_view {provider->displayData.name} == L"Vibeshine Steam user-mode isolation";
      if (provider) FwpmFreeMemory0(reinterpret_cast<void **>(&provider));
      status = FwpmSubLayerGetByKey0(engine, &sublayer_key, &sublayer);
      const bool sublayer_ok = status == ERROR_SUCCESS && sublayer && sublayer->providerKey &&
        std::memcmp(sublayer->providerKey, &provider_key, sizeof(provider_key)) == 0 &&
        (sublayer->flags & FWPM_SUBLAYER_FLAG_PERSISTENT) && sublayer->weight == 0xffff;
      if (sublayer) FwpmFreeMemory0(reinterpret_cast<void **>(&sublayer));
      if (!provider_ok || !sublayer_ok) { error = "Owned WFP provider/sublayer schema is not exact; isolation is refused."; return false; }
      return true;
    }
#endif
  }

  manager_t::~manager_t() {
    std::string ignored;
    (void)release(ignored);
  }

  manager_t::manager_t(manager_t &&other) noexcept
      : engine_(other.engine_), filter_keys_(std::move(other.filter_keys_)), preparation_(std::move(other.preparation_)),
        seat_id_(std::move(other.seat_id_)), generation_(other.generation_), quarantined_(other.quarantined_) {
    other.engine_ = nullptr;
    other.generation_ = 0;
    other.quarantined_ = false;
  }

  manager_t &manager_t::operator=(manager_t &&other) noexcept {
    if (this == &other) return *this;
    std::string ignored;
    (void)release(ignored);
    engine_ = other.engine_;
    filter_keys_ = std::move(other.filter_keys_);
    preparation_ = std::move(other.preparation_);
    seat_id_ = std::move(other.seat_id_);
    generation_ = other.generation_;
    quarantined_ = other.quarantined_;
    other.engine_ = nullptr;
    other.generation_ = 0;
    other.quarantined_ = false;
    return *this;
  }

  bool manager_t::available(std::string &error) noexcept {
#ifdef _WIN32
    if (!local_system()) { error = "Steam isolation requires the LocalSystem broker."; return false; }
    HANDLE engine = nullptr;
    const auto status = FwpmEngineOpen0(nullptr, RPC_C_AUTHN_WINNT, nullptr, nullptr, &engine);
    if (status != ERROR_SUCCESS || !engine) {
      error = "Windows Filtering Platform/BFE is unavailable (" + std::to_string(status) + ").";
      if (engine) FwpmEngineClose0(engine);
      return false;
    }
    FwpmEngineClose0(engine);
    return true;
#else
    error = "Steam isolation is Windows-only.";
    return false;
#endif
  }

  bool manager_t::prepare(const std::filesystem::path &steam_executable,
                          const std::filesystem::path &proxy_executable,
                          void *source_impersonation_token,
                          const std::string_view seat_id, const std::string_view user_sid,
                          const std::uint64_t generation,
                          preparation_t &result, std::string &error) noexcept {
    if (active() || !valid_seat(seat_id) || user_sid.empty() || generation == 0) {
      error = "Steam isolation admission identity is invalid or already active.";
      return false;
    }
    // Filter identities are transaction-local until BFE commit succeeds.
    filter_keys_.clear();
    preparation_ = {};
#ifdef _WIN32
    if (!local_system()) { error = "Steam isolation preparation must run in the SYSTEM broker."; return false; }
    const auto source_token = static_cast<HANDLE>(source_impersonation_token);
    if (!source_token || !token_matches_sid(source_token, std::wstring {user_sid.begin(), user_sid.end()})) {
      error = "Steam isolation requires a duplicated source token for the exact console user SID.";
      return false;
    }
    std::error_code ec;
    std::filesystem::path source;
    {
      impersonation_scope impersonating {source_token};
      if (!impersonating.active) { error = "Steam source impersonation could not be established."; return false; }
      source = std::filesystem::weakly_canonical(steam_executable, ec);
      auto source_name = source.filename().wstring();
      std::ranges::transform(source_name, source_name.begin(), [](wchar_t ch) { return static_cast<wchar_t>(std::towlower(ch)); });
      if (ec || !std::filesystem::is_regular_file(source, ec) || source_name != L"steam.exe") {
        error = "The trusted Steam command is not a canonical steam.exe file.";
        return false;
      }
    }
    const auto source_root = source.parent_path();
    {
      impersonation_scope impersonating {source_token};
      if (!impersonating.active || !open_no_reparse_directory(source_root)) {
        error = "The trusted Steam source root is a reparse point or could not be opened safely as the console user.";
        return false;
      }
    }
    std::filesystem::path proxy;
    {
      // Resolve and compare the proxy while the same validated console token
      // is impersonated as the source.  SYSTEM must not canonicalize a
      // user-raceable ancestor and then use that result for disclosure policy.
      impersonation_scope impersonating {source_token};
      if (!impersonating.active) { error = "Steam proxy impersonation could not be established."; return false; }
      proxy = std::filesystem::weakly_canonical(proxy_executable, ec);
      if (ec || !std::filesystem::is_regular_file(proxy, ec) || under_root(proxy, source_root)) {
        error = "The packaged steam webhelper proxy is missing or is inside the Steam source tree.";
        return false;
      }
    }

    HANDLE engine = nullptr;
    auto status = FwpmEngineOpen0(nullptr, RPC_C_AUTHN_WINNT, nullptr, nullptr, &engine);
    if (status != ERROR_SUCCESS || !engine) { error = "BFE engine open failed (" + std::to_string(status) + ")."; return false; }
    std::wstring seat_wide;
    seat_wide.reserve(seat_id.size());
    for (const auto ch : seat_id) seat_wide.push_back(static_cast<wchar_t>(static_cast<unsigned char>(ch)));
    FWPM_PROVIDER0 provider {};
    const auto provider_key = guid(provider_key_bytes);
    provider.providerKey = provider_key;
    provider.flags = FWPM_PROVIDER_FLAG_PERSISTENT;
    provider.displayData.name = const_cast<wchar_t *>(L"Vibeshine Steam user-mode isolation");
    provider.displayData.description = const_cast<wchar_t *>(L"Persistent SYSTEM-owned Steam client seat filters");
    FWPM_SUBLAYER0 sublayer {};
    const auto sublayer_key = guid(sublayer_key_bytes);
    sublayer.subLayerKey = sublayer_key;
    sublayer.displayData.name = const_cast<wchar_t *>(L"Vibeshine Steam seat isolation");
    sublayer.providerKey = const_cast<GUID *>(&provider_key);
    // Highest ordinary user-mode sublayer weight. This is not a callout veto:
    // administrator/higher-priority policy may still override the admission.
    sublayer.weight = 0xffff;
    sublayer.flags = FWPM_SUBLAYER_FLAG_PERSISTENT;
    const auto program_data_root = program_data();
    if (!program_data_root) { FwpmEngineClose0(engine); error = "The OS ProgramData root could not be resolved."; return false; }
    const auto owned_root = *program_data_root / L"VibeshineSteamSeats";
    const auto seat_root = owned_root / seat_wide;
    const auto epoch = service_epoch();
    if (epoch == L"epoch-unavailable") { FwpmEngineClose0(engine); error = "A unique service epoch could not be established."; return false; }
    const auto epoch_root = seat_root / (L"epoch-" + epoch);
    const auto generation_root = epoch_root / (L"generation-" + std::to_wstring(generation));
    GUID stage_id {};
    if (CoCreateGuid(&stage_id) != S_OK) { FwpmEngineClose0(engine); error = "Secure staging identity could not be generated."; return false; }
    wchar_t stage_name[64] {};
    StringFromGUID2(stage_id, stage_name, ARRAYSIZE(stage_name));
    const auto stage = seat_root / (L".staging-" + std::wstring {stage_name});
    const auto mirror = generation_root / L"client";
    const auto cache = generation_root / L"cache";
    const auto discard_published = [&] {
      (void)remove_owned_tree(mirror);
      (void)remove_owned_tree(cache);
    };
    auto owned = ensure_protected_directory(owned_root);
    auto seat = owned ? ensure_protected_directory(seat_root) : unique_handle {};
    auto epoch_handle = seat ? ensure_protected_directory(epoch_root) : unique_handle {};
    auto generation_handle = epoch_handle ? ensure_protected_directory(generation_root) : unique_handle {};
    bool stage_created = false;
    if (generation_handle) stage_created = make_protected_directory(stage, stage_created);
    auto stage_handle = stage_created ? open_secure_directory(stage, true) : unique_handle {};
    std::vector<std::filesystem::path> executables;
    std::vector<std::filesystem::path> original_client_executables;
    std::size_t files = 0;
    std::size_t directories = 0, entries = 0;
    std::uintmax_t tree_bytes = 0;
    bool copy_failed = false;
    constexpr std::size_t max_directories = 4096, max_entries = 16384, max_executables = 8192;
    if (!owned || !seat || !epoch_handle || !generation_handle || !stage_handle) { FwpmEngineClose0(engine); error = "The SYSTEM-owned Steam mirror root could not be opened safely."; return false; }
    if (!reconcile_generation_root(generation_root, error)) { FwpmEngineClose0(engine); return false; }
    std::vector<std::pair<std::filesystem::path, std::filesystem::path>> pending;
    pending.emplace_back(source_root, std::filesystem::path {});
    while (!pending.empty() && !copy_failed) {
      const auto [directory, directory_relative] = std::move(pending.back()); pending.pop_back();
      if (++directories > max_directories || directory_relative.native().size() > max_path_chars ||
          static_cast<std::size_t>(std::distance(directory_relative.begin(), directory_relative.end())) > max_depth) { copy_failed = true; break; }
      std::vector<std::filesystem::path> children;
      {
        impersonation_scope impersonating {source_token};
        if (!impersonating.active || !open_no_reparse_directory(directory)) { copy_failed = true; break; }
        std::error_code enumerate_error;
        for (std::filesystem::directory_iterator it(directory, std::filesystem::directory_options::none, enumerate_error), end;
             it != end && !enumerate_error; it.increment(enumerate_error)) {
          children.push_back(it->path());
          if (++entries > max_entries) { copy_failed = true; break; }
        }
        if (enumerate_error) copy_failed = true;
      }
      for (const auto &child : children) {
        if (copy_failed) break;
        const auto rel = child.lexically_relative(source_root);
        if (rel.empty() || rel.native().size() > max_path_chars ||
            static_cast<std::size_t>(std::distance(rel.begin(), rel.end())) > max_depth) { copy_failed = true; break; }
        std::filesystem::file_status status {};
        {
          impersonation_scope impersonating {source_token};
          if (!impersonating.active) { copy_failed = true; break; }
          status = std::filesystem::symlink_status(child, ec);
          if (ec) { copy_failed = true; break; }
          if (std::filesystem::is_symlink(status) || std::filesystem::is_other(status)) { copy_failed = true; break; }
        }
        if (excluded_directory(rel)) { if (std::filesystem::is_directory(status)) continue; else { copy_failed = true; break; } }
        if (std::filesystem::is_directory(status)) {
          {
            impersonation_scope impersonating {source_token};
            if (!impersonating.active || !open_no_reparse_directory(child)) { copy_failed = true; break; }
          }
          if (!create_owned_path(stage, stage / rel)) { copy_failed = true; break; }
          pending.emplace_back(child, rel);
          continue;
        }
        if (!std::filesystem::is_regular_file(status)) { copy_failed = true; break; }
        if (++files > max_files || tree_bytes > max_tree_bytes || executables.size() > max_executables) { copy_failed = true; break; }
        const auto destination = stage / rel;
        std::uintmax_t copied_size = 0;
        if (!under_root(destination, stage) || !create_owned_path(stage, destination.parent_path()) ||
            !copy_regular_file(source_token, child, destination, max_tree_bytes - tree_bytes, copied_size)) { copy_failed = true; break; }
        if (copied_size > max_file_bytes || copied_size > max_tree_bytes - tree_bytes) { copy_failed = true; break; }
        tree_bytes += copied_size;
        auto lower_name = destination.filename().wstring(); std::ranges::transform(lower_name, lower_name.begin(), [](wchar_t ch) { return static_cast<wchar_t>(std::towlower(ch)); });
        if (lower_name == L"steamservice.exe") { if (!DeleteFileW(destination.c_str())) { copy_failed = true; break; } continue; }
        auto extension = destination.extension().wstring();
        std::ranges::transform(extension, extension.begin(), [](wchar_t ch) { return static_cast<wchar_t>(std::towlower(ch)); });
        if (extension == L".exe") {
          executables.push_back(destination);
          auto source_name = child.filename().wstring();
          std::ranges::transform(source_name, source_name.begin(), [](wchar_t ch) { return static_cast<wchar_t>(std::towlower(ch)); });
          if (source_name != L"steamservice.exe") original_client_executables.push_back(child);
          if (executables.size() > max_executables) copy_failed = true;
        }
      }
    }
    if (ec || copy_failed || executables.empty()) {
      (void)remove_owned_tree(stage); FwpmEngineClose0(engine); error = "Steam mirror could not be completely enumerated."; return false;
    }
    std::vector<std::filesystem::path> helper_paths;
    for (const auto &candidate : executables) {
      auto name = candidate.filename().wstring();
      std::ranges::transform(name, name.begin(), [](wchar_t ch) { return static_cast<wchar_t>(std::towlower(ch)); });
      if (name == L"steamwebhelper.exe") helper_paths.push_back(candidate);
    }
    if (helper_paths.empty()) { (void)remove_owned_tree(stage); FwpmEngineClose0(engine); error = "The Steam mirror contains no real steamwebhelper.exe."; return false; }
    const auto real_suffix = L".vibeshine-real.exe";
    for (const auto &helper : helper_paths) {
      const auto real_helper = helper.parent_path() / (helper.stem().wstring() + real_suffix);
      std::uintmax_t proxy_size = 0;
      if (std::filesystem::exists(real_helper, ec) ||
          !MoveFileExW(helper.c_str(), real_helper.c_str(), MOVEFILE_WRITE_THROUGH) ||
          !copy_regular_file(source_token, proxy, helper, max_tree_bytes - tree_bytes, proxy_size)) {
        (void)remove_owned_tree(stage); FwpmEngineClose0(engine); error = "Steam webhelper proxy publication collided or failed."; return false;
      }
      if (proxy_size > max_tree_bytes - tree_bytes) { (void)remove_owned_tree(stage); FwpmEngineClose0(engine); error = "Steam webhelper proxy exceeds mirror bounds."; return false; }
      tree_bytes += proxy_size;
      const auto old = std::find(executables.begin(), executables.end(), helper);
      if (old != executables.end()) *old = real_helper;
      executables.push_back(helper);
    }
    if (executables.empty() || executables.size() > max_executables || executables.size() > std::numeric_limits<std::size_t>::max() / 2) {
      (void)remove_owned_tree(stage); FwpmEngineClose0(engine); error = "The final Steam mirror executable count is outside the bounded admission policy."; return false;
    }
    PSID user = nullptr;
    std::wstring sid_wide; sid_wide.reserve(user_sid.size()); for (const auto ch : user_sid) sid_wide.push_back(static_cast<wchar_t>(static_cast<unsigned char>(ch)));
    if (!ConvertStringSidToSidW(sid_wide.c_str(), &user) || !user) { (void)remove_owned_tree(stage); FwpmEngineClose0(engine); error = "Steam seat user SID is invalid."; return false; }
    const auto user_guard = std::unique_ptr<void, decltype(&LocalFree)> {user, LocalFree};
    constexpr ACCESS_MASK user_read_execute = FILE_GENERIC_READ | FILE_GENERIC_EXECUTE | FILE_TRAVERSE | SYNCHRONIZE;
    constexpr ACCESS_MASK user_modify = user_read_execute | FILE_GENERIC_WRITE | DELETE | FILE_DELETE_CHILD;
    // No user ACL is visible until every staged object has an explicit,
    // protected DACL. Inheritance is not trusted across protected children.
    if (!apply_client_acl_tree(stage, user, user_read_execute)) {
      (void)remove_owned_tree(stage); FwpmEngineClose0(engine); error = "Steam mirror descendant ACL publication failed closed."; return false;
    }
    // The generation parent is SYSTEM/Admin-only. Publish exactly once, then
    // reopen and revalidate the published leaf before deriving AppIds.
    if (std::filesystem::exists(mirror, ec) || !MoveFileExW(stage.c_str(), mirror.c_str(), MOVEFILE_WRITE_THROUGH)) {
      (void)remove_owned_tree(stage); FwpmEngineClose0(engine); error = "Steam mirror publication failed closed."; return false;
    }
    auto mirror_handle = open_secure_directory(mirror, true, user);
    if (!mirror_handle) { discard_published(); FwpmEngineClose0(engine); error = "Published Steam mirror failed SYSTEM/reparse validation."; return false; }
    bool cache_created = false, html_created = false, profile_created = false;
    if (!make_protected_directory(cache, cache_created)) { discard_published(); FwpmEngineClose0(engine); error = "Steam cache root creation failed."; return false; }
    const auto html = cache / L"htmlcache"; const auto profile = cache / L"userdata";
    auto cache_handle = open_secure_directory(cache, true);
    const bool html_ready = cache_handle && make_protected_directory(html, html_created);
    const bool profile_ready = cache_handle && make_protected_directory(profile, profile_created);
    auto html_handle = html_ready ? open_secure_directory(html, true, user) : unique_handle {};
    auto profile_handle = profile_ready ? open_secure_directory(profile, true, user) : unique_handle {};
    if (!cache_handle || !html_handle || !profile_handle || !set_leaf_acl(cache_handle.get(), user, FILE_TRAVERSE | SYNCHRONIZE, false) ||
        !verify_protected_user_acl(cache_handle.get(), user, FILE_TRAVERSE | SYNCHRONIZE) || !set_leaf_acl(html_handle.get(), user, user_modify) ||
        !verify_protected_user_acl(html_handle.get(), user, user_modify) || !set_leaf_acl(profile_handle.get(), user, user_modify) ||
        !verify_protected_user_acl(profile_handle.get(), user, user_modify)) {
      discard_published(); FwpmEngineClose0(engine); error = "Seat-private Steam cache/profile ACL setup failed."; return false;
    }

    if (!remove_stale_filters(engine, seat_wide, generation, error)) { discard_published(); FwpmEngineClose0(engine); return false; }
    status = FwpmTransactionBegin0(engine, 0);
    if (status == ERROR_SUCCESS) status = FwpmProviderAdd0(engine, &provider, nullptr);
    if (status == FWP_E_ALREADY_EXISTS) status = ERROR_SUCCESS;
    if (status == ERROR_SUCCESS) status = FwpmSubLayerAdd0(engine, &sublayer, nullptr);
    if (status == FWP_E_ALREADY_EXISTS) status = ERROR_SUCCESS;
    if (status != ERROR_SUCCESS || !validate_owned_schema(engine, provider_key, sublayer_key, error)) {
      (void)FwpmTransactionAbort0(engine); discard_published(); FwpmEngineClose0(engine); if (error.empty()) error = "WFP provider/sublayer schema transaction failed."; return false;
    }

    std::vector<std::filesystem::path> published;
    published.reserve(executables.size());
    std::vector<std::array<std::uint8_t, 16>> staged_filter_keys;
    staged_filter_keys.reserve(executables.size() * 2);
    for (const auto &staged : executables) {
      const auto published_path = mirror / staged.lexically_relative(stage);
      PFWP_BYTE_BLOB app_id = nullptr;
      status = FwpmGetAppIdFromFileName0(published_path.c_str(), &app_id);
      if (status != ERROR_SUCCESS || !app_id || app_id->size == 0) {
        if (app_id) FwpmFreeMemory0(reinterpret_cast<void **>(&app_id));
        FwpmTransactionAbort0(engine); discard_published(); FwpmEngineClose0(engine); error = "Steam mirror app identity could not be canonicalized."; return false;
      }
      std::wstring owner = L"VibeshineSteamSeat|seat=" + seat_wide + L"|epoch=" + epoch + L"|generation=" + std::to_wstring(generation);
      const auto v4 = key_for(deterministic_filter_key(seat_id, generation, narrow(published_path), false));
      const auto v6 = key_for(deterministic_filter_key(seat_id, generation, narrow(published_path), true));
      if (!add_filter(engine, guid(v4), provider_key, sublayer_key, FWPM_LAYER_ALE_AUTH_CONNECT_V4, app_id->data, app_id->size, owner, error) ||
          !add_filter(engine, guid(v6), provider_key, sublayer_key, FWPM_LAYER_ALE_AUTH_CONNECT_V6, app_id->data, app_id->size, owner, error)) {
        FwpmFreeMemory0(reinterpret_cast<void **>(&app_id)); FwpmTransactionAbort0(engine); discard_published(); FwpmEngineClose0(engine); return false;
      }
      FwpmFreeMemory0(reinterpret_cast<void **>(&app_id));
      staged_filter_keys.push_back(v4); staged_filter_keys.push_back(v6); published.push_back(published_path);
    }
    if (published.size() != executables.size() || staged_filter_keys.size() != executables.size() * 2) {
      FwpmTransactionAbort0(engine); discard_published(); FwpmEngineClose0(engine); error = "The final Steam mirror/filter count is inconsistent."; return false;
    }
    status = FwpmTransactionCommit0(engine);
    if (status != ERROR_SUCCESS) { FwpmTransactionAbort0(engine); filter_keys_.clear(); discard_published(); FwpmEngineClose0(engine); error = "WFP filter transaction commit failed (" + std::to_string(status) + ")."; return false; }
    engine_ = engine; filter_keys_ = std::move(staged_filter_keys); seat_id_ = seat_id; generation_ = generation;
    preparation_ = {.mirror_root = mirror, .cache_root = cache, .steam_executable = mirror / L"steam.exe",
                    .proxy_executable = mirror / L"steamwebhelper.exe",
                    .original_client_executables = std::move(original_client_executables),
                    .manifest_digest = digest_manifest(published),
                    .filtered_executable_count = published.size()};
    result = preparation_;
    return true;
#else
    (void)steam_executable; (void)proxy_executable; (void)source_impersonation_token; (void)seat_id; (void)user_sid; (void)generation; (void)result;
    error = "Steam isolation is Windows-only."; return false;
#endif
  }

  bool manager_t::healthy(std::string &error) const noexcept {
    if (!active()) { error = "Steam isolation filters are not installed."; return false; }
#ifdef _WIN32
    if (!local_system()) { error = "Steam isolation health must be checked by the SYSTEM broker."; return false; }
    const auto provider = guid(provider_key_bytes); const auto sublayer = guid(sublayer_key_bytes);
    if (!validate_owned_schema(static_cast<HANDLE>(engine_), provider, sublayer, error)) return false;
    FWPM_FILTER_ENUM_TEMPLATE0 templ {}; templ.providerKey = const_cast<GUID *>(&provider);
    HANDLE enumeration = nullptr;
    auto status = FwpmFilterCreateEnumHandle0(static_cast<HANDLE>(engine_), &templ, &enumeration);
    if (status != ERROR_SUCCESS || !enumeration) { error = "Owned WFP filter health enumeration failed; reconnect is blocked."; return false; }
    std::vector<std::array<std::uint8_t, 16>> observed; observed.reserve(filter_keys_.size());
    constexpr UINT32 page_size = 128, max_pages = 256, max_objects = page_size * max_pages;
    bool ok = true, complete = false; UINT32 pages = 0;
    UINT32 total_objects = 0;
    while (ok && pages <= max_pages) {
      UINT32 count = 0; FWPM_FILTER0 **filters = nullptr;
      status = FwpmFilterEnum0(static_cast<HANDLE>(engine_), enumeration, page_size, &filters, &count);
      if (status != ERROR_SUCCESS) { ok = false; break; }
      if (count == 0) { complete = true; if (filters) FwpmFreeMemory0(reinterpret_cast<void **>(&filters)); break; }
      ++pages;
      if (pages > max_pages) { ok = false; if (filters) FwpmFreeMemory0(reinterpret_cast<void **>(&filters)); break; }
      total_objects += count;
      if (total_objects > max_objects) { ok = false; if (filters) FwpmFreeMemory0(reinterpret_cast<void **>(&filters)); break; }
      for (UINT32 i = 0; i < count; ++i) if (filters[i]) {
        filter_owner owner;
        if (!filters[i]->subLayerKey || std::memcmp(filters[i]->subLayerKey, &sublayer, sizeof(sublayer)) != 0 ||
            !parse_filter_owner(filters[i]->displayData.description, owner)) { ok = false; break; }
        if (owner.seat != std::wstring {seat_id_.begin(), seat_id_.end()} || owner.epoch != service_epoch() || owner.generation != generation_) continue;
        std::array<std::uint8_t, 16> key {}; std::memcpy(key.data(), &filters[i]->filterKey, sizeof(GUID)); observed.push_back(key);
        if (std::ranges::count(observed, key) != 1) { ok = false; break; }
      }
      if (filters) FwpmFreeMemory0(reinterpret_cast<void **>(&filters));
    }
    if (!complete || observed.size() != filter_keys_.size()) ok = false;
    if (ok) for (const auto &key : filter_keys_) if (std::ranges::find(observed, key) == observed.end()) { ok = false; break; }
    (void)FwpmFilterDestroyEnumHandle0(static_cast<HANDLE>(engine_), enumeration);
    if (!ok) { error = "A persistent owned Steam seat filter is missing or mismatched; reconnect is blocked."; return false; }
    return true;
#else
    error = "Steam isolation is Windows-only."; return false;
#endif
  }

  bool manager_t::release(std::string &error) noexcept {
    if (!engine_) return true;
#ifdef _WIN32
    if (quarantined_) { error = "Steam seat remains quarantined because worker termination was not proven; filters stay installed."; return false; }
    if (!local_system()) { error = "Steam isolation cleanup must run by the SYSTEM broker."; return false; }
    const auto program_data_root = program_data();
    if (!program_data_root || !under_root(preparation_.mirror_root, *program_data_root) ||
        !under_root(preparation_.cache_root, *program_data_root) || !remove_owned_tree(preparation_.mirror_root) ||
        !remove_owned_tree(preparation_.cache_root)) {
      error = "The isolated Steam mirror/cache could not be securely removed; filters remain installed.";
      return false;
    }
    auto *engine = static_cast<HANDLE>(engine_);
    auto status = FwpmTransactionBegin0(engine, 0);
    if (status == ERROR_SUCCESS) {
      for (const auto &key : filter_keys_) {
        const auto filter_key = guid(key);
        status = FwpmFilterDeleteByKey0(engine, &filter_key);
        if (status != ERROR_SUCCESS && status != FWP_E_FILTER_NOT_FOUND) break;
      }
    }
    if (status == ERROR_SUCCESS) status = FwpmTransactionCommit0(engine);
    if (status != ERROR_SUCCESS) {
      (void)FwpmTransactionAbort0(engine);
      error = "Steam seat filters could not be removed; the clone remains blocked (" + std::to_string(status) + ").";
      return false;
    }
    FwpmEngineClose0(engine);
    engine_ = nullptr; filter_keys_.clear(); preparation_ = {}; seat_id_.clear(); generation_ = 0; quarantined_ = false;
    return true;
#else
    error = "Steam isolation is Windows-only."; return false;
#endif
  }

  std::filesystem::path trusted_steam_executable(const std::filesystem::path &apps_manifest,
                                                 const std::span<const std::uint8_t> launch_payload,
                                                 std::string &error) noexcept {
    try {
      const auto payload = nlohmann::json::from_cbor(launch_payload.begin(), launch_payload.end());
      const auto id = std::to_string(payload.at("appid").get<int>());
      std::ifstream stream(apps_manifest);
      nlohmann::json apps;
      stream >> apps;
      if (!apps.is_array()) throw std::runtime_error("apps manifest is not an array");
      std::filesystem::path found;
      for (const auto &app : apps) {
        if (!app.is_object() || app.value("id", "") != id) continue;
        const auto command = app.value("cmd", "");
        if (command.empty()) break;
        std::string first;
        bool quoted = false;
        for (const char ch : command) {
          if (ch == '"') { quoted = !quoted; continue; }
          if (!quoted && std::isspace(static_cast<unsigned char>(ch))) break;
          first.push_back(ch);
        }
        if (first.empty()) break;
        found = std::filesystem::path {std::filesystem::u8path(first)};
        break;
      }
      if (found.empty()) error = "Steam offline isolation requires an exact configured Steam client command.";
      return found;
    } catch (const std::exception &exception) {
      error = std::string {"Steam launch manifest is invalid: "} + exception.what();
      return {};
    } catch (...) {
      error = "Steam launch manifest is invalid.";
      return {};
    }
  }
}

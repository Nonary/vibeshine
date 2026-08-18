#include "terminal_session_seat_provider.h"

#include "src/platform/windows/ipc/pipes.h"
#include "src/terminal_session_seat_pool.h"

#include <Windows.h>
#include <Aclapi.h>
#include <Lm.h>
#include <Sddl.h>
#include <TlHelp32.h>
#include <UserEnv.h>
#include <WtsApi32.h>
#include <winternl.h>
#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/sha.h>
#include <openssl/rand.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cwctype>
#include <filesystem>
#include <fstream>
#include <format>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <iterator>
#include <set>
#include <span>
#include <string>
#include <stdexcept>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

namespace terminal_session::windows {
  namespace {
    constexpr std::wstring_view account_prefix = L"VibeshineSeat";
    constexpr std::wstring_view account_comment = L"Managed by Vibeshine terminal seat broker";
    // The hidden ActiveX controller uses the normal Windows RDP endpoint. Do
    // not create or depend on a product-specific listener: its registry and
    // live WTS state are the listener contract validated below.
    constexpr std::wstring_view listener_name = L"RDP-Tcp";
    constexpr std::wstring_view listener_key = L"SYSTEM\\CurrentControlSet\\Control\\Terminal Server\\WinStations\\RDP-Tcp";
    // Test-only compatibility pins must agree with the installer-side pins;
    // an edited state/manifest cannot authorize a different payload at runtime.
    constexpr std::string_view test_termwrap_sha256 = "220f18e0b2c2091c5f684ec063c43831bffdf25e561bd123211cce883f8d25e2";
    constexpr std::string_view test_zydis_sha256 = "5908be0af05bf7584328cf5d0ddde2c108d693709ffc77a13822fdceb75797e1";
    constexpr std::string_view test_license_sha256 = "72966f08ceaacf34475e7824ac566f2e966bef3c5e46a190dc844c1155486614";
    // Stay below Windows' default dynamic client-port range (49152-65535).
    constexpr std::uint16_t first_worker_base_port = 40000;
    constexpr std::uint16_t worker_port_stride = 32;

    // MinGW's WtsApi32.h omits the listener-query declarations present in the
    // Windows SDK. Resolve the documented API dynamically against this exact,
    // size-checked ABI instead of changing the project's Windows target level.
    struct wts_listener_config_w_t {
      ULONG version;
      ULONG fEnableListener;
      ULONG MaxConnectionCount;
      ULONG fPromptForPassword;
      ULONG fInheritColorDepth;
      ULONG ColorDepth;
      ULONG fInheritBrokenTimeoutSettings;
      ULONG BrokenTimeoutSettings;
      ULONG fDisablePrinterRedirection;
      ULONG fDisableDriveRedirection;
      ULONG fDisableComPortRedirection;
      ULONG fDisableLPTPortRedirection;
      ULONG fDisableClipboardRedirection;
      ULONG fDisableAudioRedirection;
      ULONG fDisablePNPRedirection;
      ULONG fDisableDefaultMainClientPrinter;
      ULONG LanAdapter;
      ULONG PortNumber;
      ULONG fInheritShadowSettings;
      ULONG ShadowSettings;
      ULONG TimeoutSettingsConnection;
      ULONG TimeoutSettingsDisconnection;
      ULONG TimeoutSettingsIdle;
      ULONG SecurityLayer;
      ULONG MinEncryptionLevel;
      ULONG UserAuthentication;
      WCHAR Comment[WTS_COMMENT_LENGTH + 1];
      WCHAR LogonUserName[USERNAME_LENGTH + 1];
      WCHAR LogonDomain[DOMAIN_LENGTH + 1];
      WCHAR WorkDirectory[MAX_PATH + 1];
      WCHAR InitialProgram[MAX_PATH + 1];
    };
    static_assert(sizeof(wts_listener_config_w_t) == 1348);
    using query_listener_config_w_t = BOOL (WINAPI *)(HANDLE, PVOID, DWORD, LPWSTR, wts_listener_config_w_t *);

    std::optional<unsigned int> managed_account_index(const std::wstring_view account) {
      if (account.size() != account_prefix.size() + 2 || !account.starts_with(account_prefix)) return std::nullopt;
      const wchar_t tens = account[account_prefix.size()];
      const wchar_t ones = account[account_prefix.size() + 1];
      if (tens < L'0' || tens > L'9' || ones < L'0' || ones > L'9') return std::nullopt;
      const auto value = static_cast<unsigned int>((tens - L'0') * 10 + (ones - L'0'));
      return value >= 1 && value <= 99 ? std::optional<unsigned int> {value} : std::nullopt;
    }

    struct handle_closer {
      void operator()(void *value) const noexcept { if (value && value != INVALID_HANDLE_VALUE) CloseHandle(value); }
    };
    using unique_handle = std::unique_ptr<void, handle_closer>;
    struct service_handle_closer { void operator()(void *value) const noexcept { if (value) CloseServiceHandle(static_cast<SC_HANDLE>(value)); } };
    using unique_service_handle = std::unique_ptr<void, service_handle_closer>;

    std::string utf8(const std::wstring_view value) {
      if (value.empty()) return {};
      const int size = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
      if (size <= 0) return {};
      std::string result(static_cast<std::size_t>(size), '\0');
      if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), result.data(), size, nullptr, nullptr) != size) return {};
      return result;
    }

    std::wstring wide(const std::string_view value) {
      if (value.empty()) return {};
      const int size = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), nullptr, 0);
      if (size <= 0) return {};
      std::wstring result(static_cast<std::size_t>(size), L'\0');
      if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), result.data(), size) != size) return {};
      return result;
    }

    std::wstring module_path() {
      std::wstring value(32768, L'\0');
      const DWORD size = GetModuleFileNameW(nullptr, value.data(), static_cast<DWORD>(value.size()));
      if (size == 0 || size >= value.size()) return {};
      value.resize(size);
      return value;
    }

    std::wstring sid_string(PSID sid) {
      LPWSTR text = nullptr;
      if (!sid || !ConvertSidToStringSidW(sid, &text) || !text) return {};
      std::wstring result {text};
      LocalFree(text);
      return result;
    }

    std::wstring token_user_sid(HANDLE token) {
      DWORD size = 0;
      GetTokenInformation(token, TokenUser, nullptr, 0, &size);
      if (!size) return {};
      std::vector<std::uint8_t> buffer(size);
      if (!GetTokenInformation(token, TokenUser, buffer.data(), size, &size)) return {};
      return sid_string(reinterpret_cast<TOKEN_USER *>(buffer.data())->User.Sid);
    }

    std::wstring token_logon_sid(HANDLE token) {
      DWORD size = 0;
      GetTokenInformation(token, TokenLogonSid, nullptr, 0, &size);
      if (!size) return {};
      std::vector<std::uint8_t> buffer(size);
      if (!GetTokenInformation(token, TokenLogonSid, buffer.data(), size, &size)) return {};
      const auto groups = reinterpret_cast<TOKEN_GROUPS *>(buffer.data());
      return groups->GroupCount == 1 ? sid_string(groups->Groups[0].Sid) : std::wstring {};
    }

    std::optional<std::vector<std::uint8_t>> sid_bytes(const std::wstring &text) {
      PSID sid = nullptr;
      if (!ConvertStringSidToSidW(text.c_str(), &sid) || !sid) return std::nullopt;
      const DWORD size = GetLengthSid(sid);
      std::vector<std::uint8_t> result(size);
      if (!CopySid(size, result.data(), sid)) result.clear();
      LocalFree(sid);
      if (result.empty()) return std::nullopt;
      return result;
    }

    DWORD grant_object(HANDLE object, SE_OBJECT_TYPE type, PSID sid, ACCESS_MASK mask) {
      PACL old_acl = nullptr;
      PSECURITY_DESCRIPTOR descriptor = nullptr;
      const DWORD query = GetSecurityInfo(object, type, DACL_SECURITY_INFORMATION, nullptr, nullptr, &old_acl, nullptr, &descriptor);
      if (query != ERROR_SUCCESS) return query;
      EXPLICIT_ACCESSW entry {};
      entry.grfAccessPermissions = mask;
      entry.grfAccessMode = GRANT_ACCESS;
      entry.grfInheritance = NO_INHERITANCE;
      entry.Trustee.TrusteeForm = TRUSTEE_IS_SID;
      entry.Trustee.TrusteeType = TRUSTEE_IS_USER;
      entry.Trustee.ptstrName = static_cast<LPWSTR>(sid);
      PACL updated = nullptr;
      const DWORD merged = SetEntriesInAclW(1, &entry, old_acl, &updated);
      DWORD result = merged;
      if (merged == ERROR_SUCCESS) result = SetSecurityInfo(object, type, DACL_SECURITY_INFORMATION, nullptr, nullptr, updated, nullptr);
      if (updated) LocalFree(updated);
      if (descriptor) LocalFree(descriptor);
      return result;
    }

    using nt_open_directory_object_t = LONG (NTAPI *)(PHANDLE, ACCESS_MASK, POBJECT_ATTRIBUTES);

    bool grant_seat_acl(const std::uint32_t session_id, const std::wstring &user_sid_text, const std::wstring &logon_sid_text) {
      const auto user_sid = sid_bytes(user_sid_text);
      const auto logon_sid = sid_bytes(logon_sid_text);
      if (!user_sid || !logon_sid) return false;
      const HWINSTA station = OpenWindowStationW(L"winsta0", FALSE, READ_CONTROL | WRITE_DAC);
      if (!station) return false;
      const HDESK desktop = OpenDesktopW(L"default", 0, FALSE, READ_CONTROL | WRITE_DAC);
      if (!desktop) { CloseWindowStation(station); return false; }

      constexpr ACCESS_MASK station_access = WINSTA_ACCESSCLIPBOARD | WINSTA_ACCESSGLOBALATOMS | WINSTA_ENUMDESKTOPS |
        WINSTA_ENUMERATE | WINSTA_READATTRIBUTES | WINSTA_READSCREEN | WINSTA_WRITEATTRIBUTES | READ_CONTROL;
      constexpr ACCESS_MASK desktop_access = DESKTOP_READOBJECTS | DESKTOP_CREATEWINDOW | DESKTOP_CREATEMENU |
        DESKTOP_ENUMERATE | DESKTOP_WRITEOBJECTS | READ_CONTROL;
      bool ok = true;
      const std::array<PSID, 2> sids {
        reinterpret_cast<PSID>(const_cast<std::uint8_t *>(user_sid->data())),
        reinterpret_cast<PSID>(const_cast<std::uint8_t *>(logon_sid->data())),
      };
      for (const auto sid : sids) {
        ok = grant_object(station, SE_WINDOW_OBJECT, sid, station_access) == ERROR_SUCCESS && ok;
        ok = grant_object(desktop, SE_WINDOW_OBJECT, sid, desktop_access) == ERROR_SUCCESS && ok;
      }
      CloseDesktop(desktop);
      CloseWindowStation(station);

      const auto ntdll = GetModuleHandleW(L"ntdll.dll");
      const auto open_directory = ntdll ? reinterpret_cast<nt_open_directory_object_t>(GetProcAddress(ntdll, "NtOpenDirectoryObject")) : nullptr;
      if (!open_directory) return false;
      std::wstring path = std::format(L"\\Sessions\\{}\\BaseNamedObjects", session_id);
      UNICODE_STRING name {};
      name.Buffer = path.data();
      name.Length = static_cast<USHORT>(path.size() * sizeof(wchar_t));
      name.MaximumLength = name.Length;
      OBJECT_ATTRIBUTES attributes {};
      InitializeObjectAttributes(&attributes, &name, OBJ_CASE_INSENSITIVE, nullptr, nullptr);
      HANDLE raw_directory = nullptr;
      if (open_directory(&raw_directory, READ_CONTROL | WRITE_DAC, &attributes) < 0 || !raw_directory) return false;
      unique_handle directory {raw_directory};
      for (const auto sid : sids) {
        constexpr ACCESS_MASK directory_access = 0x0001 /* DIRECTORY_QUERY */ | 0x0002 /* DIRECTORY_TRAVERSE */ |
          0x0004 /* DIRECTORY_CREATE_OBJECT */ | 0x0008 /* DIRECTORY_CREATE_SUBDIRECTORY */ | READ_CONTROL;
        ok = grant_object(directory.get(), SE_KERNEL_OBJECT, sid, directory_access) == ERROR_SUCCESS && ok;
      }
      return ok;
    }

    std::optional<std::wstring> random_password() {
      constexpr std::wstring_view alphabet = L"ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz23456789!%&()-_=+";
      std::wstring result;
      result.reserve(32);
      const auto ceiling = static_cast<unsigned int>(256 - (256 % alphabet.size()));
      while (result.size() < 32) {
        unsigned char value = 0;
        if (RAND_bytes(&value, 1) != 1) {
          SecureZeroMemory(result.data(), result.size() * sizeof(wchar_t));
          return std::nullopt;
        }
        if (value < ceiling) result.push_back(alphabet[value % alphabet.size()]);
      }
      return result;
    }

    bool set_account_password(const std::wstring &account, std::wstring &password) {
      USER_INFO_1003 info {password.data()};
      DWORD parameter = 0;
      return NetUserSetInfo(nullptr, account.c_str(), 1003, reinterpret_cast<LPBYTE>(&info), &parameter) == NERR_Success;
    }

    bool set_account_enabled(const std::wstring &account, const bool enabled) {
      LPUSER_INFO_1 raw = nullptr;
      if (NetUserGetInfo(nullptr, account.c_str(), 1, reinterpret_cast<LPBYTE *>(&raw)) != NERR_Success || !raw) return false;
      const DWORD current = raw->usri1_flags;
      NetApiBufferFree(raw);
      USER_INFO_1008 flags {enabled ? current & ~UF_ACCOUNTDISABLE : current | UF_ACCOUNTDISABLE};
      DWORD parameter = 0;
      return NetUserSetInfo(nullptr, account.c_str(), 1008, reinterpret_cast<LPBYTE>(&flags), &parameter) == NERR_Success;
    }

    struct secure_wstring_t {
      std::wstring *value {};
      ~secure_wstring_t() {
        if (value && !value->empty()) SecureZeroMemory(value->data(), value->size() * sizeof(wchar_t));
      }
    };

    std::wstring account_sid(const std::wstring &account) {
      DWORD sid_size = 0, domain_size = 0;
      SID_NAME_USE use {};
      LookupAccountNameW(nullptr, account.c_str(), nullptr, &sid_size, nullptr, &domain_size, &use);
      if (!sid_size) return {};
      std::vector<std::uint8_t> sid(sid_size);
      std::wstring domain(domain_size, L'\0');
      if (!LookupAccountNameW(nullptr, account.c_str(), sid.data(), &sid_size, domain.data(), &domain_size, &use)) return {};
      return sid_string(sid.data());
    }

    bool add_remote_desktop_membership(const std::wstring &account) {
      DWORD sid_size = SECURITY_MAX_SID_SIZE;
      std::array<std::uint8_t, SECURITY_MAX_SID_SIZE> sid {};
      if (!CreateWellKnownSid(WinBuiltinRemoteDesktopUsersSid, nullptr, sid.data(), &sid_size)) return false;
      wchar_t group[256] {}, domain[256] {};
      DWORD group_size = _countof(group), domain_size = _countof(domain);
      SID_NAME_USE use {};
      if (!LookupAccountSidW(nullptr, sid.data(), group, &group_size, domain, &domain_size, &use)) return false;
      LOCALGROUP_MEMBERS_INFO_3 member {const_cast<LPWSTR>(account.c_str())};
      const NET_API_STATUS status = NetLocalGroupAddMembers(nullptr, group, 3, reinterpret_cast<LPBYTE>(&member), 1);
      return status == NERR_Success || status == ERROR_MEMBER_IN_ALIAS;
    }

    bool set_account_hidden(const std::wstring &account, const bool hidden) {
      constexpr wchar_t key_path[] = L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\Winlogon\\SpecialAccounts\\UserList";
      HKEY key = nullptr;
      DWORD disposition = 0;
      if (RegCreateKeyExW(HKEY_LOCAL_MACHINE, key_path, 0, nullptr, REG_OPTION_NON_VOLATILE, KEY_SET_VALUE, nullptr, &key, &disposition) != ERROR_SUCCESS) {
        return false;
      }
      const DWORD value = hidden ? 0 : 1;
      const LSTATUS status = hidden
        ? RegSetValueExW(key, account.c_str(), 0, REG_DWORD, reinterpret_cast<const BYTE *>(&value), sizeof(value))
        : RegDeleteValueW(key, account.c_str());
      RegCloseKey(key);
      return status == ERROR_SUCCESS || (!hidden && status == ERROR_FILE_NOT_FOUND);
    }

    std::wstring query_session_user(const DWORD session_id) {
      LPWSTR value = nullptr;
      DWORD bytes = 0;
      if (!WTSQuerySessionInformationW(WTS_CURRENT_SERVER_HANDLE, session_id, WTSUserName, &value, &bytes) || !value) return {};
      std::wstring result {value};
      WTSFreeMemory(value);
      return result;
    }

    std::map<std::wstring, std::pair<DWORD, WTS_CONNECTSTATE_CLASS>, std::less<>> enumerate_account_sessions() {
      std::map<std::wstring, std::pair<DWORD, WTS_CONNECTSTATE_CLASS>, std::less<>> result;
      PWTS_SESSION_INFOW sessions = nullptr;
      DWORD count = 0;
      if (!WTSEnumerateSessionsW(WTS_CURRENT_SERVER_HANDLE, 0, 1, &sessions, &count)) return result;
      for (DWORD index = 0; index < count; ++index) {
        auto user = query_session_user(sessions[index].SessionId);
        if (user.empty()) continue;
        std::transform(user.begin(), user.end(), user.begin(), ::towlower);
        const auto found = result.find(user);
        if (found == result.end() || sessions[index].State == WTSActive || sessions[index].State == WTSConnected) {
          result[user] = {sessions[index].SessionId, sessions[index].State};
        }
      }
      WTSFreeMemory(sessions);
      return result;
    }

    bool desktop_ready(const DWORD session_id) {
      bool dwm = false;
      bool shell = false;
      unique_handle snapshot {CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0)};
      if (!snapshot || snapshot.get() == INVALID_HANDLE_VALUE) return false;
      PROCESSENTRY32W process {.dwSize = sizeof(PROCESSENTRY32W)};
      if (!Process32FirstW(snapshot.get(), &process)) return false;
      do {
        DWORD current_session = 0;
        if (!ProcessIdToSessionId(process.th32ProcessID, &current_session) || current_session != session_id) continue;
        if (_wcsicmp(process.szExeFile, L"dwm.exe") == 0) dwm = true;
        if (_wcsicmp(process.szExeFile, L"explorer.exe") == 0 || _wcsicmp(process.szExeFile, L"ShellExperienceHost.exe") == 0) shell = true;
      } while (Process32NextW(snapshot.get(), &process));
      return dwm && shell;
    }

    bool is_system_process() {
      unique_handle token;
      HANDLE raw = nullptr;
      if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &raw)) return false;
      token.reset(raw);
      DWORD size = 0;
      GetTokenInformation(token.get(), TokenUser, nullptr, 0, &size);
      std::vector<std::uint8_t> buffer(size);
      return size && GetTokenInformation(token.get(), TokenUser, buffer.data(), size, &size) &&
             IsWellKnownSid(reinterpret_cast<TOKEN_USER *>(buffer.data())->User.Sid, WinLocalSystemSid);
    }

    std::optional<DWORD> service_pid(const wchar_t *name) {
      unique_service_handle scm {OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT)};
      if (!scm) return std::nullopt;
      unique_service_handle service {OpenServiceW(static_cast<SC_HANDLE>(scm.get()), name, SERVICE_QUERY_STATUS)};
      if (!service) return std::nullopt;
      SERVICE_STATUS_PROCESS status {};
      DWORD bytes = 0;
      if (!QueryServiceStatusEx(static_cast<SC_HANDLE>(service.get()), SC_STATUS_PROCESS_INFO, reinterpret_cast<LPBYTE>(&status), sizeof(status), &bytes) ||
          status.dwCurrentState != SERVICE_RUNNING || !status.dwProcessId) return std::nullopt;
      return status.dwProcessId;
    }

    struct terminal_isolation_contract_t {
      std::filesystem::path owned_path;
      std::string owned_sha256;
    };

    std::optional<std::filesystem::path> canonical_existing_path(const std::filesystem::path &path) {
      std::error_code ec;
      const auto canonical = std::filesystem::weakly_canonical(path, ec);
      if (ec || !std::filesystem::is_regular_file(canonical, ec) || ec) return std::nullopt;
      return canonical;
    }

    std::optional<std::string> sha256_file(const std::filesystem::path &path) {
      std::ifstream file(path, std::ios::binary);
      if (!file) return std::nullopt;
      EVP_MD_CTX *context = EVP_MD_CTX_new();
      if (!context || EVP_DigestInit_ex(context, EVP_sha256(), nullptr) != 1) {
        if (context) EVP_MD_CTX_free(context);
        return std::nullopt;
      }
      std::array<char, 64 * 1024> buffer {};
      while (file) {
        file.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        const auto count = file.gcount();
        if (count > 0 && EVP_DigestUpdate(context, buffer.data(), static_cast<std::size_t>(count)) != 1) {
          EVP_MD_CTX_free(context);
          return std::nullopt;
        }
      }
      if (!file.eof()) {
        EVP_MD_CTX_free(context);
        return std::nullopt;
      }
      std::array<unsigned char, SHA256_DIGEST_LENGTH> digest {};
      unsigned int digest_length = 0;
      const bool finalized = EVP_DigestFinal_ex(context, digest.data(), &digest_length) == 1;
      EVP_MD_CTX_free(context);
      if (!finalized || digest_length != digest.size()) return std::nullopt;
      std::string result;
      result.reserve(SHA256_DIGEST_LENGTH * 2);
      constexpr char hex[] = "0123456789abcdef";
      for (const auto value : digest) {
        result.push_back(hex[value >> 4]);
        result.push_back(hex[value & 0xf]);
      }
      return result;
    }

    std::optional<terminal_isolation_contract_t> terminal_isolation_contract() {
      try {
      wchar_t program_data[32768] {};
      const DWORD length = GetEnvironmentVariableW(L"ProgramData", program_data, static_cast<DWORD>(std::size(program_data)));
      if (!length || length >= std::size(program_data)) return std::nullopt;
      const auto root = std::filesystem::path {std::wstring {program_data, length}} / L"Vibeshine" / L"TerminalIsolation";
      std::ifstream status_file(root / L"status.txt", std::ios::binary);
      std::string status;
      if (!status_file || !std::getline(status_file, status)) return std::nullopt;
      while (!status.empty() && (status.back() == '\r' || status.back() == '\n' || status.back() == ' ' || status.back() == '\t')) status.pop_back();
      std::transform(status.begin(), status.end(), status.begin(), [](const unsigned char value) { return static_cast<char>(std::tolower(value)); });
      if (status != "active") return std::nullopt;
      std::ifstream state_file(root / L"state.json", std::ios::binary);
      if (!state_file) return std::nullopt;
      const std::string state_text {std::istreambuf_iterator<char> {state_file}, std::istreambuf_iterator<char> {}};
      const auto state = nlohmann::json::parse(state_text, nullptr, false);
      if (state.is_discarded() || state.value("Schema", 0) != 2 || state.value("Owner", "") != "Vibeshine-native-rdp-tcp-v1" ||
          state.value("Provider", "") != "native-rdp-tcp" || !state.contains("ManifestSha256") || !state.contains("ManifestPath") ||
          !state.contains("PayloadDirectory") || !state.contains("OwnedServiceDll") || !state.contains("PayloadFiles") ||
          !state.at("PayloadFiles").is_array()) return std::nullopt;
      const auto manifest_sha = state.value("ManifestSha256", "");
      if (manifest_sha.size() != SHA256_DIGEST_LENGTH * 2 ||
          !std::all_of(manifest_sha.begin(), manifest_sha.end(), [](const unsigned char value) { return std::isxdigit(value) != 0; })) return std::nullopt;
      const auto owned_text = state.value("OwnedServiceDll", "");
      const auto payload_text = state.value("PayloadDirectory", "");
      const auto manifest_text = state.value("ManifestPath", "");
      if (owned_text.empty() || payload_text.empty() || manifest_text.empty()) return std::nullopt;
      const auto owned = canonical_existing_path(std::filesystem::u8path(owned_text));
      std::error_code payload_ec, manifest_ec;
      const auto payload = std::filesystem::weakly_canonical(std::filesystem::u8path(payload_text), payload_ec);
      const auto manifest = canonical_existing_path(std::filesystem::u8path(manifest_text));
      std::error_code root_ec;
      const auto canonical_root = std::filesystem::weakly_canonical(root, root_ec);
      if (!owned || !manifest || payload_ec || root_ec || canonical_root.empty()) return std::nullopt;
      const auto root_text = canonical_root.native() + L"\\";
      const auto expected_payload = std::filesystem::weakly_canonical(canonical_root / L"payload" / std::filesystem::u8path(manifest_sha), payload_ec);
      if (payload_ec || payload != expected_payload || manifest != payload / L"terminal-isolation-manifest.json" ||
          owned->native().size() <= root_text.size() || owned->native().compare(0, root_text.size(), root_text) != 0 ||
          owned->native() != (payload / L"TermWrap.dll").native()) return std::nullopt;
      if (sha256_file(*manifest) != [&] {
            std::string lower = manifest_sha;
            std::transform(lower.begin(), lower.end(), lower.begin(), [](const unsigned char value) { return static_cast<char>(std::tolower(value)); });
            return lower;
          }()) return std::nullopt;
      std::ifstream manifest_file(*manifest, std::ios::binary);
      const std::string manifest_text_content {std::istreambuf_iterator<char> {manifest_file}, std::istreambuf_iterator<char> {}};
      const auto embedded = nlohmann::json::parse(manifest_text_content, nullptr, false);
      if (embedded.is_discarded() || embedded.value("schema", 0) != 2 || embedded.value("provider", "") != "native-rdp-tcp" ||
          !embedded.contains("assets") || !embedded.at("assets").is_array() || embedded.at("assets").size() != 5 || state.at("PayloadFiles").size() != 5) return std::nullopt;
      std::set<std::string> payload_names;
      std::string expected;
      for (const auto &asset : state.at("PayloadFiles")) {
        if (!asset.contains("Name") || !asset.contains("Sha256")) return std::nullopt;
        const auto name = asset.value("Name", "");
        const auto hash = asset.value("Sha256", "");
        if (!payload_names.insert(name).second) return std::nullopt;
        if (name == "TermWrap.dll") {
          expected = hash;
          if (hash != test_termwrap_sha256) return std::nullopt;
        } else if (name == "Zydis.dll" && hash != test_zydis_sha256) {
          return std::nullopt;
        } else if (name == "LICENSE" && hash != test_license_sha256) {
          return std::nullopt;
        }
        const auto matches = std::count_if(embedded.at("assets").begin(), embedded.at("assets").end(), [&](const auto &embedded_asset) {
          return embedded_asset.value("path", "") == name && embedded_asset.value("sha256", "") == hash;
        });
        if (matches != 1) return std::nullopt;
      }
      if (payload_names != std::set<std::string> {"TermWrap.dll", "Zydis.dll", "LICENSE", "terminal-isolation.ps1", "status-contract.txt"}) return std::nullopt;
      if (expected.size() != SHA256_DIGEST_LENGTH * 2) return std::nullopt;
      std::transform(expected.begin(), expected.end(), expected.begin(), [](const unsigned char value) { return static_cast<char>(std::tolower(value)); });
      if (sha256_file(*owned) != expected) return std::nullopt;

      HKEY key = nullptr;
      constexpr std::wstring_view service_key = L"SYSTEM\\CurrentControlSet\\Services\\TermService\\Parameters";
      if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, service_key.data(), 0, KEY_QUERY_VALUE, &key) != ERROR_SUCCESS) return std::nullopt;
      wchar_t value[32768] {};
      DWORD type = 0, size = sizeof(value);
      const bool read = RegQueryValueExW(key, L"ServiceDll", nullptr, &type, reinterpret_cast<LPBYTE>(value), &size) == ERROR_SUCCESS &&
        (type == REG_SZ || type == REG_EXPAND_SZ) && size >= sizeof(wchar_t) && size <= sizeof(value);
      RegCloseKey(key);
      if (!read) return std::nullopt;
      value[(size / sizeof(wchar_t)) - 1] = L'\0';
      wchar_t expanded[32768] {};
      const DWORD expanded_length = ExpandEnvironmentStringsW(value, expanded, static_cast<DWORD>(std::size(expanded)));
      if (!expanded_length || expanded_length >= std::size(expanded)) return std::nullopt;
      const auto registry_path = canonical_existing_path(std::filesystem::path {expanded});
      if (!registry_path || *registry_path != *owned) return std::nullopt;
      return terminal_isolation_contract_t {.owned_path = *owned, .owned_sha256 = expected};
      } catch (...) {
        return std::nullopt;
      }
    }

    bool termwrap_loaded() {
      const auto contract = terminal_isolation_contract();
      if (!contract) return false;
      const auto pid = service_pid(L"TermService");
      if (!pid) return false;
      unique_handle snapshot {CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, *pid)};
      if (!snapshot || snapshot.get() == INVALID_HANDLE_VALUE) return false;
      MODULEENTRY32W entry {.dwSize = sizeof(MODULEENTRY32W)};
      bool termwrap = false, inbox_termsrv = false;
      if (Module32FirstW(snapshot.get(), &entry)) {
        do {
          std::wstring name {entry.szModule};
          std::transform(name.begin(), name.end(), name.begin(), ::towlower);
          if (name == L"termwrap.dll") {
            const auto loaded = canonical_existing_path(entry.szExePath);
            termwrap = loaded && *loaded == contract->owned_path;
          }
          if (name == L"termsrv.dll") {
            std::wstring system_directory(32768, L'\0');
            const UINT length = GetSystemDirectoryW(system_directory.data(), static_cast<UINT>(system_directory.size()));
            if (!length || length >= system_directory.size()) continue;
            system_directory.resize(length);
            const std::filesystem::path expected = std::filesystem::path {system_directory} / L"termsrv.dll";
            std::error_code ec;
            inbox_termsrv = std::filesystem::equivalent(expected, entry.szExePath, ec) && !ec;
          }
        } while (Module32NextW(snapshot.get(), &entry));
      }
      return termwrap && inbox_termsrv;
    }

    std::string terminal_isolation_status() {
      wchar_t program_data[32768] {};
      const DWORD length = GetEnvironmentVariableW(L"ProgramData", program_data, static_cast<DWORD>(std::size(program_data)));
      if (!length || length >= std::size(program_data)) return {};
      std::ifstream status_file(std::filesystem::path {std::wstring {program_data, length}} /
                                L"Vibeshine" / L"TerminalIsolation" / L"status.txt", std::ios::binary);
      if (!status_file) return {};
      std::string status;
      std::getline(status_file, status);
      while (!status.empty() && (status.back() == '\r' || status.back() == '\n' || status.back() == ' ' || status.back() == '\t')) status.pop_back();
      std::transform(status.begin(), status.end(), status.begin(), [](const unsigned char value) { return static_cast<char>(std::tolower(value)); });
      return status;
    }

    struct listener_contract_t {
      std::uint16_t port {};
      bool enabled {};
    };

    listener_contract_t listener_contract() {
      HKEY key = nullptr;
      if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, listener_key.data(), 0, KEY_QUERY_VALUE, &key) != ERROR_SUCCESS) return {};
      DWORD port = 0, enabled = 0, disabled = 1, logon_disabled = 1;
      DWORD security_layer = 0, min_encryption_level = 0, user_authentication = 0;
      const auto query_dword = [key](LPCWSTR name, DWORD &value) {
        DWORD type = 0, size = sizeof(value);
        return RegQueryValueExW(key, name, nullptr, &type, reinterpret_cast<LPBYTE>(&value), &size) == ERROR_SUCCESS &&
          type == REG_DWORD && size == sizeof(value);
      };
      const bool ok = query_dword(L"PortNumber", port) && query_dword(L"fEnableWinStation", enabled) &&
        query_dword(L"WinStationDisabled", disabled) && query_dword(L"fLogonDisabled", logon_disabled) &&
        query_dword(L"SecurityLayer", security_layer) && query_dword(L"MinEncryptionLevel", min_encryption_level) &&
        query_dword(L"UserAuthentication", user_authentication);
      RegCloseKey(key);
      const auto wtsapi = GetModuleHandleW(L"Wtsapi32.dll");
      const auto query_listener = wtsapi ? reinterpret_cast<query_listener_config_w_t>(GetProcAddress(wtsapi, "WTSQueryListenerConfigW")) : nullptr;
      wts_listener_config_w_t live {};
      const bool live_ok = query_listener && query_listener(WTS_CURRENT_SERVER_HANDLE, nullptr, 0, const_cast<LPWSTR>(listener_name.data()), &live) != FALSE;
      const bool secure_registry = security_layer >= 1 && min_encryption_level >= 2 && user_authentication == 1;
      const bool valid_port = port > 0 && port <= std::numeric_limits<std::uint16_t>::max();
      const bool secure_live = valid_port && live.fEnableListener != 0 && live.PortNumber == port &&
        live.SecurityLayer >= 1 && live.MinEncryptionLevel >= 2 && live.UserAuthentication == 1;
      return {valid_port ? static_cast<std::uint16_t>(port) : 0,
              ok && secure_registry && live_ok && valid_port && enabled == 1 && disabled == 0 && logon_disabled == 0 && secure_live};
    }

    std::string unique_pipe_name() {
      std::array<unsigned char, 16> bytes {};
      if (RAND_bytes(bytes.data(), static_cast<int>(bytes.size())) != 1) return {};
      constexpr char hex[] = "0123456789abcdef";
      std::string result = "VibeshineSeatController-";
      for (const auto value : bytes) { result.push_back(hex[value >> 4]); result.push_back(hex[value & 0xf]); }
      OPENSSL_cleanse(bytes.data(), bytes.size());
      return result;
    }

    void append_u16(std::vector<std::uint8_t> &out, const std::uint16_t value) {
      out.push_back(static_cast<std::uint8_t>(value)); out.push_back(static_cast<std::uint8_t>(value >> 8));
    }
    void append_u32(std::vector<std::uint8_t> &out, const std::uint32_t value) {
      for (int index = 0; index < 4; ++index) out.push_back(static_cast<std::uint8_t>(value >> (index * 8)));
    }
    bool append_string(std::vector<std::uint8_t> &out, const std::string_view value) {
      if (value.size() > 512) return false;
      append_u16(out, static_cast<std::uint16_t>(value.size()));
      out.insert(out.end(), value.begin(), value.end());
      return true;
    }
    std::uint16_t read_u16(const std::span<const std::uint8_t> in, std::size_t &offset) {
      if (in.size() - offset < 2) throw std::runtime_error("short controller response");
      const auto value = static_cast<std::uint16_t>(in[offset] | (in[offset + 1] << 8)); offset += 2; return value;
    }
    std::uint32_t read_u32(const std::span<const std::uint8_t> in, std::size_t &offset) {
      if (in.size() - offset < 4) throw std::runtime_error("short controller response");
      std::uint32_t value = 0; for (int index = 0; index < 4; ++index) value |= static_cast<std::uint32_t>(in[offset++]) << (index * 8); return value;
    }

    struct controller_t {
      unique_handle process;
      unique_handle job;
      std::unique_ptr<platf::dxgi::INamedPipe> pipe;
      DWORD pid {};
    };

    class windows_backend_t final: public seat_pool::backend_t {
    public:
      seat_pool::capability_t preflight() override {
        if (!is_system_process()) return {.error = "The terminal seat provider must run as LocalSystem."};
        if (!termwrap_loaded()) {
          const auto isolation_status = terminal_isolation_status();
          if (isolation_status == "pending-restart") {
            return {.error = "Terminal isolation is installed but pending a Windows restart; managed terminal seats are unavailable until then."};
          }
          if (isolation_status == "pending-native-restart") {
            return {.error = "Terminal isolation is rolling back to the native TermService and needs a Windows restart before managed terminal seats can be used."};
          }
          if (isolation_status == "foreign-unavailable") {
            return {.error = "Terminal isolation found another TermService provider and left it unchanged; managed terminal seats remain disabled."};
          }
          if (isolation_status == "unavailable") {
            return {.error = "Terminal isolation is unavailable; managed terminal seats remain disabled."};
          }
          return {.error = "Terminal isolation is not active; managed terminal seats remain disabled."};
        }
        const auto listener = listener_contract();
        if (!listener.enabled) return {.termwrap_ready = true, .error = "The native RDP-Tcp listener is absent, disabled, insecure, or not live; the broker will not restart TermService automatically."};
        const auto controller = std::filesystem::path(module_path()).parent_path() / L"vibeshine_seat_controller.exe";
        if (!std::filesystem::is_regular_file(controller)) return {.termwrap_ready = true, .remote_display = true, .error = "The hidden Vibeshine seat controller is not installed beside sunshinesvc."};
        HANDLE raw = nullptr;
        const DWORD console = WTSGetActiveConsoleSessionId();
        const bool token_ready = console != 0xffffffffu && WTSQueryUserToken(console, &raw);
        if (raw) CloseHandle(raw);
        if (!token_ready) return {.termwrap_ready = true, .session_controller = true, .remote_display = true, .error = "No active console-user token is available for seat application launch."};
        return {.supported = true, .termwrap_ready = true, .session_controller = true, .remote_display = true, .audio_endpoint = true, .token_launch = true};
      }

      std::vector<seat_pool::seat_t> discover(std::string &error) override {
        std::vector<seat_pool::seat_t> result;
        const auto sessions = enumerate_account_sessions();
        DWORD resume = 0;
        do {
          LPUSER_INFO_1 buffer = nullptr;
          DWORD read = 0, total = 0;
          const NET_API_STATUS status = NetUserEnum(nullptr, 1, FILTER_NORMAL_ACCOUNT, reinterpret_cast<LPBYTE *>(&buffer), MAX_PREFERRED_LENGTH, &read, &total, &resume);
          if (status != NERR_Success && status != ERROR_MORE_DATA) {
            if (buffer) NetApiBufferFree(buffer);
            error = "Managed Vibeshine account enumeration failed.";
            return {};
          }
          for (DWORD index = 0; index < read; ++index) {
            const auto &user = buffer[index];
            if (!user.usri1_name || !user.usri1_comment || std::wstring_view {user.usri1_comment} != account_comment ||
                !managed_account_index(user.usri1_name)) continue;
            std::wstring account {user.usri1_name};
            if (!(user.usri1_flags & UF_ACCOUNTDISABLE) && !set_account_enabled(account, false)) {
              if (buffer) NetApiBufferFree(buffer);
              error = "A managed Vibeshine account was left enabled and could not be secured.";
              return {};
            }
            std::wstring lower = account;
            std::transform(lower.begin(), lower.end(), lower.begin(), ::towlower);
            seat_pool::seat_t seat {.seat_id = utf8(lower), .account_name = utf8(account), .account_sid = utf8(account_sid(account)), .managed = true};
            if (const auto found = sessions.find(lower); found != sessions.end()) {
              seat.windows_session_id = found->second.first;
              seat.connection = found->second.second == WTSActive || found->second.second == WTSConnected ? seat_pool::connection_state_e::connected : seat_pool::connection_state_e::disconnected;
              seat.reusable = !has_retained_applications(seat);
            }
            result.push_back(std::move(seat));
          }
          if (buffer) NetApiBufferFree(buffer);
          if (status == NERR_Success) break;
        } while (resume != 0);
        return result;
      }

      std::optional<seat_pool::seat_t> create(std::string &error) override {
        std::set<std::wstring, std::less<>> existing;
        DWORD resume = 0;
        do {
          LPUSER_INFO_0 buffer = nullptr; DWORD read = 0, total = 0;
          const NET_API_STATUS status = NetUserEnum(nullptr, 0, FILTER_NORMAL_ACCOUNT, reinterpret_cast<LPBYTE *>(&buffer), MAX_PREFERRED_LENGTH, &read, &total, &resume);
          if (status != NERR_Success && status != ERROR_MORE_DATA) { if (buffer) NetApiBufferFree(buffer); error = "Local account inventory failed."; return std::nullopt; }
          for (DWORD index = 0; index < read; ++index) {
            if (!buffer[index].usri0_name) continue;
            std::wstring name {buffer[index].usri0_name};
            std::transform(name.begin(), name.end(), name.begin(), ::towlower);
            existing.emplace(std::move(name));
          }
          if (buffer) NetApiBufferFree(buffer);
          if (status == NERR_Success) break;
        } while (resume != 0);

        std::wstring account;
        for (unsigned int index = 1; index <= 99; ++index) {
          const auto candidate = std::format(L"{}{:02}", account_prefix, index);
          std::wstring lower = candidate;
          std::transform(lower.begin(), lower.end(), lower.begin(), ::towlower);
          if (!existing.contains(lower)) { account = candidate; break; }
        }
        if (account.empty()) { error = "No managed Vibeshine account name remains available."; return std::nullopt; }
        auto password = random_password();
        if (!password) { error = "Managed-seat password generation failed."; return std::nullopt; }
        USER_INFO_1 info {};
        info.usri1_name = account.data(); info.usri1_password = password->data(); info.usri1_priv = USER_PRIV_USER;
        info.usri1_comment = const_cast<LPWSTR>(account_comment.data());
        info.usri1_flags = UF_SCRIPT | UF_NORMAL_ACCOUNT | UF_ACCOUNTDISABLE | UF_DONT_EXPIRE_PASSWD | UF_PASSWD_CANT_CHANGE | UF_NOT_DELEGATED;
        DWORD parameter = 0;
        const NET_API_STATUS created = NetUserAdd(nullptr, 1, reinterpret_cast<LPBYTE>(&info), &parameter);
        if (created != NERR_Success) { SecureZeroMemory(password->data(), password->size() * sizeof(wchar_t)); error = "Managed Vibeshine account creation failed."; return std::nullopt; }
        bool committed = false;
        const auto rollback = [&] {
          if (!committed) {
            const bool deleted = NetUserDel(nullptr, account.c_str()) == NERR_Success;
            if (deleted) {
              (void) set_account_hidden(account, false);
            } else {
              // Never expose a partially configured account when immediate
              // deletion fails. The startup sweep will retry retirement.
              (void) set_account_hidden(account, true);
              (void) set_account_enabled(account, false);
            }
            return deleted;
          }
          return true;
        };
        if (!add_remote_desktop_membership(account) || !set_account_hidden(account, true)) {
          SecureZeroMemory(password->data(), password->size() * sizeof(wchar_t));
          const bool retired = rollback();
          error = retired ? "Managed account could not be admitted to Remote Desktop and hidden from the sign-in picker." :
                            "Managed account setup failed and immediate deletion failed; it remains hidden and disabled for startup cleanup.";
          return std::nullopt;
        }
        SecureZeroMemory(password->data(), password->size() * sizeof(wchar_t));
        committed = true;
        std::wstring lower = account; std::transform(lower.begin(), lower.end(), lower.begin(), ::towlower);
        return seat_pool::seat_t {.seat_id = utf8(lower), .account_name = utf8(account), .account_sid = utf8(account_sid(account)),
                                  .connection = seat_pool::connection_state_e::disconnected, .managed = true, .reusable = true};
      }

      bool connect(seat_pool::seat_t &seat, const seat_pool::request_t &owner, std::string &error) override {
        if (controllers_.contains(seat.seat_id)) { error = "The managed seat already owns an RDP controller."; return false; }
        const auto listener = listener_contract();
        if (!listener.enabled) { error = "The native RDP-Tcp listener is not live with secure NLA settings."; return false; }
        const auto account = wide(seat.account_name);
        auto password = random_password();
        if (!password) { error = "A one-connect managed seat credential could not be generated."; return false; }
        secure_wstring_t password_guard {&*password};
        if (!set_account_enabled(account, false) || !set_account_password(account, *password) || !set_account_enabled(account, true)) {
          (void) set_account_enabled(account, false);
          error = "The disabled managed account could not be armed with a one-connect credential.";
          return false;
        }

        std::optional<controller_t> admitted_controller;
        const bool connected = [&]() -> bool {
        const std::string pipe_name = unique_pipe_name();
        platf::dxgi::FramedPipeFactory factory {std::make_unique<platf::dxgi::NamedPipeFactory>()};
        auto pipe = factory.create_server(pipe_name);
        if (!pipe) { error = "The first-instance seat controller pipe could not be created."; return false; }

        const DWORD console_session = WTSGetActiveConsoleSessionId();
        HANDLE raw_system = nullptr;
        if (console_session == 0xffffffffu || !OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY | TOKEN_DUPLICATE, &raw_system)) {
          error = "The LocalSystem controller token is unavailable."; return false;
        }
        unique_handle system_token {raw_system};
        HANDLE raw_primary = nullptr;
        if (!DuplicateTokenEx(system_token.get(), TOKEN_ALL_ACCESS, nullptr, SecurityImpersonation, TokenPrimary, &raw_primary)) {
          error = "The LocalSystem controller token could not be duplicated."; return false;
        }
        unique_handle primary {raw_primary};
        DWORD target_session = console_session;
        if (!SetTokenInformation(primary.get(), TokenSessionId, &target_session, sizeof(target_session))) {
          error = "The LocalSystem controller token could not be retargeted to the console session."; return false;
        }
        HANDLE raw_restricted = nullptr;
        if (!CreateRestrictedToken(primary.get(), DISABLE_MAX_PRIVILEGE, 0, nullptr, 0, nullptr, 0, nullptr, &raw_restricted)) {
          error = "The LocalSystem controller token could not be privilege-restricted."; return false;
        }
        primary.reset(raw_restricted);
        LPVOID environment = nullptr;
        if (!CreateEnvironmentBlock(&environment, primary.get(), FALSE)) {
          error = "The LocalSystem controller environment could not be created."; return false;
        }
        const auto environment_guard = std::unique_ptr<void, decltype(&DestroyEnvironmentBlock)> {environment, DestroyEnvironmentBlock};
        unique_handle job {CreateJobObjectW(nullptr, nullptr)};
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits {};
        limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        if (!job || !SetInformationJobObject(job.get(), JobObjectExtendedLimitInformation, &limits, sizeof(limits))) {
          error = "The seat controller job could not be created."; return false;
        }
        const auto executable = std::filesystem::path(module_path()).parent_path() / L"vibeshine_seat_controller.exe";
        std::wstring command = std::format(L"\"{}\" --pipe={} --broker-pid={}", executable.wstring(), wide(pipe_name), GetCurrentProcessId());
        STARTUPINFOW startup {.cb = sizeof(STARTUPINFOW)};
        PROCESS_INFORMATION process {};
        if (!CreateProcessAsUserW(primary.get(), executable.c_str(), command.data(), nullptr, nullptr, FALSE,
                                  CREATE_UNICODE_ENVIRONMENT | CREATE_NO_WINDOW | CREATE_SUSPENDED,
                                  environment, executable.parent_path().c_str(), &startup, &process)) {
          error = "The hidden LocalSystem seat controller could not be launched."; return false;
        }
        unique_handle process_handle {process.hProcess}; unique_handle thread {process.hThread};
        if (!AssignProcessToJobObject(job.get(), process_handle.get()) || ResumeThread(thread.get()) == static_cast<DWORD>(-1)) {
          TerminateProcess(process_handle.get(), ERROR_PROCESS_ABORTED);
          error = "The seat controller could not be contained and resumed."; return false;
        }
        pipe->wait_for_client_connection(5000);
        DWORD peer = 0;
        if (!pipe->is_connected() || !pipe->get_client_process_id(peer) || peer != process.dwProcessId) {
          TerminateProcess(process_handle.get(), ERROR_PROCESS_ABORTED);
          error = "The seat controller did not authenticate to its protected pipe."; return false;
        }
        std::vector<std::uint8_t> request;
        append_u32(request, 0x31525356); append_u16(request, 1);
        std::string password_utf8 = utf8(*password);
        std::wstring host(256, L'\0');
        DWORD host_size = static_cast<DWORD>(host.size());
        if (!GetComputerNameExW(ComputerNameDnsFullyQualified, host.data(), &host_size) || !host_size) {
          TerminateProcess(process_handle.get(), ERROR_PROCESS_ABORTED); error = "The local RDP certificate hostname is unavailable."; return false;
        }
        host.resize(host_size);
        const bool encoded = append_string(request, utf8(host)) && (append_u16(request, listener.port), true) &&
          append_string(request, ".") && append_string(request, seat.account_name) && append_string(request, password_utf8) &&
          (append_u16(request, std::max<std::uint16_t>(owner.width, 640)),
           append_u16(request, std::max<std::uint16_t>(owner.height, 480)), true);
        SecureZeroMemory(password->data(), password->size() * sizeof(wchar_t));
        if (!password_utf8.empty()) OPENSSL_cleanse(password_utf8.data(), password_utf8.size());
        if (!encoded || !pipe->send(request, 5000)) {
          OPENSSL_cleanse(request.data(), request.size()); TerminateProcess(process_handle.get(), ERROR_PROCESS_ABORTED);
          error = "The protected seat credential transfer failed."; return false;
        }
        OPENSSL_cleanse(request.data(), request.size());
        std::array<std::uint8_t, 1024> response {};
        std::size_t response_size = 0;
        if (pipe->receive(response, response_size, 50000) != platf::dxgi::PipeResult::Success) {
          TerminateProcess(process_handle.get(), ERROR_PROCESS_ABORTED); error = "The managed RDP desktop did not become ready."; return false;
        }
        try {
          std::size_t offset = 0;
          const auto data = std::span<const std::uint8_t> {response.data(), response_size};
          if (read_u32(data, offset) != 0x32525356 || read_u16(data, offset) != 1 || offset >= data.size()) throw std::runtime_error("invalid response");
          const bool accepted = data[offset++] != 0;
          const DWORD session_id = read_u32(data, offset);
          const auto error_size = read_u16(data, offset);
          if (data.size() - offset != error_size || !accepted || session_id == 0) {
            const std::string message = data.size() - offset >= error_size ? std::string(reinterpret_cast<const char *>(data.data() + offset), error_size) : std::string {};
            throw std::runtime_error(message.empty() ? "controller rejected the seat" : message);
          }
          const auto account = query_session_user(session_id);
          if (_wcsicmp(account.c_str(), wide(seat.account_name).c_str()) != 0) throw std::runtime_error("controller returned another account's session");
          const auto desktop_deadline = std::chrono::steady_clock::now() + std::chrono::seconds {15};
          while (!desktop_ready(session_id) && std::chrono::steady_clock::now() < desktop_deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds {250});
          }
          if (!desktop_ready(session_id)) throw std::runtime_error("Winlogon/DWM/shell did not become desktop-ready");
          seat.windows_session_id = session_id;
          seat.connection = seat_pool::connection_state_e::connected;
        } catch (const std::exception &exception) {
          TerminateProcess(process_handle.get(), ERROR_PROCESS_ABORTED); error = "Seat controller response rejected: " + std::string {exception.what()}; return false;
        }
        admitted_controller.emplace(controller_t {.process = std::move(process_handle), .job = std::move(job), .pipe = std::move(pipe), .pid = process.dwProcessId});
        return true;
        }();

        if (!set_account_enabled(account, false)) {
          if (admitted_controller) TerminateProcess(admitted_controller->process.get(), ERROR_PROCESS_ABORTED);
          if (seat.windows_session_id) (void) WTSLogoffSession(WTS_CURRENT_SERVER_HANDLE, seat.windows_session_id, TRUE);
          const bool deleted = NetUserDel(nullptr, account.c_str()) == NERR_Success;
          if (deleted) {
            (void) set_account_hidden(account, false);
          } else {
            (void) set_account_hidden(account, true);
            (void) set_account_enabled(account, false);
          }
          seat.windows_session_id = 0;
          seat.connection = seat_pool::connection_state_e::faulted;
          error = "The one-connect managed account could not be disabled after the controller transaction; the unsafe seat was retired.";
          return false;
        }
        if (!connected || !admitted_controller) return false;
        controllers_.emplace(seat.seat_id, std::move(*admitted_controller));
        return true;
      }

      bool disconnect(seat_pool::seat_t &seat, std::string &error) override {
        bool ok = true;
        if (const auto found = controllers_.find(seat.seat_id); found != controllers_.end()) {
          const std::array<std::uint8_t, 1> stop {1};
          (void) found->second.pipe->send(stop, 1000);
          if (WaitForSingleObject(found->second.process.get(), 5000) != WAIT_OBJECT_0) {
            ok = TerminateProcess(found->second.process.get(), ERROR_PROCESS_ABORTED) && WaitForSingleObject(found->second.process.get(), 5000) == WAIT_OBJECT_0;
          }
          if (ok) controllers_.erase(found);
        }
        if (seat.windows_session_id != 0 && !WTSDisconnectSession(WTS_CURRENT_SERVER_HANDLE, seat.windows_session_id, TRUE)) {
          const DWORD native = GetLastError();
          if (native != ERROR_CTX_WINSTATION_NOT_FOUND && native != ERROR_CTX_CLOSE_PENDING) ok = false;
        }
        if (!ok) error = "The managed seat could not be disconnected without logging it off.";
        if (ok) seat.connection = seat_pool::connection_state_e::disconnected;
        return ok;
      }

      bool has_retained_applications(const seat_pool::seat_t &seat) noexcept override {
        if (!seat.windows_session_id) return false;
        const DWORD console = WTSGetActiveConsoleSessionId();
        HANDLE raw_console = nullptr;
        if (console == 0xffffffffu || !WTSQueryUserToken(console, &raw_console)) return true;
        unique_handle console_token {raw_console};
        const auto expected_sid = token_user_sid(console_token.get());
        if (expected_sid.empty()) return true;
        unique_handle snapshot {CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0)};
        if (!snapshot || snapshot.get() == INVALID_HANDLE_VALUE) return true;
        PROCESSENTRY32W process {.dwSize = sizeof(PROCESSENTRY32W)};
        if (!Process32FirstW(snapshot.get(), &process)) return true;
        do {
          DWORD session = 0;
          if (!ProcessIdToSessionId(process.th32ProcessID, &session) || session != seat.windows_session_id) continue;
          unique_handle handle {OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, process.th32ProcessID)};
          if (!handle) continue;
          HANDLE raw_token = nullptr;
          if (!OpenProcessToken(handle.get(), TOKEN_QUERY, &raw_token)) continue;
          unique_handle token {raw_token};
          if (_wcsicmp(token_user_sid(token.get()).c_str(), expected_sid.c_str()) == 0) return true;
        } while (Process32NextW(snapshot.get(), &process));
        return false;
      }

      std::optional<HANDLE> prepare_launch_token(const seat_pool::seat_t &seat, std::string &sid, std::string &error) {
        const DWORD console = WTSGetActiveConsoleSessionId();
        HANDLE raw_console = nullptr;
        if (console == 0xffffffffu || !WTSQueryUserToken(console, &raw_console)) { error = "The active console token is unavailable."; return std::nullopt; }
        unique_handle console_token {raw_console};
        HANDLE primary = nullptr;
        DWORD target_session = seat.windows_session_id;
        if (!DuplicateTokenEx(console_token.get(), TOKEN_ALL_ACCESS, nullptr, SecurityImpersonation, TokenPrimary, &primary) ||
            !SetTokenInformation(primary, TokenSessionId, &target_session, sizeof(target_session))) {
          if (primary) CloseHandle(primary); error = "The console token could not be retargeted to the managed seat."; return std::nullopt;
        }
        const auto user = token_user_sid(primary);
        const auto logon = token_logon_sid(primary);
        if (user.empty() || logon.empty() || !run_acl_helper(seat.windows_session_id, user, logon)) {
          CloseHandle(primary); error = "The target session desktop/BaseNamedObjects ACL preparation failed."; return std::nullopt;
        }
        sid = utf8(user);
        return primary;
      }

    private:
      bool run_acl_helper(const DWORD session, const std::wstring &user, const std::wstring &logon) {
        HANDLE current = nullptr;
        if (!OpenProcessToken(GetCurrentProcess(), TOKEN_DUPLICATE, &current)) return false;
        unique_handle current_token {current};
        HANDLE primary = nullptr;
        if (!DuplicateTokenEx(current_token.get(), TOKEN_ALL_ACCESS, nullptr, SecurityImpersonation, TokenPrimary, &primary)) return false;
        unique_handle token {primary};
        DWORD target_session = session;
        if (!SetTokenInformation(token.get(), TokenSessionId, &target_session, sizeof(target_session))) return false;
        const auto executable = module_path();
        std::wstring command = std::format(L"\"{}\" --prepare-seat-acl {} {} {}", executable, session, user, logon);
        STARTUPINFOW startup {.cb = sizeof(STARTUPINFOW), .lpDesktop = const_cast<LPWSTR>(L"winsta0\\default")};
        PROCESS_INFORMATION process {};
        if (!CreateProcessAsUserW(token.get(), executable.c_str(), command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr,
                                  std::filesystem::path(executable).parent_path().c_str(), &startup, &process)) return false;
        unique_handle process_handle {process.hProcess}; unique_handle thread {process.hThread};
        if (WaitForSingleObject(process_handle.get(), 10000) != WAIT_OBJECT_0) { TerminateProcess(process_handle.get(), ERROR_TIMEOUT); return false; }
        DWORD exit = ERROR_PROCESS_ABORTED;
        return GetExitCodeProcess(process_handle.get(), &exit) && exit == 0;
      }

      std::unordered_map<std::string, controller_t> controllers_;
    };

    class windows_provider_t final: public seat_provider_t {
    public:
      explicit windows_provider_t(const std::size_t maximum) {
        auto backend = std::make_unique<windows_backend_t>();
        backend_ = backend.get();
        pool_ = std::make_unique<seat_pool::pool_t>(std::move(backend), maximum);
      }

      provider_capability_t preflight() override {
        const auto capability = pool_->preflight();
        return {.supported = capability.supported, .concurrent_sessions = capability.termwrap_ready && capability.session_controller,
                .remote_display = capability.remote_display, .audio_endpoint = capability.audio_endpoint,
                .token_launch = capability.token_launch, .error = capability.error};
      }

      std::optional<provider_resource_t> allocate(const provider_request_t &request, std::string &error) override {
        auto lease = pool_->acquire({request.client_uuid, request.generation, request.launch_id, request.width, request.height}, error);
        if (!lease) return std::nullopt;
        std::string user_sid;
        auto token = backend_->prepare_launch_token(lease->seat, user_sid, error);
        if (!token) {
          std::string release_error;
          (void) pool_->release(lease->owner, lease->resumed ? seat_pool::release_disposition_e::retain : seat_pool::release_disposition_e::abandon, release_error);
          return std::nullopt;
        }
        const auto suffix = managed_account_index(wide(lease->seat.account_name));
        if (!suffix) {
          CloseHandle(*token);
          std::string release_error;
          (void) pool_->release(lease->owner, lease->resumed ? seat_pool::release_disposition_e::retain : seat_pool::release_disposition_e::abandon, release_error);
          error = "The managed seat account name does not have a valid numeric allocation suffix.";
          return std::nullopt;
        }
        const auto base = static_cast<std::uint16_t>(first_worker_base_port + *suffix * worker_port_stride);
        auto retained = std::find_if(live_.begin(), live_.end(), [&](const auto &entry) {
          return entry.second.owner.client_uuid == request.client_uuid && entry.second.owner.generation == request.generation;
        });
        const std::uint64_t opaque = retained == live_.end() ? next_opaque_++ : retained->first;
        if (retained == live_.end()) {
          live_.emplace(opaque, live_t {lease->owner, *token});
        } else {
          if (retained->second.token) CloseHandle(retained->second.token);
          retained->second.owner = lease->owner;
          retained->second.token = *token;
        }
        return provider_resource_t {.windows_session_id = lease->seat.windows_session_id, .seat_id = lease->seat.seat_id, .opaque_id = opaque,
          .rtsp_port = static_cast<std::uint16_t>(base + 21), .control_port = static_cast<std::uint16_t>(base + 10),
          .video_port = static_cast<std::uint16_t>(base + 9), .audio_port = static_cast<std::uint16_t>(base + 11),
          .launch_token = reinterpret_cast<std::uintptr_t>(*token), .desktop_name = "winsta0\\default", .user_sid = std::move(user_sid)};
      }

      void release(const provider_resource_t &resource) noexcept override { (void) release_checked(resource, protocol::release_mode::retain); }

      bool release_checked(const provider_resource_t &resource, const protocol::release_mode mode) noexcept override {
        const auto found = live_.find(resource.opaque_id);
        if (found == live_.end()) return true;
        std::string error;
        const auto disposition = mode == protocol::release_mode::abandon ? seat_pool::release_disposition_e::abandon :
          mode == protocol::release_mode::shutdown ? seat_pool::release_disposition_e::shutdown : seat_pool::release_disposition_e::retain;
        if (!pool_->release(found->second.owner, disposition, error)) return false;
        if (found->second.token) {
          CloseHandle(found->second.token);
          found->second.token = nullptr;
        }
        if (mode != protocol::release_mode::retain) live_.erase(found);
        return true;
      }

      ~windows_provider_t() override {
        for (auto &[_, resource] : live_) if (resource.token) CloseHandle(resource.token);
      }

    private:
      struct live_t { seat_pool::request_t owner; HANDLE token {}; };
      windows_backend_t *backend_ {};
      std::unique_ptr<seat_pool::pool_t> pool_;
      std::unordered_map<std::uint64_t, live_t> live_;
      std::uint64_t next_opaque_ {1};
    };
  }

  std::unique_ptr<seat_provider_t> make_seat_provider(const std::size_t maximum_seats) {
    return std::make_unique<windows_provider_t>(maximum_seats);
  }

  bool secure_managed_accounts(const bool remove) {
    if (!is_system_process()) return false;
    bool secured = true;
    const auto sessions = remove ? enumerate_account_sessions() : decltype(enumerate_account_sessions()) {};
    DWORD resume = 0;
    do {
      LPUSER_INFO_1 buffer = nullptr;
      DWORD read = 0, total = 0;
      const NET_API_STATUS status = NetUserEnum(nullptr, 1, FILTER_NORMAL_ACCOUNT, reinterpret_cast<LPBYTE *>(&buffer),
                                                MAX_PREFERRED_LENGTH, &read, &total, &resume);
      if (status != NERR_Success && status != ERROR_MORE_DATA) {
        if (buffer) NetApiBufferFree(buffer);
        return false;
      }
      for (DWORD index = 0; index < read; ++index) {
        const auto &user = buffer[index];
        if (!user.usri1_name || !user.usri1_comment || std::wstring_view {user.usri1_comment} != account_comment ||
            !managed_account_index(user.usri1_name)) continue;
        const std::wstring account {user.usri1_name};
        secured = set_account_hidden(account, true) && secured;
        const bool disabled = set_account_enabled(account, false);
        secured = disabled && secured;
        if (!remove || !disabled) continue;
        std::wstring lower = account;
        std::transform(lower.begin(), lower.end(), lower.begin(), ::towlower);
        if (const auto found = sessions.find(lower); found != sessions.end()) {
          const BOOL logged_off = WTSLogoffSession(WTS_CURRENT_SERVER_HANDLE, found->second.first, TRUE);
          const DWORD native = logged_off ? ERROR_SUCCESS : GetLastError();
          if (!logged_off && native != ERROR_CTX_WINSTATION_NOT_FOUND && native != ERROR_CTX_CLOSE_PENDING) {
            secured = false;
            continue;
          }
        }
        const NET_API_STATUS deleted = NetUserDel(nullptr, account.c_str());
        if (deleted == NERR_Success || deleted == NERR_UserNotFound) {
          secured = set_account_hidden(account, false) && secured;
        } else {
          // Never expose an account that could not be retired.
          (void) set_account_hidden(account, true);
          secured = false;
        }
      }
      if (buffer) NetApiBufferFree(buffer);
      if (status == NERR_Success) break;
    } while (resume != 0);
    return secured;
  }

  int run_seat_acl_helper(const int argc, wchar_t **argv) {
    if (argc != 5 || !argv || _wcsicmp(argv[1], L"--prepare-seat-acl") != 0) return ERROR_INVALID_PARAMETER;
    if (!is_system_process()) return ERROR_ACCESS_DENIED;
    wchar_t *end = nullptr;
    const unsigned long session = std::wcstoul(argv[2], &end, 10);
    if (!end || *end != L'\0' || session > std::numeric_limits<DWORD>::max()) return ERROR_INVALID_PARAMETER;
    DWORD actual = 0xffffffffu;
    if (!ProcessIdToSessionId(GetCurrentProcessId(), &actual) || actual != session) return ERROR_CTX_WINSTATION_ACCESS_DENIED;
    const DWORD console = WTSGetActiveConsoleSessionId();
    HANDLE raw_console = nullptr;
    if (console == 0xffffffffu || !WTSQueryUserToken(console, &raw_console)) return ERROR_NO_TOKEN;
    unique_handle console_token {raw_console};
    if (_wcsicmp(token_user_sid(console_token.get()).c_str(), argv[3]) != 0 ||
        _wcsicmp(token_logon_sid(console_token.get()).c_str(), argv[4]) != 0) return ERROR_ACCESS_DENIED;
    return grant_seat_acl(static_cast<DWORD>(session), argv[3], argv[4]) ? 0 : ERROR_ACCESS_DENIED;
  }
}

#include <Windows.h>
#include <Aclapi.h>
#include <shellapi.h>

#include <algorithm>
#include <cwctype>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {
  struct handle_closer { void operator()(HANDLE h) const noexcept { if (h && h != INVALID_HANDLE_VALUE) CloseHandle(h); } };
  using unique_handle = std::unique_ptr<void, handle_closer>;

  std::wstring lower(std::wstring value) {
    std::ranges::transform(value, value.begin(), [](wchar_t ch) { return static_cast<wchar_t>(towlower(ch)); });
    return value;
  }

  std::wstring environment(const wchar_t *name) {
    std::wstring value(32768, L'\0');
    const auto length = GetEnvironmentVariableW(name, value.data(), static_cast<DWORD>(value.size()));
    if (!length || length >= value.size()) return {};
    value.resize(length); return value;
  }

  bool secure_existing_directory(const std::filesystem::path &path, const std::filesystem::path &root, const bool check_acl = true) {
    HANDLE raw = CreateFileW(path.c_str(), FILE_READ_ATTRIBUTES | (check_acl ? READ_CONTROL : 0),
      FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
      FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (raw == INVALID_HANDLE_VALUE) return false;
    unique_handle handle {raw}; FILE_ATTRIBUTE_TAG_INFO tag {};
    if (!GetFileInformationByHandleEx(handle.get(), FileAttributeTagInfo, &tag, sizeof(tag)) ||
        !(tag.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) || (tag.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) return false;
    std::wstring final_path(32768, L'\0');
    const auto size = GetFinalPathNameByHandleW(handle.get(), final_path.data(), static_cast<DWORD>(final_path.size()), FILE_NAME_NORMALIZED);
    if (!size || size >= final_path.size()) return false;
    final_path.resize(size);
    auto canonical = [](std::wstring value) { value = lower(std::move(value)); while (value.size() > 3 && value.ends_with(L"\\")) value.pop_back(); return value; };
    auto canonical_root = canonical(root.wstring()); if (!canonical_root.ends_with(L"\\")) canonical_root.push_back(L'\\');
    if (canonical(final_path).compare(0, canonical_root.size(), canonical_root) != 0) return false;
    if (!check_acl) return true;
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    if (GetSecurityInfo(handle.get(), SE_FILE_OBJECT, DACL_SECURITY_INFORMATION, nullptr, nullptr, nullptr, nullptr, &descriptor) != ERROR_SUCCESS || !descriptor) return false;
    BOOL present = FALSE, defaulted = FALSE; PACL dacl = nullptr;
    const bool acl_ok = GetSecurityDescriptorDacl(descriptor, &present, &dacl, &defaulted) && present && dacl;
    SECURITY_DESCRIPTOR_CONTROL control {}; DWORD revision = 0;
    const bool protected_acl = GetSecurityDescriptorControl(descriptor, &control, &revision) && (control & SE_DACL_PROTECTED);
    LocalFree(descriptor);
    return acl_ok && protected_acl;
  }

  std::wstring real_helper() {
    wchar_t value[32768] {};
    const auto length = GetModuleFileNameW(nullptr, value, ARRAYSIZE(value));
    if (!length || length >= ARRAYSIZE(value)) return {};
    std::filesystem::path path {std::wstring {value, length}};
    return (path.parent_path() / L"steamwebhelper.real.exe").wstring();
  }

  // CommandLineToArgvW performs the inverse parse. This serializer implements
  // the matching backslash-before-quote and trailing-backslash rules.
  std::wstring serialize_arg(std::wstring_view value) {
    std::wstring result = L"\""; std::size_t slashes = 0;
    for (const wchar_t ch : value) {
      if (ch == L'\\') { ++slashes; continue; }
      if (ch == L'\"') { result.append(slashes * 2 + 1, L'\\'); result.push_back(L'\"'); slashes = 0; continue; }
      result.append(slashes, L'\\'); slashes = 0; result.push_back(ch);
    }
    result.append(slashes * 2, L'\\'); result.push_back(L'\"'); return result;
  }

  enum class profile_kind { none, cache, user_data };
  profile_kind profile_flag(std::wstring_view arg, std::wstring &inline_value) {
    const auto normalized = lower(std::wstring {arg});
    const auto equals = normalized.find(L'=');
    const auto name = normalized.substr(0, equals);
    if (name == L"-cachedir" || name == L"--cachedir") { inline_value = equals == std::wstring::npos ? L"" : normalized.substr(equals + 1); return profile_kind::cache; }
    if (name == L"-userdatadir" || name == L"--userdatadir" || name == L"-user-data-dir" || name == L"--user-data-dir") {
      inline_value = equals == std::wstring::npos ? L"" : normalized.substr(equals + 1); return profile_kind::user_data;
    }
    return profile_kind::none;
  }

  bool build_arguments(const std::vector<std::wstring> &original, const std::wstring &html,
                       const std::wstring &profile, std::vector<std::wstring> &result) {
    bool cache_seen = false, profile_seen = false;
    for (std::size_t i = 0; i < original.size(); ++i) {
      std::wstring inline_value; const auto kind = profile_flag(original[i], inline_value);
      if (kind == profile_kind::none) { result.push_back(original[i]); continue; }
      if (inline_value.empty() && original[i].find(L'=') == std::wstring::npos) {
        if (++i >= original.size() || original[i].empty() || original[i].front() == L'-') return false;
      } else if (inline_value.empty()) return false;
      if (kind == profile_kind::cache) { result.emplace_back(L"-cachedir"); result.push_back(html); cache_seen = true; }
      else { result.emplace_back(L"-userdatadir"); result.push_back(profile); profile_seen = true; }
    }
    if (!cache_seen) { result.emplace_back(L"-cachedir"); result.push_back(html); }
    if (!profile_seen) { result.emplace_back(L"-userdatadir"); result.push_back(profile); }
    return true;
  }
}

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
  const std::filesystem::path cache {environment(L"VIBESHINE_STEAM_CACHE_ROOT")};
  const auto helper = real_helper();
  if (cache.empty() || helper.empty()) return ERROR_INVALID_DATA;
  const auto html = cache / L"htmlcache"; const auto profile = cache / L"userdata";
  if (!secure_existing_directory(cache, cache.parent_path(), false) || !secure_existing_directory(html, cache) ||
      !secure_existing_directory(profile, cache)) return ERROR_ACCESS_DENIED;

  int argc = 0; auto *raw = CommandLineToArgvW(GetCommandLineW(), &argc);
  if (!raw) return ERROR_INVALID_DATA;
  std::vector<std::wstring> original; for (int i = 1; i < argc; ++i) original.emplace_back(raw[i]);
  LocalFree(raw);
  std::vector<std::wstring> args;
  if (!build_arguments(original, html.wstring(), profile.wstring(), args)) return ERROR_INVALID_DATA;
  std::wstring command = serialize_arg(helper);
  for (const auto &arg : args) { command.push_back(L' '); command += serialize_arg(arg); }
  STARTUPINFOW startup {.cb = sizeof(startup)}; PROCESS_INFORMATION process {};
  if (!CreateProcessW(helper.c_str(), command.data(), nullptr, nullptr, FALSE, CREATE_UNICODE_ENVIRONMENT,
                      nullptr, cache.parent_path().c_str(), &startup, &process)) return static_cast<int>(GetLastError());
  CloseHandle(process.hThread); WaitForSingleObject(process.hProcess, INFINITE);
  DWORD exit_code = ERROR_PROCESS_ABORTED; GetExitCodeProcess(process.hProcess, &exit_code); CloseHandle(process.hProcess);
  return static_cast<int>(exit_code);
}

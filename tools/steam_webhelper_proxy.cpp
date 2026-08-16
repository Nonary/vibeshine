#include <Windows.h>
#include <shellapi.h>

#include <algorithm>
#include <cwctype>
#include <string>
#include <vector>

namespace {
  std::wstring quote(const std::wstring &value) {
    std::wstring result = L"\"";
    result += value;
    result += L"\"";
    return result;
  }

  std::wstring cache_root() {
    wchar_t value[32768] {};
    const auto length = GetEnvironmentVariableW(L"VIBESHINE_STEAM_CACHE_ROOT", value, ARRAYSIZE(value));
    return length && length < ARRAYSIZE(value) ? std::wstring {value, length} : std::wstring {};
  }

  std::wstring real_helper() {
    wchar_t value[32768] {};
    const auto length = GetModuleFileNameW(nullptr, value, ARRAYSIZE(value));
    if (!length || length >= ARRAYSIZE(value)) return {};
    std::wstring path {value, length};
    const auto slash = path.find_last_of(L"\\/");
    return slash == std::wstring::npos ? L"steamwebhelper.real.exe" : path.substr(0, slash + 1) + L"steamwebhelper.real.exe";
  }

  bool is_profile_flag(const std::wstring &arg) {
    std::wstring lower = arg;
    std::transform(lower.begin(), lower.end(), lower.begin(), towlower);
    return lower == L"-cachedir" || lower.starts_with(L"-cachedir=") || lower == L"--cachedir" ||
      lower.starts_with(L"--cachedir=") || lower == L"-userdatadir" || lower.starts_with(L"-userdatadir=") ||
      lower == L"-user-data-dir" || lower.starts_with(L"-user-data-dir=") ||
      lower == L"--user-data-dir" || lower.starts_with(L"--user-data-dir=") ||
      lower == L"--userdatadir" || lower.starts_with(L"--userdatadir=");
  }

  std::vector<std::wstring> arguments() {
    std::vector<std::wstring> result;
    int argc = 0;
    auto *argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (!argv) return result;
    for (int i = 1; i < argc; ++i) result.emplace_back(argv[i]);
    LocalFree(argv);
    return result;
  }
}

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
  const auto cache = cache_root();
  const auto helper = real_helper();
  if (cache.empty() || helper.empty()) return ERROR_INVALID_DATA;
  const auto html = cache + L"\\htmlcache";
  const auto profile = cache + L"\\userdata";
  CreateDirectoryW(cache.c_str(), nullptr);
  CreateDirectoryW(html.c_str(), nullptr);
  CreateDirectoryW(profile.c_str(), nullptr);

  std::vector<std::wstring> args;
  const auto original = arguments();
  bool saw_cache = false;
  bool saw_profile = false;
  for (std::size_t i = 0; i < original.size(); ++i) {
    if (is_profile_flag(original[i])) {
      auto lower = original[i];
      std::transform(lower.begin(), lower.end(), lower.begin(), towlower);
      const bool cache_flag = lower.find(L"cache") != std::wstring::npos;
      if (cache_flag && !saw_cache) { args.push_back(L"-cachedir"); args.push_back(html); saw_cache = true; }
      else if (!cache_flag && !saw_profile) { args.push_back(L"-userdatadir"); args.push_back(profile); saw_profile = true; }
      if (original[i].find(L'=') == std::wstring::npos && i + 1 < original.size()) ++i;
      continue;
    }
    args.push_back(original[i]);
  }
  if (!saw_cache) { args.push_back(L"-cachedir"); args.push_back(html); }
  if (!saw_profile) { args.push_back(L"-userdatadir"); args.push_back(profile); }

  std::wstring command = quote(helper);
  for (const auto &arg : args) command += L" " + quote(arg);
  STARTUPINFOW startup {.cb = sizeof(startup)};
  PROCESS_INFORMATION process {};
  if (!CreateProcessW(helper.c_str(), command.data(), nullptr, nullptr, FALSE, CREATE_UNICODE_ENVIRONMENT,
                      nullptr, nullptr, &startup, &process)) return static_cast<int>(GetLastError());
  CloseHandle(process.hThread);
  WaitForSingleObject(process.hProcess, INFINITE);
  DWORD exit_code = ERROR_PROCESS_ABORTED;
  GetExitCodeProcess(process.hProcess, &exit_code);
  CloseHandle(process.hProcess);
  return static_cast<int>(exit_code);
}

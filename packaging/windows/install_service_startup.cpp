// Native backup/restore for direct MSI maintenance.
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <msiquery.h>
#include <winsvc.h>

#include "../../src/platform/windows/service_startup.h"

#ifdef __MINGW32__
// MinGW-w64 imports this Windows Installer 5.0 API but does not declare it in msi.h.
extern "C" INSTALLSTATE WINAPI MsiGetComponentPathExW(LPCWSTR, LPCWSTR, LPCWSTR, MSIINSTALLCONTEXT, LPWSTR, LPDWORD);
#endif

namespace {
  constexpr wchar_t manual_startup_path[] = L"VibeshineManualServiceStartupPath";

  std::wstring property(MSIHANDLE session, const wchar_t *name) {
    wchar_t empty = 0;
    DWORD size = 0;
    if (MsiGetPropertyW(session, name, &empty, &size) != ERROR_MORE_DATA) return {};
    std::wstring value(++size, L'\0');
    if (MsiGetPropertyW(session, name, value.data(), &size) != ERROR_SUCCESS) return {};
    value.resize(size);
    return value;
  }

  std::wstring installed_service_path(MSIHANDLE session) {
    auto product = property(session, L"ProductCode");
    if (property(session, L"Installed").empty()) {
      product = property(session, L"WIX_UPGRADE_DETECTED");
      if (product.size() != 38) return {}; // Skip ambiguous related products.
      const auto family = property(session, L"UpgradeCode");
      wchar_t related[39];
      bool found = false;
      for (DWORD i = 0; !found && MsiEnumRelatedProductsW(family.c_str(), 0, i, related) == ERROR_SUCCESS; ++i) {
        found = CompareStringOrdinal(product.c_str(), -1, related, -1, TRUE) == CSTR_EQUAL;
      }
      if (!found) return {};
    }

    PMSIHANDLE db = MsiGetActiveDatabase(session), view, row;
    wchar_t component[39];
    DWORD size = 39;
    if (!db || MsiDatabaseOpenViewW(db, L"SELECT \x60ComponentId\x60 FROM \x60Component\x60 WHERE \x60Component\x60='SunshineSvc'", &view) != ERROR_SUCCESS
        || MsiViewExecute(view, 0) != ERROR_SUCCESS || MsiViewFetch(view, &row) != ERROR_SUCCESS
        || MsiRecordGetStringW(row, 1, component, &size) != ERROR_SUCCESS) return {};
    size = 32768;
    std::wstring path(size, L'\0');
    if (MsiGetComponentPathExW(product.c_str(), component, nullptr, MSIINSTALLCONTEXT_MACHINE, path.data(), &size) != INSTALLSTATE_LOCAL) return {};
    path.resize(size);
    return path;
  }

  service_ctrl::startup_owner owns_service(const std::wstring &path) {
    return [path](const QUERY_SERVICE_CONFIGW &config) {
      if (config.dwServiceType != SERVICE_WIN32_OWN_PROCESS || !config.lpDisplayName || !config.lpBinaryPathName
          || CompareStringOrdinal(config.lpDisplayName, -1, L"Vibeshine Service", -1, TRUE) != CSTR_EQUAL) return false;
      const auto quoted = L"\"" + path + L"\" --service";
      const auto plain = path + L" --service";
      return CompareStringOrdinal(config.lpBinaryPathName, -1, quoted.c_str(), -1, TRUE) == CSTR_EQUAL
        || (path.find_first_of(L" \t") == std::wstring::npos
          && CompareStringOrdinal(config.lpBinaryPathName, -1, plain.c_str(), -1, TRUE) == CSTR_EQUAL);
    };
  }
}

extern "C" __declspec(dllexport) UINT __stdcall CaptureManualServiceStartup(MSIHANDLE session) {
  try {
    const auto path = installed_service_path(session);
    // AppSearch properties are public. Confirm actual Manual and ownership
    // before writing the path used by the deferred actions.
    if (!path.empty() && service_ctrl::get_service_startup(owns_service(path)).start_type == SERVICE_DEMAND_START) {
      MsiSetPropertyW(session, manual_startup_path, path.c_str());
    }
  } catch (...) {}
  return ERROR_SUCCESS;
}

extern "C" __declspec(dllexport) UINT __stdcall RestoreManualServiceStartup(MSIHANDLE session) {
  try {
    const auto path = property(session, L"CustomActionData");
    if (!path.empty()) service_ctrl::set_service_startup(false, SERVICE_AUTO_START, owns_service(path));
  } catch (...) {}
  return ERROR_SUCCESS;
}

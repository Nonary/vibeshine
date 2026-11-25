// Embedded in the MSI Binary table alongside the existing installer actions.
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <msiquery.h>
#include <winsvc.h>

#include "../../src/platform/windows/service_startup.h"

namespace {
  // Mixed case makes this property private to the execute sequence. Neither
  // custom action accepts a service name or a startup type from the caller.
  constexpr wchar_t manual_startup_property[] = L"VibeshineManualServiceStartup";

  void log_startup(MSIHANDLE session, const wchar_t *message, DWORD status) {
    const MSIHANDLE record = MsiCreateRecord(2);
    if (record) {
      MsiRecordSetStringW(record, 0, L"Service startup: [1] (status [2])");
      MsiRecordSetStringW(record, 1, message);
      MsiRecordSetInteger(record, 2, static_cast<int>(status));
      MsiProcessMessage(session, INSTALLMESSAGE_INFO, record);
      MsiCloseHandle(record);
    }
  }
}

extern "C" __declspec(dllexport) UINT __stdcall CaptureManualServiceStartup(MSIHANDLE session) {
  try {
    const auto before = service_ctrl::get_service_startup();
    if (before.status && before.start_type == SERVICE_DEMAND_START) {
      const auto result = MsiSetPropertyW(session, manual_startup_property, L"1");
      if (result != ERROR_SUCCESS) {
        log_startup(session, L"could not record Manual; using installer defaults", result);
      }
    } else if (!before.status) {
      log_startup(session, L"could not read the prior service mode; using installer defaults", before.error_code);
    }
  } catch (...) {
    log_startup(session, L"could not inspect the prior service mode; using installer defaults", ERROR_INVALID_DATA);
  }
  return ERROR_SUCCESS;
}

extern "C" __declspec(dllexport) UINT __stdcall RestoreManualServiceStartup(MSIHANDLE session) {
  try {
    // InstallServices can recreate the service as Automatic. Leave an
    // already-Manual service and any other service mode alone.
    const auto restored = service_ctrl::set_service_startup(false, SERVICE_AUTO_START);
    if (!restored.status) {
      log_startup(session, L"could not restore Manual", restored.error_code);
    } else if (restored.start_type == SERVICE_DEMAND_START) {
      log_startup(session, L"retained Manual", ERROR_SUCCESS);
    } else {
      log_startup(session, L"left an unexpected service mode alone", ERROR_SUCCESS);
    }
  } catch (...) {
    log_startup(session, L"could not restore Manual", ERROR_INVALID_DATA);
  }
  return ERROR_SUCCESS;
}

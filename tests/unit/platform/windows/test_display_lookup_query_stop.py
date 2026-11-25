"""Replay the production DXGI and passive Sunshine SetupAPI lookup boundaries."""
from pathlib import Path
import os
import re
import subprocess
import sys
import tempfile


def section(source, first, last):
    start = source.index(first)
    end = re.search(r"^    }$", source[source.index(last, start):], re.M)
    assert end
    return source[start:source.index(last, start) + end.end()]


def replay(root):
    misc = (root / "src/platform/windows/misc.cpp").read_text()
    adapter = section(misc, "    struct enumerated_adapter_t {", "    adapter_enumeration_t enumerate_dxgi_adapters(")
    assert "enumerate_dxgi_adapters(display_lookup_query_t {checkpoint})" in misc
    capture = (root / "src/platform/windows/display_base.cpp").read_text()
    for text in (capture, misc[misc.index("  bool configured_capture_adapter_has_output("):]):
        assert re.search(r"resolve_adapter\(\s*config::video.adapter_name,\s*config::video.adapter_pnp_id,\s*"
                         r"display_device::check_display_query_stop\s*\)", text)
    sunshine = (root / "src/platform/windows/virtual_display_sunshine.cpp").read_text()
    vdd = section(sunshine, "    bool equals_ci(std::wstring_view", "    std::optional<std::wstring> find_virtual_display_device_instance_id(")
    passive = sunshine[sunshine.index("  static bool is_sunshine_driver_installed_passive()"):
                       sunshine.index("  bool isVirtualDisplayDriverInstalled() {")]
    assert "display_lookup_query_t query {display_device::check_display_query_stop}" in passive
    assert "find_virtual_display_device_instance_id(query).has_value()" in passive

    return r"""
#include "src/platform/windows/display_lookup_query.h"
#include "src/utility.h"
#include <display_device/windows/win_display_query_stop.h>
#include <algorithm>
#include <cwchar>
#include <cwctype>
#include <iostream>
#include <sstream>
#include <string_view>

#undef FAILED
#undef IID_PPV_ARGS
#undef INVALID_HANDLE_VALUE
#undef ERROR_SUCCESS
#undef ERROR_INSUFFICIENT_BUFFER
#undef ERROR_NO_MORE_ITEMS
#undef DXGI_ERROR_NOT_FOUND
#undef DXGI_ADAPTER_FLAG_SOFTWARE
#undef DIGCF_PRESENT
#undef SPDRP_HARDWAREID
#undef SPDRP_FRIENDLYNAME
#undef REG_MULTI_SZ
#undef REG_SZ
#undef REG_EXPAND_SZ
#define IID_PPV_ARGS(address) (address)
#define BOOST_LOG(level) std::ostringstream {}

namespace fixture {
using platf::display_lookup_query_t;
using DWORD = unsigned long;
using UINT = unsigned int;
using HRESULT = long;
using HDEVINFO = void *;
using PBYTE = unsigned char *;
struct LUID {};
struct SP_DEVINFO_DATA { DWORD cbSize = 0; };
struct SP_DEVICE_INTERFACE_DATA { DWORD cbSize = 0; };
struct SP_DEVICE_INTERFACE_DETAIL_DATA_W { DWORD cbSize; wchar_t DevicePath[8]; };
struct DISPLAYCONFIG_HEADER { DWORD type, size; LUID adapterId; DWORD id; };
struct DISPLAYCONFIG_ADAPTER_NAME { DISPLAYCONFIG_HEADER header; wchar_t adapterDevicePath[8]; };
struct DXGI_ADAPTER_DESC1 { LUID AdapterLuid {}; UINT Flags = 0; };
const auto INVALID_HANDLE_VALUE = reinterpret_cast<void *>(-1);
constexpr DWORD DISPLAYCONFIG_DEVICE_INFO_GET_ADAPTER_NAME = 1, GUID_DEVCLASS_DISPLAY = 1, DIGCF_PRESENT = 2;
constexpr DWORD ERROR_SUCCESS = 0, ERROR_INSUFFICIENT_BUFFER = 122;
constexpr DWORD SPDRP_HARDWAREID = 1, SPDRP_FRIENDLYNAME = 2, ERROR_NO_MORE_ITEMS = 259;
constexpr DWORD REG_MULTI_SZ = 7, REG_SZ = 1, REG_EXPAND_SZ = 2;
constexpr UINT DXGI_ADAPTER_FLAG_SOFTWARE = 2;
constexpr HRESULT DXGI_ERROR_NOT_FOUND = -1;
constexpr std::wstring_view SUNSHINE_DRIVER_HARDWARE_ID = L"SUN-HW", SUNSHINE_DRIVER_FRIENDLY_NAME_W = L"SUN";

std::stop_source *worker_stop = nullptr;
int native_calls = 0, stop_after = 0, sets_closed = 0;
DWORD last_error = 0;
void native_call() {
  if (++native_calls == stop_after) worker_stop->request_stop();
}
bool FAILED(HRESULT result) { return result < 0; }
void SetLastError(DWORD error) { last_error = error; }
DWORD GetLastError() { return last_error; }
std::string to_utf8(const std::wstring &text) { return {text.begin(), text.end()}; }

namespace Microsoft::WRL {
template<class T>
struct ComPtr {
  T *ptr = nullptr;
  ~ComPtr() { delete ptr; }
  T **GetAddressOf() { return &ptr; }
  T *operator->() { return ptr; }
  explicit operator bool() const { return ptr != nullptr; }
};
}

struct IDXGIAdapter1 {
  HRESULT GetDesc1(DXGI_ADAPTER_DESC1 *) { native_call(); return 0; }
};
struct IDXGIFactory1 {
  HRESULT EnumAdapters1(UINT index, IDXGIAdapter1 **adapter) {
    native_call();
    if (index) return DXGI_ERROR_NOT_FOUND;
    *adapter = new IDXGIAdapter1;
    return 0;
  }
};
HRESULT CreateDXGIFactory1(IDXGIFactory1 **factory) {
  native_call();
  *factory = new IDXGIFactory1;
  return 0;
}
long DisplayConfigGetDeviceInfo(DISPLAYCONFIG_HEADER *header) {
  native_call();
  std::wcscpy(reinterpret_cast<DISPLAYCONFIG_ADAPTER_NAME *>(header)->adapterDevicePath, L"device");
  return ERROR_SUCCESS;
}
HDEVINFO SetupDiCreateDeviceInfoList(void *, void *) { native_call(); return &last_error; }
bool SetupDiDestroyDeviceInfoList(HDEVINFO) { ++sets_closed; return true; }
bool SetupDiOpenDeviceInterfaceW(HDEVINFO, const wchar_t *, DWORD, SP_DEVICE_INTERFACE_DATA *) {
  native_call();
  return true;
}
bool SetupDiGetDeviceInterfaceDetailW(HDEVINFO, SP_DEVICE_INTERFACE_DATA *, SP_DEVICE_INTERFACE_DETAIL_DATA_W *out,
                                      DWORD, DWORD *needed, SP_DEVINFO_DATA *) {
  native_call();
  if (!out) {
    *needed = sizeof(*out);
    SetLastError(ERROR_INSUFFICIENT_BUFFER);
    return false;
  }
  std::wcscpy(out->DevicePath, L"device");
  return true;
}
bool SetupDiGetDeviceInstanceIdW(HDEVINFO, SP_DEVINFO_DATA *, wchar_t *out, DWORD, DWORD *needed) {
  native_call();
  if (!out) {
    *needed = 3;
    SetLastError(ERROR_INSUFFICIENT_BUFFER);
    return false;
  }
  std::wcscpy(out, L"ID");
  return true;
}
HDEVINFO SetupDiGetClassDevsW(const DWORD *, const wchar_t *, void *, DWORD) {
  native_call();
  return &last_error;
}
bool SetupDiEnumDeviceInfo(HDEVINFO, DWORD index, SP_DEVINFO_DATA *) {
  native_call();
  if (index) SetLastError(ERROR_NO_MORE_ITEMS);
  return index == 0;
}
bool SetupDiGetDeviceRegistryPropertyW(HDEVINFO, SP_DEVINFO_DATA *, DWORD property, DWORD *kind,
                                      PBYTE out, DWORD, DWORD *needed) {
  native_call();
  // Use a different hardware ID so the real friendly-name fallback also runs.
  const auto *words = property == SPDRP_HARDWAREID ? L"OTHER" : L"SUN";
  *kind = property == SPDRP_HARDWAREID ? REG_MULTI_SZ : REG_SZ;
  *needed = static_cast<DWORD>((std::wcslen(words) + 2) * sizeof(wchar_t));
  if (!out) {
    SetLastError(ERROR_INSUFFICIENT_BUFFER);
    return false;
  }
  std::wcscpy(reinterpret_cast<wchar_t *>(out), words);
  return true;
}
""" + adapter + "\n" + vdd + r"""

template<class Lookup>
bool test_boundaries(const char *name, int success_calls, int handle_call, Lookup lookup) {
  bool ok = true;
  auto check = [&](bool result, const char *what) {
    if (!result) {
      std::cerr << name << ": " << what << " (stop after " << stop_after << ")\n";
      ok = false;
    }
  };
  for (int boundary = 0; boundary <= success_calls; ++boundary) {
    std::stop_source stop;
    worker_stop = &stop;
    native_calls = sets_closed = 0;
    stop_after = boundary;
    display_device::DisplayQueryStopGuard guard {stop.get_token()};
    bool stopped = false;
    try {
      check(lookup(display_lookup_query_t {display_device::check_display_query_stop}), "lookup preserves the resolved identity");
    } catch (const display_device::DisplayQueryStopped &) {
      stopped = true;
    }
    check(stopped == (boundary != 0), "cancellation is distinct from a missing identity");
    check(native_calls == (boundary ? boundary : success_calls), "no next native query after stop");
    check(sets_closed == ((boundary == 0 || boundary >= handle_call) ? 1 : 0), "device info set is released exactly once");
  }

  {
    std::stop_source stop;
    worker_stop = &stop;
    native_calls = sets_closed = 0;
    stop_after = 1;
    display_device::DisplayQueryStopGuard guard {stop.get_token()};
    check(lookup(display_lookup_query_t {}), "default lookup retains its ordinary result");
    check(native_calls == success_calls && sets_closed == 1, "default lookup completes with normal cleanup");
  }
  if (ok) std::cout << name << ": live, default and " << success_calls << " native stop boundaries passed\n";
  return ok;
}
}  // namespace fixture

int main() {
  using namespace fixture;
  const auto explicit_gpu = test_boundaries("Explicit DXGI adapter", 11, 5, [](const auto &query) {
    const auto result = enumerate_dxgi_adapters(query);
    return result.enumeration_complete && result.identity_complete &&
           result.adapters.size() == 1 && result.adapters.front().pnp_id == "ID";
  });
  const auto passive_vdd = test_boundaries("Passive Sunshine device", 8, 1, [](const auto &query) {
    return find_virtual_display_device_instance_id(query) == L"ID";
  });
  return !(explicit_gpu && passive_vdd);
}
"""


if __name__ == "__main__":
    root = Path(sys.argv[1]).resolve()
    lib = Path(sys.argv[3]).resolve() if len(sys.argv) > 3 else root / "third-party/libdisplaydevice"
    with tempfile.TemporaryDirectory(prefix="display-lookup-stop-") as directory:
        source = Path(directory) / "lookup.cpp"
        binary = Path(directory) / ("lookup.exe" if os.name == "nt" else "lookup")
        source.write_text(replay(root))
        subprocess.run([sys.argv[2], "-std=c++23", "-pthread", "-Wall", "-Wextra", "-Werror",
                        "-I", str(root), "-I", str(lib / "src/windows/include"), str(source),
                        "-o", str(binary)], check=True)
        subprocess.run([str(binary)], check=True)

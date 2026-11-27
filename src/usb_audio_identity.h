#pragma once
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <mmdeviceapi.h>
#include <cfgmgr32.h>
#include <devpkey.h>
#include <functiondiscoverykeys_devpkey.h>
#include <wrl/client.h>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>
#include <algorithm>
#include <cstring>
#include "usbip_win2_abi.h"

namespace usb_lab {
using Microsoft::WRL::ComPtr;
inline void Need(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
inline void Check(HRESULT hr, const char* message) { Need(SUCCEEDED(hr), message); }
inline std::string Utf8(const std::wstring& value) {
    if (value.empty()) return {};
    const int count = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    Need(count > 0, "Invalid Unicode"); std::string result(static_cast<size_t>(count), '\0');
    Need(WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), result.data(), count, nullptr, nullptr) == count, "UTF8 conversion failed");
    return result;
}
inline std::string Quote(const std::string& value) {
    std::ostringstream s; s << '"';
    for (const unsigned char c : value) {
        if (c == '"' || c == '\\') s << '\\' << static_cast<char>(c);
        else if (c < 32) s << "\\u" << std::hex << std::setw(4) << std::setfill('0') << static_cast<unsigned>(c) << std::dec;
        else s << static_cast<char>(c);
    }
    s << '"'; return s.str();
}
inline std::string Hex(HRESULT hr) { std::ostringstream s; s << "0x" << std::hex << std::uppercase << std::setw(8) << std::setfill('0') << static_cast<unsigned long>(hr); return s.str(); }
inline LONGLONG Qpc() { LARGE_INTEGER n{}; Need(QueryPerformanceCounter(&n) != FALSE, "QPC failed"); return n.QuadPart; }
inline uint32_t Number(const std::wstring& value) {
    Need(!value.empty() && value.find_first_of(L"-+ \r\n\t") == std::wstring::npos, "Invalid number");
    size_t used = 0; const auto n = std::stoul(value, &used, 0); Need(used == value.size() && n <= UINT32_MAX, "Invalid number"); return static_cast<uint32_t>(n);
}
struct Identity { uint32_t vid = 0, pid = 0; std::wstring serial, hostInstance, service; int port = 0; };
inline void Validate(const Identity& id) {
    Need(id.vid == 0x1209 && id.pid == 0x7140, "Private requested Vibe Speakers identity 1209:7140 required");
    Need(id.serial.size() == 15 && id.serial.substr(0, 2) == L"VA" && id.serial.substr(2).find_first_not_of(L"0123456789abcdefABCDEF") == std::wstring::npos, "Expected exact VA plus 13 hex lab serial");
}
inline std::wstring HardwarePrefix(const Identity& id) { std::wostringstream s; s << L"USB\\VID_" << std::uppercase << std::hex << std::setw(4) << std::setfill(L'0') << id.vid << L"&PID_" << std::setw(4) << id.pid; return s.str(); }
inline bool Same(const std::wstring& a, const std::wstring& b) { return _wcsicmp(a.c_str(), b.c_str()) == 0; }
inline bool IsUsbIdentity(const std::wstring& node, const Identity& id) {
    const auto prefix = HardwarePrefix(id); const auto slash = node.find(L'\\', prefix.size());
    if (slash == std::wstring::npos || !Same(node.substr(0, prefix.size()), prefix)) return false;
    // Match only the physical USB function's serial, not an MI interface or a prefix collision.
    if (slash != prefix.size()) return false;
    return Same(node.substr(slash + 1), id.serial);
}
struct Apartment { HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED); Apartment() { Check(hr, "COM initialize failed"); } ~Apartment() { CoUninitialize(); } };
inline std::wstring DevProperty(DEVINST node, const DEVPROPKEY& key) {
    wchar_t text[1024]{}; ULONG bytes = sizeof(text); DEVPROPTYPE type = 0;
    if (CM_Get_DevNode_PropertyW(node, &key, &type, reinterpret_cast<PBYTE>(text), &bytes, 0) != CR_SUCCESS || type != DEVPROP_TYPE_STRING || bytes < sizeof(wchar_t) || bytes > sizeof(text) || text[bytes / sizeof(wchar_t) - 1] != 0) return {};
    return text;
}
template<size_t N> inline bool ExactAscii(const char (&text)[N], const std::wstring& expected) {
    const auto value = Utf8(expected); return value.size() < N && std::equal(value.begin(), value.end(), text) && text[value.size()] == 0;
}
inline bool ImportedMatches(const vibe::usb_audio::driver_abi::imported_device& row, const Identity& id) {
    return row.port == id.port && row.vendor == id.vid && row.product == id.pid && row.speed == 3 && row.devid == 0x10001 &&
        ExactAscii(row.host, L"127.0.0.1") && ExactAscii(row.service, id.service) && ExactAscii(row.serial, id.serial);
}
struct OwnedUsb { DEVINST node = 0; GUID container{}; std::wstring instance, service, audioFunction; };
inline OwnedUsb ResolveOwnedUsb(const Identity& id) {
    Validate(id); Need(id.port > 0 && id.port <= 255 && !id.hostInstance.empty() && id.service.size() >= 1 && id.service.size() <= 5 && id.service.find_first_not_of(L"0123456789") == std::wstring::npos, "Exact attached host/port/service required");
    Need(Number(id.service) > 0 && Number(id.service) <= 65535, "Invalid loopback TCP service");
    using namespace vibe::usb_audio; auto guid = driver_abi::interface_guid;
    DEVINST host = 0; Need(CM_Locate_DevNodeW(&host, const_cast<wchar_t*>(id.hostInstance.c_str()), CM_LOCATE_DEVNODE_NORMAL) == CR_SUCCESS, "Owned host controller missing");
    ULONG count = 0; Need(CM_Get_Device_Interface_List_SizeW(&count, &guid, const_cast<wchar_t*>(id.hostInstance.c_str()), CM_GET_DEVICE_INTERFACE_LIST_PRESENT) == CR_SUCCESS && count >= 2 && count <= 65536, "Owned UDE interface unavailable");
    std::vector<wchar_t> paths(count); Need(CM_Get_Device_Interface_ListW(&guid, const_cast<wchar_t*>(id.hostInstance.c_str()), paths.data(), count, CM_GET_DEVICE_INTERFACE_LIST_PRESENT) == CR_SUCCESS, "Owned interface enumeration changed");
    size_t interfaceCount = 0; const wchar_t* chosen = nullptr;
    for (size_t i = 0; i + 1 < paths.size() && paths[i];) { const auto end = std::find(paths.begin() + i, paths.end(), L'\0'); Need(end != paths.end(), "Unterminated interface list"); chosen = paths.data() + i; ++interfaceCount; i = static_cast<size_t>(end - paths.begin()) + 1; }
    if (interfaceCount != 1 || !chosen) throw std::runtime_error("Expected exactly one owned UDE interface");
    DEVPROPTYPE type = 0; wchar_t actual[MAX_DEVICE_ID_LEN]{}; ULONG bytes = sizeof(actual);
    Need(CM_Get_Device_Interface_PropertyW(chosen, &DEVPKEY_Device_InstanceId, &type, reinterpret_cast<PBYTE>(actual), &bytes, 0) == CR_SUCCESS && type == DEVPROP_TYPE_STRING && bytes >= 2 && bytes <= sizeof(actual) && actual[bytes / 2 - 1] == 0 && Same(actual, id.hostInstance), "Interface controller identity mismatch");
    struct File { HANDLE h = INVALID_HANDLE_VALUE; ~File() { if (h != INVALID_HANDLE_VALUE) CloseHandle(h); } } file;
    file.h = CreateFileW(chosen, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    Need(file.h != INVALID_HANDLE_VALUE, "Cannot open owned UDE query interface");
    // This is the released driver's read-only GET_IMPORTED_DEVICES operation;
    // no attach, detach, format change or USB control transfer is issued.
    ULONG query = sizeof(driver_abi::get_imported_devices); DWORD returned = 0;
    std::vector<uint8_t> rows(driver_abi::imported_devices_offset + 255 * sizeof(driver_abi::imported_device));
    Need(DeviceIoControl(file.h, driver_abi::get_imported_devices_ioctl, &query, sizeof(query), rows.data(), static_cast<DWORD>(rows.size()), &returned, nullptr) != FALSE && returned >= driver_abi::imported_devices_offset && returned <= rows.size() && (returned - driver_abi::imported_devices_offset) % sizeof(driver_abi::imported_device) == 0, "Imported-device ABI query failed");
    unsigned matches = 0;
    for (size_t offset = driver_abi::imported_devices_offset; offset < returned; offset += sizeof(driver_abi::imported_device)) { driver_abi::imported_device row{}; std::memcpy(&row, rows.data() + offset, sizeof(row)); if (ImportedMatches(row, id)) ++matches; }
    Need(matches == 1, "Exact live imported host/port/service/serial/VID/PID tuple is not unique");
    OwnedUsb owned; unsigned deviceMatches = 0; DEVINST hub = 0;
    Need(CM_Get_Child(&hub, host, 0) == CR_SUCCESS, "Owned controller has no hub");
    for (unsigned hubs = 0; hubs < 32; ++hubs) {
        DEVINST child = 0;
        if (CM_Get_Child(&child, hub, 0) == CR_SUCCESS) for (unsigned n = 0; n < 256; ++n) {
            wchar_t nodeId[MAX_DEVICE_ID_LEN]{}; ULONG address = 0, regType = 0, addressBytes = sizeof(address);
            const auto prefix = HardwarePrefix(id) + L"\\";
            if (CM_Get_Device_IDW(child, nodeId, MAX_DEVICE_ID_LEN, 0) == CR_SUCCESS && _wcsnicmp(nodeId, prefix.c_str(), prefix.size()) == 0 && CM_Get_DevNode_Registry_PropertyW(child, CM_DRP_ADDRESS, &regType, &address, &addressBytes, 0) == CR_SUCCESS && regType == REG_DWORD && addressBytes == sizeof(address) && address == static_cast<ULONG>(id.port)) { owned.node = child; owned.instance = nodeId; ++deviceMatches; }
            DEVINST next = 0; if (CM_Get_Sibling(&next, child, 0) != CR_SUCCESS) break; child = next;
        }
        DEVINST next = 0; if (CM_Get_Sibling(&next, hub, 0) != CR_SUCCESS) break; hub = next;
    }
    Need(deviceMatches == 1, "Owned USB port does not identify exactly one VID/PID function");
    bytes = sizeof(owned.container); type = 0;
    const GUID emptyContainer{};
    Need(CM_Get_DevNode_PropertyW(owned.node, &DEVPKEY_Device_ContainerId, &type, reinterpret_cast<PBYTE>(&owned.container), &bytes, 0) == CR_SUCCESS && type == DEVPROP_TYPE_GUID && bytes == sizeof(GUID) && owned.container != emptyContainer, "USB container unavailable");
    // The owned port may be a composite usbccgp parent. Bind its one inbox
    // audio function below that exact parent, never another matching VID/PID.
    std::vector<std::pair<DEVINST, unsigned>> pending{{owned.node, 0}}; unsigned audioFunctions = 0, visited = 0;
    while (!pending.empty()) {
        const auto current = pending.back(); pending.pop_back(); Need(++visited <= 256, "Owned USB descendant bound exceeded");
        const auto service = DevProperty(current.first, DEVPKEY_Device_Service);
        if (Same(service, L"usbaudio2") || Same(service, L"usbaudio")) {
            wchar_t function[MAX_DEVICE_ID_LEN]{}; Need(CM_Get_Device_IDW(current.first, function, MAX_DEVICE_ID_LEN, 0) == CR_SUCCESS, "Audio function instance unavailable");
            const auto prefix = HardwarePrefix(id); const std::wstring functionId = function;
            const bool ownName = current.first == owned.node ||
                (functionId.size() > prefix.size() + 6 && Same(functionId.substr(0, prefix.size()), prefix) &&
                 Same(functionId.substr(prefix.size(), 4), L"&MI_") && functionId[prefix.size() + 6] == L'\\' &&
                 functionId.substr(prefix.size() + 4, 2).find_first_not_of(L"0123456789abcdefABCDEF") == std::wstring::npos);
            GUID functionContainer{}; ULONG functionBytes = sizeof(functionContainer); DEVPROPTYPE functionType = 0; ULONG nodeStatus = 0, problem = 0;
            Need(ownName && CM_Get_DevNode_PropertyW(current.first, &DEVPKEY_Device_ContainerId, &functionType, reinterpret_cast<PBYTE>(&functionContainer), &functionBytes, 0) == CR_SUCCESS && functionType == DEVPROP_TYPE_GUID && functionBytes == sizeof(GUID) && functionContainer == owned.container &&
                CM_Get_DevNode_Status(&nodeStatus, &problem, current.first, 0) == CR_SUCCESS && problem == 0 && (nodeStatus & DN_STARTED), "Owned audio function identity/container/state mismatch");
            owned.service = service; owned.audioFunction = functionId; ++audioFunctions;
        }
        if (current.second >= 8) continue;
        DEVINST child = 0; if (CM_Get_Child(&child, current.first, 0) == CR_SUCCESS) for (unsigned siblings = 0; siblings < 256; ++siblings) {
            Need(pending.size() < 256, "Owned USB descendant capacity exceeded"); pending.emplace_back(child, current.second + 1);
            DEVINST next = 0; if (CM_Get_Sibling(&next, child, 0) != CR_SUCCESS) break; child = next;
        }
    }
    Need(audioFunctions == 1, "Owned USB port must have exactly one active inbox audio function");
    return owned;
}
struct BoundEndpoint {
    ComPtr<IMMDevice> device; std::wstring endpoint, usbInstance, service, friendly, audioFunction;
    std::vector<std::wstring> ancestry;
    std::string Json(const Identity& id) const {
        std::string ancestors = "["; for (const auto& n : ancestry) { if (ancestors.size() > 1) ancestors += ','; ancestors += Quote(Utf8(n)); } ancestors += ']';
        return "{\"endpoint_id\":" + Quote(Utf8(endpoint)) + ",\"usb_instance\":" + Quote(Utf8(usbInstance)) + ",\"audio_function_instance\":" + Quote(Utf8(audioFunction)) + ",\"vid\":" + std::to_string(id.vid) + ",\"pid\":" + std::to_string(id.pid) + ",\"serial\":" + Quote(Utf8(id.serial)) + ",\"host_instance\":" + Quote(Utf8(id.hostInstance)) + ",\"port\":" + std::to_string(id.port) + ",\"loopback_service\":" + Quote(Utf8(id.service)) + ",\"binding_method\":\"imported_tuple_owned_port_audio_function_container\",\"audio_service\":" + Quote(Utf8(service)) + ",\"friendly_name\":" + Quote(Utf8(friendly)) + ",\"binding_nodes\":" + ancestors + ",\"active_render\":true,\"identity_verified\":true}";
    }
};
inline BoundEndpoint Bind(IMMDevice* device, const Identity& expected) {
    Validate(expected); BoundEndpoint b; b.device = device; DWORD state = 0; Check(device->GetState(&state), "Endpoint state failed"); Need(state == DEVICE_STATE_ACTIVE, "Endpoint not active");
    ComPtr<IMMEndpoint> ep; Check(device->QueryInterface(IID_PPV_ARGS(&ep)), "Endpoint flow unavailable"); EDataFlow flow = eAll; Check(ep->GetDataFlow(&flow), "Read flow failed"); Need(flow == eRender, "Render endpoint required");
    LPWSTR raw = nullptr; Check(device->GetId(&raw), "Get endpoint ID failed"); b.endpoint = raw ? raw : L""; CoTaskMemFree(raw); Need(!b.endpoint.empty(), "Empty endpoint ID");
    const auto owned = ResolveOwnedUsb(expected); b.usbInstance = owned.instance; b.service = owned.service; b.audioFunction = owned.audioFunction; b.ancestry = {b.endpoint, owned.audioFunction, owned.instance, expected.hostInstance};
    ComPtr<IPropertyStore> props; Check(device->OpenPropertyStore(STGM_READ, &props), "Endpoint properties unavailable");
    const PROPERTYKEY containerKey{DEVPKEY_Device_ContainerId.fmtid, DEVPKEY_Device_ContainerId.pid}; PROPVARIANT container{};
    const HRESULT containerHr = props->GetValue(containerKey, &container); const bool sameContainer = SUCCEEDED(containerHr) && container.vt == VT_CLSID && container.puuid && *container.puuid == owned.container; PropVariantClear(&container);
    Need(sameContainer, "Endpoint container differs from exact owned USB port");
    PROPVARIANT value{}; if (SUCCEEDED(props->GetValue(PKEY_Device_FriendlyName, &value)) && value.vt == VT_LPWSTR && value.pwszVal) b.friendly = value.pwszVal; PropVariantClear(&value);
    return b;
}
inline ComPtr<IMMDeviceEnumerator> Enumerator() { ComPtr<IMMDeviceEnumerator> result; Check(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&result)), "Create enumerator failed"); return result; }
inline BoundEndpoint Resolve(const std::wstring& endpoint, const Identity& id) { Need(!endpoint.empty(), "Explicit endpoint required"); auto e = Enumerator(); ComPtr<IMMDevice> d; Check(e->GetDevice(endpoint.c_str(), &d), "Resolve endpoint failed"); return Bind(d.Get(), id); }
inline void Save(const std::wstring& path, const std::string& json) {
    Need(!path.empty(), "Output path required"); Need(json.size() < MAXDWORD, "Evidence too large");
    // CREATE_NEW makes the no-overwrite contract atomic, including concurrent writers.
    struct File { HANDLE h = INVALID_HANDLE_VALUE; ~File() { if (h != INVALID_HANDLE_VALUE) CloseHandle(h); } } file;
    file.h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    Need(file.h != INVALID_HANDLE_VALUE, "Cannot create new evidence; existing files are never replaced");
    DWORD written = 0;
    Need(WriteFile(file.h, json.data(), static_cast<DWORD>(json.size()), &written, nullptr) != FALSE && written == json.size(), "Cannot write evidence");
    const char newline = '\n'; Need(WriteFile(file.h, &newline, 1, &written, nullptr) != FALSE && written == 1, "Cannot finish evidence");
    Need(FlushFileBuffers(file.h) != FALSE, "Cannot flush evidence");
}
} // namespace usb_lab

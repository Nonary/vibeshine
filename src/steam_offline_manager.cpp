#include "steam_offline_manager.h"
#include "steam_offline_policy.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstring>
#include <fstream>
#include <limits>
#include <ranges>
#include <sstream>
#include <stdexcept>

#include <nlohmann/json.hpp>

#ifdef _WIN32
  #include <Windows.h>
  #include <fwpmu.h>
  #include <sddl.h>
  #include <Aclapi.h>
  #pragma comment(lib, "fwpuclnt.lib")
#endif

namespace steam_offline {
  namespace {
    constexpr std::size_t max_files = 4096;
    constexpr std::uintmax_t max_file_bytes = 1ULL << 31;
    constexpr std::uintmax_t max_tree_bytes = 8ULL << 30;
    constexpr std::wstring_view volatile_directory_names[] = {
      L"steamapps", L"userdata", L"htmlcache", L"logs", L"dumps", L"confightml", L"package"};

    bool valid_seat(std::string_view value) {
      return !value.empty() && value.size() <= 64 && std::ranges::all_of(value, [](unsigned char ch) {
        return std::isalnum(ch) || ch == '-' || ch == '_' || ch == '.';
      });
    }

#ifdef _WIN32
    bool set_mirror_acl(const std::filesystem::path &path, std::string_view user_sid) {
      if (user_sid.empty()) return false;
      std::wstring sid;
      sid.reserve(user_sid.size());
      for (const auto ch : user_sid) sid.push_back(static_cast<wchar_t>(static_cast<unsigned char>(ch)));
      const std::wstring sddl = L"D:P(A;;FA;;;SY)(A;;FA;;;BA)(A;;0x120089;;;" + sid + L")";
      PSECURITY_DESCRIPTOR descriptor = nullptr;
      if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(), SDDL_REVISION_1, &descriptor, nullptr)) return false;
      PACL dacl = nullptr;
      BOOL present = FALSE, defaulted = FALSE;
      const bool read = GetSecurityDescriptorDacl(descriptor, &present, &dacl, &defaulted) != FALSE;
      auto mutable_path = path.wstring();
      const bool applied = read && present && dacl &&
        SetNamedSecurityInfoW(mutable_path.data(), SE_FILE_OBJECT, DACL_SECURITY_INFORMATION,
                              nullptr, nullptr, dacl, nullptr) == ERROR_SUCCESS;
      LocalFree(descriptor);
      return applied;
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
      auto p = std::filesystem::weakly_canonical(path);
      auto r = std::filesystem::weakly_canonical(root);
      auto pit = p.begin();
      for (auto rit = r.begin(); rit != r.end(); ++rit, ++pit) {
        if (pit == p.end() || *pit != *rit) return false;
      }
      return true;
    }

    bool excluded_directory(const std::filesystem::path &relative) {
      return std::ranges::any_of(relative, [](const auto &part) {
        const auto value = part.wstring();
        return std::ranges::any_of(volatile_directory_names, [&](std::wstring_view name) {
          return _wcsicmp(value.c_str(), name.data()) == 0;
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

    void remove_stale_filters(HANDLE engine, std::wstring_view seat, std::uint64_t generation) {
      UINT32 count = 0;
      FWPM_FILTER0 **filters = nullptr;
      if (FwpmFilterEnum0(engine, nullptr, 4096, &filters, &count) != ERROR_SUCCESS) return;
      const auto provider = guid(provider_key_bytes);
      const std::wstring marker = L"Vibeshine Steam seat=" + std::wstring {seat} + L" generation=";
      const auto current = std::to_wstring(generation);
      for (UINT32 i = 0; i < count; ++i) {
        const auto description = filters[i] && filters[i]->displayData.description ?
          std::wstring_view {filters[i]->displayData.description} : std::wstring_view {};
        if (filters[i] && filters[i]->providerKey && std::memcmp(filters[i]->providerKey, &provider, sizeof(provider)) == 0 &&
            description.starts_with(marker) && description.substr(marker.size()).find(current) != 0) {
          (void)FwpmFilterDeleteByKey0(engine, &filters[i]->filterKey);
        }
      }
      FwpmFreeMemory0(reinterpret_cast<void **>(&filters));
    }
#endif
  }

  manager_t::~manager_t() {
    std::string ignored;
    (void)release(ignored);
  }

  manager_t::manager_t(manager_t &&other) noexcept
      : engine_(other.engine_), filter_keys_(std::move(other.filter_keys_)), preparation_(std::move(other.preparation_)),
        seat_id_(std::move(other.seat_id_)), generation_(other.generation_) {
    other.engine_ = nullptr;
    other.generation_ = 0;
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
    other.engine_ = nullptr;
    other.generation_ = 0;
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
                          const std::string_view seat_id, const std::string_view user_sid,
                          const std::uint64_t generation,
                          preparation_t &result, std::string &error) noexcept {
    if (active() || !valid_seat(seat_id) || user_sid.empty() || generation == 0) {
      error = "Steam isolation admission identity is invalid or already active.";
      return false;
    }
#ifdef _WIN32
    if (!local_system()) { error = "Steam isolation preparation must run in the SYSTEM broker."; return false; }
    std::error_code ec;
    const auto source = std::filesystem::weakly_canonical(steam_executable, ec);
    if (ec || !std::filesystem::is_regular_file(source, ec) || _wcsicmp(source.filename().c_str(), L"steam.exe") != 0) {
      error = "The trusted Steam command is not a canonical steam.exe file.";
      return false;
    }
    const auto source_root = source.parent_path();
    const auto proxy = std::filesystem::weakly_canonical(proxy_executable, ec);
    if (ec || !std::filesystem::is_regular_file(proxy, ec)) {
      error = "The packaged steam webhelper proxy is missing.";
      return false;
    }
    if (under_root(proxy, source_root)) {
      error = "The webhelper proxy must not be supplied from the Steam source tree.";
      return false;
    }

    HANDLE engine = nullptr;
    auto status = FwpmEngineOpen0(nullptr, RPC_C_AUTHN_WINNT, nullptr, nullptr, &engine);
    if (status != ERROR_SUCCESS || !engine) { error = "BFE engine open failed (" + std::to_string(status) + ")."; return false; }
    std::wstring seat_wide;
    seat_wide.reserve(seat_id.size());
    for (const auto ch : seat_id) seat_wide.push_back(static_cast<wchar_t>(static_cast<unsigned char>(ch)));
    remove_stale_filters(engine, seat_wide, generation);

    FWPM_PROVIDER0 provider {};
    const auto provider_key = guid(provider_key_bytes);
    provider.providerKey = provider_key;
    provider.displayData.name = const_cast<wchar_t *>(L"Vibeshine Steam user-mode isolation");
    provider.displayData.description = const_cast<wchar_t *>(L"Persistent SYSTEM-owned Steam client seat filters");
    status = FwpmTransactionBegin0(engine, 0);
    if (status == ERROR_SUCCESS) status = FwpmProviderAdd0(engine, &provider, nullptr);
    FWPM_SUBLAYER0 sublayer {};
    const auto sublayer_key = guid(sublayer_key_bytes);
    sublayer.subLayerKey = sublayer_key;
    sublayer.displayData.name = const_cast<wchar_t *>(L"Vibeshine Steam seat isolation");
    sublayer.providerKey = const_cast<GUID *>(&provider_key);
    sublayer.weight = 0x7fff;
    if (status == ERROR_SUCCESS) status = FwpmSubLayerAdd0(engine, &sublayer, nullptr);
    if (status != ERROR_SUCCESS) {
      if (status == FWP_E_ALREADY_EXISTS) status = ERROR_SUCCESS;
      else { FwpmTransactionAbort0(engine); FwpmEngineClose0(engine); error = "WFP provider transaction failed (" + std::to_string(status) + ")."; return false; }
    }

    const auto seat_root = std::filesystem::path {L"C:\\ProgramData\\VibeshineSteamSeats"} / seat_wide;
    const auto generation_root = seat_root / std::to_wstring(generation);
    const auto stage = generation_root.parent_path() / (std::to_wstring(generation) + L".staging");
    const auto mirror = generation_root / L"client";
    const auto cache = generation_root / L"cache";
    if (std::filesystem::is_symlink(seat_root, ec) || std::filesystem::is_symlink(generation_root, ec) ||
        std::filesystem::is_symlink(stage, ec) || std::filesystem::is_symlink(mirror, ec) ||
        std::filesystem::is_symlink(cache, ec)) {
      FwpmTransactionAbort0(engine); FwpmEngineClose0(engine); error = "Steam mirror root contains an unsafe reparse point."; return false;
    }
    std::filesystem::remove_all(stage, ec);
    std::filesystem::create_directories(stage, ec);
    std::vector<std::filesystem::path> executables;
    std::size_t files = 0;
    std::uintmax_t tree_bytes = 0;
    bool copy_failed = false;
    if (ec) { FwpmTransactionAbort0(engine); FwpmEngineClose0(engine); error = "Steam mirror staging root could not be created."; return false; }
    std::filesystem::recursive_directory_iterator it(source_root, std::filesystem::directory_options::skip_permission_denied, ec), end;
    for (; it != end && !ec; it.increment(ec)) {
      const auto rel = std::filesystem::relative(it->path(), source_root, ec);
      if (ec || excluded_directory(rel)) { if (it->is_directory(ec)) it.disable_recursion_pending(); continue; }
      if (it->is_symlink(ec) || it->is_other(ec)) { copy_failed = true; break; }
      const auto file_bytes = it->is_regular_file(ec) ? std::filesystem::file_size(it->path(), ec) : 0;
      if (ec || !it->is_regular_file(ec) || ++files > max_files || file_bytes > max_file_bytes ||
          tree_bytes > max_tree_bytes - std::min(file_bytes, max_tree_bytes)) { copy_failed = true; break; }
      tree_bytes += file_bytes;
      const auto destination = stage / rel;
      std::filesystem::create_directories(destination.parent_path(), ec);
      if (ec || !std::filesystem::copy_file(it->path(), destination, std::filesystem::copy_options::overwrite_existing, ec)) { copy_failed = true; break; }
      if (_wcsicmp(destination.filename().c_str(), L"steamservice.exe") == 0) { std::filesystem::remove(destination, ec); continue; }
      if (_wcsicmp(destination.extension().c_str(), L".exe") == 0) executables.push_back(destination);
    }
    if (ec || copy_failed || executables.empty()) {
      FwpmTransactionAbort0(engine); FwpmEngineClose0(engine); error = "Steam mirror could not be completely enumerated."; return false;
    }
    const auto helper = stage / L"steamwebhelper.exe";
    const auto real_helper = stage / L"steamwebhelper.real.exe";
    executables.erase(std::remove(executables.begin(), executables.end(), helper), executables.end());
    if (std::filesystem::exists(helper, ec)) std::filesystem::rename(helper, real_helper, ec);
    if (ec || !std::filesystem::copy_file(proxy, helper, std::filesystem::copy_options::overwrite_existing, ec)) {
      FwpmTransactionAbort0(engine); FwpmEngineClose0(engine); error = "Steam webhelper proxy publication failed."; return false;
    }
    executables.push_back(real_helper);
    executables.push_back(helper);
    std::filesystem::create_directories(cache, ec);
    std::filesystem::create_directories(generation_root, ec);
    std::filesystem::remove_all(mirror, ec);
    std::filesystem::rename(stage, mirror, ec);
    if (ec || !set_mirror_acl(mirror, user_sid) || !set_mirror_acl(cache, user_sid)) {
      FwpmTransactionAbort0(engine); FwpmEngineClose0(engine); error = "Steam mirror publication or ACL setup failed."; return false;
    }

    std::vector<std::filesystem::path> published;
    published.reserve(executables.size());
    for (const auto &staged : executables) {
      const auto published_path = mirror / std::filesystem::relative(staged, stage, ec);
      PFWP_BYTE_BLOB app_id = nullptr;
      status = FwpmGetAppIdFromFileName0(published_path.c_str(), &app_id);
      if (status != ERROR_SUCCESS || !app_id || app_id->size == 0) {
        if (app_id) FwpmFreeMemory0(reinterpret_cast<void **>(&app_id));
        FwpmTransactionAbort0(engine); FwpmEngineClose0(engine); error = "Steam mirror app identity could not be canonicalized."; return false;
      }
      std::wstring owner = L"Vibeshine Steam seat=" + seat_wide + L" generation=" + std::to_wstring(generation);
      const auto v4 = key_for(deterministic_filter_key(seat_id, generation, narrow(published_path), false));
      const auto v6 = key_for(deterministic_filter_key(seat_id, generation, narrow(published_path), true));
      if (!add_filter(engine, guid(v4), provider_key, sublayer_key, FWPM_LAYER_ALE_AUTH_CONNECT_V4, app_id->data, app_id->size, owner, error) ||
          !add_filter(engine, guid(v6), provider_key, sublayer_key, FWPM_LAYER_ALE_AUTH_CONNECT_V6, app_id->data, app_id->size, owner, error)) {
        FwpmFreeMemory0(reinterpret_cast<void **>(&app_id)); FwpmTransactionAbort0(engine); FwpmEngineClose0(engine); return false;
      }
      FwpmFreeMemory0(reinterpret_cast<void **>(&app_id));
      filter_keys_.push_back(v4); filter_keys_.push_back(v6); published.push_back(published_path);
    }
    status = FwpmTransactionCommit0(engine);
    if (status != ERROR_SUCCESS) { FwpmTransactionAbort0(engine); FwpmEngineClose0(engine); filter_keys_.clear(); error = "WFP filter transaction commit failed (" + std::to_string(status) + ")."; return false; }
    engine_ = engine; seat_id_ = seat_id; generation_ = generation;
    preparation_ = {.mirror_root = mirror, .cache_root = cache, .steam_executable = mirror / L"steam.exe",
                    .proxy_executable = mirror / L"steamwebhelper.exe", .manifest_digest = digest_manifest(published),
                    .filtered_executable_count = published.size()};
    result = preparation_;
    return true;
#else
    (void)steam_executable; (void)proxy_executable; (void)seat_id; (void)user_sid; (void)generation; (void)result;
    error = "Steam isolation is Windows-only."; return false;
#endif
  }

  bool manager_t::healthy(std::string &error) const noexcept {
    if (!active()) { error = "Steam isolation filters are not installed."; return false; }
#ifdef _WIN32
    if (!local_system()) { error = "Steam isolation health must be checked by the SYSTEM broker."; return false; }
    for (const auto &key : filter_keys_) {
      FWPM_FILTER0 *filter = nullptr;
      const auto guid_key = guid(key);
      if (FwpmFilterGetByKey0(static_cast<HANDLE>(engine_), &guid_key, &filter) != ERROR_SUCCESS || !filter) {
        if (filter) FwpmFreeMemory0(reinterpret_cast<void **>(&filter));
        error = "A persistent Steam seat filter is missing; reconnect is blocked."; return false;
      }
      FwpmFreeMemory0(reinterpret_cast<void **>(&filter));
    }
    return true;
#else
    error = "Steam isolation is Windows-only."; return false;
#endif
  }

  bool manager_t::release(std::string &error) noexcept {
    if (!engine_) return true;
#ifdef _WIN32
    if (!local_system()) { error = "Steam isolation cleanup must run by the SYSTEM broker."; return false; }
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
    engine_ = nullptr; filter_keys_.clear(); preparation_ = {}; seat_id_.clear(); generation_ = 0;
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

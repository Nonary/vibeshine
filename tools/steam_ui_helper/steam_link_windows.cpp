/**
 * @file tools/steam_ui_helper/steam_link_windows.cpp
 * @brief Steam's CEF debugger over WinHTTP, with checks that the port belongs to Steam.
 */
#include "steam_link_windows.h"

// clang-format off
#include <winsock2.h>
#include <windows.h>
#include <iphlpapi.h>
#include <tlhelp32.h>
#include <winhttp.h>
// clang-format on

#include <atomic>
#include <future>
#include <map>
#include <stdexcept>
#include <thread>
#include <vector>

namespace steam_ui_helper {
  namespace policy = platf::steam::big_picture::policy;

  std::wstring widen(std::string_view text) {
    if (text.empty()) {
      return {};
    }
    const int size = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    std::wstring wide(size, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), wide.data(), size);
    return wide;
  }

  std::string narrow(std::wstring_view text) {
    if (text.empty()) {
      return {};
    }
    const int size = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    std::string utf8(size, '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), utf8.data(), size, nullptr, nullptr);
    return utf8;
  }

  namespace {
    constexpr auto call_timeout = std::chrono::seconds(20);
    constexpr std::size_t max_message_bytes = 16 * 1024 * 1024;

    struct internet_closer {
      void operator()(HINTERNET handle) const {
        if (handle) {
          WinHttpCloseHandle(handle);
        }
      }
    };
    using internet_handle = std::unique_ptr<void, internet_closer>;

    [[noreturn]] void throw_last_error(const char *what) {
      throw std::runtime_error(std::string(what) + " failed (error " + std::to_string(GetLastError()) + ")");
    }

    DWORD session_of(DWORD pid) {
      DWORD session = 0xFFFFFFFF;
      return ProcessIdToSessionId(pid, &session) ? session : 0xFFFFFFFF;
    }

    struct steam_process_t {
      bool running = false;
      std::uint64_t instance = 0;  // Start time of the oldest steam.exe; 0 if unknown.
    };

    // The oldest steam.exe in the session is the client itself, not a short-lived steam.exe that hands
    // it a command such as -shutdown or a steam:// link.
    steam_process_t steam_process(DWORD session) {
      steam_process_t steam;
      const HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
      if (snapshot == INVALID_HANDLE_VALUE) {
        return steam;
      }
      PROCESSENTRY32W entry {};
      entry.dwSize = sizeof(entry);
      for (BOOL more = Process32FirstW(snapshot, &entry); more; more = Process32NextW(snapshot, &entry)) {
        if (_wcsicmp(entry.szExeFile, L"steam.exe") != 0 || session_of(entry.th32ProcessID) != session) {
          continue;
        }
        steam.running = true;
        const HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, entry.th32ProcessID);
        if (!process) {
          continue;
        }
        FILETIME created {}, exited {}, kernel {}, user {};
        if (GetProcessTimes(process, &created, &exited, &kernel, &user)) {
          const auto started = (static_cast<std::uint64_t>(created.dwHighDateTime) << 32) | created.dwLowDateTime;
          if (steam.instance == 0 || started < steam.instance) {
            steam.instance = started;
          }
        }
        CloseHandle(process);
      }
      CloseHandle(snapshot);
      return steam;
    }

    std::optional<std::filesystem::path> process_image(DWORD pid) {
      const HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
      if (!process) {
        return std::nullopt;
      }
      std::wstring path(32768, L'\0');
      DWORD size = static_cast<DWORD>(path.size());
      const bool ok = QueryFullProcessImageNameW(process, 0, path.data(), &size);
      CloseHandle(process);
      if (!ok) {
        return std::nullopt;
      }
      path.resize(size);
      return std::filesystem::path(path);
    }

    // The process listening on 127.0.0.1:port, preferring an exact loopback binding over a wildcard
    // one (the exact binding is the one a connection to 127.0.0.1 reaches).
    std::optional<DWORD> listener_pid(int port) {
      ULONG size = 0;
      GetExtendedTcpTable(nullptr, &size, FALSE, AF_INET, TCP_TABLE_OWNER_PID_LISTENER, 0);
      std::vector<unsigned char> buffer(size);
      if (size == 0 || GetExtendedTcpTable(buffer.data(), &size, FALSE, AF_INET, TCP_TABLE_OWNER_PID_LISTENER, 0) != NO_ERROR) {
        return std::nullopt;
      }
      const auto *table = reinterpret_cast<const MIB_TCPTABLE_OWNER_PID *>(buffer.data());
      std::optional<DWORD> wildcard;
      for (DWORD i = 0; i < table->dwNumEntries; ++i) {
        const auto &row = table->table[i];
        const auto row_port = static_cast<int>(((row.dwLocalPort & 0xFFU) << 8) | ((row.dwLocalPort >> 8) & 0xFFU));
        if (row_port != port) {
          continue;
        }
        const auto *address = reinterpret_cast<const unsigned char *>(&row.dwLocalAddr);
        if (address[0] == 127 && address[1] == 0 && address[2] == 0 && address[3] == 1) {
          return row.dwOwningPid;
        }
        if (row.dwLocalAddr == 0) {
          wildcard = row.dwOwningPid;
        }
      }
      return wildcard;
    }

    void read_body(HINTERNET request, std::string &body) {
      for (;;) {
        DWORD available = 0;
        if (!WinHttpQueryDataAvailable(request, &available)) {
          throw_last_error("WinHttpQueryDataAvailable");
        }
        if (available == 0) {
          return;
        }
        const auto offset = body.size();
        body.resize(offset + available);
        DWORD read = 0;
        if (!WinHttpReadData(request, body.data() + offset, available, &read)) {
          throw_last_error("WinHttpReadData");
        }
        body.resize(offset + read);
        if (body.size() > max_message_bytes) {
          throw std::runtime_error("response too large");
        }
      }
    }

    DWORD status_code(HINTERNET request) {
      DWORD status = 0;
      DWORD size = sizeof(status);
      WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &status, &size, WINHTTP_NO_HEADER_INDEX);
      return status;
    }

    struct local_url_t {
      std::wstring host;
      INTERNET_PORT port;
      std::wstring path;
    };

    // Only called with URLs accepted by policy::is_local_debugger_url().
    local_url_t split_ws_url(const std::string &url) {
      const auto rest = url.substr(5);  // after "ws://"
      const auto port_start = rest.rfind(':', rest.find('/'));
      const auto path_start = rest.find('/');
      auto host = rest.substr(0, port_start);
      if (host.size() > 2 && host.front() == '[') {
        host = host.substr(1, host.size() - 2);
      }
      return {widen(host), static_cast<INTERNET_PORT>(std::stoi(rest.substr(port_start + 1, path_start - port_start - 1))), widen(rest.substr(path_start))};
    }

    class websocket_session_t final: public cdp_session_t {
    public:
      websocket_session_t(const std::string &ws_url, std::function<void(cdp_event_t)> on_event):
          on_event_(std::move(on_event)) {
        const auto url = split_ws_url(ws_url);
        session_.reset(WinHttpOpen(L"Vibeshine-SteamUiHelper", WINHTTP_ACCESS_TYPE_NO_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0));
        if (!session_) {
          throw_last_error("WinHttpOpen");
        }
        connection_.reset(WinHttpConnect(session_.get(), url.host.c_str(), url.port, 0));
        if (!connection_) {
          throw_last_error("WinHttpConnect");
        }
        internet_handle request {WinHttpOpenRequest(connection_.get(), L"GET", url.path.c_str(), nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, 0)};
        if (!request) {
          throw_last_error("WinHttpOpenRequest");
        }
        WinHttpSetTimeouts(request.get(), 2000, 2000, 5000, 5000);
        if (!WinHttpSetOption(request.get(), WINHTTP_OPTION_UPGRADE_TO_WEB_SOCKET, nullptr, 0) ||
            !WinHttpSendRequest(request.get(), WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
            !WinHttpReceiveResponse(request.get(), nullptr)) {
          throw_last_error("WebSocket upgrade");
        }
        if (status_code(request.get()) != 101) {
          throw std::runtime_error("Steam refused the WebSocket upgrade");
        }
        socket_ = WinHttpWebSocketCompleteUpgrade(request.get(), 0);
        if (!socket_) {
          throw_last_error("WinHttpWebSocketCompleteUpgrade");
        }
        reader_ = std::thread([this] {
          read_loop();
        });
      }

      ~websocket_session_t() override {
        // Ask Steam to close (the reader then sees the close frame); closing the handle aborts the
        // reader's pending receive if Steam doesn't answer.
        closed_ = true;
        WinHttpWebSocketShutdown(socket_, WINHTTP_WEB_SOCKET_SUCCESS_CLOSE_STATUS, nullptr, 0);
        WinHttpCloseHandle(socket_);
        if (reader_.joinable()) {
          reader_.join();
        }
      }

      json call(std::string_view method, const json &params) override {
        if (closed_) {
          throw std::runtime_error("Steam connection closed");
        }
        std::future<json> reply;
        int id;
        {
          std::lock_guard lock(pending_mutex_);
          id = next_id_++;
          reply = pending_[id].get_future();
        }
        const auto text = json {{"id", id}, {"method", method}, {"params", params}}.dump();
        DWORD sent;
        {
          std::lock_guard lock(send_mutex_);
          sent = WinHttpWebSocketSend(socket_, WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE, const_cast<char *>(text.data()), static_cast<DWORD>(text.size()));
        }
        if (sent != NO_ERROR || reply.wait_for(call_timeout) != std::future_status::ready) {
          std::lock_guard lock(pending_mutex_);
          pending_.erase(id);
          throw std::runtime_error(std::string(method) + (sent != NO_ERROR ? " could not be sent" : " timed out"));
        }
        const auto message = reply.get();
        if (const auto error = message.find("error"); error != message.end()) {
          throw std::runtime_error(error->value("message", std::string("CDP error")));
        }
        return message.value("result", json::object());
      }

    private:
      void read_loop() {
        std::string message;
        std::vector<char> buffer(64 * 1024);
        for (;;) {
          DWORD read = 0;
          WINHTTP_WEB_SOCKET_BUFFER_TYPE type {};
          const DWORD error = WinHttpWebSocketReceive(socket_, buffer.data(), static_cast<DWORD>(buffer.size()), &read, &type);
          if (error == ERROR_WINHTTP_TIMEOUT && !closed_) {
            continue;
          }
          if (error != NO_ERROR || type == WINHTTP_WEB_SOCKET_CLOSE_BUFFER_TYPE) {
            break;
          }
          message.append(buffer.data(), read);
          if (message.size() > max_message_bytes) {
            break;
          }
          if (type == WINHTTP_WEB_SOCKET_UTF8_FRAGMENT_BUFFER_TYPE || type == WINHTTP_WEB_SOCKET_BINARY_FRAGMENT_BUFFER_TYPE) {
            continue;
          }
          dispatch(message);
          message.clear();
        }
        closed_ = true;
        {
          std::lock_guard lock(pending_mutex_);
          for (auto &[id, reply] : pending_) {
            reply.set_exception(std::make_exception_ptr(std::runtime_error("Steam connection closed")));
          }
          pending_.clear();
        }
        on_event_({std::string(closed_event), json::object()});
      }

      void dispatch(const std::string &text) {
        // Runtime.enable makes Steam stream every console message; none of them matter here.
        if (text.starts_with(R"({"method":"Runtime.consoleAPICalled")")) {
          return;
        }
        auto message = json::parse(text, nullptr, false);
        if (message.is_discarded() || !message.is_object()) {
          return;
        }
        if (const auto id = message.find("id"); id != message.end() && id->is_number_integer()) {
          std::lock_guard lock(pending_mutex_);
          if (const auto pending = pending_.find(id->get<int>()); pending != pending_.end()) {
            pending->second.set_value(std::move(message));
            pending_.erase(pending);
          }
          return;
        }
        if (const auto method = message.find("method"); method != message.end() && method->is_string()) {
          on_event_({method->get<std::string>(), message.value("params", json::object())});
        }
      }

      std::function<void(cdp_event_t)> on_event_;
      internet_handle session_;
      internet_handle connection_;
      HINTERNET socket_ = nullptr;
      std::atomic_bool closed_ {false};
      std::mutex send_mutex_;
      std::mutex pending_mutex_;
      std::map<int, std::promise<json>> pending_;
      int next_id_ = 1;
      std::thread reader_;
    };
  }  // namespace

  std::filesystem::path windows_steam_link_t::steam_root() {
    const auto read = [](HKEY root, const wchar_t *key, const wchar_t *value) -> std::filesystem::path {
      std::wstring text(MAX_PATH * 4, L'\0');
      DWORD size = static_cast<DWORD>(text.size() * sizeof(wchar_t));
      if (RegGetValueW(root, key, value, RRF_RT_REG_SZ, nullptr, text.data(), &size) != ERROR_SUCCESS) {
        return {};
      }
      text.resize(wcsnlen(text.c_str(), text.size()));
      return std::filesystem::path(text).make_preferred();
    };
    // The helper runs as the signed-in user, so HKCU is that user's Steam.
    if (auto path = read(HKEY_CURRENT_USER, L"Software\\Valve\\Steam", L"SteamPath"); !path.empty()) {
      return path;
    }
    if (auto path = read(HKEY_LOCAL_MACHINE, L"SOFTWARE\\WOW6432Node\\Valve\\Steam", L"InstallPath"); !path.empty()) {
      return path;
    }
    return read(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Valve\\Steam", L"InstallPath");
  }

  policy::endpoint_facts_t windows_steam_link_t::endpoint_facts() {
    policy::endpoint_facts_t facts;
    const DWORD own_session = session_of(GetCurrentProcessId());
    const auto steam = steam_process(own_session);
    facts.steam_running = steam.running;
    facts.steam_instance = steam.instance;
    const auto owner = listener_pid(policy::steam_debug_port);
    if (!owner) {
      return facts;
    }
    facts.listening = true;
    facts.owner_in_helper_session = session_of(*owner) == own_session;
    const auto root = steam_root();
    const auto image = process_image(*owner);
    if (!root.empty() && image) {
      std::error_code error;
      facts.owner_is_steam_webhelper = std::filesystem::equivalent(*image, root / "bin" / "cef" / "cef.win64" / "steamwebhelper.exe", error) && !error;
    }
    return facts;
  }

  json windows_steam_link_t::targets() {
    internet_handle session {WinHttpOpen(L"Vibeshine-SteamUiHelper", WINHTTP_ACCESS_TYPE_NO_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0)};
    if (!session) {
      throw_last_error("WinHttpOpen");
    }
    WinHttpSetTimeouts(session.get(), 2000, 2000, 5000, 5000);
    internet_handle connection {WinHttpConnect(session.get(), L"127.0.0.1", policy::steam_debug_port, 0)};
    internet_handle request {connection ? WinHttpOpenRequest(connection.get(), L"GET", L"/json", nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, 0) : nullptr};
    if (!request || !WinHttpSendRequest(request.get(), WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) || !WinHttpReceiveResponse(request.get(), nullptr)) {
      throw_last_error("GET /json");
    }
    if (status_code(request.get()) != 200) {
      throw std::runtime_error("GET /json returned HTTP " + std::to_string(status_code(request.get())));
    }
    std::string body;
    read_body(request.get(), body);
    auto targets = json::parse(body, nullptr, false);
    return targets.is_discarded() ? json::array() : targets;
  }

  std::unique_ptr<cdp_session_t> windows_steam_link_t::connect(const std::string &ws_url, std::function<void(cdp_event_t)> on_event) {
    if (!policy::is_local_debugger_url(ws_url)) {
      throw std::runtime_error("refusing a debugger URL that isn't local");
    }
    return std::make_unique<websocket_session_t>(ws_url, std::move(on_event));
  }
}  // namespace steam_ui_helper

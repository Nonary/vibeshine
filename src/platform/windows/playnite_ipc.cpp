/**
 * @file src/platform/windows/playnite_ipc.cpp
 * @brief Playnite plugin IPC server using Windows named pipes with anonymous handshake.
 */

#ifndef WIN32_LEAN_AND_MEAN
  #define WIN32_LEAN_AND_MEAN
#endif
#include "playnite_ipc.h"
#include "src/platform/windows/misc.h"
#include "src/utility.h"

#include <chrono>
#include <sddl.h>
#include <span>
#include <string>
#include <vector>
#include <windows.h>
#include <winsock2.h>

using namespace std::chrono_literals;

namespace platf::playnite {
  namespace {
    // Local UTF-16 to UTF-8 converter to avoid linking extra objects into tools
    static std::string to_utf8_local(const std::wstring &ws) {
      if (ws.empty()) return std::string();
      int size = WideCharToMultiByte(CP_UTF8, 0, ws.c_str(), -1, nullptr, 0, nullptr, nullptr);
      if (size <= 0) return std::string();
      std::string out;
      out.resize(static_cast<size_t>(size - 1));
      if (size > 1) {
        WideCharToMultiByte(CP_UTF8, 0, ws.c_str(), -1, out.data(), size, nullptr, nullptr);
      }
      return out;
    }
  }
  // Well-known public control pipe name that the Playnite plugin connects to when using
  // the legacy static mode. Newer flows should pass a dynamic control name.
  inline constexpr char kControlPipeName[] = "sunshine_playnite_connector";

  IpcServer::IpcServer():
      control_name_(kControlPipeName) {}

  IpcServer::IpcServer(const std::string &control_name):
      control_name_(control_name.empty() ? kControlPipeName : control_name) {}

  IpcServer::~IpcServer() {
    stop();
  }

  void IpcServer::start() {
    if (running_.exchange(true)) {
      return;
    }
    BOOST_LOG(info) << "Playnite IPC: starting server thread";
    worker_ = std::thread([this]() {
      run();
    });
  }

  void IpcServer::stop() {
    if (!running_.exchange(false)) {
      return;
    }
    BOOST_LOG(info) << "Playnite IPC: stopping server thread";
    if (pipe_) {
      pipe_->stop();
      pipe_.reset();
    }
    if (worker_.joinable()) {
      worker_.join();
    }
  }

  void IpcServer::run() {
    while (running_.load()) {
      broken_.store(false);
      // Previously we deferred pipe startup until an "interactive" desktop session existed.
      // That prevented connections from RDP sessions and while the screen was locked.
      // Remove that dependency so IPC is available in those scenarios as well.
      if (no_session_logged_) {
        // If we logged a prior missing-session note, clear it once on first loop after start
        no_session_logged_ = false;
      }

      // Reduce log spam by only attempting to create/listen when Playnite is actually running
      if (!is_playnite_running()) {
        if (!no_playnite_logged_) {
          BOOST_LOG(info) << "Playnite IPC: Playnite not running; deferring pipe startup";
          no_playnite_logged_ = true;
        }
        std::this_thread::sleep_for(2s);
        continue;
      } else if (no_playnite_logged_) {
        BOOST_LOG(info) << "Playnite IPC: Playnite detected; enabling pipe handshake";
        no_playnite_logged_ = false;
      }
      auto data_pipe = wait_for_handshake_and_get_data_pipe();
      if (!data_pipe) {
        std::this_thread::sleep_for(500ms);
        continue;
      }
      auto on_msg = [this](std::span<const uint8_t> b) {
        accumulate_and_dispatch_lines(b);
      };
      auto on_err = [](const std::string &e) {
        BOOST_LOG(error) << "Playnite IPC error: " << e;
      };
      auto on_broken = [this]() {
        BOOST_LOG(warning) << "Playnite IPC: broken pipe";
        broken_.store(true);
      };
      pipe_ = std::make_unique<platf::dxgi::AsyncNamedPipe>(std::move(data_pipe));
      pipe_->start(on_msg, on_err, on_broken);
      active_.store(true);
      serve_connected_loop();
      if (pipe_) {
        pipe_->stop();
        pipe_.reset();
      }
      active_.store(false);
      if (!running_.load()) {
        break;
      }
      std::this_thread::sleep_for(300ms);
    }
  }

  void IpcServer::accumulate_and_dispatch_lines(std::span<const uint8_t> bytes) {
    if (!bytes.empty()) {
      recv_buffer_.append((const char *) bytes.data(), bytes.size());
    }
    size_t start = 0;
    while (true) {
      auto pos = recv_buffer_.find('\n', start);
      if (pos == std::string::npos) {
        break;
      }
      std::string line = recv_buffer_.substr(start, pos - start);
      if (!line.empty() && line.back() == '\r') {
        line.pop_back();
      }
      start = pos + 1;
      auto non_ws = line.find_first_not_of(" \t\r\n");
      if (non_ws == std::string::npos) {
        continue;
      }
      if (handler_) {
        handler_(std::span<const uint8_t>((const uint8_t *) line.data(), line.size()));
      }
    }
    if (start > 0) {
      recv_buffer_.erase(0, start);
    }
  }

  std::unique_ptr<platf::dxgi::INamedPipe> IpcServer::wait_for_handshake_and_get_data_pipe() {
    platf::dxgi::AnonymousPipeFactory factory;
    const char *ctl = control_name_.empty() ? kControlPipeName : control_name_.c_str();
    // Frequent poll; keep at debug to avoid spam
    BOOST_LOG(debug) << "Playnite IPC: waiting for handshake on control pipe '" << ctl << "'";
    auto pipe = factory.create_server(ctl);
    if (!pipe) {
      return nullptr;
    }
    pipe->wait_for_client_connection(15000);
    auto wp = dynamic_cast<platf::dxgi::WinPipe *>(pipe.get());
    if (!wp || !pipe->is_connected() || !validate_client_and_get_expected_sid(wp)) {
      return nullptr;
    }
    return pipe;
  }

  bool IpcServer::get_client_pid(platf::dxgi::WinPipe *wp, DWORD &pid) {
    if (!wp->get_client_process_id(pid)) {
      return false;
    }
    BOOST_LOG(info) << "Playnite IPC: client connected, PID=" << pid;
    return true;
  }

  bool IpcServer::is_playnite_process(DWORD pid) {
    auto v1 = platf::dxgi::find_process_ids_by_name(L"Playnite.DesktopApp.exe");
    auto v2 = platf::dxgi::find_process_ids_by_name(L"Playnite.FullscreenApp.exe");
    return std::find(v1.begin(), v1.end(), pid) != v1.end() || std::find(v2.begin(), v2.end(), pid) != v2.end();
  }

  bool IpcServer::is_playnite_running() {
    try {
      auto v1 = platf::dxgi::find_process_ids_by_name(L"Playnite.DesktopApp.exe");
      if (!v1.empty()) {
        return true;
      }
      auto v2 = platf::dxgi::find_process_ids_by_name(L"Playnite.FullscreenApp.exe");
      return !v2.empty();
    } catch (...) {
      return false;
    }
  }

  bool IpcServer::get_client_sid(platf::dxgi::WinPipe *wp, std::wstring &sid) {
    return wp->get_client_user_sid_string(sid);
  }

  bool IpcServer::get_expected_user_sid(std::wstring &sid) {
#ifdef SUNSHINE_PLAYNITE_LAUNCHER
    HANDLE tok = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &tok)) {
      return false;
    }
    DWORD len = 0;
    GetTokenInformation(tok, TokenUser, nullptr, 0, &len);
    auto buf = std::make_unique<uint8_t[]>(len);
    auto tu = reinterpret_cast<TOKEN_USER *>(buf.get());
    if (!GetTokenInformation(tok, TokenUser, tu, len, &len)) {
      CloseHandle(tok);
      return false;
    }
    LPWSTR sidW = nullptr;
    if (!ConvertSidToStringSidW(tu->User.Sid, &sidW)) {
      CloseHandle(tok);
      return false;
    }
    sid.assign(sidW);
    LocalFree(sidW);
    CloseHandle(tok);
    return true;
#else
    // Primary path: expected SID is the active user's token (works when running as SYSTEM)
    platf::dxgi::safe_token user_token;
    user_token.reset(platf::dxgi::retrieve_users_token(false));
    HANDLE tok = user_token.get();
    if (!tok) {
      // Fallback: when running as a regular user, WTSQueryUserToken often fails.
      // In that case, use our own process token as the expected SID.
      BOOST_LOG(debug) << "Playnite IPC: retrieve_users_token failed; falling back to current process token";
      if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &tok)) {
        BOOST_LOG(warning) << "Playnite IPC: OpenProcessToken fallback failed: " << GetLastError();
        return false;
      }
    }
    auto close_fallback = util::fail_guard([&]() {
      // Only close the handle if it did not come from safe_token (fallback path)
      if (!user_token && tok) CloseHandle(tok);
    });

    DWORD len = 0;
    GetTokenInformation(tok, TokenUser, nullptr, 0, &len);
    if (len == 0) {
      BOOST_LOG(warning) << "Playnite IPC: GetTokenInformation length query failed: " << GetLastError();
      return false;
    }
    auto buf = std::make_unique<uint8_t[]>(len);
    auto tu = reinterpret_cast<TOKEN_USER *>(buf.get());
    if (!GetTokenInformation(tok, TokenUser, tu, len, &len)) {
      BOOST_LOG(warning) << "Playnite IPC: GetTokenInformation(TokenUser) failed: " << GetLastError();
      return false;
    }
    LPWSTR sidW = nullptr;
    if (!ConvertSidToStringSidW(tu->User.Sid, &sidW)) {
      BOOST_LOG(warning) << "Playnite IPC: ConvertSidToStringSidW failed: " << GetLastError();
      return false;
    }
    sid.assign(sidW);
    LocalFree(sidW);
    return true;
#endif
  }

  bool IpcServer::validate_client_and_get_expected_sid(platf::dxgi::WinPipe *wp) {
    DWORD pid = 0;
    if (!get_client_pid(wp, pid)) {
      BOOST_LOG(warning) << "Playnite IPC: failed to obtain client PID; disconnecting";
      wp->disconnect();
      return false;
    }
    if (!is_playnite_process(pid)) {
      BOOST_LOG(warning) << "Playnite IPC: rejecting non-Playnite client PID=" << pid;
      wp->disconnect();
      return false;
    }
    std::wstring client_sid;
    if (!get_client_sid(wp, client_sid)) {
      BOOST_LOG(warning) << "Playnite IPC: failed to get client SID for PID=" << pid << "; disconnecting";
      wp->disconnect();
      return false;
    }
    std::wstring expected_sid;
    if (!get_expected_user_sid(expected_sid)) {
      BOOST_LOG(warning) << "Playnite IPC: could not determine expected user SID; disconnecting";
      wp->disconnect();
      return false;
    }
    if (client_sid != expected_sid) {
      BOOST_LOG(warning) << "Playnite IPC: SID mismatch; client=" << to_utf8_local(client_sid)
                         << " expected=" << to_utf8_local(expected_sid) << "; disconnecting";
      wp->disconnect();
      return false;
    }
    return true;
  }

  void IpcServer::serve_connected_loop() {
    BOOST_LOG(info) << "Playnite IPC: data pipe established";
    while (running_.load() && pipe_->is_connected() && !broken_.load()) {
      std::this_thread::sleep_for(200ms);
    }
    BOOST_LOG(info) << "Playnite IPC: disconnected";
  }

  bool IpcServer::is_user_session_available() {
    // IPC availability should not depend on an "active desktop". Always report available
    // so callers do not gate pipe creation on console-desktop presence.
    return true;
  }

  bool IpcServer::send_json_line(const std::string &json) {
    if (!pipe_ || !pipe_->is_connected()) {
      BOOST_LOG(warning) << "Playnite IPC: send_json_line called but pipe not connected";
      return false;
    }
    // Append a newline delimiter for the plugin's line-based reader
    std::string payload = json;
    payload.push_back('\n');
    auto bytes = std::span<const uint8_t>(reinterpret_cast<const uint8_t *>(payload.data()), payload.size());
    BOOST_LOG(debug) << "Playnite IPC: sending command (" << payload.size() << " bytes)";
    pipe_->send(bytes);
    return true;
  }

}  // namespace platf::playnite

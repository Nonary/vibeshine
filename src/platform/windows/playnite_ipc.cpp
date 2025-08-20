/**
 * @file src/platform/windows/playnite_ipc.cpp
 * @brief Playnite plugin IPC server using Windows named pipes with anonymous handshake.
 */

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <windows.h>

#include "playnite_ipc.h"

#include <chrono>
#include <span>
#include <string>
#include <vector>
#include <sddl.h>

using namespace std::chrono_literals;

namespace platf::playnite {
  // Well-known public control pipe name that the Playnite plugin connects to when using
  // the legacy static mode. Newer flows should pass a dynamic control name.
  inline constexpr char kControlPipeName[] = "sunshine_playnite_connector";

  IpcServer::IpcServer(): control_name_(kControlPipeName) {}
  IpcServer::IpcServer(const std::string &control_name): control_name_(control_name.empty() ? kControlPipeName : control_name) {}
  IpcServer::~IpcServer() { stop(); }

  void IpcServer::start() {
    if (running_.exchange(true)) {
      return;
    }
    BOOST_LOG(info) << "Playnite IPC: starting server thread";
    worker_ = std::thread([this]() { run(); });
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
      // Skip attempting to create/connect pipes if there is no active user session.
      if (!is_user_session_available()) {
        if (!no_session_logged_) {
          BOOST_LOG(info) << "Playnite IPC: no interactive user session detected; deferring pipe startup";
          no_session_logged_ = true;
        }
        std::this_thread::sleep_for(2s);
        continue;
      } else if (no_session_logged_) {
        // Session appeared after being absent; note once.
        BOOST_LOG(info) << "Playnite IPC: interactive user session detected; starting pipe server";
        no_session_logged_ = false;
      }
      auto data_pipe = wait_for_handshake_and_get_data_pipe();
      if (!data_pipe) { std::this_thread::sleep_for(500ms); continue; }
      auto on_msg = [this](std::span<const uint8_t> b){ accumulate_and_dispatch_lines(b); };
      auto on_err = [](const std::string &e){ BOOST_LOG(error) << "Playnite IPC error: " << e; };
      auto on_broken = [this](){ BOOST_LOG(warning) << "Playnite IPC: broken pipe"; broken_.store(true); };
      pipe_ = std::make_unique<platf::dxgi::AsyncNamedPipe>(std::move(data_pipe));
      pipe_->start(on_msg, on_err, on_broken);
      active_.store(true);
      serve_connected_loop();
      if (pipe_) { pipe_->stop(); pipe_.reset(); }
      active_.store(false);
      if (!running_.load()) break;
      std::this_thread::sleep_for(300ms);
    }
  }

  void IpcServer::accumulate_and_dispatch_lines(std::span<const uint8_t> bytes) {
    if (!bytes.empty()) recv_buffer_.append((const char*)bytes.data(), bytes.size());
    size_t start = 0;
    while (true) {
      auto pos = recv_buffer_.find('\n', start);
      if (pos == std::string::npos) break;
      std::string line = recv_buffer_.substr(start, pos - start);
      if (!line.empty() && line.back() == '\r') line.pop_back();
      start = pos + 1; auto non_ws = line.find_first_not_of(" \t\r\n");
      if (non_ws == std::string::npos) continue;
      if (handler_) handler_(std::span<const uint8_t>((const uint8_t*)line.data(), line.size()));
    }
    if (start > 0) recv_buffer_.erase(0, start);
  }

  std::unique_ptr<platf::dxgi::INamedPipe> IpcServer::wait_for_handshake_and_get_data_pipe() {
    platf::dxgi::AnonymousPipeFactory factory;
    const char *ctl = control_name_.empty() ? kControlPipeName : control_name_.c_str();
    BOOST_LOG(info) << "Playnite IPC: waiting for handshake on control pipe '" << ctl << "'";
    auto pipe = factory.create_server(ctl);
    if (!pipe) return nullptr;
    pipe->wait_for_client_connection(15000);
    auto wp = dynamic_cast<platf::dxgi::WinPipe *>(pipe.get());
    if (!wp || !pipe->is_connected() || !validate_client_and_get_expected_sid(wp)) return nullptr;
    return pipe;
  }

  bool IpcServer::get_client_pid(platf::dxgi::WinPipe *wp, DWORD &pid) {
    if (!wp->get_client_process_id(pid)) return false;
    BOOST_LOG(info) << "Playnite IPC: client connected, PID=" << pid;
    return true;
  }

  bool IpcServer::is_playnite_process(DWORD pid) {
    auto v1 = platf::dxgi::find_process_ids_by_name(L"Playnite.DesktopApp.exe");
    auto v2 = platf::dxgi::find_process_ids_by_name(L"Playnite.FullscreenApp.exe");
    return std::find(v1.begin(), v1.end(), pid) != v1.end() || std::find(v2.begin(), v2.end(), pid) != v2.end();
  }

  bool IpcServer::get_client_sid(platf::dxgi::WinPipe *wp, std::wstring &sid) {
    return wp->get_client_user_sid_string(sid);
  }

  bool IpcServer::get_expected_user_sid(std::wstring &sid) {
#ifdef SUNSHINE_PLAYNITE_LAUNCHER
    HANDLE tok = nullptr; if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &tok)) return false;
    DWORD len = 0; GetTokenInformation(tok, TokenUser, nullptr, 0, &len);
    auto buf = std::make_unique<uint8_t[]>(len);
    auto tu = reinterpret_cast<TOKEN_USER *>(buf.get());
    if (!GetTokenInformation(tok, TokenUser, tu, len, &len)) { CloseHandle(tok); return false; }
    LPWSTR sidW = nullptr; if (!ConvertSidToStringSidW(tu->User.Sid, &sidW)) { CloseHandle(tok); return false; }
    sid.assign(sidW); LocalFree(sidW); CloseHandle(tok); return true;
#else
    platf::dxgi::safe_token user_token; user_token.reset(platf::dxgi::retrieve_users_token(false));
    if (!user_token) return false; DWORD len = 0; GetTokenInformation(user_token.get(), TokenUser, nullptr, 0, &len);
    auto buf = std::make_unique<uint8_t[]>(len);
    auto tu = reinterpret_cast<TOKEN_USER *>(buf.get());
    if (!GetTokenInformation(user_token.get(), TokenUser, tu, len, &len)) return false;
    LPWSTR sidW = nullptr; if (!ConvertSidToStringSidW(tu->User.Sid, &sidW)) return false;
    sid.assign(sidW); LocalFree(sidW); return true;
#endif
  }

  bool IpcServer::validate_client_and_get_expected_sid(platf::dxgi::WinPipe *wp) {
    DWORD pid = 0; if (!get_client_pid(wp, pid)) { wp->disconnect(); return false; }
    if (!is_playnite_process(pid)) { wp->disconnect(); return false; }
    std::wstring client_sid; if (!get_client_sid(wp, client_sid)) { wp->disconnect(); return false; }
    std::wstring expected_sid; if (!get_expected_user_sid(expected_sid)) { wp->disconnect(); return false; }
    if (client_sid != expected_sid) { wp->disconnect(); return false; }
    return true;
  }

  void IpcServer::serve_connected_loop() {
    BOOST_LOG(info) << "Playnite IPC: data pipe established after handshake";
    while (running_.load() && pipe_->is_connected() && !broken_.load()) {
      std::this_thread::sleep_for(200ms);
    }
    BOOST_LOG(info) << "Playnite IPC: disconnected";
  }

  bool IpcServer::is_user_session_available() {
    // If not running as SYSTEM, we're already in a user session.
    if (!platf::dxgi::is_running_as_system()) {
      return true;
    }
    // When running as SYSTEM (service), ensure an interactive user session exists.
    auto tok = platf::dxgi::retrieve_users_token(false);
    if (!tok) return false;
    CloseHandle(tok);
    return true;
  }

  bool IpcServer::send_json_line(const std::string &json) {
    if (!pipe_ || !pipe_->is_connected()) {
      BOOST_LOG(info) << "Playnite IPC: send_json_line called but pipe not connected";
      return false;
    }
    // Append a newline delimiter for the plugin's line-based reader
    std::string payload = json;
    payload.push_back('\n');
    auto bytes = std::span<const uint8_t>(reinterpret_cast<const uint8_t *>(payload.data()), payload.size());
    BOOST_LOG(info) << "Playnite IPC: sending command (" << payload.size() << " bytes)";
    pipe_->send(bytes);
    return true;
  }

}  // namespace platf::playnite

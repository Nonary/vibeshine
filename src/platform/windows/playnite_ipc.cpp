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

  namespace {
    // Well-known public control pipe name that the Playnite plugin connects to when using
    // the legacy static mode. Newer flows should pass a dynamic control name.
    constexpr const char *kControlPipeName = "sunshine_playnite_connector";
  }

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
      auto on_message = [this](std::span<const uint8_t> bytes) {
        BOOST_LOG(info) << "Playnite IPC: received message (" << bytes.size() << " bytes)";
        // Accumulate and split on newline to get complete JSON frames
        if (!bytes.empty()) {
          recv_buffer_.append(reinterpret_cast<const char *>(bytes.data()), bytes.size());
        }
        size_t start = 0;
        while (true) {
          auto pos = recv_buffer_.find('\n', start);
          if (pos == std::string::npos) break;
          std::string line = recv_buffer_.substr(start, pos - start);
          // Trim optional CR
          if (!line.empty() && line.back() == '\r') line.pop_back();
          // Advance start past this newline
          start = pos + 1;
          // Skip empty lines
          auto non_ws = line.find_first_not_of(" \t\r\n");
          if (non_ws == std::string::npos) continue;
          if (handler_) {
            auto sp = std::span<const uint8_t>(reinterpret_cast<const uint8_t *>(line.data()), line.size());
            handler_(sp);
          }
        }
        if (start > 0) {
          // Keep leftover partial frame
          recv_buffer_.erase(0, start);
        }
      };

      auto on_error = [](const std::string &err) {
        BOOST_LOG(error) << "Playnite IPC error: " << err.c_str();
      };

      broken_.store(false);
      auto on_broken = [this]() {
        BOOST_LOG(warning) << "Playnite IPC: broken pipe";
        broken_.store(true);
      };

      // Require anonymous handshake: client connects to control pipe and receives data pipe name
      platf::dxgi::AnonymousPipeFactory factory;

      const char *ctl = control_name_.empty() ? kControlPipeName : control_name_.c_str();
      BOOST_LOG(info) << "Playnite IPC: waiting for handshake on control pipe '" << ctl << "'";
      auto data_pipe = factory.create_server(ctl);
      if (!data_pipe) {
        if (!running_.load()) break;
        std::this_thread::sleep_for(500ms);
        continue;
      }
      // Ensure the data pipe is connected (handshake server already created it)
      data_pipe->wait_for_client_connection(15000);
      if (!data_pipe->is_connected()) {
        if (!running_.load()) break;
        std::this_thread::sleep_for(300ms);
        continue;
      }

      // Validate the connecting client: PID must be Playnite's and user SID must match active session user.
      auto wp = dynamic_cast<platf::dxgi::WinPipe *>(data_pipe.get());
      if (!wp) {
        BOOST_LOG(error) << "Playnite IPC: internal error, unexpected pipe type";
        data_pipe->disconnect();
        std::this_thread::sleep_for(300ms);
        continue;
      }

      DWORD client_pid = 0;
      if (!wp->get_client_process_id(client_pid)) {
        BOOST_LOG(error) << "Playnite IPC: unable to get client PID";
        data_pipe->disconnect();
        std::this_thread::sleep_for(300ms);
        continue;
      }
      BOOST_LOG(info) << "Playnite IPC: client connected, PID=" << client_pid;

      std::vector<DWORD> playnite_pids;
      {
        auto v1 = platf::dxgi::find_process_ids_by_name(L"Playnite.DesktopApp.exe");
        auto v2 = platf::dxgi::find_process_ids_by_name(L"Playnite.FullscreenApp.exe");
        playnite_pids.reserve(v1.size() + v2.size());
        playnite_pids.insert(playnite_pids.end(), v1.begin(), v1.end());
        playnite_pids.insert(playnite_pids.end(), v2.begin(), v2.end());
      }

      if (std::find(playnite_pids.begin(), playnite_pids.end(), client_pid) == playnite_pids.end()) {
        BOOST_LOG(error) << "Playnite IPC: client PID " << client_pid << " is not a Playnite process; rejecting";
        data_pipe->disconnect();
        std::this_thread::sleep_for(300ms);
        continue;
      }

      std::wstring client_sid;
      if (!wp->get_client_user_sid_string(client_sid)) {
        BOOST_LOG(error) << "Playnite IPC: unable to query client SID";
        data_pipe->disconnect();
        std::this_thread::sleep_for(300ms);
        continue;
      }

      std::wstring expected_sid;
      {
#ifdef SUNSHINE_PLAYNITE_LAUNCHER
        // In the helper launcher context, compare to this process's user SID
        HANDLE tok = nullptr;
        if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &tok)) {
          BOOST_LOG(error) << "Playnite IPC: OpenProcessToken failed";
          data_pipe->disconnect();
          std::this_thread::sleep_for(300ms);
          continue;
        }
        DWORD len = 0;
        GetTokenInformation(tok, TokenUser, nullptr, 0, &len);
        auto buf = std::make_unique<uint8_t[]>(len);
        auto tu = reinterpret_cast<TOKEN_USER *>(buf.get());
        if (!GetTokenInformation(tok, TokenUser, tu, len, &len)) {
          CloseHandle(tok);
          BOOST_LOG(error) << "Playnite IPC: failed to get token user info";
          data_pipe->disconnect();
          std::this_thread::sleep_for(300ms);
          continue;
        }
        LPWSTR sidW = nullptr;
        if (!ConvertSidToStringSidW(tu->User.Sid, &sidW)) {
          CloseHandle(tok);
          BOOST_LOG(error) << "Playnite IPC: failed to convert expected SID";
          data_pipe->disconnect();
          std::this_thread::sleep_for(300ms);
          continue;
        }
        expected_sid.assign(sidW);
        LocalFree(sidW);
        CloseHandle(tok);
#else
        platf::dxgi::safe_token user_token;
        user_token.reset(platf::dxgi::retrieve_users_token(false));
        if (!user_token) {
          BOOST_LOG(error) << "Playnite IPC: failed to get active user token";
          data_pipe->disconnect();
          std::this_thread::sleep_for(300ms);
          continue;
        }
        DWORD len = 0;
        GetTokenInformation(user_token.get(), TokenUser, nullptr, 0, &len);
        auto buf = std::make_unique<uint8_t[]>(len);
        auto tu = reinterpret_cast<TOKEN_USER *>(buf.get());
        if (!GetTokenInformation(user_token.get(), TokenUser, tu, len, &len)) {
          BOOST_LOG(error) << "Playnite IPC: failed to get token user info";
          data_pipe->disconnect();
          std::this_thread::sleep_for(300ms);
          continue;
        }
        LPWSTR sidW = nullptr;
        if (!ConvertSidToStringSidW(tu->User.Sid, &sidW)) {
          BOOST_LOG(error) << "Playnite IPC: failed to convert expected SID";
          data_pipe->disconnect();
          std::this_thread::sleep_for(300ms);
          continue;
        }
        expected_sid.assign(sidW);
        LocalFree(sidW);
#endif
      }

      if (client_sid != expected_sid) {
        BOOST_LOG(error) << "Playnite IPC: client SID mismatch; rejecting";
        data_pipe->disconnect();
        std::this_thread::sleep_for(300ms);
        continue;
      }

      pipe_ = std::make_unique<platf::dxgi::AsyncNamedPipe>(std::move(data_pipe));
      pipe_->start(on_message, on_error, on_broken);
      active_.store(true);
      BOOST_LOG(info) << "Playnite IPC: data pipe established after handshake";

      // Wait until disconnection/broken or stop requested
      while (running_.load() && pipe_->is_connected() && !broken_.load()) {
        std::this_thread::sleep_for(200ms);
      }

      if (pipe_) {
        pipe_->stop();
        pipe_.reset();
      }
      active_.store(false);
      BOOST_LOG(info) << "Playnite IPC: disconnected";

      if (!running_.load()) break;
      std::this_thread::sleep_for(300ms);
    }
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

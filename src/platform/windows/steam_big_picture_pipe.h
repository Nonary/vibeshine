/**
 * @file src/platform/windows/steam_big_picture_pipe.h
 * @brief Framed JSON messages over an overlapped named pipe, shared by the service and the Steam UI helper.
 *
 * Framing is protocol::encode_frame(): a 4-byte little-endian length, then UTF-8 JSON. A frame that is
 * empty, oversized or not a JSON object closes the pipe: both ends are Vibeshine, so a bad frame means
 * something is wrong, not something to recover from.
 */
#pragma once

#ifdef _WIN32
  #include "src/steam_big_picture_controls_protocol.h"

  #include <windows.h>

  #include <array>
  #include <atomic>
  #include <chrono>
  #include <cstring>
  #include <mutex>
  #include <nlohmann/json.hpp>
  #include <optional>
  #include <string>

namespace platf::steam::big_picture {
  class framed_pipe_t {
  public:
    /** Wraps a pipe handle opened with FILE_FLAG_OVERLAPPED. Does not take ownership. */
    explicit framed_pipe_t(HANDLE pipe):
        pipe_(pipe),
        read_event_(CreateEventW(nullptr, TRUE, FALSE, nullptr)),
        write_event_(CreateEventW(nullptr, TRUE, FALSE, nullptr)) {
      closed_ = !read_event_ || !write_event_;
    }

    ~framed_pipe_t() {
      // Drain a pending read before its OVERLAPPED and buffer go away.
      if (read_pending_) {
        CancelIoEx(pipe_, &read_overlapped_);
        DWORD ignored = 0;
        GetOverlappedResult(pipe_, &read_overlapped_, &ignored, TRUE);
      }
      if (read_event_) {
        CloseHandle(read_event_);
      }
      if (write_event_) {
        CloseHandle(write_event_);
      }
    }

    framed_pipe_t(const framed_pipe_t &) = delete;
    framed_pipe_t &operator=(const framed_pipe_t &) = delete;

    bool closed() const {
      return closed_;
    }

    /** Send one message. Thread-safe. Returns false (and marks the pipe closed) on failure or timeout. */
    bool write(const nlohmann::json &message, std::chrono::milliseconds timeout = std::chrono::seconds(2)) {
      const auto frame = protocol::encode_frame(message.dump());
      std::lock_guard lock(write_mutex_);
      if (closed_ || !frame) {
        return false;
      }
      OVERLAPPED overlapped {};
      overlapped.hEvent = write_event_;
      ResetEvent(write_event_);
      DWORD written = 0;
      if (!WriteFile(pipe_, frame->data(), static_cast<DWORD>(frame->size()), &written, &overlapped)) {
        if (GetLastError() != ERROR_IO_PENDING) {
          closed_ = true;
          return false;
        }
        if (WaitForSingleObject(write_event_, static_cast<DWORD>(timeout.count())) != WAIT_OBJECT_0) {
          CancelIoEx(pipe_, &overlapped);
          GetOverlappedResult(pipe_, &overlapped, &written, TRUE);
          closed_ = true;
          return false;
        }
        if (!GetOverlappedResult(pipe_, &overlapped, &written, FALSE)) {
          closed_ = true;
          return false;
        }
      }
      if (written != frame->size()) {
        closed_ = true;
        return false;
      }
      return true;
    }

    /** Wait up to timeout for the next message. Call from one thread only. Returns nullopt on timeout;
     * check closed() to tell a timeout from a closed pipe. */
    std::optional<nlohmann::json> read(std::chrono::milliseconds timeout) {
      const auto deadline = std::chrono::steady_clock::now() + timeout;
      while (!closed_) {
        if (auto message = take_frame()) {
          return message;
        }
        if (closed_) {
          break;
        }
        if (!read_pending_) {
          ResetEvent(read_event_);
          read_overlapped_ = {};
          read_overlapped_.hEvent = read_event_;
          DWORD bytes = 0;
          if (ReadFile(pipe_, chunk_.data(), static_cast<DWORD>(chunk_.size()), &bytes, &read_overlapped_)) {
            if (bytes == 0) {
              closed_ = true;
              break;
            }
            buffer_.append(reinterpret_cast<const char *>(chunk_.data()), bytes);
            continue;
          }
          if (GetLastError() != ERROR_IO_PENDING) {
            closed_ = true;
            break;
          }
          read_pending_ = true;
        }
        const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now());
        if (remaining.count() <= 0 || WaitForSingleObject(read_event_, static_cast<DWORD>(remaining.count())) != WAIT_OBJECT_0) {
          return std::nullopt;
        }
        DWORD bytes = 0;
        read_pending_ = false;
        if (!GetOverlappedResult(pipe_, &read_overlapped_, &bytes, FALSE) || bytes == 0) {
          closed_ = true;
          break;
        }
        buffer_.append(reinterpret_cast<const char *>(chunk_.data()), bytes);
      }
      return std::nullopt;
    }

  private:
    std::optional<nlohmann::json> take_frame() {
      if (buffer_.size() < protocol::frame_header_bytes) {
        return std::nullopt;
      }
      std::array<std::uint8_t, protocol::frame_header_bytes> header {};
      std::memcpy(header.data(), buffer_.data(), header.size());
      const auto length = protocol::decode_frame_length(header);
      if (!length) {
        closed_ = true;
        return std::nullopt;
      }
      if (buffer_.size() < protocol::frame_header_bytes + *length) {
        return std::nullopt;
      }
      auto message = nlohmann::json::parse(buffer_.substr(protocol::frame_header_bytes, *length), nullptr, false);
      buffer_.erase(0, protocol::frame_header_bytes + *length);
      if (message.is_discarded() || !message.is_object()) {
        closed_ = true;
        return std::nullopt;
      }
      return message;
    }

    HANDLE pipe_;
    HANDLE read_event_;
    HANDLE write_event_;
    OVERLAPPED read_overlapped_ {};
    bool read_pending_ = false;
    std::array<std::uint8_t, 16 * 1024> chunk_ {};
    std::string buffer_;
    std::atomic_bool closed_ {false};
    std::mutex write_mutex_;
  };
}  // namespace platf::steam::big_picture
#endif

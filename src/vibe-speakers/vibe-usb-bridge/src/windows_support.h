// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <windows.h>
#include <bcrypt.h>
#include <sddl.h>
#include <algorithm>
#include <array>
#include <cstdio>
#include <string>
#include <vector>
#include "bridge_core.h"


namespace vibe_bridge {
class Handle {
    HANDLE value_ = INVALID_HANDLE_VALUE;
public:
    Handle() = default;
    explicit Handle(HANDLE value) : value_(value) {}
    ~Handle() { Close(); }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    Handle(Handle&& other) noexcept : value_(other.Release()) {}
    Handle& operator=(Handle&& other) noexcept {
        if (this != &other) { Close(); value_ = other.Release(); }
        return *this;
    }
    bool Valid() const { return value_ && value_ != INVALID_HANDLE_VALUE; }
    HANDLE Get() const { return value_; }
    HANDLE Release() { HANDLE result = value_; value_ = INVALID_HANDLE_VALUE; return result; }
    void Close() { if (Valid()) CloseHandle(value_); value_ = INVALID_HANDLE_VALUE; }
};
inline void WinCheck(bool ok, ErrorCode code, const char* operation) {
    if (!ok) throw Failure(code, operation, GetLastError());
}
inline uint64_t Qpc() {
    LARGE_INTEGER value{};
    WinCheck(QueryPerformanceCounter(&value) != FALSE, InternalFailure, "QueryPerformanceCounter");
    return static_cast<uint64_t>(value.QuadPart);
}
inline uint64_t Frequency() {
    LARGE_INTEGER value{};
    WinCheck(QueryPerformanceFrequency(&value) != FALSE, InternalFailure, "QueryPerformanceFrequency");
    return static_cast<uint64_t>(value.QuadPart);
}
inline std::string Utf8(const std::wstring& value) {
    if (value.empty()) return {};
    const int length = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(),
        static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    WinCheck(length > 0, InternalFailure, "UTF-16 conversion");
    std::string result(static_cast<size_t>(length), 0);
    WinCheck(WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(),
        static_cast<int>(value.size()), result.data(), length, nullptr, nullptr) == length,
        InternalFailure, "UTF-16 conversion");
    return result;
}
inline std::string Json(const std::string& value) {
    std::string result = "\"";
    for (unsigned char c : value) {
        if (c == '"' || c == '\\') { result += '\\'; result += static_cast<char>(c); }
        else if (c < 32) {
            char escape[7];
            std::snprintf(escape, sizeof(escape), "\\u%04x", c);
            result += escape;
        } else result += static_cast<char>(c);
    }
    return result + '"';
}
inline std::string Json(const std::wstring& value) { return Json(Utf8(value)); }
inline std::string NonceText(const uint64_t (&nonce)[2]) {
    char output[33];
    std::snprintf(output, sizeof(output), "%016llx%016llx", nonce[0], nonce[1]);
    return output;
}
inline void NewNonce(uint64_t (&nonce)[2]) {
    Require(BCryptGenRandom(nullptr, reinterpret_cast<PUCHAR>(nonce), sizeof(nonce),
            BCRYPT_USE_SYSTEM_PREFERRED_RNG) == 0 && (nonce[0] || nonce[1]),
            InternalFailure, "Session nonce generation failed");
}
inline Handle OpenNamedEvent(const std::wstring& name, DWORD access) {
    if (name.empty()) return {};
    Require(name.rfind(L"Local\\", 0) == 0 && name.size() <= 180,
            InvalidHandshake, "A Local event name is required");
    Handle event(OpenEventW(access, FALSE, name.c_str()));
    WinCheck(event.Valid(), InvalidHandshake, "Open coordination event");
    return event;
}
inline std::wstring CurrentSid() {
    HANDLE tokenRaw = nullptr;
    WinCheck(OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &tokenRaw), InternalFailure, "Read own token");
    Handle token(tokenRaw);
    DWORD size = 0;
    GetTokenInformation(token.Get(), TokenUser, nullptr, 0, &size);
    Require(size > 0 && size < 65536, InternalFailure, "Invalid token size");
    std::vector<uint8_t> buffer(size);
    WinCheck(GetTokenInformation(token.Get(), TokenUser, buffer.data(), size, &size),
             InternalFailure, "Read own user SID");
    LPWSTR text = nullptr;
    WinCheck(ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(buffer.data())->User.Sid, &text),
             InternalFailure, "Format SID");
    std::wstring result = text;
    LocalFree(text);
    return result;
}
#ifdef VIBE_BRIDGE_TESTING
inline std::atomic<std::uint64_t> peerImageHashCalls{0};
#endif
inline std::string Sha256(HANDLE file) {
#ifdef VIBE_BRIDGE_TESTING
    ++peerImageHashCalls;
#endif
    LARGE_INTEGER zero{};
    WinCheck(SetFilePointerEx(file, zero, nullptr, FILE_BEGIN), InternalFailure, "Hash file seek");
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    Require(BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) == 0,
            InternalFailure, "Open SHA256 provider");
    BCRYPT_HASH_HANDLE hash = nullptr;
    try {
        Require(BCryptCreateHash(algorithm, &hash, nullptr, 0, nullptr, 0, 0) == 0,
                InternalFailure, "Create SHA256");
        std::vector<uint8_t> bytes(65536);
        DWORD count = 0;
        for (;;) {
            WinCheck(ReadFile(file, bytes.data(), static_cast<DWORD>(bytes.size()), &count, nullptr),
                     InternalFailure, "Hash file read");
            if (!count) break;
            Require(BCryptHashData(hash, bytes.data(), count, 0) == 0, InternalFailure, "Hash file");
        }
        std::array<uint8_t, 32> digest{};
        Require(BCryptFinishHash(hash, digest.data(), static_cast<ULONG>(digest.size()), 0) == 0,
                InternalFailure, "Finish SHA256");
        BCryptDestroyHash(hash); hash = nullptr;
        BCryptCloseAlgorithmProvider(algorithm, 0); algorithm = nullptr;
        std::string result;
        constexpr char digits[] = "0123456789abcdef";
        for (auto c : digest) { result += digits[c >> 4]; result += digits[c & 15]; }
        return result;
    } catch (...) {
        if (hash) BCryptDestroyHash(hash);
        if (algorithm) BCryptCloseAlgorithmProvider(algorithm, 0);
        throw;
    }
}
inline Handle OpenPinnedImage(const std::wstring& path, const std::string& expectedHash) {
    // Deny concurrent writes and replacement while the connection is trusted.
    Handle file(CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                            OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr));
    WinCheck(file.Valid(), BindingMismatch, "Open expected peer image");
    Require(Sha256(file.Get()) == expectedHash, BindingMismatch, "Peer image SHA256 mismatch");
    return file;
}
inline std::wstring ProcessImage(HANDLE process) {
    std::wstring path(32768, L'\0');
    DWORD chars = static_cast<DWORD>(path.size());
    WinCheck(QueryFullProcessImageNameW(process, 0, path.data(), &chars), BindingMismatch, "Query peer image");
    path.resize(chars);
    return path;
}
inline void VerifySameFile(HANDLE expected, HANDLE actual) {
    FILE_ID_INFO first{}, second{};
    WinCheck(GetFileInformationByHandleEx(expected, FileIdInfo, &first, sizeof(first)) &&
             GetFileInformationByHandleEx(actual, FileIdInfo, &second, sizeof(second)),
             BindingMismatch, "Query peer file identity");
    Require(first.VolumeSerialNumber == second.VolumeSerialNumber &&
            std::memcmp(first.FileId.Identifier, second.FileId.Identifier, sizeof(first.FileId.Identifier)) == 0,
            BindingMismatch, "Peer executes another image file");
}

// The pinned handle is deliberately not exposed or replaceable. FILE_SHARE_READ
// excludes writes, replacement and deletion throughout all connection lifetimes.
class LockedPeerImage {
    const std::string expectedHash_;
    Handle anchor_;
public:
    LockedPeerImage(const std::wstring& path,const std::string& hash):
        expectedHash_(hash),anchor_(OpenPinnedImage(path,hash)) {}
    LockedPeerImage(const LockedPeerImage&)=delete;
    LockedPeerImage& operator=(const LockedPeerImage&)=delete;
    Handle Match(const std::wstring& path,const std::string& configuredHash) const {
        Require(configuredHash == expectedHash_,BindingMismatch,"Peer pin differs from the locked SHA256");
        Handle actual(CreateFileW(path.c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr));
        WinCheck(actual.Valid(),BindingMismatch,"Open actual process image without write/delete sharing");
        VerifySameFile(anchor_.Get(),actual.Get());
        return actual;
    }
};

// Complete one message with bounded waiting. On any exit the OVERLAPPED is
// retired before its event and stack storage can disappear.
class PipeOperation {
    Handle event_{CreateEventW(nullptr, TRUE, FALSE, nullptr)};
    OVERLAPPED operation_{};
public:
#ifdef VIBE_BRIDGE_TESTING
    std::function<void()> overflowObserved;
    std::function<void(HANDLE)> beforeCancel;
#endif
    PipeOperation() { WinCheck(event_.Valid(), InternalFailure, "Create session pipe completion event"); }
    DWORD Transfer(HANDLE pipe, void* buffer, DWORD size, bool writing,
                   HANDLE stop, HANDLE fault, DWORD timeoutMs,
                   const std::atomic<ErrorCode>* faultReason=nullptr,
                   std::uint64_t writeDeadlineQpc=0,std::uint64_t qpcFrequency=0) {
    WinCheck(ResetEvent(event_.Get()) != FALSE, InternalFailure, "Reset completed pipe operation");
    operation_ = {};
    operation_.hEvent = event_.Get();
    DWORD count = 0;
    const BOOL completed = writing ? WriteFile(pipe, buffer, size, &count, &operation_)
                                   : ReadFile(pipe, buffer, size, &count, &operation_);
    if (!completed) {
        const DWORD error = GetLastError();
        if (error != ERROR_IO_PENDING) throw Failure(PeerDisconnected, "Pipe I/O failed", error);
        HANDLE waitHandles[3] = {event_.Get()};
        DWORD waits = 1, stopIndex = MAXDWORD, faultIndex = MAXDWORD;
        if (stop) { stopIndex = waits; waitHandles[waits++] = stop; }
        if (fault) { faultIndex = waits; waitHandles[waits++] = fault; }
        DWORD result = WaitForMultipleObjects(waits, waitHandles, FALSE, timeoutMs);
        if (fault && result == WAIT_OBJECT_0 + faultIndex && writing && faultReason &&
            faultReason->load(std::memory_order_acquire) == QueueOverflow && writeDeadlineQpc && qpcFrequency) {
#ifdef VIBE_BRIDGE_TESTING
            if(overflowObserved)overflowObserved();
#endif
            // Exactly one already-popped message may still be on the pipe.
            // Confirm it within its ORIGINAL age deadline. Never enqueue
            // another block or extend the stale limit to manufacture recovery.
            LARGE_INTEGER stamp{};const bool sampled=QueryPerformanceCounter(&stamp)!=FALSE;
            const auto now=static_cast<std::uint64_t>(stamp.QuadPart);
            const auto left=sampled&&now<writeDeadlineQpc?writeDeadlineQpc-now:0;
            const auto millis=static_cast<DWORD>((left*1000u+qpcFrequency-1)/qpcFrequency);
            result=WaitForMultipleObjects(stop?2u:1u,waitHandles,FALSE,millis);
        }
        if (result != WAIT_OBJECT_0) {
#ifdef VIBE_BRIDGE_TESTING
            if(beforeCancel)beforeCancel(event_.Get());
#endif
            CancelIoEx(pipe, &operation_);
            const auto retired=GetOverlappedResult(pipe, &operation_, &count, TRUE);
            const auto retireError=retired?ERROR_SUCCESS:GetLastError();
            // A completion that wins the cancel race is a completed message.
            if(retired&&count==size)return count;
            if(faultReason&&faultReason->load(std::memory_order_acquire)==QueueOverflow)
                throw Failure(QueueOverflow,"USB queue overflow with an unconfirmed pipe transfer",
                              retireError?retireError:ERROR_OPERATION_ABORTED);
            if (result == WAIT_TIMEOUT) throw Failure(PipeTimeout, "Pipe I/O timed out");
            if (stop && result == WAIT_OBJECT_0 + stopIndex) throw Failure(ControlledStop, "Bridge stop requested");
            if (fault && result == WAIT_OBJECT_0 + faultIndex) {
                const auto reason=faultReason?faultReason->load(std::memory_order_acquire):InternalFailure;
                throw Failure(reason==NoError?InternalFailure:reason,"USB capture failed");
            }
            throw Failure(InternalFailure, "Pipe wait failed", GetLastError());
        }
        WinCheck(GetOverlappedResult(pipe, &operation_, &count, FALSE),
                 PeerDisconnected, "Complete pipe I/O");
    }
    Require(count == size, InvalidHandshake, "Unexpected pipe message length");
    return count;
    }
};
// Client sends only Hello. A separate pending read observes a living client's
// idle close without polling or waiting for the next audio WriteFile. Duplex
// operations use distinct OVERLAPPED storage. The read is retired on every exit.
class PeerReadWatch {
    Handle event_{CreateEventW(nullptr, TRUE, FALSE, nullptr)};
    HANDLE pipe_;
    OVERLAPPED operation_{};
    unsigned char byte_ = 0;
    bool pending_ = false;
public:
    explicit PeerReadWatch(HANDLE pipe) : pipe_(pipe) {
        WinCheck(event_.Valid(), InternalFailure, "Create peer close watch");
        operation_.hEvent = event_.Get();
        DWORD count = 0;
        if (ReadFile(pipe_, &byte_, 1, &count, &operation_))
            throw Failure(InvalidHandshake, "Unexpected client message after Hello");
        const auto error = GetLastError();
        if (error != ERROR_IO_PENDING)
            throw Failure(error == ERROR_MORE_DATA ? InvalidHandshake : PeerDisconnected,
                          "Peer close watch failed", error);
        pending_ = true;
    }
    PeerReadWatch(const PeerReadWatch&) = delete;
    PeerReadWatch& operator=(const PeerReadWatch&) = delete;
    ~PeerReadWatch() {
        if (pending_) {
            CancelIoEx(pipe_, &operation_);
            DWORD count = 0;
            GetOverlappedResult(pipe_, &operation_, &count, TRUE);
        }
    }
    HANDLE Event() const noexcept { return event_.Get(); }
private:
    DWORD CompleteRead() {
        DWORD count = 0;
        const auto ok = GetOverlappedResult(pipe_, &operation_, &count, FALSE);
        const auto error = GetLastError();
        if (!ok && error == ERROR_IO_INCOMPLETE)
            throw Failure(InternalFailure, "Peer watch signaled before completion", error);
        pending_ = false;
        if (!ok && (error == ERROR_BROKEN_PIPE || error == ERROR_NO_DATA || error == ERROR_PIPE_NOT_CONNECTED))
            return error;
        if (!ok && error != ERROR_MORE_DATA)
            throw Failure(PeerDisconnected, "Peer read watch failed", error);
        throw Failure(InvalidHandshake, "Unexpected client data after Hello", ok ? 0 : error);
    }
public:
    [[noreturn]] void Completed() {
        const auto error = CompleteRead();
        throw Failure(PeerDisconnected, "Peer closed its pipe", error);
    }
    // Called only after the complete NoError terminal message was written.
    // The client sends no ACK bytes. Its recorded Goodbye receipt proves
    // delivery; this wait prevents a premature server-side pipe discard.
    DWORD AwaitTerminalClose(HANDLE stop, DWORD milliseconds) {
        HANDLE waits[] = {event_.Get(), stop};
        const auto result = WaitForMultipleObjects(2, waits, FALSE, milliseconds);
        if (result == WAIT_OBJECT_0) return CompleteRead();
        if (result == WAIT_OBJECT_0 + 1) throw Failure(ControlledStop, "Stopped while awaiting terminal client close");
        if (result == WAIT_TIMEOUT) throw Failure(PipeTimeout, "Timed out awaiting terminal client close");
        throw Failure(InternalFailure, "Wait for terminal client close failed", GetLastError());
    }
};
} // namespace vibe_bridge

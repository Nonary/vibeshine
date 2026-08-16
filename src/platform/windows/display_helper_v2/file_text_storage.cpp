#include "src/platform/windows/display_helper_v2/file_text_storage.h"

#include <algorithm>
#include <cstdio>
#include <cwctype>
#include <limits>
#include <memory>
#include <string>

#ifdef _WIN32
  #include <windows.h>
#endif

namespace display_helper::v2 {
#ifdef _WIN32
  namespace {
    std::wstring normalized_path(std::wstring value) {
      if (value.rfind(L"\\\\?\\UNC\\", 0) == 0) {
        value.erase(0, 7);
        value.insert(0, L"\\\\");
      } else if (value.rfind(L"\\\\?\\", 0) == 0) {
        value.erase(0, 4);
      }
      for (auto &ch : value) {
        if (ch == L'/') ch = L'\\';
        ch = static_cast<wchar_t>(std::towlower(ch));
      }
      return value;
    }

    bool handle_matches_path(HANDLE handle, const std::filesystem::path &path) {
      wchar_t actual[MAX_PATH * 4] {};
      const DWORD actual_length = GetFinalPathNameByHandleW(handle, actual, _countof(actual), FILE_NAME_NORMALIZED);
      wchar_t expected[MAX_PATH * 4] {};
      const DWORD expected_length = GetFullPathNameW(path.wstring().c_str(), _countof(expected), expected, nullptr);
      return actual_length != 0 && actual_length < _countof(actual) &&
             expected_length != 0 && expected_length < _countof(expected) &&
             normalized_path(std::wstring {actual, actual_length}) ==
               normalized_path(std::wstring {expected, expected_length});
    }
  }
#endif

  std::optional<std::string> AtomicFileTextStorage::read(const std::string &key) {
    constexpr std::uintmax_t kMaxSnapshotBytes = 1024u * 1024u;
    const std::filesystem::path path {key};
#ifdef _WIN32
    // Open and size the file through one handle.  A path-only status check
    // followed by fopen permits a same-user sibling to swap a junction or
    // reparse point between validation and the read.
    const auto parent = path.parent_path();
    const auto parent_handle = CreateFileW(
      parent.wstring().c_str(), FILE_READ_ATTRIBUTES,
      FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
      OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (parent_handle == INVALID_HANDLE_VALUE) return std::nullopt;
    const auto close_parent = std::unique_ptr<void, void (*)(void *)> {
      parent_handle, [](void *value) { CloseHandle(value); }};
    BY_HANDLE_FILE_INFORMATION parent_info {};
    if (!GetFileInformationByHandle(parent_handle, &parent_info) ||
        (parent_info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0 ||
        (parent_info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0 ||
        !handle_matches_path(parent_handle, parent)) {
      return std::nullopt;
    }
    const auto file_handle = CreateFileW(
      path.wstring().c_str(), GENERIC_READ,
      FILE_SHARE_READ, nullptr, OPEN_EXISTING,
      FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (file_handle == INVALID_HANDLE_VALUE) return std::nullopt;
    const auto close_file = std::unique_ptr<void, void (*)(void *)> {
      file_handle, [](void *value) { CloseHandle(value); }};
    BY_HANDLE_FILE_INFORMATION info {};
    if (!GetFileInformationByHandle(file_handle, &info) ||
        (info.dwFileAttributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY)) != 0) {
      return std::nullopt;
    }
    LARGE_INTEGER size {};
    if (!GetFileSizeEx(file_handle, &size) || size.QuadPart < 0 ||
        static_cast<std::uint64_t>(size.QuadPart) > kMaxSnapshotBytes) {
      return std::nullopt;
    }
    std::string data(static_cast<std::size_t>(size.QuadPart), '\0');
    std::size_t offset = 0;
    while (offset < data.size()) {
      const DWORD chunk = static_cast<DWORD>(std::min<std::size_t>(data.size() - offset, 64 * 1024));
      DWORD read = 0;
      if (!ReadFile(file_handle, data.data() + offset, chunk, &read, nullptr) || read != chunk) {
        return std::nullopt;
      }
      offset += read;
    }
    return data;
#else
    std::error_code ec;
    const auto status = std::filesystem::symlink_status(path, ec);
    if (ec || !std::filesystem::is_regular_file(status)) {
      return std::nullopt;
    }
#ifdef _WIN32
    const auto attributes = GetFileAttributesW(path.wstring().c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
      return std::nullopt;
    }
#endif
    const auto size = std::filesystem::file_size(path, ec);
    if (ec || size > kMaxSnapshotBytes) {
      return std::nullopt;
    }
    FILE *file = _wfopen(path.wstring().c_str(), L"rb");
    if (!file) {
      return std::nullopt;
    }
    const auto guard = std::unique_ptr<FILE, int (*)(FILE *)> {file, fclose};
    std::string data;
    char buffer[4096];
    data.reserve(static_cast<std::size_t>(size));
    while (const size_t count = fread(buffer, 1, sizeof(buffer), file)) {
      if (data.size() > kMaxSnapshotBytes - count) {
        return std::nullopt;
      }
      data.append(buffer, count);
    }
    return data;
#endif
  }

  bool AtomicFileTextStorage::write_atomically(const std::string &key, const std::string &text) {
    constexpr std::size_t kMaxSnapshotBytes = 1024u * 1024u;
    const std::filesystem::path path {key};
    if (path.empty() || text.size() > kMaxSnapshotBytes) {
      return false;
    }
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
#ifdef _WIN32
    const auto parent = path.parent_path();
    const auto parent_handle = CreateFileW(
      parent.wstring().c_str(), FILE_READ_ATTRIBUTES,
      FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
      OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (parent_handle == INVALID_HANDLE_VALUE) return false;
    const auto close_parent = std::unique_ptr<void, void (*)(void *)> {
      parent_handle, [](void *value) { CloseHandle(value); }};
    BY_HANDLE_FILE_INFORMATION parent_info {};
    if (!GetFileInformationByHandle(parent_handle, &parent_info) ||
        (parent_info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0 ||
        (parent_info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0 ||
        !handle_matches_path(parent_handle, parent)) {
      return false;
    }
    auto temporary = path;
    temporary += L".tmp." + std::to_wstring(GetCurrentProcessId()) + L"." + std::to_wstring(GetTickCount64());
    const auto temporary_handle = CreateFileW(
      temporary.wstring().c_str(), GENERIC_WRITE,
      0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (temporary_handle == INVALID_HANDLE_VALUE) return false;
    auto close_temporary = std::unique_ptr<void, void (*)(void *)> {
      temporary_handle, [](void *value) { CloseHandle(value); }};
    std::size_t offset = 0;
    while (offset < text.size()) {
      const DWORD chunk = static_cast<DWORD>(std::min<std::size_t>(text.size() - offset, 64 * 1024));
      DWORD written = 0;
      if (!WriteFile(temporary_handle, text.data() + offset, chunk, &written, nullptr) || written != chunk) {
        close_temporary.release();
        CloseHandle(temporary_handle);
        std::filesystem::remove(temporary, ec);
        return false;
      }
      offset += written;
    }
    if (!FlushFileBuffers(temporary_handle)) {
      close_temporary.release();
      CloseHandle(temporary_handle);
      std::filesystem::remove(temporary, ec);
      return false;
    }
    close_temporary.release();
    CloseHandle(temporary_handle);
    if (!MoveFileExW(temporary.wstring().c_str(), path.wstring().c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
      std::filesystem::remove(temporary, ec);
      return false;
    }
    return true;
#else
    auto temporary = path;
    temporary += L".tmp";
    {
      FILE *file = _wfopen(temporary.wstring().c_str(), L"wb");
      if (!file) {
        return false;
      }
      auto guard = std::unique_ptr<FILE, int (*)(FILE *)> {file, fclose};
      if (fwrite(text.data(), 1, text.size(), file) != text.size()) {
        guard.reset();
        std::filesystem::remove(temporary, ec);
        return false;
      }
    }
    if (!std::filesystem::exists(path, ec) || ec) {
      std::filesystem::rename(temporary, path, ec);
      if (!ec) {
        return true;
      }
    }
    std::filesystem::copy_file(temporary, path, std::filesystem::copy_options::overwrite_existing, ec);
    if (ec) {
      return false;
    }
    std::filesystem::remove(temporary, ec);
    return true;
#endif
  }

  bool AtomicFileTextStorage::remove(const std::string &key) {
    std::error_code ec;
    return std::filesystem::remove(std::filesystem::path {key}, ec);
  }

  bool AtomicFileTextStorage::exists(const std::string &key) {
    std::error_code ec;
    return std::filesystem::exists(std::filesystem::path {key}, ec) && !ec;
  }
}  // namespace display_helper::v2

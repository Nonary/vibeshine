#include "src/platform/windows/display_helper_v2/file_text_storage.h"

#include <atomic>
#include <cstdio>
#include <memory>

#include <io.h>
#include <windows.h>

namespace display_helper::v2 {
  std::optional<std::string> AtomicFileTextStorage::read(const std::string &key) {
    const std::filesystem::path path {key};
    if (path.empty()) {
      return std::nullopt;
    }
    FILE *file = _wfopen(path.wstring().c_str(), L"rb");
    if (!file) {
      return std::nullopt;
    }
    const auto guard = std::unique_ptr<FILE, int (*)(FILE *)> {file, fclose};
    std::string data;
    char buffer[4096];
    while (const size_t count = fread(buffer, 1, sizeof(buffer), file)) {
      data.append(buffer, count);
    }
    if (ferror(file)) {
      return std::nullopt;
    }
    return data;
  }

  bool AtomicFileTextStorage::write_atomically(const std::string &key, const std::string &text) {
    const std::filesystem::path path {key};
    if (path.empty()) {
      return false;
    }
    std::error_code ec;
    if (!path.parent_path().empty()) {
      std::filesystem::create_directories(path.parent_path(), ec);
      if (ec) {
        return false;
      }
    }
    // Keep staging on the same volume and isolate concurrent writers. An
    // abandoned staging file is never mistaken for a committed baseline.
    static std::atomic<unsigned long long> sequence {0};
    auto temporary = path;
    temporary += L".tmp." + std::to_wstring(GetCurrentProcessId()) + L"." +
                 std::to_wstring(sequence.fetch_add(1, std::memory_order_relaxed));
    {
      FILE *file = _wfopen(temporary.wstring().c_str(), L"wb");
      if (!file) {
        return false;
      }
      auto guard = std::unique_ptr<FILE, int (*)(FILE *)> {file, fclose};
      if (fwrite(text.data(), 1, text.size(), file) != text.size() ||
          fflush(file) != 0 || _commit(_fileno(file)) != 0) {
        guard.reset();
        std::filesystem::remove(temporary, ec);
        return false;
      }
      if (fclose(guard.release()) != 0) {
        std::filesystem::remove(temporary, ec);
        return false;
      }
    }
    // Never copy over the destination: an interrupted copy destroys the last
    // complete recovery snapshot. Flush the replacement operation as well.
    if (!MoveFileExW(temporary.wstring().c_str(), path.wstring().c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
      std::filesystem::remove(temporary, ec);
      return false;
    }
    return true;
  }

  bool AtomicFileTextStorage::remove(const std::string &key) {
    std::error_code ec;
    return std::filesystem::remove(std::filesystem::path {key}, ec);
  }

  bool AtomicFileTextStorage::exists(const std::string &key) {
    if (key.empty()) {
      // An unconfigured recovery path cannot prove there is no baseline.
      return true;
    }
    if (GetFileAttributesW(std::filesystem::path {key}.wstring().c_str()) != INVALID_FILE_ATTRIBUTES) {
      return true;
    }
    const auto error = GetLastError();
    return error != ERROR_FILE_NOT_FOUND && error != ERROR_PATH_NOT_FOUND;
  }
}  // namespace display_helper::v2

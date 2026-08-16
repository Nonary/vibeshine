#include "src/platform/windows/display_helper_v2/file_text_storage.h"

#include <algorithm>
#include <cstddef>
#include <cstdio>
#include <cwctype>
#include <cstring>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <openssl/crypto.h>
#include <openssl/rand.h>
#include <openssl/sha.h>

#include "src/platform/windows/display_helper_session.h"
#include "src/terminal_session_display_protocol.h"

#ifdef _WIN32
  #include <windows.h>
#endif

namespace display_helper::v2 {
  namespace {
#pragma pack(push, 1)
    struct snapshot_envelope_header {
      std::uint8_t magic[8];
      std::uint32_t version {};
      std::uint32_t session_id {};
      std::uint64_t generation {};
      std::uint64_t display_id {};
      std::uint32_t tier {};
      std::uint64_t sequence {};
      std::uint32_t payload_size {};
      std::array<std::uint8_t, 32> digest {};
      std::array<std::uint8_t, 32> tag {};
    };
#pragma pack(pop)

    static_assert(sizeof(snapshot_envelope_header) == 112);

    // The helper stores only a broker tag. The signing key and replay state
    // remain exclusively in the SYSTEM terminal worker broker.

    constexpr std::uint8_t kSnapshotEnvelopeMagic[8] {'V', 'S', 'N', 'A', 'P', 'M', 'A', 'C'};
    constexpr std::uint32_t kSnapshotEnvelopeVersion = 1;

    std::array<std::uint8_t, 32> snapshot_digest(const std::string &payload) {
      std::array<std::uint8_t, 32> digest {};
      SHA256(reinterpret_cast<const unsigned char *>(payload.data()), payload.size(), digest.data());
      return digest;
    }

    std::optional<std::string> unwrap_snapshot(const std::string &data, const SnapshotTier tier) {
      if (data.size() < sizeof(snapshot_envelope_header)) return std::nullopt;
      snapshot_envelope_header header {};
      std::memcpy(&header, data.data(), sizeof(header));
      if (std::memcmp(header.magic, kSnapshotEnvelopeMagic, sizeof(header.magic)) != 0 ||
          header.version != kSnapshotEnvelopeVersion ||
          header.session_id != display_helper_session::managed_context().session_id ||
          header.generation != display_helper_session::managed_generation() ||
          header.tier != static_cast<std::uint32_t>(tier) || header.display_id == 0 || header.sequence == 0 ||
          header.payload_size > 1024u * 1024u ||
          data.size() != sizeof(header) + header.payload_size) {
        return std::nullopt;
      }
      const auto payload = data.substr(sizeof(header), header.payload_size);
      if (snapshot_digest(payload) != header.digest) {
        return std::nullopt;
      }
      const auto verified = terminal_session::display::transact_snapshot(
        terminal_session::display::operation::verify_snapshot,
        display_helper_session::managed_generation(),
        header.tier, header.sequence, header.display_id, header.digest, header.tag);
      return verified && verified->result == static_cast<std::uint8_t>(terminal_session::display::result::success) ?
               std::optional<std::string> {payload} : std::nullopt;
    }

    struct sealed_snapshot {
      std::string envelope;
      SnapshotTier tier {};
      std::uint64_t sequence {};
      std::uint64_t display_id {};
      std::array<std::uint8_t, 32> digest {};
      std::array<std::uint8_t, 32> tag {};
    };

    std::optional<sealed_snapshot> wrap_snapshot(const std::string &payload, const SnapshotTier tier) {
      if (payload.size() > 1024u * 1024u) return std::nullopt;
      const auto digest = snapshot_digest(payload);
      const auto sealed = terminal_session::display::transact_snapshot(
        terminal_session::display::operation::seal_snapshot,
        display_helper_session::managed_generation(), static_cast<std::uint32_t>(tier), 0, 0, digest);
      if (!sealed || sealed->result != static_cast<std::uint8_t>(terminal_session::display::result::success) ||
          sealed->display_id == 0 || sealed->snapshot_sequence == 0 ||
          std::all_of(sealed->snapshot_tag.begin(), sealed->snapshot_tag.end(), [](const auto byte) { return byte == 0; })) {
        return std::nullopt;
      }
      snapshot_envelope_header header {};
      std::memcpy(header.magic, kSnapshotEnvelopeMagic, sizeof(header.magic));
      header.version = kSnapshotEnvelopeVersion;
      header.session_id = display_helper_session::managed_context().session_id;
      header.generation = display_helper_session::managed_generation();
      header.display_id = sealed->display_id;
      header.tier = static_cast<std::uint32_t>(tier);
      header.sequence = sealed->snapshot_sequence;
      header.payload_size = static_cast<std::uint32_t>(payload.size());
      header.digest = digest;
      header.tag = sealed->snapshot_tag;
      std::string result {
        reinterpret_cast<const char *>(&header), sizeof(header)};
      result += payload;
      return sealed_snapshot {
        .envelope = std::move(result),
        .tier = tier,
        .sequence = header.sequence,
        .display_id = header.display_id,
        .digest = header.digest,
        .tag = header.tag,
      };
    }
  }

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

    std::optional<std::wstring> unpredictable_temp_suffix() {
      std::array<std::uint8_t, 16> bytes {};
      if (RAND_bytes(bytes.data(), static_cast<int>(bytes.size())) != 1) return std::nullopt;
      constexpr wchar_t hex[] = L"0123456789abcdef";
      std::wstring suffix;
      suffix.reserve(bytes.size() * 2);
      for (const auto byte : bytes) {
        suffix.push_back(hex[byte >> 4]);
        suffix.push_back(hex[byte & 0x0f]);
      }
      OPENSSL_cleanse(bytes.data(), bytes.size());
      return suffix;
    }
  }
#endif

  std::optional<std::string> AtomicFileTextStorage::read(const std::string &key) {
    return read(key, SnapshotTier::Current);
  }

  std::optional<std::string> AtomicFileTextStorage::read(const std::string &key, const SnapshotTier tier) {
    const std::uintmax_t kMaxSnapshotBytes = snapshot_envelope_ && display_helper_session::has_managed_context() ?
      1024u * 1024u + sizeof(snapshot_envelope_header) : 1024u * 1024u;
    const std::filesystem::path path {key};
#ifdef _WIN32
    // Open and size the file through one handle.  A path-only status check
    // followed by fopen permits a same-user sibling to swap a junction or
    // reparse point between validation and the read.
    const auto parent = path.parent_path();
    const auto parent_handle = CreateFileW(
      parent.wstring().c_str(), FILE_READ_ATTRIBUTES,
      FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
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
    if (snapshot_envelope_ && display_helper_session::has_managed_context()) {
      return unwrap_snapshot(data, tier);
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
    return write_atomically(key, text, SnapshotTier::Current);
  }

  bool AtomicFileTextStorage::write_atomically(
    const std::string &key,
    const std::string &text,
    const SnapshotTier tier) {
    std::string stored_text = text;
    std::optional<sealed_snapshot> sealed;
    if (snapshot_envelope_ && display_helper_session::has_managed_context()) {
      sealed = wrap_snapshot(text, tier);
      if (!sealed) return false;
      stored_text = sealed->envelope;
    }
    const std::size_t kMaxSnapshotBytes = snapshot_envelope_ && display_helper_session::has_managed_context() ?
      1024u * 1024u + sizeof(snapshot_envelope_header) : 1024u * 1024u;
    const std::filesystem::path path {key};
    if (path.empty() || stored_text.size() > kMaxSnapshotBytes) {
      return false;
    }
    std::error_code ec;
    if (!(snapshot_envelope_ && display_helper_session::has_managed_context())) {
      std::filesystem::create_directories(path.parent_path(), ec);
    }
#ifdef _WIN32
    const auto parent = path.parent_path();
    const auto parent_handle = CreateFileW(
      parent.wstring().c_str(), DELETE | FILE_READ_ATTRIBUTES,
      FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
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
    const auto suffix = unpredictable_temp_suffix();
    if (!suffix) return false;
    auto temporary = path;
    temporary += L".tmp." + *suffix;
    const auto temporary_handle = CreateFileW(
      temporary.wstring().c_str(), GENERIC_WRITE | DELETE,
      0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (temporary_handle == INVALID_HANDLE_VALUE) return false;
    auto close_temporary = std::unique_ptr<void, void (*)(void *)> {
      temporary_handle, [](void *value) { CloseHandle(value); }};
    std::size_t offset = 0;
    while (offset < stored_text.size()) {
      const DWORD chunk = static_cast<DWORD>(std::min<std::size_t>(stored_text.size() - offset, 64 * 1024));
      DWORD written = 0;
      if (!WriteFile(temporary_handle, stored_text.data() + offset, chunk, &written, nullptr) || written != chunk) {
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
    const auto target_name = path.filename().wstring();
    const auto rename_size = offsetof(FILE_RENAME_INFO, FileName) + target_name.size() * sizeof(wchar_t);
    std::vector<std::uint8_t> rename_buffer(rename_size);
    auto *rename_info = reinterpret_cast<FILE_RENAME_INFO *>(rename_buffer.data());
    rename_info->ReplaceIfExists = TRUE;
    rename_info->RootDirectory = parent_handle;
    rename_info->FileNameLength = static_cast<DWORD>(target_name.size() * sizeof(wchar_t));
    std::memcpy(rename_info->FileName, target_name.data(), rename_info->FileNameLength);
    if (!SetFileInformationByHandle(
          temporary_handle, FileRenameInfo, rename_info, static_cast<DWORD>(rename_buffer.size()))) {
      close_temporary.release();
      CloseHandle(temporary_handle);
      std::filesystem::remove(temporary, ec);
      return false;
    }
    // Keep the same handle through rename and validation. Reopening the
    // destination would permit a sibling to replace the published file
    // between validation and broker commit.
    BY_HANDLE_FILE_INFORMATION final_info {};
    if (!GetFileInformationByHandle(temporary_handle, &final_info) ||
        (final_info.dwFileAttributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY)) != 0 ||
        !handle_matches_path(temporary_handle, path)) {
      return false;
    }
    if (sealed) {
      const auto committed = terminal_session::display::transact_snapshot(
        terminal_session::display::operation::commit_snapshot,
        display_helper_session::managed_generation(),
        static_cast<std::uint32_t>(sealed->tier), sealed->sequence, sealed->display_id,
        sealed->digest, sealed->tag);
      if (!committed || committed->result != static_cast<std::uint8_t>(terminal_session::display::result::success)) {
        return false;
      }
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
      if (fwrite(stored_text.data(), 1, stored_text.size(), file) != stored_text.size()) {
        guard.reset();
        std::filesystem::remove(temporary, ec);
        return false;
      }
    }
    std::filesystem::rename(temporary, path, ec);
    if (ec) {
      std::filesystem::remove(temporary, ec);
      return false;
    }
    if (sealed) {
      const auto committed = terminal_session::display::transact_snapshot(
        terminal_session::display::operation::commit_snapshot,
        display_helper_session::managed_generation(),
        static_cast<std::uint32_t>(sealed->tier), sealed->sequence, sealed->display_id,
        sealed->digest, sealed->tag);
      if (!committed || committed->result != static_cast<std::uint8_t>(terminal_session::display::result::success)) {
        return false;
      }
    }
    return true;
#endif
  }

  bool AtomicFileTextStorage::remove(const std::string &key) {
#ifdef _WIN32
    const std::filesystem::path path {key};
    const auto parent = path.parent_path();
    const auto parent_handle = CreateFileW(
      parent.wstring().c_str(), FILE_READ_ATTRIBUTES,
      FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
      FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (parent_handle == INVALID_HANDLE_VALUE) return false;
    const auto close_parent = std::unique_ptr<void, void (*)(void *)> {
      parent_handle, [](void *value) { CloseHandle(value); }};
    BY_HANDLE_FILE_INFORMATION parent_info {};
    if (!GetFileInformationByHandle(parent_handle, &parent_info) ||
        (parent_info.dwFileAttributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY)) != FILE_ATTRIBUTE_DIRECTORY ||
        !handle_matches_path(parent_handle, parent)) {
      return false;
    }
    const auto file_handle = CreateFileW(
      path.wstring().c_str(), DELETE | FILE_READ_ATTRIBUTES,
      FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
      FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (file_handle == INVALID_HANDLE_VALUE) return false;
    const auto close_file = std::unique_ptr<void, void (*)(void *)> {
      file_handle, [](void *value) { CloseHandle(value); }};
    BY_HANDLE_FILE_INFORMATION info {};
    if (!GetFileInformationByHandle(file_handle, &info) ||
        (info.dwFileAttributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY)) != 0 ||
        !handle_matches_path(file_handle, path)) {
      return false;
    }
    FILE_DISPOSITION_INFO disposition {.DeleteFile = TRUE};
    return SetFileInformationByHandle(file_handle, FileDispositionInfo, &disposition, sizeof(disposition)) != FALSE;
#else
    std::error_code ec;
    return std::filesystem::remove(std::filesystem::path {key}, ec);
#endif
  }

  bool AtomicFileTextStorage::exists(const std::string &key) {
    std::error_code ec;
    return std::filesystem::exists(std::filesystem::path {key}, ec) && !ec;
  }
}  // namespace display_helper::v2

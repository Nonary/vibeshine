/**
 * @file src/file_handler.cpp
 * @brief Default operating-system adapter for file handling policy.
 */
#if defined(__linux__) && !defined(_GNU_SOURCE)
  #define _GNU_SOURCE 1
#endif

#include "file_handler.h"

#include "logging.h"

#include <chrono>
#include <fstream>
#include <functional>
#include <system_error>
#include <thread>

#ifdef _WIN32
  #include <Windows.h>
#else
  #include <cerrno>
  #include <fcntl.h>
  #include <sys/stat.h>
  #include <unistd.h>
#endif

namespace file_handler {
  namespace {
    class native_filesystem_t: public filesystem_t {
    public:
      bool exists(const std::filesystem::path &path) const override {
        std::error_code error;
        return std::filesystem::exists(path, error);
      }

      bool create_directories(const std::filesystem::path &path) override {
        std::error_code error;
        const bool created = std::filesystem::create_directories(path, error);
        return !error && (created || std::filesystem::exists(path));
      }

      std::string read(const std::filesystem::path &path) const override {
        std::ifstream input(path);
        return std::string {(std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>()};
      }

      bool write(const std::filesystem::path &path, std::string_view contents) override {
#ifdef _WIN32
        std::ofstream output(path, std::ios::binary | std::ios::trunc);
        if (!output.is_open()) {
          return false;
        }
        output.write(contents.data(), static_cast<std::streamsize>(contents.size()));
        output.flush();
        output.close();
        return static_cast<bool>(output);
#else
        // Atomic callers always write a fresh temporary path. O_EXCL and
        // O_NOFOLLOW prevent a same-directory attacker from redirecting a
        // predictable temporary name through a pre-positioned symlink.
        const int fd = ::open(
          path.c_str(),
          O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW,
          S_IRUSR | S_IWUSR
        );
        if (fd < 0) {
          return false;
        }

        size_t offset = 0;
        bool success = true;
        while (offset < contents.size()) {
          const auto written = ::write(fd, contents.data() + offset, contents.size() - offset);
          if (written < 0) {
            if (errno == EINTR) {
              continue;
            }
            success = false;
            break;
          }
          if (written == 0) {
            success = false;
            break;
          }
          offset += static_cast<size_t>(written);
        }

        if (success && ::fsync(fd) != 0) {
          success = false;
        }
        if (::close(fd) != 0) {
          success = false;
        }
        return success;
#endif
      }

#ifdef __linux__
      static replace_result_e sync_filesystem_and_close(int fd) {
        int synced;
        do {
          synced = ::syncfs(fd);
        } while (synced != 0 && errno == EINTR);
        const int closed = ::close(fd);
        return synced == 0 && closed == 0 ? replace_result_e::success : replace_result_e::error;
      }
#endif

      replace_result_e replace(const std::filesystem::path &temporary,
                               const std::filesystem::path &target) override {
#ifdef _WIN32
        if (MoveFileExW(
              temporary.c_str(),
              target.c_str(),
              MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH
            )) {
          return replace_result_e::success;
        }
        return GetLastError() == ERROR_ACCESS_DENIED ? replace_result_e::access_denied : replace_result_e::error;
#else
  #ifdef __linux__
        // Flushing the staged inode does not persist the rename's directory
        // entry. Open the containing directory before replacement so an open
        // failure leaves the last committed target untouched.
        const auto parent = target.parent_path().empty() ? std::filesystem::path {"."} : target.parent_path();
        int parent_fd = ::open(parent.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        bool filesystem_flush_only = false;
        if (parent_fd < 0 && (errno == EACCES || errno == EPERM)) {
          // A writable/searchable directory need not be readable. The private
          // staging inode provides a descriptor on the same filesystem.
          parent_fd = ::open(temporary.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
          filesystem_flush_only = true;
        }
        if (parent_fd < 0) {
          return replace_result_e::error;
        }
  #endif
        std::error_code error;
        std::filesystem::rename(temporary, target, error);
  #ifdef __linux__
        if (!error) {
          if (filesystem_flush_only) {
            return sync_filesystem_and_close(parent_fd);
          }
          // Parent directories may have been created by this save, an earlier
          // failed attempt, or configuration setup. Commit every directory
          // entry bottom-up rather than treating an existing name as durable.
          // Walk directory descriptors so symlinks and '..' use real ancestry.
          int directory_fd = parent_fd;
          for (;;) {
            int synced;
            do {
              synced = ::fsync(directory_fd);
            } while (synced != 0 && errno == EINTR);
            struct stat directory_info {};
            if (synced != 0 || ::fstat(directory_fd, &directory_info) != 0) {
              (void) ::close(directory_fd);
              return replace_result_e::error;
            }
            const int ancestor_fd = ::openat(directory_fd, "..", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
            if (ancestor_fd < 0) {
              if (errno == EACCES || errno == EPERM) {
                // Searchable but unreadable ancestors are valid state paths.
                // Commit their metadata via the filesystem instead of adding
                // a new read-permission requirement or skipping durability.
                return sync_filesystem_and_close(directory_fd);
              }
              (void) ::close(directory_fd);
              return replace_result_e::error;
            }
            struct stat ancestor_info {};
            if (::fstat(ancestor_fd, &ancestor_info) != 0) {
              (void) ::close(ancestor_fd);
              (void) ::close(directory_fd);
              return replace_result_e::error;
            }
            const bool at_root = directory_info.st_dev != ancestor_info.st_dev ||
                                 directory_info.st_ino == ancestor_info.st_ino;
            const int closed = ::close(directory_fd);
            if (at_root || closed != 0) {
              // Crossing a mount does not create its mountpoint: that belongs
              // to the already-established outer filesystem, which may be RO.
              const int ancestor_closed = ::close(ancestor_fd);
              return closed == 0 && ancestor_closed == 0 ? replace_result_e::success : replace_result_e::error;
            }
            directory_fd = ancestor_fd;
          }
        }
        (void) ::close(parent_fd);
  #endif
        if (!error) {
          return replace_result_e::success;
        }
        return error == std::errc::permission_denied ? replace_result_e::access_denied : replace_result_e::error;
#endif
      }

      std::optional<bool> read_only(const std::filesystem::path &path) const override {
#ifdef _WIN32
        const auto attributes = GetFileAttributesW(path.c_str());
        if (attributes == INVALID_FILE_ATTRIBUTES) {
          return std::nullopt;
        }
        return (attributes & FILE_ATTRIBUTE_READONLY) != 0;
#else
        (void) path;
        return false;
#endif
      }

      bool set_read_only(const std::filesystem::path &path, bool value) override {
#ifdef _WIN32
        const auto attributes = GetFileAttributesW(path.c_str());
        if (attributes == INVALID_FILE_ATTRIBUTES) {
          return false;
        }
        const auto updated = value ? attributes | FILE_ATTRIBUTE_READONLY : attributes & ~FILE_ATTRIBUTE_READONLY;
        return SetFileAttributesW(path.c_str(), updated) != 0;
#else
        (void) path;
        (void) value;
        return false;
#endif
      }

      void remove(const std::filesystem::path &path) override {
        std::error_code error;
        std::filesystem::remove(path, error);
      }

      std::filesystem::path temporary_path(const std::filesystem::path &target) override {
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        const auto tid = std::hash<std::thread::id> {}(std::this_thread::get_id());
        auto temporary = target;
        temporary += ".tmp." + std::to_string(stamp) + "." + std::to_string(tid);
        return temporary;
      }
    };

    handler_t &default_handler() {
      static native_filesystem_t filesystem;
      static handler_t handler {filesystem, [](std::string_view message) {
        BOOST_LOG(error) << message;
      }};
      return handler;
    }
  }  // namespace

  std::string get_parent_directory(const std::string &path) {
    return default_handler().get_parent_directory(path);
  }

  bool make_directory(const std::string &path) {
    return default_handler().make_directory(path);
  }

  std::string read_file(const char *path) {
    return default_handler().read_file(path);
  }

  int write_file(const char *path, const std::string_view &contents) {
    return default_handler().write_file(path, contents);
  }
}  // namespace file_handler

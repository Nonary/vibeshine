"""Exercise the production POSIX writer and atomic policy with real files.

Usage: python3 tests/unit/test_file_handler_posix_storage.py [repo] [compiler]
The native adapter is extracted unchanged except for OS-boundary substitutions.
On non-Linux POSIX hosts, __linux__ is enabled only after native headers load;
this executes Linux directory durability policy without claiming Linux runtime QA.
"""

import pathlib
import subprocess
import sys
import tempfile

root = pathlib.Path(sys.argv[1]).resolve() if len(sys.argv) > 1 else pathlib.Path(__file__).resolve().parents[2]
compiler = sys.argv[2] if len(sys.argv) > 2 else "c++"
source = (root / "src/file_handler.cpp").read_text()
start = source.index("class native_filesystem_t:")
brace = source.index("{", start)
depth, end = 1, brace + 1
while depth:
    depth += (source[end] == "{") - (source[end] == "}")
    end += 1
native = source[start:end] + ";"
for name in ("open", "openat", "write", "fsync", "syncfs", "fstat", "close"):
    native = native.replace(f"::{name}(", f"boundary::{name}(")
native = native.replace("std::filesystem::rename(", "boundary::rename(")

program = r'''
#include <algorithm>
#include <cassert>
#include <cerrno>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <map>
#include <string>
#include <system_error>
#include <thread>
#include <vector>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include "src/file_handler.h"
#include "src/file_handler_core.cpp"

enum class Fault { none, file_write, file_sync, file_close, directory_open, rename,
                   directory_sync, directory_close, directory_interrupt,
                   ancestor_open, ancestor_sync, ancestor_close, ancestor_stat,
                   ancestor_denied, ancestor_forbidden, filesystem_sync, filesystem_close,
                   filesystem_interrupt, directory_denied, directory_forbidden, staging_read };
Fault fault = Fault::none;
std::vector<std::string> effects;
std::vector<std::filesystem::path> directories;
std::vector<std::filesystem::path> synced_directories;
std::map<int, std::filesystem::path> descriptor_paths;
std::map<int, int> descriptor_depths;
bool mount_boundary = false;
int directory_syncs = 0;
int filesystem_syncs = 0;
int live_fds = 0;
namespace boundary {
  bool directory(int fd) {
    struct stat info {};
    assert(fstat(fd, &info) == 0);
    return S_ISDIR(info.st_mode);
  }
  int open(const char *path, int flags, mode_t mode = 0) {
    const bool is_directory = flags & O_DIRECTORY;
    const bool is_staging_read = !is_directory && !(flags & O_CREAT);
    effects.push_back(is_directory ? "directory-open" : is_staging_read ? "staging-open" : "file-open");
    if (is_directory) {
      assert(flags & O_CLOEXEC);
      directories.emplace_back(path);
      if (fault == Fault::directory_open) { errno = EMFILE; return -1; }
      if (fault == Fault::directory_denied || fault == Fault::staging_read) { errno = EACCES; return -1; }
      if (fault == Fault::directory_forbidden) { errno = EPERM; return -1; }
    } else if (is_staging_read) {
      assert((flags & (O_NOFOLLOW | O_CLOEXEC)) == (O_NOFOLLOW | O_CLOEXEC));
      assert(mode == 0);
      if (fault == Fault::staging_read) { errno = EACCES; return -1; }
    } else {
      assert((flags & (O_EXCL | O_NOFOLLOW | O_CLOEXEC)) == (O_EXCL | O_NOFOLLOW | O_CLOEXEC));
      assert(mode == (S_IRUSR | S_IWUSR));
    }
    const int fd = ::open(path, flags, mode);
    if (fd >= 0) {
      ++live_fds;
      descriptor_paths[fd] = std::filesystem::canonical(path);
      descriptor_depths[fd] = is_directory ? 0 : is_staging_read ? -2 : -1;
    }
    return fd;
  }
  int openat(int current, const char *name, int flags) {
    effects.push_back("ancestor-open");
    assert(std::string(name) == ".." && (flags & (O_DIRECTORY | O_CLOEXEC)) == (O_DIRECTORY | O_CLOEXEC));
    assert(descriptor_depths.at(current) < 32);  // Root detection must terminate.
    if (fault == Fault::ancestor_open) { errno = EMFILE; return -1; }
    if (fault == Fault::ancestor_forbidden) { errno = EPERM; return -1; }
    if (fault == Fault::ancestor_denied || fault == Fault::filesystem_sync ||
        fault == Fault::filesystem_close || fault == Fault::filesystem_interrupt) { errno = EACCES; return -1; }
    const int fd = ::openat(current, name, flags);
    if (fd >= 0) {
      ++live_fds;
      descriptor_paths[fd] = descriptor_paths.at(current).parent_path();
      if (descriptor_paths[fd].empty()) descriptor_paths[fd] = "/";
      descriptor_depths[fd] = descriptor_depths.at(current) + 1;
      directories.push_back(descriptor_paths.at(fd));
    }
    return fd;
  }
  int fstat(int fd, struct stat *info) {
    if (fault == Fault::ancestor_stat && descriptor_depths.at(fd) > 0) { errno = EIO; return -1; }
    const int result = ::fstat(fd, info);
    if (mount_boundary && descriptor_depths.at(fd) > 0 && result == 0) ++info->st_dev;
    return result;
  }
  ssize_t write(int fd, const void *data, size_t size) {
    effects.push_back("file-write");
    if (fault == Fault::file_write) { errno = EIO; return -1; }
    // Short positive writes are normal and must be completed by production.
    return ::write(fd, data, std::min<size_t>(size, 7));
  }
  int fsync(int fd) {
    const bool is_directory = directory(fd);
    effects.push_back(is_directory ? "directory-sync" : "file-sync");
    if (is_directory) {
      ++directory_syncs;
      synced_directories.push_back(descriptor_paths.at(fd));
      if (fault == Fault::directory_sync) { errno = EIO; return -1; }
      if (fault == Fault::ancestor_sync && descriptor_depths.at(fd) > 0) { errno = EIO; return -1; }
      if (mount_boundary && descriptor_depths.at(fd) > 0) { errno = EROFS; return -1; }
      if (fault == Fault::directory_interrupt && directory_syncs == 1) { errno = EINTR; return -1; }
    } else if (fault == Fault::file_sync) { errno = ENOSPC; return -1; }
    return ::fsync(fd);
  }
  int syncfs(int fd) {
    effects.push_back("filesystem-sync");
    ++filesystem_syncs;
    if (fault == Fault::filesystem_sync) { errno = EIO; return -1; }
    if (fault == Fault::filesystem_interrupt && filesystem_syncs == 1) { errno = EINTR; return -1; }
#ifdef __linux__
    return ::syncfs(fd);
#else
    // macOS has no syncfs; only the native Linux run validates that syscall.
    return ::fsync(fd);
#endif
  }
  int close(int fd) {
    const bool is_directory = directory(fd);
    const int depth = descriptor_depths.at(fd);
    effects.push_back(is_directory ? "directory-close" : "file-close");
    const int result = ::close(fd);
    --live_fds;
    descriptor_paths.erase(fd);
    descriptor_depths.erase(fd);
    if ((is_directory && fault == Fault::directory_close && depth == 0) ||
        (is_directory && fault == Fault::ancestor_close && depth > 0) ||
        ((is_directory || depth == -2) && fault == Fault::filesystem_close) ||
        (!is_directory && fault == Fault::file_close)) { errno = EIO; return -1; }
    return result;
  }
  void rename(const std::filesystem::path &from, const std::filesystem::path &to, std::error_code &error) {
    effects.push_back("rename");
    assert(from.parent_path() == to.parent_path());
    if (fault == Fault::rename) {
      error = std::make_error_code(std::errc::permission_denied);
      return;
    }
    std::filesystem::rename(from, to, error);
  }
}
// Select only the production branch after loading the host's system headers.
#ifdef TEST_LINUX_DURABILITY
  #ifndef __linux__
    #define __linux__ 1
  #endif
#else
  #undef __linux__
#endif
namespace file_handler {
''' + native + r'''
}
std::string bytes(const std::filesystem::path &path) {
  std::ifstream input(path, std::ios::binary);
  return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}
void reset(Fault next = Fault::none) {
  assert(live_fds == 0);
  fault = next;
  effects.clear();
  directories.clear();
  synced_directories.clear();
  directory_syncs = 0;
  filesystem_syncs = 0;
  mount_boundary = false;
  assert(descriptor_paths.empty() && descriptor_depths.empty());
}
void no_staging_files(const std::filesystem::path &directory) {
  for (const auto &entry : std::filesystem::directory_iterator(directory))
    assert(entry.path().filename().string().find(".tmp.") == std::string::npos);
}
size_t position(const char *effect) {
  const auto it = std::find(effects.begin(), effects.end(), effect);
  assert(it != effects.end());
  return static_cast<size_t>(it - effects.begin());
}
void complete_ancestry(const std::filesystem::path &parent) {
  auto expected = std::filesystem::canonical(parent);
  size_t index = 0;
  for (;;) {
    assert(index < synced_directories.size() && synced_directories[index++] == expected);
    const auto next = expected.parent_path();
    if (next == expected) break;
    struct stat current_info {}, next_info {};
    assert(::stat(expected.c_str(), &current_info) == 0 && ::stat(next.c_str(), &next_info) == 0);
    if (current_info.st_dev != next_info.st_dev) break;
    expected = next;
  }
  assert(index == synced_directories.size());
}
int main(int argc, char **argv) {
  assert(argc == 2);
  const std::filesystem::path directory(argv[1]);
  const auto target = directory / "state" / "vibeshine_state.json";
  file_handler::native_filesystem_t filesystem;
  file_handler::handler_t handler(filesystem);
  const std::string original = "{\"root\":{\"linux_display_topology\":\"original pending baseline\"}}";
  const std::string updated = "{\"root\":{\"linux_display_topology\":\"new pending baseline\"}}";
  assert(handler.write_file(target, original) == 0);
  assert(bytes(target) == original && live_fds == 0);
  struct stat mode {};
  assert(stat(target.c_str(), &mode) == 0 && (mode.st_mode & 0777) == 0600);
  assert(position("file-sync") < position("rename"));
#ifdef TEST_LINUX_DURABILITY
  assert(position("file-close") < position("directory-open"));
  assert(position("directory-open") < position("rename"));
  assert(position("rename") < position("directory-sync"));
  assert(position("directory-sync") < position("directory-close"));
  assert(directory_syncs >= 2 && directories.front() == target.parent_path());
  assert(filesystem_syncs == 0);
  complete_ancestry(target.parent_path());
#else
  assert(directories.empty() && directory_syncs == 0);
#endif

  // All failures before rename preserve the previous baseline exactly.
  for (auto failure : {Fault::file_write, Fault::file_sync, Fault::file_close, Fault::rename
#ifdef TEST_LINUX_DURABILITY
                       , Fault::directory_open, Fault::staging_read
#endif
       }) {
    reset(failure);
    assert(handler.write_file(target, updated) == -1);
    assert(bytes(target) == original && live_fds == 0);
    assert(directory_syncs == 0);
    no_staging_files(target.parent_path());
  }
#ifdef TEST_LINUX_DURABILITY
  // A completed rename is visible even if its directory cannot be committed.
  // It must not become a successful persistence acknowledgement or be undone.
  for (auto failure : {Fault::directory_sync, Fault::directory_close, Fault::ancestor_open,
                       Fault::ancestor_sync, Fault::ancestor_close, Fault::ancestor_stat}) {
    reset(failure);
    assert(handler.write_file(target, updated) == -1);
    assert(bytes(target) == updated && directory_syncs >= 1 && live_fds == 0);
    assert(position("rename") < position("directory-sync"));
    no_staging_files(target.parent_path());
    reset();
    assert(handler.write_file(target, original) == 0);
    assert(bytes(target) == original && directory_syncs >= 2 && live_fds == 0);
    complete_ancestry(target.parent_path());
  }
  reset(Fault::directory_interrupt);
  assert(handler.write_file(target, updated) == 0);
  assert(directory_syncs >= 3 && bytes(target) == updated && live_fds == 0);
  assert(synced_directories[0] == synced_directories[1]);
  synced_directories.erase(synced_directories.begin());
  complete_ancestry(target.parent_path());

  // A first nested write can fail after rename. Retrying must still flush the
  // newly existing ancestors instead of mistaking their names for durability.
  reset(Fault::ancestor_sync);
  const auto nested = directory / "fresh" / "two" / "three" / "state.json";
  assert(handler.write_file(nested, original) == -1 && bytes(nested) == original);
  assert(live_fds == 0);
  reset();
  assert(handler.write_file(nested, updated) == 0 && bytes(nested) == updated);
  complete_ancestry(nested.parent_path());

  // A mounted filesystem's outer parent may be read-only. Commit the mount's
  // own directory and stop before attempting to sync a different device.
  reset();
  mount_boundary = true;
  assert(handler.write_file(target, original) == 0);
  assert(directory_syncs == 1 && live_fds == 0);

  for (auto permission_error : {Fault::ancestor_denied, Fault::ancestor_forbidden}) {
    reset(permission_error);
    assert(handler.write_file(target, original) == 0);
    assert(filesystem_syncs == 1 && live_fds == 0);
    assert(position("rename") < position("filesystem-sync"));
  }
  for (auto permission_error : {Fault::directory_denied, Fault::directory_forbidden}) {
    reset(permission_error);
    assert(handler.write_file(target, original) == 0);
    assert(filesystem_syncs == 1 && directory_syncs == 0 && live_fds == 0);
    assert(position("staging-open") < position("rename"));
    assert(position("rename") < position("filesystem-sync"));
  }
  for (auto failure : {Fault::filesystem_sync, Fault::filesystem_close}) {
    reset(failure);
    assert(handler.write_file(target, updated) == -1);
    assert(bytes(target) == updated && filesystem_syncs == 1 && live_fds == 0);
    reset(Fault::ancestor_denied);
    assert(handler.write_file(target, original) == 0 && filesystem_syncs == 1);
  }
  reset(Fault::filesystem_interrupt);
  assert(handler.write_file(target, updated) == 0 && filesystem_syncs == 2 && live_fds == 0);
#endif

  // Plain relative filenames must sync '.', not try to open an empty path.
  reset();
  std::filesystem::current_path(directory);
  assert(handler.write_file("relative.json", "") == 0);
  assert(std::filesystem::exists("relative.json") && bytes("relative.json").empty());
  assert(live_fds == 0);
#ifdef TEST_LINUX_DURABILITY
  assert(directories.front() == ".");
  complete_ancestry(directory);
  reset();
  assert(handler.write_file("new/relative/parents/state.json", original) == 0);
  complete_ancestry(directory / "new/relative/parents");

  // Actual ancestry must follow the symlink target, including path/../child.
  std::filesystem::create_directories(directory / "actual" / "inside");
  std::filesystem::create_directory_symlink(directory / "actual" / "inside", directory / "link");
  reset();
  assert(handler.write_file("link/../child/state.json", original) == 0);
  complete_ancestry(directory / "actual" / "child");
  assert(live_fds == 0);

  // Synchronizing existing ancestor entries does not require write permission
  // on the ancestor itself, and replacing a read-only file still uses its parent.
  const auto read_only_parent = directory / "read-only-ancestor";
  const auto writable_child = read_only_parent / "child";
  std::filesystem::create_directories(writable_child);
  assert(::chmod(read_only_parent.c_str(), 0555) == 0);
  reset();
  assert(handler.write_file(writable_child / "state.json", original) == 0);
  complete_ancestry(writable_child);
  assert(::chmod(read_only_parent.c_str(), 0700) == 0);
  assert(::chmod(target.c_str(), 0444) == 0);
  reset();
  assert(handler.write_file(target, updated) == 0 && bytes(target) == updated);
  complete_ancestry(target.parent_path());

  // A real execute-only ancestor remains supported. Native validation runs
  // unprivileged so this reaches syncfs rather than bypassing mode permissions.
  const auto execute_only_parent = directory / "execute-only-ancestor";
  const auto execute_child = execute_only_parent / "child";
  std::filesystem::create_directories(execute_child);
  assert(::chmod(execute_only_parent.c_str(), 0111) == 0);
  reset();
  assert(handler.write_file(execute_child / "state.json", original) == 0);
  assert(bytes(execute_child / "state.json") == original && live_fds == 0);
  if (::geteuid() != 0) assert(filesystem_syncs == 1);
  assert(::chmod(execute_only_parent.c_str(), 0700) == 0);

  // Preserve writable/searchable leaf directories without read permission.
  const auto unreadable_leaf = directory / "unreadable-leaf";
  std::filesystem::create_directories(unreadable_leaf);
  assert(::chmod(unreadable_leaf.c_str(), 0300) == 0);
  reset();
  assert(handler.write_file(unreadable_leaf / "state.json", original) == 0);
  assert(bytes(unreadable_leaf / "state.json") == original && live_fds == 0);
  if (::geteuid() != 0) {
    assert(filesystem_syncs == 1 && directory_syncs == 0);
    for (auto failure : {Fault::filesystem_sync, Fault::filesystem_close}) {
      reset(failure);
      assert(handler.write_file(unreadable_leaf / "state.json", updated) == -1);
      assert(bytes(unreadable_leaf / "state.json") == updated && live_fds == 0);
      reset();
      assert(handler.write_file(unreadable_leaf / "state.json", original) == 0 && filesystem_syncs == 1);
    }
    reset(Fault::filesystem_interrupt);
    assert(handler.write_file(unreadable_leaf / "state.json", updated) == 0);
    assert(filesystem_syncs == 2 && live_fds == 0);
  }
  assert(::chmod(unreadable_leaf.c_str(), 0700) == 0);
  std::cout << "Production Linux storage: durable ancestry, nested/relative retries, permissions, checked syncfs fallback and injected failures passed\n";
#else
  assert(directories.empty());
  std::cout << "Production non-Linux POSIX storage: existing replacement behavior preserved\n";
#endif
}
'''

with tempfile.TemporaryDirectory(prefix="vibeshine-posix-storage-") as temporary:
    path = pathlib.Path(temporary)
    fixture = path / "storage.cpp"
    fixture.write_text(program)
    for variant in ("linux", "other-posix"):
        binary, data = path / variant, path / f"{variant}-data"
        data.mkdir()
        defines = ["-DTEST_LINUX_DURABILITY"] if variant == "linux" else []
        subprocess.run(
            [compiler, "-std=c++23", "-Wall", "-Wextra", "-Werror", *defines,
             "-I", str(root), str(fixture), "-o", str(binary)], check=True,
        )
        subprocess.run([str(binary), str(data)], check=True)

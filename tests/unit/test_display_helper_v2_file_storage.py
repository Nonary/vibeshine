"""Execute the complete production Windows storage adapter with faultable OS/CRT calls.

Usage: python3 tests/unit/test_display_helper_v2_file_storage.py [repo] [compiler]
Only the Windows/CRT boundary is replaced; all storage control flow comes from
file_text_storage.cpp. Real temporary files verify the retained baseline bytes.
"""

import pathlib
import subprocess
import sys
import tempfile


root = pathlib.Path(sys.argv[1]).resolve() if len(sys.argv) > 1 else pathlib.Path(__file__).resolve().parents[2]
compiler = sys.argv[2] if len(sys.argv) > 2 else "c++"

windows_header = r'''
#pragma once
#include <cstdint>
using DWORD = std::uint32_t;
using BOOL = int;
constexpr DWORD INVALID_FILE_ATTRIBUTES = 0xffffffff;
constexpr DWORD ERROR_FILE_NOT_FOUND = 2, ERROR_PATH_NOT_FOUND = 3;
constexpr DWORD ERROR_ACCESS_DENIED = 5, ERROR_NOT_READY = 21;
constexpr DWORD ERROR_SHARING_VIOLATION = 32, ERROR_INVALID_NAME = 123;
constexpr DWORD MOVEFILE_REPLACE_EXISTING = 1, MOVEFILE_WRITE_THROUGH = 8;
DWORD GetCurrentProcessId();
DWORD GetFileAttributesW(const wchar_t *);
DWORD GetLastError();
BOOL MoveFileExW(const wchar_t *, const wchar_t *, DWORD);
'''

io_header = r'''
#pragma once
#include <cstdio>
FILE *_wfopen(const wchar_t *, const wchar_t *);
int _fileno(FILE *);
int _commit(int);
size_t boundary_write(const void *, size_t, size_t, FILE *);
size_t boundary_read(void *, size_t, size_t, FILE *);
int boundary_flush(FILE *);
int boundary_close(FILE *);
int boundary_error(FILE *);
#define fwrite boundary_write
#define fread boundary_read
#define fflush boundary_flush
#define fclose boundary_close
#define ferror boundary_error
'''

program = r'''
#include <algorithm>
#include <atomic>
#include <cassert>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <string>
#include <vector>
#include <unistd.h>

// Compile the actual implementation, with only platform effects shimmed.
#include "src/platform/windows/display_helper_v2/file_text_storage.cpp"
#undef fwrite
#undef fread
#undef fflush
#undef fclose
#undef ferror

enum class Fault { none, open_write, short_write, flush, commit, close, rename, open_read, partial_read, late_read };
Fault fault = Fault::none;
DWORD attribute_error = 0;
DWORD last_error = 0;
FILE *write_stream = nullptr;
FILE *read_stream = nullptr;
bool injected_read_error = false;
int read_calls = 0;
std::vector<std::string> effects;
std::filesystem::path staged;
const std::string original = "{\"baseline\":\"physical\",\"width\":2560}";
const std::string replacement = "{\"baseline\":\"physical\",\"width\":3840}";

std::string bytes(const std::filesystem::path &path) {
  std::ifstream input(path, std::ios::binary);
  return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}
void reset(Fault next = Fault::none) {
  fault = next;
  attribute_error = 0;
  last_error = 0;
  assert(!write_stream && !read_stream);
  injected_read_error = false;
  read_calls = 0;
  effects.clear();
  staged.clear();
}

FILE *_wfopen(const wchar_t *name, const wchar_t *mode) {
  const std::filesystem::path path(name);
  if (std::wstring(mode) == L"wb") {
    effects.push_back("open");
    staged = path;
    if (fault == Fault::open_write) return nullptr;
    write_stream = std::fopen(path.string().c_str(), "wb");
    return write_stream;
  }
  assert(std::wstring(mode) == L"rb");
  if (fault == Fault::open_read) return nullptr;
  read_stream = std::fopen(path.string().c_str(), "rb");
  return read_stream;
}
size_t boundary_write(const void *data, size_t size, size_t count, FILE *stream) {
  effects.push_back("write");
  assert(stream == write_stream);
  if (fault == Fault::short_write && count) --count;
  return std::fwrite(data, size, count, stream);
}
int boundary_flush(FILE *stream) {
  effects.push_back("flush");
  if (fault == Fault::flush) return EOF;
  return std::fflush(stream);
}
int _fileno(FILE *stream) { return fileno(stream); }
int _commit(int fd) {
  effects.push_back("commit");
  if (fault == Fault::commit) return -1;
  return fsync(fd);
}
int boundary_close(FILE *stream) {
  const bool writing = stream == write_stream;
  if (writing) {
    effects.push_back("close");
    write_stream = nullptr;
  }
  if (stream == read_stream) read_stream = nullptr;
  const int result = std::fclose(stream);
  return writing && fault == Fault::close ? EOF : result;
}
size_t boundary_read(void *data, size_t size, size_t count, FILE *stream) {
  if (fault == Fault::partial_read || fault == Fault::late_read) {
    if (read_calls++) {
      injected_read_error = true;
      return 0;
    }
    if (fault == Fault::partial_read) count = std::min<size_t>(count, 4);
  }
  return std::fread(data, size, count, stream);
}
int boundary_error(FILE *stream) {
  return injected_read_error || std::ferror(stream);
}
DWORD GetCurrentProcessId() { return static_cast<DWORD>(getpid()); }
DWORD GetLastError() { return last_error; }
DWORD GetFileAttributesW(const wchar_t *name) {
  if (attribute_error) {
    last_error = attribute_error;
    return INVALID_FILE_ATTRIBUTES;
  }
  std::error_code error;
  if (std::filesystem::exists(std::filesystem::path(name), error)) return 0;
  last_error = error ? ERROR_ACCESS_DENIED : ERROR_FILE_NOT_FOUND;
  return INVALID_FILE_ATTRIBUTES;
}
BOOL MoveFileExW(const wchar_t *from, const wchar_t *to, DWORD flags) {
  effects.push_back("rename");
  assert(flags == (MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH));
  const std::filesystem::path source(from), destination(to);
  assert(source != destination && source.parent_path() == destination.parent_path());
  assert(!write_stream);  // Flush and close must precede replacement.
  assert((effects == std::vector<std::string> {"open", "write", "flush", "commit", "close", "rename"}));
  if (fault == Fault::rename) return 0;
  std::error_code error;
  std::filesystem::rename(source, destination, error);
  return !error;
}

int main(int argc, char **argv) {
  assert(argc == 2);
  const std::filesystem::path directory(argv[1]);
  const auto baseline = directory / "snapshots" / "current.json";
  display_helper::v2::AtomicFileTextStorage storage;

  // A first write creates the directory and commits in durable order.
  assert(storage.write_atomically(baseline.string(), original));
  assert(bytes(baseline) == original && !std::filesystem::exists(staged));
  assert(storage.read(baseline.string()) == original);
  const auto first_staging = staged;

  // Every failure leaves the last complete baseline intact and cleans staging.
  for (auto failure : {Fault::open_write, Fault::short_write, Fault::flush,
                       Fault::commit, Fault::close, Fault::rename}) {
    reset(failure);
    assert(!storage.write_atomically(baseline.string(), replacement));
    assert(bytes(baseline) == original);
    assert(!staged.empty() && staged != baseline && staged != first_staging);
    assert(!std::filesystem::exists(staged));
    assert(!write_stream);
    if (failure != Fault::rename)
      assert(std::find(effects.begin(), effects.end(), "rename") == effects.end());
  }
  reset();
  assert(storage.write_atomically(baseline.string(), replacement));
  assert(bytes(baseline) == replacement);
  assert(storage.read(baseline.string()) == replacement);

  // A read error must not return partial or even apparently complete JSON.
  for (auto failure : {Fault::open_read, Fault::partial_read, Fault::late_read}) {
    reset(failure);
    assert(!storage.read(baseline.string()));
    assert(storage.exists(baseline.string()));
    assert(bytes(baseline) == replacement && !read_stream);
  }
  reset();
  const std::string multi_buffer_text(12345, 'x');
  assert(storage.write_atomically(baseline.string(), multi_buffer_text));
  assert(storage.read(baseline.string()) == multi_buffer_text);

  // Only authoritative not-found errors erase the recovery obligation.
  for (auto error : {ERROR_ACCESS_DENIED, ERROR_NOT_READY,
                     ERROR_SHARING_VIOLATION, ERROR_INVALID_NAME}) {
    reset();
    attribute_error = error;
    assert(storage.exists(baseline.string()));
  }
  for (auto error : {ERROR_FILE_NOT_FOUND, ERROR_PATH_NOT_FOUND}) {
    reset();
    attribute_error = error;
    assert(!storage.exists((directory / "missing.json").string()));
  }
  reset();
  assert(storage.exists("") && !storage.read(""));
  assert(!storage.write_atomically("", original) && effects.empty());
  assert(storage.exists(directory.string()));  // An invalid directory is not absence.
  assert(!storage.exists((directory / "missing.json").string()));
  assert(!storage.read((directory / "missing.json").string()));

  // A blocked parent path fails without replacing any existing bytes.
  assert(!storage.write_atomically((baseline / "child.json").string(), original));
  assert(bytes(baseline) == multi_buffer_text);
  assert(storage.remove(baseline.string()));
  assert(!storage.exists(baseline.string()));

  // Relative filenames and empty payloads are valid storage operations.
  reset();
  std::filesystem::current_path(directory);
  assert(storage.write_atomically("relative.json", ""));
  assert(storage.read("relative.json") == "");
  assert(storage.remove("relative.json"));
  std::cout << "Production v2 file storage: durable replacement, six write failures, "
               "three read failures, and conservative recovery evidence passed\n";
}
'''

with tempfile.TemporaryDirectory(prefix="vibeshine-storage-") as temporary:
    path = pathlib.Path(temporary)
    (path / "windows.h").write_text(windows_header)
    (path / "io.h").write_text(io_header)
    fixture, binary = path / "storage.cpp", path / "storage"
    fixture.write_text(program)
    subprocess.run(
        [compiler, "-std=c++23", "-Wall", "-Wextra", "-Werror", "-I", str(path),
         "-I", str(root), str(fixture), "-o", str(binary)],
        check=True,
    )
    subprocess.run([str(binary), str(path)], check=True)

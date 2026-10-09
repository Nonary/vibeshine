"""Run production Current path selection and retirement against real files.

Usage: python3 tests/unit/test_display_snapshot_path_binding.py [repo] [compiler]
Only filesystem error boundaries are injected. Selection, legacy retirement,
and v2 path-to-text-storage delegation are extracted unchanged from production.
"""

import pathlib
import subprocess
import sys
import tempfile

root = pathlib.Path(sys.argv[1]).resolve() if len(sys.argv) > 1 else pathlib.Path(__file__).resolve().parents[2]
compiler = sys.argv[2] if len(sys.argv) > 2 else "c++"


def extract(source, signature):
    start = source.index(signature)
    brace = source.index("{", start)
    depth, end = 1, brace + 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end]


paths = (root / "tools/display_helper_paths.h").read_text()
legacy = (root / "tools/display_settings_helper.cpp").read_text()
v2 = (root / "tools/display_settings_helper_v2.cpp").read_text()
snapshot = (root / "src/platform/windows/display_helper_v2/snapshot.cpp").read_text()
files = (root / "src/platform/windows/display_helper_v2/snapshot_file_storage.cpp").read_text()

# Both startup modes must bind the selected path before recovery workers exist.
assert legacy.count("state.session_current_path = retained_current;") == 2
assert "const auto retained_current = display_helper_paths::select_current_snapshot_path(" in legacy
assert "copy_file_overwrite(paths.session_current" not in legacy
assert ".current = display_helper_paths::select_current_snapshot_path(" in v2
assert v2.index(".current = display_helper_paths::select_current_snapshot_path(") < v2.index("FileSnapshotStorage storage(paths)")
history_adoption = extract(v2, "void adopt_previous_snapshot_from_search_roots(")
assert "session_current" not in history_adoption and "active_current" not in history_adoption
assert "adopt_snapshots_from_search_roots(" not in v2
assert legacy.count("if (!ServiceState::path_may_exist(state.session_previous_path))") == 2

selector = extract(paths, "inline std::filesystem::path select_current_snapshot_path(")
selector = selector.replace("std::filesystem::exists", "boundary::exists")
retire = extract(legacy, "static bool retire_snapshot_file(")
retire = retire.replace("std::filesystem::exists", "boundary::exists")
retire = retire.replace("std::filesystem::remove", "boundary::remove")

program = r'''
#include <cassert>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <string>
#include <utility>
#include <vector>
#include "src/platform/windows/display_helper_v2/text_storage.h"
namespace boundary {
  std::filesystem::path stat_error, remove_error;
  std::vector<std::filesystem::path> removed;
  bool exists(const std::filesystem::path &path, std::error_code &error) {
    if (!stat_error.empty() && path == stat_error) {
      error = std::make_error_code(std::errc::permission_denied);
      return false;
    }
    return std::filesystem::exists(path, error);
  }
  bool remove(const std::filesystem::path &path, std::error_code &error) {
    removed.push_back(path);
    if (!remove_error.empty() && path == remove_error) {
      error = std::make_error_code(std::errc::permission_denied);
      return false;
    }
    return std::filesystem::remove(path, error);
  }
}
namespace display_helper_paths {
'''
program += extract(paths, "struct SnapshotPaths {") + ";\n"
program += extract(paths, "inline SnapshotPaths make_snapshot_paths(") + "\n" + selector
program += r'''
}
struct LegacyRetirement {
''' + retire + r'''
};
namespace display_helper::v2 {
  enum class SnapshotTier { Current, Previous, Golden };
  struct SnapshotPaths { std::filesystem::path current, previous, golden; };
  struct SnapshotStorageKeys { std::string current, previous, golden; };
  struct NativeTextStorage : ITextStorage {
    std::optional<std::string> read(const std::string &) override { return std::nullopt; }
    bool write_atomically(const std::string &, const std::string &) override { assert(false); return false; }
    bool exists(const std::string &key) override {
      std::error_code error;
      return boundary::exists(key, error) || error;
    }
    bool remove(const std::string &key) override {
      std::error_code error;
      return boundary::remove(key, error);
    }
  };
  class TextSnapshotStorage {
  public:
    TextSnapshotStorage(SnapshotStorageKeys, ITextStorage &);
    bool remove(SnapshotTier);
    const std::string &key_for(SnapshotTier) const;
  private:
    SnapshotStorageKeys keys_;
    ITextStorage &text_storage_;
  };
  class FileSnapshotStorage {
  public:
    explicit FileSnapshotStorage(SnapshotPaths);
    bool remove(SnapshotTier);
  private:
    NativeTextStorage files_;
    TextSnapshotStorage core_;
  };
'''
for source, signature in [
    (files, "SnapshotStorageKeys keys_from("),
    (snapshot, "TextSnapshotStorage::TextSnapshotStorage("),
    (snapshot, "bool TextSnapshotStorage::remove("),
    (snapshot, "const std::string &TextSnapshotStorage::key_for("),
    (files, "FileSnapshotStorage::FileSnapshotStorage("),
    (files, "bool FileSnapshotStorage::remove("),
]:
    program += extract(source, signature) + "\n"
program += r'''
}
void put(const std::filesystem::path &path, const std::string &text) {
  std::filesystem::create_directories(path.parent_path());
  std::ofstream(path, std::ios::binary) << text;
}
std::string bytes(const std::filesystem::path &path) {
  std::ifstream input(path, std::ios::binary);
  return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}
int main(int argc, char **argv) {
  assert(argc == 2);
  const std::filesystem::path directory(argv[1]);
  const auto active = display_helper_paths::make_snapshot_paths(directory / "active").session_current;
  const std::vector<std::filesystem::path> roots {directory / "first", directory / "second"};
  const auto first = display_helper_paths::make_snapshot_paths(roots[0]).session_current;
  const auto second = display_helper_paths::make_snapshot_paths(roots[1]).session_current;
  const auto select = [&] { return display_helper_paths::select_current_snapshot_path(active, roots); };
  assert(select() == active && !std::filesystem::exists(active));

  put(second, "valid physical snapshot");
  assert(select() == second && !std::filesystem::exists(active));
  put(first, "corrupt snapshot");
  assert(select() == first && bytes(first) == "corrupt snapshot");
  assert(bytes(second) == "valid physical snapshot");
  boundary::stat_error = first;
  assert(select() == first);  // An unreadable first root remains authoritative.
  boundary::stat_error = active;
  assert(select() == active);  // Even if stat cannot prove that it exists.
  boundary::stat_error.clear();
  for (const auto &text : {"valid active", "corrupt active"}) {
    put(active, text);
    assert(select() == active && bytes(active) == text);
    assert(bytes(first) == "corrupt snapshot");
  }
  std::filesystem::remove(active);
  std::filesystem::remove(first);
  std::filesystem::remove(second);

  // Startup/restart creates no copy. Confirmed retirement targets the source
  // in both engines, and a failed delete retains exactly that source for retry.
  for (bool legacy_engine : {true, false}) {
    put(first, "retained physical desktop");
    const auto selected = select();
    assert(selected == first && select() == first);
    assert(!std::filesystem::exists(active));
    display_helper::v2::FileSnapshotStorage storage({selected, directory / "previous", directory / "golden"});
    const auto retire_current = [&] {
      return legacy_engine ? LegacyRetirement::retire_snapshot_file(selected) :
                             storage.remove(display_helper::v2::SnapshotTier::Current);
    };
    boundary::removed.clear();
    boundary::remove_error = first;
    assert(!retire_current());
    assert(bytes(first) == "retained physical desktop" && select() == first);
    assert(boundary::removed == std::vector<std::filesystem::path> {first});
    boundary::remove_error.clear();
    assert(retire_current());
    assert(!std::filesystem::exists(first) && !std::filesystem::exists(active));
    assert(select() == active);  // No copied source remains to replay next boot.
    assert(boundary::removed == (std::vector<std::filesystem::path> {first, first}));
  }

  // Old ambiguous duplicates cannot safely be inferred to share ownership.
  // The selected active marker retires; a different retained root is untouched.
  put(active, "active session");
  put(first, "independent newer baseline");
  assert(select() == active);
  assert(LegacyRetirement::retire_snapshot_file(select()));
  assert(bytes(first) == "independent newer baseline");
  assert(display_helper_paths::select_current_snapshot_path({}, roots).empty());
  std::cout << "Current path binding: source/active/error authority and both engines' exact-file retirement passed\n";
}
'''

with tempfile.TemporaryDirectory(prefix="vibeshine-snapshot-binding-") as temporary:
    path = pathlib.Path(temporary)
    fixture, binary = path / "binding.cpp", path / "binding"
    fixture.write_text(program)
    subprocess.run(
        [compiler, "-std=c++23", "-Wall", "-Wextra", "-Werror", "-I", str(root),
         str(fixture), "-o", str(binary)], check=True,
    )
    subprocess.run([str(binary), str(path)], check=True)

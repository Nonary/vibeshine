"""Exercise the production migration on real temporary extension directories."""
from pathlib import Path
import os
import shutil
import subprocess
import tempfile

repo = Path(__file__).resolve().parents[4]
compiler = shutil.which(os.environ.get("CXX", "g++"))
if not compiler:
    raise SystemExit(77)
with tempfile.TemporaryDirectory(prefix="playnite-duplicate-") as temporary:
    root = Path(temporary)
    cpp = root / "migration.cpp"
    cpp.write_text(r'''
#include "src/platform/windows/playnite_plugin_migration.h"
#include <cassert>
#include <iostream>
using namespace platf::playnite::migration;
void manifest(const fs::path &dir, bool script = false) {
  fs::create_directories(dir);
  std::ofstream(dir / "extension.yaml") << (script ?
    "Id: Sunshine.Playnite.PowerShell\nType: Script\nModule: SunshinePlaynite.psm1\n" :
    "Id: Sunshine.Playnite.Integration\nType: GenericPlugin\nModule: VibeshinePlaynite.dll\n");
  std::ofstream(dir / (script ? "SunshinePlaynite.psm1" : "VibeshinePlaynite.dll")) << "module";
}
int main(int, char **argv) {
  fs::path root = argv[1], active = root / "Roaming/Playnite/Extensions/SunshinePlaynite";
  fs::path old = root / "Local/Playnite/Extensions/SunshinePlaynite";
  manifest(active); manifest(old, true);
  assert(alternate_root(active, active, old) == old);
  assert(alternate_root(old, active, old) == active);
  const auto portable = root / "Portable/Extensions/SunshinePlaynite";
  manifest(portable);
  assert(!alternate_root(portable, active, old));
  assert(!alternate_root(root / "arbitrary-override", active, old));
  std::ofstream(old / "settings.json") << "user settings";
  std::string error;
  assert(disable_duplicate(active, old, error));
  assert(!fs::exists(old / "extension.yaml"));
  assert(fs::exists(old / "extension.yaml.vibeshine-disabled"));
  assert(fs::exists(old / "settings.json") && fs::exists(old / "SunshinePlaynite.psm1"));
  assert(fs::exists(active / "extension.yaml"));
  assert(disable_duplicate(active, old, error)); // already migrated
  manifest(old, true); // repair/reinstall must not overwrite the previous backup
  assert(disable_duplicate(active, old, error));
  assert(fs::exists(old / "extension.yaml.vibeshine-disabled.1"));
  manifest(old); // stale compiled connectors are recognized too
  assert(disable_duplicate(active, old, error));
  manifest(old, true);
  fs::rename(active / "extension.yaml", active / "partial");
  assert(!disable_duplicate(active, old, error)); // replacement not activated
  assert(fs::exists(old / "extension.yaml"));
  fs::rename(active / "partial", active / "extension.yaml");
  assert(disable_duplicate(active, old, error)); // retry succeeds
  assert(disable_duplicate(active, active, error));
  assert(fs::exists(active / "extension.yaml"));
  manifest(old, true);
  for (const auto &text : {
       "Id: Another.Extension\nType: Script\nModule: SunshinePlaynite.psm1\n",
       "Id: Sunshine.Playnite.PowerShell\nType: Script\nModule: ../other.psm1\n",
       "Id: Sunshine.Playnite.PowerShell\nId: Other\nType: Script\nModule: SunshinePlaynite.psm1\n",
       "nested:\n  Id: Sunshine.Playnite.PowerShell\nType: Script\nModule: SunshinePlaynite.psm1\n",
       "Id: Sunshine.Playnite.PowerShell\nType: GenericPlugin\nModule: SunshinePlaynite.psm1\n"}) {
    std::ofstream(old / "extension.yaml") << text;
    assert(disable_duplicate(active, old, error));
    assert(fs::exists(old / "extension.yaml"));
  }
  manifest(old, true);
  assert(disable_duplicate(active, old.parent_path() / ".." / "Extensions" / "SunshinePlaynite", error));
  assert(fs::exists(old / "extension.yaml"));
  std::error_code ec;
  fs::create_directory_symlink(old, root / "linked", ec);
  if (!ec) {
    assert(disable_duplicate(active, root / "linked", error));
    assert(fs::exists(old / "extension.yaml"));
  }
  // A file sharing an expected directory name must never be treated as a plugin.
  std::ofstream(root / "not-directory") << "untouched";
  assert(disable_duplicate(active, root / "not-directory", error));
  // Force a recoverable failure without relying on Unix permissions/root status.
  for (unsigned i = 0; i < 1000; ++i)
    std::ofstream(old / ("extension.yaml.vibeshine-disabled" + (i ? "." + std::to_string(i) : ""))) << "previous backup";
  assert(!disable_duplicate(active, old, error));
  assert(fs::exists(old / "extension.yaml") && fs::exists(active / "extension.yaml"));
  fs::remove(old / "extension.yaml.vibeshine-disabled.999");
  assert(disable_duplicate(active, old, error));
  std::cout << "PASS: two-root migration, partial deployment, retry, preservation, ownership and path safety\n";
}
''')
    executable = root / "migration"
    subprocess.run([compiler, "-std=c++17", "-Wall", "-Wextra", "-I", str(repo), str(cpp), "-o", str(executable)], check=True)
    subprocess.run([str(executable), str(root)], check=True, timeout=10)

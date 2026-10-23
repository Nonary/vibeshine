"""Small real ELF SDK fixtures: no Chromium download or host installation."""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]


class LinuxWebrtcTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)
        self.sdk = self.root / 'sdk'
        (self.sdk / 'include').mkdir(parents=True)
        (self.sdk / 'include/libwebrtc.h').write_text('int rtc_fixture(void);\n')
        (self.sdk / 'lib').mkdir()
        (self.root / 'rtc.c').write_text('int rtc_fixture(void) { return 42; }\n')
        self.run_cmd('cc', '-shared', '-fPIC', str(self.root / 'rtc.c'), '-Wl,-soname,libwebrtc.so.1', '-o', str(self.sdk / 'lib/libwebrtc.so.1.2'))
        (self.sdk / 'lib/libwebrtc.so.1').symlink_to('libwebrtc.so.1.2')
        (self.sdk / 'lib/libwebrtc.so').symlink_to('libwebrtc.so.1')

    def run_cmd(self, *args, **kw):
        return subprocess.run(args, check=True, text=True, capture_output=True, **kw)

    def configure(self, libdir='lib', explicit=False, enabled=True):
        source = self.root / 'project'
        source.mkdir(exist_ok=True)
        (source / 'main.c').write_text('int rtc_fixture(void); int main(void) { return rtc_fixture() != 42; }\n')
        (source / 'CMakeLists.txt').write_text(f'''cmake_minimum_required(VERSION 3.20)
project(Fixture C)
include(GNUInstallDirs)
set(VIBESHINE_PRIVILEGED_LIBEXEC_INSTALL_DIR "libexec/vibeshine")
set(SUNSHINE_ENABLE_WEBRTC {'ON' if enabled else 'OFF'})
include("{ROOT}/cmake/dependencies/webrtc.cmake")
add_executable(sunshine main.c)
target_link_libraries(sunshine PRIVATE ${{WEBRTC_LIBRARIES}})
set_property(TARGET sunshine PROPERTY INSTALL_RPATH "$ORIGIN/existing")
install(TARGETS sunshine DESTINATION "${{CMAKE_INSTALL_BINDIR}}")
include("{ROOT}/cmake/packaging/linux_webrtc.cmake")
if(SUNSHINE_ENABLE_WEBRTC AND WEBRTC_RUNTIME_LIB)
    vibeshine_install_webrtc_host()
endif()
file(WRITE "${{CMAKE_BINARY_DIR}}/selected" "${{WEBRTC_LIBRARY}}")
''')
        build = self.root / ('build' + libdir.replace('/', '_'))
        args = ['cmake', '-S', str(source), '-B', str(build), '-DCMAKE_INSTALL_PREFIX=/usr', f'-DCMAKE_INSTALL_LIBDIR={libdir}', f'-DWEBRTC_ROOT={self.sdk}', '-DWEBRTC_INCLUDE_DIR=', '-DWEBRTC_LIBRARY=' + (str(self.sdk / 'lib/libwebrtc.so.1.2') if explicit else '')]
        self.run_cmd(*args)
        self.run_cmd(*args)  # repeat configure with empty cache overrides
        return build

    def test_versioned_sdk_relocates_with_soname_and_preserved_rpath(self):
        for libdir in ('lib', 'lib64', 'lib/x86_64-linux-gnu', '/usr/lib/custom'):
            with self.subTest(libdir=libdir):
                build = self.configure(libdir, explicit=True)
                self.run_cmd('cmake', '--build', str(build))
                stage = self.root / ('stage' + libdir.replace('/', '_'))
                self.run_cmd('cmake', '--install', str(build), env=dict(os.environ, DESTDIR=str(stage)))
                destination = stage / (libdir.lstrip('/') if libdir.startswith('/') else 'usr/' + libdir)
                self.assertTrue((destination / 'libwebrtc.so.1').is_symlink())
                self.assertTrue((destination / 'libwebrtc.so.1.2').is_file())
                # Hide SDK so only the installed tree can satisfy the SONAME.
                self.sdk.rename(self.root / 'hidden')
                try:
                    self.run_cmd(str(stage / 'usr/bin/sunshine'), env=dict(os.environ, LD_LIBRARY_PATH=''))
                    host = stage / 'usr/libexec/vibeshine/vibeshine-host'
                    self.run_cmd(str(host), env=dict(os.environ, LD_LIBRARY_PATH=''))
                    self.assertEqual(host.read_bytes(), (stage / 'usr/bin/sunshine').read_bytes())
                    self.assertNotEqual(host.stat().st_ino, (stage / 'usr/bin/sunshine').stat().st_ino)
                    dynamic = self.run_cmd('readelf', '-d', str(stage / 'usr/bin/sunshine')).stdout
                    self.assertIn('$ORIGIN/existing', dynamic)
                finally:
                    (self.root / 'hidden').rename(self.sdk)

    def test_empty_cache_discovers_library(self):
        build = self.configure()
        self.assertEqual((build / 'selected').read_text(), str(self.sdk / 'lib/libwebrtc.so'))

    def test_unversioned_shared_sdk_is_staged(self):
        for path in (self.sdk / 'lib').iterdir():
            path.unlink()
        self.run_cmd('cc', '-shared', '-fPIC', str(self.root / 'rtc.c'),
                     '-o', str(self.sdk / 'lib/libwebrtc.so'))
        build = self.configure()
        self.run_cmd('cmake', '--build', str(build))
        stage = self.root / 'stage'
        self.run_cmd('cmake', '--install', str(build), env=dict(os.environ, DESTDIR=str(stage)))
        self.sdk.rename(self.root / 'hidden')
        self.run_cmd(str(stage / 'usr/bin/sunshine'), env=dict(os.environ, LD_LIBRARY_PATH=''))
        self.run_cmd(str(stage / 'usr/libexec/vibeshine/vibeshine-host'),
                     env=dict(os.environ, LD_LIBRARY_PATH=''))

    def test_cache_defaults_and_explicit_root_priority(self):
        script = self.root / 'discover.cmake'
        script.write_text(f'''set(CMAKE_SYSTEM_NAME Linux)
set(SUNSHINE_ENABLE_WEBRTC ON)
include("{ROOT}/cmake/dependencies/webrtc.cmake")
file(WRITE "${{RESULT}}" "${{WEBRTC_ROOT}}")
''')
        for variable, suffix in (('VIBESHINE_DEPS_DIR', 'libwebrtc/out'),
                                 ('XDG_CACHE_HOME', 'Vibeshine/deps/libwebrtc/out'),
                                 ('HOME', '.cache/Vibeshine/deps/libwebrtc/out')):
            with self.subTest(variable=variable):
                cache = self.root / variable
                cached_sdk = cache / suffix
                shutil.copytree(self.sdk, cached_sdk, symlinks=True)
                env = {key: value for key, value in os.environ.items()
                       if key not in ('VIBESHINE_DEPS_DIR', 'XDG_CACHE_HOME', 'HOME')}
                env[variable] = str(cache)
                result = self.root / 'result'
                command = ['cmake', f'-DRESULT={result}', '-P', str(script)]
                self.run_cmd(*command, env=env)
                self.assertEqual(result.read_text(), str(cached_sdk))
                self.run_cmd('cmake', f'-DWEBRTC_ROOT={self.sdk}', *command[1:], env=env)
                self.assertEqual(result.read_text(), str(self.sdk))

    def test_static_sdk_does_not_install_a_runtime_library(self):
        for path in (self.sdk / 'lib').iterdir():
            path.unlink()
        self.run_cmd('cc', '-c', str(self.root / 'rtc.c'), '-o', str(self.root / 'rtc.o'))
        self.run_cmd('ar', 'rcs', str(self.sdk / 'lib/libwebrtc.a'), str(self.root / 'rtc.o'))
        build = self.configure()
        self.run_cmd('cmake', '--build', str(build))
        stage = self.root / 'stage'
        self.run_cmd('cmake', '--install', str(build), env=dict(os.environ, DESTDIR=str(stage)))
        self.assertFalse((stage / 'usr/lib').exists())
        self.run_cmd(str(stage / 'usr/bin/sunshine'))

    def test_steamos_keeps_bundle_rpath_and_dependency_scan(self):
        source = self.root / 'steam-project'
        source.mkdir()
        for name in ('packaging', 'LICENSE', 'NOTICE'):
            (source / name).symlink_to(ROOT / name)
        (source / 'main.c').write_text('int rtc_fixture(void); int main(void) { return rtc_fixture() != 42; }\n')
        (source / 'CMakeLists.txt').write_text(f'''cmake_minimum_required(VERSION 3.20)
project(Fixture C)
include(GNUInstallDirs)
set(SUNSHINE_ENABLE_WEBRTC ON)
set(SUNSHINE_BUILD_STEAMOS ON)
set(SUNSHINE_SOURCE_ASSETS_DIR "{ROOT}/src_assets")
set(SUNSHINE_ASSETS_DIR share/vibeshine)
include("{ROOT}/cmake/dependencies/webrtc.cmake")
add_executable(sunshine main.c)
target_link_libraries(sunshine PRIVATE ${{WEBRTC_LIBRARIES}})
include("{ROOT}/cmake/packaging/unix.cmake")
include("{ROOT}/cmake/packaging/linux.cmake")
get_target_property(rpath sunshine INSTALL_RPATH)
if(NOT rpath STREQUAL "$ORIGIN/../lib")
    message(FATAL_ERROR "SteamOS RPATH changed: ${{rpath}}")
endif()
''')
        build = self.root / 'steam-build'
        self.run_cmd('cmake', '-S', str(source), '-B', str(build),
                     '-DCMAKE_INSTALL_PREFIX=/usr', '-DCMAKE_INSTALL_LIBDIR=lib64',
                     f'-DWEBRTC_ROOT={self.sdk}')
        self.run_cmd('cmake', '--build', str(build))
        stage = self.root / 'steam-stage'
        self.run_cmd('cmake', '--install', str(build), env=dict(os.environ, DESTDIR=str(stage)))
        self.assertTrue((stage / 'usr/lib/libwebrtc.so.1').exists())
        self.assertFalse((stage / 'usr/libexec').exists())
        self.sdk.rename(self.root / 'hidden')
        self.run_cmd(str(stage / 'usr/bin/sunshine'), env=dict(os.environ, LD_LIBRARY_PATH=''))

    def test_disabled_does_not_discover(self):
        build = self.configure(enabled=False)
        self.assertEqual((build / 'selected').read_text(), '')

    def test_script_stages_and_retains_caller_workspace(self):
        repo = self.root / 'repo'
        (repo / 'scripts').mkdir(parents=True)
        shutil.copy(ROOT / 'scripts/build_linux_webrtc.sh', repo / 'scripts/build_linux_webrtc.sh')
        wrapper = repo / 'third-party/libwebrtc'
        (wrapper / 'include').mkdir(parents=True)
        (wrapper / 'BUILD.gn').write_text('# fixture\n')
        (wrapper / 'include/libwebrtc.h').write_text('fixture\n')
        work = self.root / 'work'
        (work / 'src/buildtools/linux64').mkdir(parents=True)
        (work / 'src/third_party/ninja').mkdir(parents=True)
        (work / 'depot_tools/.git').mkdir(parents=True)
        (work / 'src/BUILD.gn').write_text('deps = [ ":webrtc" ]\n')
        (work / 'unrelated').write_text('keep')
        for path, body in [('src/buildtools/linux64/gn', 'mkdir -p "$2"'), ('src/third_party/ninja/ninja', 'printf fixture > "$2/libwebrtc.so"')]:
            tool = work / path
            tool.write_text('#!/bin/bash\nset -eu\n' + body + '\n')
            tool.chmod(0o755)
        env = dict(os.environ, WEBRTC_BUILD_DIR=str(work), WEBRTC_OUT_DIR=str(self.root / 'out'))
        self.run_cmd('bash', str(repo / 'scripts/build_linux_webrtc.sh'), '--stage=build', env=env)
        self.assertEqual((self.root / 'out/lib/libwebrtc.so').read_text(), 'fixture')
        self.assertTrue((work / 'unrelated').is_file())
        for unsafe in (repo, self.root, self.root / 'out'):
            result = subprocess.run(['bash', str(repo / 'scripts/build_linux_webrtc.sh'), '--stage=build'], env=dict(env, WEBRTC_BUILD_DIR=str(unsafe)), capture_output=True)
            self.assertNotEqual(result.returncode, 0)


if __name__ == '__main__':
    unittest.main()

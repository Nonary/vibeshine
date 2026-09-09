#!/usr/bin/env python3
"""Exercise the real Lossless Scaling launch function with Win32 call doubles.

These checks validate launch identity, desktop, environment and failure paths;
they do not replace a native Windows launch/focus smoke test.
"""
import pathlib
import shutil
import subprocess
import tempfile


ROOT = pathlib.Path(__file__).resolve().parents[2]


def main():
    source = (ROOT / "tools/playnite_launcher/lossless_scaling.cpp").read_text()
    start = source.index("  bool launch_lossless_executable(")
    end = source.index("\n  bool lossless_scaling_apply_global_profile", start)
    function = source[start:end]
    harness = r'''
#include <cassert>
#include <filesystem>
#include <iostream>
#include <memory>
#include <string>
#include <utility>
#include <vector>
using DWORD = unsigned long;
using BOOL = int;
using LPVOID = void*;
constexpr BOOL FALSE = 0;
constexpr DWORD ERROR_SUCCESS = 0, ERROR_NO_TOKEN = 1008;
constexpr DWORD STARTF_USESHOWWINDOW = 1, SW_SHOWNORMAL = 1;
constexpr DWORD CREATE_UNICODE_ENVIRONMENT = 0x400;
struct STARTUPINFOW {
  unsigned long cb; DWORD dwFlags{}; unsigned short wShowWindow{};
  wchar_t* lpDesktop{};
};
struct PROCESS_INFORMATION { void* hProcess{}; void* hThread{}; DWORD dwProcessId{}; };
struct State {
  bool system = true, token = true, environment = true, impersonation = true;
  bool user_launch = true, regular_launch = true;
  int as_user_calls = 0, regular_calls = 0, reverted = 0, focused = 0;
  DWORD error = ERROR_SUCCESS;
  std::wstring desktop, directory;
  void* passed_environment = nullptr;
} state;
int marker;
DWORD GetLastError() { return state.error; }
void SetLastError(DWORD value) { state.error = value; }
void DebugBreak() { assert(false); }
BOOL CloseHandle(void*) { return 1; }
BOOL CreateEnvironmentBlock(void** env, void*, BOOL) {
  if (!state.environment) { SetLastError(8); return 0; }
  *env = &marker; return 1;
}
BOOL DestroyEnvironmentBlock(void*) { return 1; }
BOOL ImpersonateLoggedOnUser(void*) {
  if (!state.impersonation) SetLastError(5);
  return state.impersonation;
}
BOOL RevertToSelf() { ++state.reverted; return 1; }
struct Log { template<class T> Log& operator<<(const T&) { return *this; } };
#define BOOST_LOG(level) Log{}
namespace winrt {
  struct handle {
    void* value;
    explicit operator bool() const { return value != nullptr; }
    void* get() const { return value; }
  };
}
namespace platf::dxgi {
  bool is_running_as_system() { return state.system; }
  void* retrieve_users_token(bool) { return state.token ? &marker : nullptr; }
}
namespace util {
  template<class F> struct Guard { F function; ~Guard() { function(); } };
  template<class F> auto fail_guard(F function) { return Guard<F>{function}; }
}
BOOL launch(bool user, DWORD flags, void* environment, const wchar_t* directory,
            STARTUPINFOW* startup, PROCESS_INFORMATION* process) {
  assert(flags & CREATE_UNICODE_ENVIRONMENT);
  state.desktop = startup->lpDesktop ? startup->lpDesktop : L"";
  state.directory = directory ? directory : L"";
  state.passed_environment = environment;
  bool result = user ? state.user_launch : state.regular_launch;
  if (result) { process->hProcess = &marker; process->hThread = &marker; process->dwProcessId = 42; }
  else SetLastError(740);
  return result;
}
BOOL CreateProcessAsUserW(void*, const wchar_t*, wchar_t*, void*, void*, BOOL,
                          DWORD flags, void* env, const wchar_t* dir,
                          STARTUPINFOW* si, PROCESS_INFORMATION* pi) {
  ++state.as_user_calls;
  return launch(true, flags, env, dir, si, pi);
}
BOOL CreateProcessW(const wchar_t*, wchar_t*, void*, void*, BOOL, DWORD flags,
                    void* env, const wchar_t* dir, STARTUPINFOW* si, PROCESS_INFORMATION* pi) {
  ++state.regular_calls;
  return launch(false, flags, env, dir, si, pi);
}
std::pair<bool, bool> focus_and_minimize_new_process(PROCESS_INFORMATION& pi, DWORD, bool minimize) {
  ++state.focused; pi.hProcess = nullptr; pi.hThread = nullptr;
  return {true, minimize};
}
''' + function + r'''
int main() {
  const std::wstring exe = L"/games/Lossless Scaling/LosslessScaling.exe";
  state = {};
  assert(launch_lossless_executable(exe, 10, true));
  assert(state.as_user_calls == 1 && state.regular_calls == 0);
  assert(state.desktop == LR"(winsta0\default)");
  assert(state.directory == L"/games/Lossless Scaling");
  assert(state.passed_environment == &marker && state.reverted == 1);

  state = {}; state.system = false;
  assert(launch_lossless_executable(exe, 10, false));
  assert(state.as_user_calls == 0 && state.regular_calls == 1);
  assert(state.directory == L"/games/Lossless Scaling");

  state = {}; state.token = false;
  assert(!launch_lossless_executable(exe, 10, true));
  assert(state.as_user_calls == 0 && state.regular_calls == 0);

  state = {}; state.environment = false;
  assert(!launch_lossless_executable(exe, 10, true));
  assert(state.as_user_calls == 0 && state.regular_calls == 0);

  state = {}; state.impersonation = false;
  assert(!launch_lossless_executable(exe, 10, true));
  assert(state.as_user_calls == 0 && state.regular_calls == 0);

  state = {}; state.user_launch = false;
  assert(!launch_lossless_executable(exe, 10, true));
  assert(state.as_user_calls == 1 && state.regular_calls == 0 && state.reverted == 1);

  state = {}; state.system = false; state.regular_launch = false;
  assert(!launch_lossless_executable(exe, 10, true));
  assert(state.focused == 0);

  state = {};
  assert(!launch_lossless_executable(L"", 10, true));
  assert(state.as_user_calls == 0 && state.regular_calls == 0);
  std::cout << "PASS: 8 Lossless Scaling launch-context cases\n";
}
'''
    compiler = shutil.which("clang++") or shutil.which("g++")
    if not compiler:
        raise RuntimeError("A C++ compiler is required")
    with tempfile.TemporaryDirectory(prefix="vibeshine-lossless-launch-") as temporary:
        directory = pathlib.Path(temporary)
        cpp = directory / "launch.cpp"
        cpp.write_text(harness)
        binary = directory / "launch-test"
        subprocess.run([compiler, "-std=c++20", "-fsanitize=address,undefined",
                        "-fno-omit-frame-pointer", str(cpp), "-o", str(binary)], check=True)
        subprocess.run([str(binary)], check=True)


if __name__ == "__main__":
    main()

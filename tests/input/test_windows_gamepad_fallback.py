#!/usr/bin/env python3
"""Compile the real Windows allocation path against failing/succeeding drivers.

No Windows SDK is needed. --source accepts an older input.cpp to demonstrate
that automatic PlayStation/Nintendo/capability selections used to reject fallback.
"""
import argparse
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]


def block(source, signature):
    start = source.index(signature)
    end = source.index('{', start) + 1
    depth = 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]


PRELUDE = r'''
#include <array>
#include <cstdint>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <string_view>
#include <moonlight-common-c/src/Limelight.h>
using namespace std::literals;
std::ostringstream logOutput;
#define BOOST_LOG(level) logOutput
constexpr int MAX_GAMEPADS = 16;
namespace config {
struct {
  std::string gamepad;
  bool motion_as_ds4 = true;
  bool touchpad_as_ds4 = true;
} input;
}
'''

BACKENDS = r'''
enum VIGEM_TARGET_TYPE { Xbox360Wired, DualShock4Wired };
enum class gamepad_backend_e { none, vhf, vigem };
using feedback_queue_t = std::shared_ptr<int>;
struct vhf_t {
  int result = -1, calls = 0;
  vhf_profile_e requested = vhf_profile_e::automatic;
  int alloc(const gamepad_id_t&, feedback_queue_t&, vhf_profile_e desired) {
    ++calls; requested = desired; return result;
  }
};
struct vigem_t {
  int result = 0, calls = 0;
  VIGEM_TARGET_TYPE selected = Xbox360Wired;
  int alloc_gamepad_internal(const gamepad_id_t&, feedback_queue_t&, VIGEM_TARGET_TYPE type) {
    ++calls; selected = type; return result;
  }
};
struct input_raw_t {
  vhf_t* vhf;
  vigem_t* vigem;
  std::array<gamepad_backend_e, MAX_GAMEPADS> gamepad_backend {};
};
using input_t = std::shared_ptr<input_raw_t>;
int cases = 0, failures = 0;
void check(bool condition, const char* message) {
  ++cases;
  if (!condition) { ++failures; std::cerr << "FAIL: " << message << '\n'; }
}
'''

TESTS = r'''
int main() {
  const gamepad_id_t id {2, 0};
  auto feedback = std::make_shared<int>(7);
  struct inferred_case {
    uint8_t type; uint16_t caps; vhf_profile_e requested; VIGEM_TARGET_TYPE fallback;
  };
  for (const auto& test : {
      inferred_case {LI_CTYPE_PS, 0, vhf_profile_e::dualsense, DualShock4Wired},
      inferred_case {LI_CTYPE_NINTENDO, 0, vhf_profile_e::switch_pro, Xbox360Wired},
      inferred_case {LI_CTYPE_UNKNOWN, LI_CCAP_GYRO, vhf_profile_e::dualsense, DualShock4Wired},
      inferred_case {LI_CTYPE_UNKNOWN, LI_CCAP_TOUCHPAD, vhf_profile_e::dualsense, DualShock4Wired},
      inferred_case {LI_CTYPE_XBOX, 0, vhf_profile_e::automatic, Xbox360Wired},
      inferred_case {LI_CTYPE_UNKNOWN, 0, vhf_profile_e::automatic, Xbox360Wired}}) {
    for (bool missingVhf : {false, true}) {
      config::input.gamepad = "vhf";
      vhf_t vhf;
      vigem_t vigem;
      auto input = std::make_shared<input_raw_t>();
      input->vhf = missingVhf ? nullptr : &vhf;
      input->vigem = &vigem;
      const int result = alloc_gamepad(input, id, {test.type, test.caps, 0}, feedback);
      check(result == 0 && vigem.calls == 1 && vigem.selected == test.fallback &&
              input->gamepad_backend[id.globalIndex] == gamepad_backend_e::vigem,
            "automatic controller survives unavailable VHF through the matching ViGEm profile");
      check(missingVhf || (vhf.calls == 1 && vhf.requested == test.requested),
            "automatic VHF first requests the client-appropriate controller");

      input->vigem = nullptr;
      input->gamepad_backend[id.globalIndex] = gamepad_backend_e::none;
      check(alloc_gamepad(input, id, {test.type, test.caps, 0}, feedback) == -1 &&
              input->gamepad_backend[id.globalIndex] == gamepad_backend_e::none,
            "automatic controller reports failure when neither backend is available");
    }
  }
  for (auto selection : {"vhf_xbox", "vhf_xbox_one", "vhf_ds4", "vhf_ds5", "vhf_switch"}) {
    for (bool missingVhf : {false, true}) {
      config::input.gamepad = selection;
      vhf_t vhf;
      vigem_t vigem;
      auto input = std::make_shared<input_raw_t>();
      input->vhf = missingVhf ? nullptr : &vhf;
      input->vigem = &vigem;
      check(alloc_gamepad(input, id, {LI_CTYPE_PS, 0, 0}, feedback) == -1 &&
              vigem.calls == 0 && input->gamepad_backend[id.globalIndex] == gamepad_backend_e::none,
            "unavailable explicit profiles fail without silently substituting ViGEm");
    }
  }
  {
    config::input.gamepad = "vhf";
    vhf_t vhf;
    vigem_t vigem;
    auto input = std::make_shared<input_raw_t>();
    input->vhf = &vhf;
    input->vigem = &vigem;
    vhf.result = 0;
    check(alloc_gamepad(input, id, {LI_CTYPE_PS, 0, 0}, feedback) == 0 &&
            vhf.calls == 1 && vigem.calls == 0 &&
            input->gamepad_backend[id.globalIndex] == gamepad_backend_e::vhf,
          "successful automatic VHF allocation keeps VHF ownership");
    input->gamepad_backend[id.globalIndex] = gamepad_backend_e::none;
    vhf.result = -1;
    vigem.result = -7;
    check(alloc_gamepad(input, id, {LI_CTYPE_PS, 0, 0}, feedback) == -7 &&
            input->gamepad_backend[id.globalIndex] == gamepad_backend_e::none,
          "fallback failure propagates without claiming a nonexistent controller");
  }
  std::cout << cases - failures << '/' << cases << " gamepad allocation checks passed\n";
  return failures ? 1 : 0;
}
'''


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', type=Path, default=ROOT / 'src/platform/windows/input.cpp')
    args = parser.parse_args()
    source = args.source.read_text()
    common = (ROOT / 'src/platform/common.h').read_text()
    vhf = (ROOT / 'src/platform/windows/vhf_gamepad.h').read_text()
    declarations = '\n'.join((block(common, 'struct gamepad_id_t') + ';',
                              block(common, 'struct gamepad_arrival_t') + ';',
                              block(vhf, 'enum class vhf_profile_e') + ';'))
    handlers = '\n'.join(block(source, signature) for signature in (
        'static bool vhf_gamepad_selected()',
        'static vhf_profile_e vhf_desired_profile(',
        'int alloc_gamepad(input_t &input,',
    ))
    with tempfile.TemporaryDirectory(prefix='vhf-fallback-test-') as directory:
        cpp = Path(directory) / 'test.cpp'
        binary = Path(directory) / 'test'
        cpp.write_text(PRELUDE + declarations + BACKENDS + handlers + TESTS)
        subprocess.run(['c++', '-std=c++20', '-Wall', '-Wextra', '-Werror',
                        '-fsanitize=undefined', '-I', str(ROOT / 'third-party'),
                        str(cpp), '-o', str(binary)], check=True)
        return subprocess.run([str(binary)], check=False).returncode


if __name__ == '__main__':
    raise SystemExit(main())

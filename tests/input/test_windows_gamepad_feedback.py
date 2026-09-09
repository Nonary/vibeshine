#!/usr/bin/env python3
"""Run the VHF driver's portable report/feedback tests without the Windows SDK.

Only GUID and IOCTL declarations are substituted; production report encoders,
decoders, state accumulation, and the descriptor test suite are compiled intact.
"""
import argparse
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', type=Path, default=ROOT / 'third-party/libvirtualgamepad')
    parser.add_argument('--compiler', default='c++')
    args = parser.parse_args()
    source = args.source.resolve()
    units = ['pid_ff', 'profile', 'dualshock4', 'dualsense', 'switch_pro',
             'xbox_series', 'xbox_one', 'report_pump']
    with tempfile.TemporaryDirectory(prefix='vhf-feedback-test-') as directory:
        temp = Path(directory)
        (temp / 'windows.h').write_text('''#pragma once
#include <cstdint>
using DWORD = std::uint32_t;
struct GUID {
  std::uint32_t Data1;
  std::uint16_t Data2, Data3;
  std::uint8_t Data4[8];
};
''')
        (temp / 'winioctl.h').write_text('''#pragma once
#define FILE_DEVICE_UNKNOWN 0x22
#define METHOD_BUFFERED 0
#define FILE_READ_DATA 1
#define FILE_WRITE_DATA 2
#define CTL_CODE(device, function, method, access) \\
  (((device) << 16) | ((access) << 14) | ((function) << 2) | (method))
''')
        binary = temp / 'test'
        subprocess.run([
            args.compiler, '-std=c++20', '-Wall', '-Wextra', '-Werror',
            '-fsanitize=undefined', '-I', str(temp),
            '-I', str(source / 'driver/src'), '-I', str(source / 'include'),
            str(source / 'driver/tests/test_pid_descriptor.cpp'),
            *(str(source / f'driver/src/{unit}.cpp') for unit in units),
            '-o', str(binary),
        ], check=True)
        return subprocess.run([str(binary)], check=False).returncode


if __name__ == '__main__':
    raise SystemExit(main())

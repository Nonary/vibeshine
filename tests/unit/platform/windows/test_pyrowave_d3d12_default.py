#!/usr/bin/env python3
"""Pin the D3D12 conversion as the default PyroWave Windows path.

Source-text checks only: nothing here executes Windows, COM, D3D or GPU APIs.
They fail if the default flips back to opt-in, the compute queue stops asking
for HIGH priority (or starts asking for realtime), or the session-start
fallback reporting disappears. Native behavior stays a hardware gate.
"""
from pathlib import Path
import re
import sys


def read(root, relative):
    return (root / relative).read_text()


def main():
    root = Path(sys.argv[1])
    failures = []

    def require(condition, message):
        if not condition:
            failures.append(message)

    options = read(root, 'cmake/prep/options.cmake')
    option = re.search(r'option\(SUNSHINE_PYROWAVE_D3D12_CONVERSION\s+"[^"]*"\s+(ON|OFF)\)', options)
    require(option and option.group(1) == 'ON', 'SUNSHINE_PYROWAVE_D3D12_CONVERSION must default ON')
    require('EXPERIMENTAL_PYROWAVE' not in options, 'the experimental option name must be gone')

    queue = read(root, 'src/platform/windows/pyrowave_d3d12_conversion.cpp')
    require('D3D12_COMMAND_QUEUE_PRIORITY_HIGH' in queue, 'the conversion queue must request HIGH priority')
    require('D3D12_COMMAND_QUEUE_PRIORITY_NORMAL' in queue, 'a NORMAL queue must remain the fallback')
    require(not re.search(r'Priority\s*=\s*D3D12_COMMAND_QUEUE_PRIORITY_GLOBAL_REALTIME', queue),
            'the conversion queue must never request realtime priority')
    require('using normal priority' in queue, 'the NORMAL fallback must be logged')
    require('prototype' not in queue.lower(), 'the conversion backend must not call itself a prototype')

    encode = read(root, 'src/platform/windows/pyrowave_encode.cpp')
    require('config.d3d12_conversion = !params.rtx_hdr && !force_d3d11;' in encode,
            'sessions must request the D3D12 conversion unless RTX HDR or the override says otherwise')
    require('VIBESHINE_PYROWAVE_CONVERSION' in encode and '"d3d11"' in encode,
            'the VIBESHINE_PYROWAVE_CONVERSION=d3d11 override must exist')
    require('is set; the D3D12 conversion is disabled' in encode, 'the override must be logged as a warning')

    core = read(root, 'src/platform/windows/pyrowave_d3d11_core.cpp')
    require('D3D12 conversion is unavailable on this adapter' in core
            and 'impl.warning(' in core.split('D3D12 conversion is unavailable on this adapter')[0].rsplit('\n', 1)[-1],
            'an unavailable D3D12 backend must be a session-start warning')
    require('experimental' not in core.lower(), 'core messages must not call the D3D12 conversion experimental')

    header = read(root, 'src/platform/windows/pyrowave_d3d11_core.h')
    require('bool d3d12_conversion = false;' in header, 'the core config field must be d3d12_conversion')

    if failures:
        for failure in failures:
            print('FAIL: ' + failure)
        return 1
    print('PyroWave D3D12 conversion default checks passed')
    return 0


if __name__ == '__main__':
    sys.exit(main())

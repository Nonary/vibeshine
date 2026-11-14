#!/usr/bin/env python3
"""Compile Linux blank-source routing with deterministic GPU boundary adapters.

Uses the production display, video dispatch, CUDA probe wrapper, capture loop
and VAAPI conversion body. The adapters do not establish real GPU encoding.
"""
import argparse
from pathlib import Path
import subprocess
import tempfile


def block(source, marker):
    start = source.index(marker)
    opening = source.index('{', start)
    depth = 1
    end = opening + 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]


def fixture(root):
    common = (root / 'src/platform/common.h').read_text()
    video = (root / 'src/video.cpp').read_text()
    cuda = (root / 'src/platform/linux/cuda.cpp').read_text()
    vaapi = (root / 'src/platform/linux/vaapi.cpp').read_text()
    synthetic = (root / 'src/platform/linux/synthetic_display.cpp').read_text()
    display = synthetic[synthetic.index('namespace platf {'):]
    linux = video[video.index('#elif defined(__linux__)\n'):]
    dispatch = block(linux, '  std::shared_ptr<platf::display_t> make_black_display(')
    # Bind the tested dispatch to the real selected-encoder call site too.
    assert 'disp = make_black_display(encoder.platform_formats->dev_type, capture_config);' in video
    probe = block(cuda, '  std::shared_ptr<platf::display_t> make_nvenc_probe_display(')
    converter = block(vaapi[vaapi.index('  class va_vram_t:'):], '    int convert(platf::img_t &img) override')
    template = Path(__file__).with_suffix('.cpp.in').read_text()
    return (template.replace('@ENUMS@', block(common, '  enum class mem_type_e') + ';\n' + block(common, '  enum class pix_fmt_e') + ';')
            .replace('@IMAGE@', block(common, '  struct img_t:') + ';')
            .replace('@CAPTURE@', block(common, '    capture_e capture_synthetic_black('))
            .replace('@DISPLAY@', display).replace('@DISPATCH@', dispatch)
            .replace('@PROBE@', probe).replace('@CONVERTER@', converter))


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--source', type=Path, default=Path(__file__).resolve().parents[4])
    parser.add_argument('--compiler', default='g++')
    parser.add_argument('--sanitize', action='store_true')
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix='linux-synthetic-display-') as directory:
        source = Path(directory) / 'synthetic_display.cpp'
        source.write_text(fixture(args.source))
        flags = ['-fsanitize=address,undefined', '-fno-omit-frame-pointer'] if args.sanitize else []
        for macros in [[], ['SUNSHINE_BUILD_VAAPI'], ['SUNSHINE_BUILD_CUDA'], ['SUNSHINE_BUILD_VAAPI', 'SUNSHINE_BUILD_CUDA']]:
            binary = Path(directory) / ('test-' + '-'.join(macros))
            subprocess.run([args.compiler, '-std=c++23', '-Wall', '-Wextra', '-Werror', *flags,
                            *['-D' + macro for macro in macros], str(source), '-o', str(binary)], check=True)
            subprocess.run([str(binary)], check=True, timeout=10)
            print('PASS: ' + (', '.join(macros) or 'no GPU backend'), flush=True)


if __name__ == '__main__':
    main()

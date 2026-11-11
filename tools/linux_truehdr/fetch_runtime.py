#!/usr/bin/env python3
"""Fetch the pinned NVIDIA Linux TrueHDR runtime without installing Python packages."""
import argparse
import ctypes
import fcntl
import hashlib
import io
import os
from pathlib import Path
import shutil
import signal
import stat
import sys
import tempfile
import urllib.request
import uuid
import zipfile

WHEEL = 'nvidia_vfx-0.2.0.0-cp312-abi3-manylinux_2_27_x86_64.manylinux_2_28_x86_64.whl'
SHA256 = '0252866a7b798a6f92c8543443d88f716e8c5bbc6a7ddd77a2754728dada9fa8'
URL = 'https://pypi.nvidia.com/nvidia-vfx/' + WHEEL
# These hashes and sizes are derived from the complete wheel after verifying SHA256.
RUNTIME_HASHES = {
    'nvidia_vfx-0.2.0.0.dist-info/licenses/packaging/NVIDIA-Open-Model-License-Agreements-24-10-2025.pdf': ('4d2fb590aa9b30c47f2058bff17291df7fa2aa0c1bd775a20703da9bb267cfab', 79005),
    'nvidia_vfx-0.2.0.0.dist-info/licenses/packaging/NVIDIA-Software-License-Agreement-2025.05.05.pdf': ('887d7381eed7cd2e6d9fbfa9c9bbda4e32df20bf506cff79dc5c8dae20ccc531', 186823),
    'nvidia_vfx-0.2.0.0.dist-info/licenses/packaging/THIRD_PARTY.md': ('2a19287af2bfb99b5017a09ab67f6fb084e591013b6d50ef89b847904eddd41e', 13210),
    'nvidia_vfx-0.2.0.0.dist-info/licenses/packaging/ThirdPartyLicenses_VFX_SDK.txt': ('207b323c79c8d73a1570378597b77dd6ce15d05d0391d9c4d1aef628490bbe70', 1383377),
    'nvidia_vfx-0.2.0.0.dist-info/licenses/packaging/product-specific-terms-for-nvidia-ai-products-2025.05.05.pdf': ('5c755ab1145deb6ce422c2d376f4c3adb55da9f6f2492f35e6d4759e7e3e5499', 66627),
    'nvidia_vfx.libs/libcudart-256e6409.so.12.9.79': ('f7d7885be683ed3cccb2571db2a83af2d9cd8e3e0dcf3841b3a37c489e8ee2df', 2115321),
    'nvvfx/libs/libNVCVImage.so': ('3ead060c31084e137eab1868535a6a8ec4135e00611d54bd7a96b3d2e17e95bb', 4943832),
    'nvvfx/libs/libVideoFX.so': ('19b111cbb98d1051f8cb4ec10ba36c44d6226d3a9565897bb752d28e8514e382', 4198857),
    'nvvfx/libs/libVideoFXLocal.so': ('ce0933ae473ba77bb906827f4ac6f2a7157fa73f1451000ef00718d085148fdd', 4206473),
    'nvvfx/libs/libcudnn.so.9': ('947352ab457779068ab09bb36712cbe61329821171bb3a80f6a6abbc6f4382dc', 125136),
    'nvvfx/libs/libnppc.so.12': ('69c1468de02b2951a3c9755a76b8246b83fbf4d8f137fd1e843767a76c344ae7', 1656080),
    'nvvfx/libs/libnppial.so.12': ('d37c9d285930dca5da32ccce15594bccdadde6da71fd1c297f79d7b435b50ce6', 22046288),
    'nvvfx/libs/libnppicc.so.12': ('bc1f7c1797fda52d0b333f91d65af2add1deeaa0e4cf5b5ae8a58e1d54117fe2', 9373288),
    'nvvfx/libs/libnppidei.so.12': ('8397ce991612229cf673dce3b594187c61ada782d5cf61f4a7212cdd84e1e552', 13633464),
    'nvvfx/libs/libnppig.so.12': ('f24d72d82ceea1b0833a2429cebd6903f0d9ca961841ee413cf6bdea7d0d1129', 55871152),
    'nvvfx/libs/libnvVFXTrueHDR.so': ('7851e321ba2c31f89314b872cd40b1075122832c1d2d57cc901a36222dbd20a6', 4203601),
    'nvvfx/libs/libnvidia-ngx-truehdr.so.1.2.3': ('2ec41bc1a5664c44f2a8a97ab59e60890ab3becda143b753337d12588c993688', 3845656),
    'nvvfx/libs/libnvngxruntime.so': ('4279a4595247e4bbc0b67cc6bc243c66875bf5797741be4e12b0c6f4d844b3fc', 79776),
}


def file_sha256(stream):
    digest = hashlib.sha256()
    while chunk := stream.read(1024 * 1024):
        digest.update(chunk)
    return digest.hexdigest()


def stop_with_parent():
    """Do not leave a download running after its Vibeshine host exits."""
    parent = os.getppid()
    libc = ctypes.CDLL(None, use_errno=True)
    if libc.prctl(1, signal.SIGTERM, 0, 0, 0) != 0:  # PR_SET_PDEATHSIG
        raise OSError(ctypes.get_errno(), 'Could not bind setup to its host process')
    def interrupted(signum, frame):
        raise SystemExit(128 + signum)
    signal.signal(signal.SIGTERM, interrupted)
    if os.getppid() != parent:
        raise SystemExit(1)


class RemoteWheel(io.RawIOBase):
    """Read only the necessary byte ranges from the pinned public wheel."""
    def __init__(self, url, progress=None):
        self.url = url
        request = urllib.request.Request(url, method='HEAD', headers={'Accept-Encoding': 'identity'})
        with urllib.request.urlopen(request, timeout=60) as response:
            self.size = int(response.headers['Content-Length'])
        if not 1 <= self.size <= 1024 * 1024 * 1024:
            raise RuntimeError('Unexpected NVIDIA wheel size')
        self.position = 0
        self.transferred = 0
        self.progress = progress

    def readable(self): return True
    def seekable(self): return True
    def tell(self): return self.position

    def seek(self, offset, whence=0):
        position = offset if whence == 0 else self.position + offset if whence == 1 else self.size + offset
        if position < 0:
            raise ValueError('Negative wheel offset')
        self.position = position
        return position

    def read(self, size=-1):
        if size is None or size < 0:
            size = self.size - self.position
        size = min(size, max(self.size - self.position, 0))
        if not size:
            return b''
        if size > 16 * 1024 * 1024:
            raise RuntimeError('Unexpectedly large NVIDIA wheel range')
        start, end = self.position, self.position + size - 1
        request = urllib.request.Request(self.url, headers={'Range': f'bytes={start}-{end}', 'Accept-Encoding': 'identity'})
        with urllib.request.urlopen(request, timeout=60) as response:
            expected = f'bytes {start}-{end}/{self.size}'
            if response.status != 206 or response.headers.get('Content-Range') != expected:
                raise RuntimeError('NVIDIA server does not support verified partial downloads')
            data = response.read(size + 1)
        if len(data) != size:
            raise RuntimeError('Incomplete NVIDIA download')
        self.transferred += size
        if self.progress:
            self.progress(self.transferred)
        self.position += size
        return data


def ready(destination):
    try:
        if (destination / '.ready').read_text().strip() != SHA256:
            return False
        for name, (digest, size) in RUNTIME_HASHES.items():
            file = destination / name
            if file.is_symlink() or not file.is_file() or file.stat().st_size != size:
                return False
            with file.open('rb') as stream:
                if file_sha256(stream) != digest:
                    return False
        return True
    except (OSError, ValueError):
        return False


def install_runtime(destination, wheel=None):
    destination = Path(os.path.abspath(destination))
    destination.parent.mkdir(parents=True, exist_ok=True)
    if destination.parent.is_symlink() or not destination.parent.is_dir():
        raise RuntimeError('Runtime directory is not a normal directory')
    lock_path = destination.parent / '.install.lock'
    fd = os.open(lock_path, os.O_CREAT | os.O_RDWR | os.O_CLOEXEC | os.O_NOFOLLOW, 0o600)
    with os.fdopen(fd, 'w') as lock:
        fcntl.flock(lock, fcntl.LOCK_EX)
        if destination.exists() or destination.is_symlink():
            if not destination.is_symlink() and ready(destination):
                return 0
            # Preserve an incomplete installation for inspection, and never
            # overwrite a path that could still be in use by another process.
            destination.rename(destination.with_name(destination.name + '.invalid-' + uuid.uuid4().hex))
        with tempfile.TemporaryDirectory(prefix='truehdr-fetch-', dir=destination.parent) as temporary:
            staged = Path(temporary) / 'runtime'
            staged.mkdir()
            if wheel is not None:
                wheel = Path(wheel)
                with wheel.open('rb') as stream:
                    if file_sha256(stream) != SHA256:
                        raise RuntimeError('Wheel digest mismatch')
                source = wheel
            else:
                def report_progress(count):
                    fd, name = tempfile.mkstemp(prefix='.progress-', dir=destination.parent)
                    try:
                        with os.fdopen(fd, 'w') as progress:
                            progress.write(str(count))
                        os.replace(name, destination.parent / '.download-progress')
                    finally:
                        if os.path.exists(name):
                            os.unlink(name)
                report_progress(0)
                source = RemoteWheel(URL, report_progress)
            with zipfile.ZipFile(source) as archive:
                infos = archive.infolist()
                for name, (expected, size) in RUNTIME_HASHES.items():
                    matches = [member for member in infos if member.filename == name]
                    if len(matches) != 1:
                        raise RuntimeError(f'Missing or duplicate NVIDIA runtime file: {name}')
                    member = matches[0]
                    if member.is_dir() or stat.S_ISLNK(member.external_attr >> 16) or member.file_size != size:
                        raise RuntimeError(f'Unsafe NVIDIA runtime file: {name}')
                    target = staged / name
                    target.parent.mkdir(parents=True, exist_ok=True)
                    digest = hashlib.sha256()
                    with archive.open(member) as src, target.open('xb') as out:
                        while chunk := src.read(1024 * 1024):
                            out.write(chunk)
                            digest.update(chunk)
                    if digest.hexdigest() != expected:
                        raise RuntimeError(f'NVIDIA runtime checksum mismatch: {name}')
                    target.chmod(0o444)
            (staged / 'runtime-provenance.txt').write_text(f'url={URL}\nsha256={SHA256}\n')
            (staged / '.ready').write_text(SHA256 + '\n')
            os.rename(staged, destination)
            return source.transferred if isinstance(source, RemoteWheel) else 0


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('destination', type=Path, help='New directory for the extracted NVIDIA wheel')
    parser.add_argument('--wheel', type=Path, help='Use an already downloaded wheel (same pinned digest)')
    parser.add_argument('--runtime-only', action='store_true', help='Install only the native TrueHDR files')
    args = parser.parse_args()
    if args.runtime_only:
        stop_with_parent()
        transferred = install_runtime(args.destination, args.wheel)
        print(f'TrueHDR runtime verified; downloaded {transferred} bytes')
        return
    destination = args.destination.resolve()
    if destination.exists():
        parser.error('destination already exists; choose a new directory')
    destination.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='truehdr-fetch-', dir=destination.parent) as tmp:
        tmp = Path(tmp)
        wheel = args.wheel
        if wheel is None:
            wheel = tmp / WHEEL
            with urllib.request.urlopen(URL, timeout=60) as response, wheel.open('wb') as out:
                shutil.copyfileobj(response, out, 1024 * 1024)
        with wheel.open('rb') as stream:
            digest = file_sha256(stream)
        if digest != SHA256:
            raise RuntimeError(f'Wheel digest mismatch: expected {SHA256}, got {digest}')
        staged = tmp / 'runtime'
        with zipfile.ZipFile(wheel) as archive:
            for member in archive.infolist():
                target = (staged / member.filename).resolve()
                if not target.is_relative_to(staged.resolve()):
                    raise RuntimeError('Archive member escapes runtime directory')
            archive.extractall(staged)
        (staged / 'runtime-provenance.txt').write_text(f'url={URL}\nsha256={SHA256}\n')
        os.rename(staged, destination)
    print(destination)


if __name__ == '__main__':
    try:
        main()
    except (OSError, RuntimeError, ValueError, zipfile.BadZipFile) as error:
        print(f'RTX HDR setup failed: {error}', file=sys.stderr)
        raise SystemExit(1)

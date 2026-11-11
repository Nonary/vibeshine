import hashlib
import io
import os
import subprocess
import sys
import time
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch
import zipfile

import fetch_runtime as fetch


class RuntimeDownloadTests(unittest.TestCase):
    def fixture(self, root):
        wheel = root / 'fixture.whl'
        files = {'nvvfx/libs/needed.so': b'needed-runtime', 'licenses/terms.txt': b'license'}
        with zipfile.ZipFile(wheel, 'w', compression=zipfile.ZIP_DEFLATED) as archive:
            for name, data in files.items():
                archive.writestr(name, data)
            archive.writestr('other/unused.so', b'not installed')
        manifest = {name: (hashlib.sha256(data).hexdigest(), len(data)) for name, data in files.items()}
        return wheel, files, manifest, hashlib.sha256(wheel.read_bytes()).hexdigest()

    def test_installs_only_verified_files_and_reuses_valid_runtime(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            wheel, files, manifest, digest = self.fixture(root)
            with patch.object(fetch, 'RUNTIME_HASHES', manifest), patch.object(fetch, 'SHA256', digest):
                destination = root / 'state' / 'runtime'
                fetch.install_runtime(destination, wheel)
                self.assertTrue(fetch.ready(destination))
                for name, data in files.items():
                    self.assertEqual((destination / name).read_bytes(), data)
                self.assertFalse((destination / 'other').exists())
                wheel.unlink()
                fetch.install_runtime(destination, wheel)  # No download or reread on reuse.

    def test_bad_download_never_commits_and_corruption_is_preserved(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            wheel, files, manifest, digest = self.fixture(root)
            with patch.object(fetch, 'RUNTIME_HASHES', manifest), patch.object(fetch, 'SHA256', digest):
                destination = root / 'state' / 'runtime'
                fetch.install_runtime(destination, wheel)
                (destination / 'nvvfx/libs/needed.so').chmod(0o644)
                (destination / 'nvvfx/libs/needed.so').write_bytes(b'corrupt')
                fetch.install_runtime(destination, wheel)
                self.assertTrue(fetch.ready(destination))
                self.assertEqual(len(list(destination.parent.glob('runtime.invalid-*'))), 1)
                destination2 = root / 'other' / 'runtime'
                wheel.write_bytes(b'tampered')
                with self.assertRaisesRegex(RuntimeError, 'digest mismatch'):
                    fetch.install_runtime(destination2, wheel)
                self.assertFalse(destination2.exists())

    def test_member_hash_mismatch_never_commits(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            wheel, files, manifest, digest = self.fixture(root)
            manifest['nvvfx/libs/needed.so'] = ('0' * 64, len(files['nvvfx/libs/needed.so']))
            with patch.object(fetch, 'RUNTIME_HASHES', manifest), patch.object(fetch, 'SHA256', digest):
                destination = root / 'state' / 'runtime'
                with self.assertRaisesRegex(RuntimeError, 'checksum mismatch'):
                    fetch.install_runtime(destination, wheel)
                self.assertFalse(destination.exists())

    def test_range_reader_rejects_full_responses(self):
        class Response(io.BytesIO):
            status = 200
            headers = {'Content-Length': '100'}
            def __enter__(self): return self
            def __exit__(self, *args): self.close()
        with patch('urllib.request.urlopen', side_effect=[Response(), Response(b'x' * 10)]):
            remote = fetch.RemoteWheel('https://example.invalid/wheel')
            with self.assertRaisesRegex(RuntimeError, 'partial downloads'):
                remote.read(10)

    def test_remote_install_fetches_only_selected_entries(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            wheel, files, manifest, digest = self.fixture(root)
            with zipfile.ZipFile(wheel, 'a', compression=zipfile.ZIP_STORED) as archive:
                archive.writestr('unused/large', os.urandom(2 * 1024 * 1024))
            payload = wheel.read_bytes()
            class Response(io.BytesIO):
                def __init__(self, content, status, headers):
                    super().__init__(content)
                    self.status, self.headers = status, headers
                def __enter__(self): return self
                def __exit__(self, *args): self.close()
            def open_request(request, timeout):
                if request.get_method() == 'HEAD':
                    return Response(b'', 200, {'Content-Length': str(len(payload))})
                start, end = map(int, request.get_header('Range')[6:].split('-'))
                return Response(payload[start:end + 1], 206,
                                {'Content-Range': f'bytes {start}-{end}/{len(payload)}'})
            with patch.object(fetch, 'RUNTIME_HASHES', manifest), patch.object(fetch, 'SHA256', digest), \
                 patch('urllib.request.urlopen', side_effect=open_request):
                destination = root / 'state' / 'runtime'
                transferred = fetch.install_runtime(destination)
                self.assertLess(transferred, len(payload) // 10)
                self.assertEqual(int((destination.parent / '.download-progress').read_text()), transferred)
                self.assertTrue(fetch.ready(destination))

    def test_downloader_exits_when_its_host_exits(self):
        with tempfile.TemporaryDirectory() as directory:
            marker = str(Path(directory) / 'stopped')
            child_code = (
                f'import sys,time; sys.path.insert(0,{str(Path(fetch.__file__).parent)!r}); '
                'import fetch_runtime as f; f.stop_with_parent(); '
                f'exec("try:\\n print(\\\"ready\\\",flush=True)\\n time.sleep(60)\\nfinally:\\n open({marker!r},\\\"w\\\").write(\\\"stopped\\\")")'
            )
            parent_code = (
                'import subprocess,sys,time; '
                f'p=subprocess.Popen([sys.executable,"-c",{child_code!r}],stdout=subprocess.PIPE,text=True); '
                'assert p.stdout.readline().strip()=="ready"; print(p.pid,flush=True); time.sleep(60)'
            )
            parent = subprocess.Popen([sys.executable, '-c', parent_code], stdout=subprocess.PIPE, text=True)
            try:
                self.assertGreater(int(parent.stdout.readline().strip()), 0)
                parent.terminate()
                parent.wait(timeout=5)
                for _ in range(50):
                    if Path(marker).exists():
                        break
                    time.sleep(0.1)
                self.assertTrue(Path(marker).exists(), 'downloader survived its host')
            finally:
                if parent.poll() is None:
                    parent.kill()
                    parent.wait()
                parent.stdout.close()


if __name__ == '__main__':
    unittest.main()

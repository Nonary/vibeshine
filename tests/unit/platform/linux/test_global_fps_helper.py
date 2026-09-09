#!/usr/bin/env python3
"""Run the desktop helper with isolated state, including crash and ownership cases."""
import os
import pathlib
import signal
import struct
import subprocess
import sys
import tempfile
import time

helper = sys.argv[1]
if os.getuid() == 0:
    print("The desktop helper requires an unprivileged test user")
    sys.exit(77)
with tempfile.TemporaryDirectory(prefix="vibeshine-global-fps-") as temporary:
    home = pathlib.Path(temporary)
    environment = dict(os.environ, HOME=str(home))
    state = home / ".cache/vibeshine/frame-limiter.state"

    def read_state():
        return struct.unpack("=IIq", state.read_bytes())

    def start(limit="59940"):
        process = subprocess.Popen([helper, limit], env=environment, stdout=subprocess.PIPE)
        assert process.stdout.readline() == b"ready\n", process.poll()
        return process

    for invalid in ["0", "1000001", "-1", "60.0", "60x"]:
        assert subprocess.run([helper, invalid], env=environment).returncode != 0

    process = start()
    try:
        version, limit, expiry = read_state()
        assert version == 1 and limit == 59940 and expiry > time.clock_gettime_ns(time.CLOCK_MONOTONIC)
        assert state.stat().st_mode & 0o777 == 0o600
        # A second owner must not overwrite or clear the active owner's lease.
        duplicate = subprocess.run([helper, "120000"], env=environment, stdout=subprocess.PIPE)
        assert duplicate.returncode != 0 and read_state()[1] == 59940
        if len(sys.argv) == 4:
            subprocess.run([sys.argv[2], sys.argv[3]], env=dict(environment, VIBESHINE_TEST_EXPECT_FPS="59940"), check=True)
    finally:
        process.terminate()
        process.wait(timeout=3)
    assert read_state()[1:] == (0, 0)

    # Stream reconnect may arrive while the broker is still stopping the old
    # helper. A new owner should wait briefly and then publish its own limit.
    process = start("60000")
    replacement = subprocess.Popen([helper, "120000"], env=environment, stdout=subprocess.PIPE)
    try:
        time.sleep(0.1)
        process.terminate()
        process.wait(timeout=3)
        assert replacement.stdout.readline() == b"ready\n"
        assert read_state()[1] == 120000
    finally:
        if process.poll() is None:
            process.kill()
            process.wait()
        replacement.terminate()
        replacement.wait(timeout=3)

    process = start("120000")
    process.kill()
    process.wait(timeout=3)
    _, _, expiry = read_state()
    remaining = expiry - time.clock_gettime_ns(time.CLOCK_MONOTONIC)
    assert 0 < remaining <= 2_000_000_000
    time.sleep(remaining / 1_000_000_000 + 0.01)
    assert read_state()[2] < time.clock_gettime_ns(time.CLOCK_MONOTONIC)

    if len(sys.argv) == 4 and sys.platform.startswith("linux"):
        # Even a 0.001 FPS deadline must release promptly on disconnect or
        # helper crash; it must not sleep the full 1000-second frame interval.
        for stop_method in ("terminate", "kill"):
            process = start("1")
            chain = subprocess.Popen([sys.argv[2], sys.argv[3]],
                                     env=dict(environment, VIBESHINE_TEST_WAIT_FOR_DISABLE="1"),
                                     stdout=subprocess.PIPE)
            try:
                assert chain.stdout.readline() == b"waiting\n"
                getattr(process, stop_method)()
                process.wait(timeout=3)
                assert chain.wait(timeout=5) == 0
            finally:
                if process.poll() is None:
                    process.kill()
                    process.wait()
                if chain.poll() is None:
                    chain.kill()
                    chain.wait()

    state.unlink()
    outside = home / "untouched"
    outside.write_bytes(b"original")
    state.symlink_to(outside)
    assert subprocess.run([helper, "60000"], env=environment, stdout=subprocess.PIPE).returncode != 0
    assert outside.read_bytes() == b"original"
    state.unlink()
    os.link(outside, state)
    assert subprocess.run([helper, "60000"], env=environment, stdout=subprocess.PIPE).returncode != 0
    assert outside.read_bytes() == b"original"

print("Global FPS helper apply, exclusive ownership, restore, crash expiry and link protection passed")

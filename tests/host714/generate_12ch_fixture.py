#!/usr/bin/env python3
"""Write a low-level, explicitly synthetic 12-channel Float32 WAVE. Never play it.

Python standard library only. WAVEFORMATEXTENSIBLE, 48 kHz, mask 0x2D63F.
One channel sounds at a time, with short ramps and an optional silent gap.
"""
from __future__ import annotations
import argparse
from array import array
import hashlib
import json
import math
from pathlib import Path
import struct
import sys
import uuid

CHANNELS = ('FL', 'FR', 'FC', 'LFE', 'BL', 'BR', 'SL', 'SR', 'TFL', 'TFR', 'TBL', 'TBR')
FREQUENCIES = (440.0, 554.365, 660.0, 60.0, 784.0, 880.0, 988.0, 1108.0, 1245.0, 1397.0, 1568.0, 1760.0)
RATE = 48000
BLOCK_ALIGN = len(CHANNELS) * 4
MASK = 0x2D63F


def chunk(tag: bytes, data: bytes) -> bytes:
    if len(tag) != 4:
        raise ValueError('A RIFF chunk tag must have four bytes')
    return tag + struct.pack('<I', len(data)) + data + (b'\x00' if len(data) & 1 else b'')


def generate(output: Path, seconds: float, gap_seconds: float, level_dbfs: float) -> dict:
    """Create a new file and return its exact channel timeline and integrity data."""
    if not (0.1 <= seconds <= 5.0 and 0.0 <= gap_seconds <= 1.0):
        raise ValueError('Tone duration must be 0.1..5 s; gap must be 0..1 s')
    if not -80.0 <= level_dbfs <= -24.0:
        raise ValueError('Peak tone level must be -80..-24 dBFS')
    if output.exists():
        raise FileExistsError(f'Refusing to overwrite: {output}')
    metadata_path = output.with_suffix(output.suffix + '.json')
    if metadata_path.exists():
        raise FileExistsError(f'Refusing to overwrite: {metadata_path}')
    tone_frames = round(seconds * RATE)
    gap_frames = round(gap_seconds * RATE)
    total_frames = (tone_frames + gap_frames) * len(CHANNELS)
    data_bytes = total_frames * BLOCK_ALIGN
    if data_bytes > 200 * 1024 * 1024:
        raise ValueError('Fixture exceeds the 200 MiB safety limit')
    amplitude = 10.0 ** (level_dbfs / 20.0)
    ramp_frames = min(round(0.02 * RATE), tone_frames // 2)
    fmt = struct.pack('<HHIIHHH', 0xFFFE, len(CHANNELS), RATE, RATE * BLOCK_ALIGN, BLOCK_ALIGN, 32, 22)
    fmt += struct.pack('<HI', 32, MASK)
    fmt += uuid.UUID('00000003-0000-0010-8000-00aa00389b71').bytes_le
    assert len(fmt) == 40
    before_data = b'WAVE' + chunk(b'fmt ', fmt) + chunk(b'fact', struct.pack('<I', total_frames))
    riff_size = len(before_data) + 8 + data_bytes
    output.parent.mkdir(parents=True, exist_ok=True)
    timeline = []
    with output.open('xb') as f:
        f.write(b'RIFF' + struct.pack('<I', riff_size) + before_data + b'data' + struct.pack('<I', data_bytes))
        for channel, (name, frequency) in enumerate(zip(CHANNELS, FREQUENCIES)):
            start = channel * (tone_frames + gap_frames)
            timeline.append({'channel_index': channel, 'name': name, 'frequency_hz': frequency,
                             'start_frame': start, 'end_frame_exclusive': start + tone_frames,
                             'gap_after_frames': gap_frames})
            for offset in range(0, tone_frames, 4096):
                count = min(4096, tone_frames - offset)
                values = array('f', [0.0]) * (count * len(CHANNELS))
                if values.itemsize != 4:
                    raise RuntimeError('This interpreter does not expose 32-bit array floats')
                for j in range(count):
                    n = offset + j
                    ramp = min(1.0, n / ramp_frames, (tone_frames - 1 - n) / ramp_frames)
                    values[j * len(CHANNELS) + channel] = amplitude * max(0.0, ramp) * math.sin(2 * math.pi * frequency * n / RATE)
                if sys.byteorder != 'little':
                    values.byteswap()
                f.write(values.tobytes())
            remaining = gap_frames * BLOCK_ALIGN
            zero = bytes(65536)
            while remaining:
                size = min(remaining, len(zero))
                f.write(zero[:size])
                remaining -= size
    digest = hashlib.sha256()
    with output.open('rb') as f:
        for data in iter(lambda: f.read(1 << 20), b''):
            digest.update(data)
    report = {'provenance': 'synthetic_test_only', 'not_game_audio': True,
              'format': 'WAVEFORMATEXTENSIBLE IEEE_FLOAT Float32LE', 'sample_rate_hz': RATE,
              'channels': list(CHANNELS), 'channel_mask': hex(MASK), 'frames': total_frames,
              'duration_seconds': total_frames / RATE, 'peak_tone_dbfs': level_dbfs,
              'ramp_frames': ramp_frames, 'timeline': timeline,
              'file_name': output.name, 'size_bytes': output.stat().st_size, 'sha256': digest.hexdigest(),
              'warning': 'Generated only, never played. Low digital level does not guarantee safe physical volume.'}
    metadata_path.write_text(json.dumps(report, indent=2) + '\n', encoding='utf8')
    return report


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--seconds-per-channel', type=float, default=0.5)
    parser.add_argument('--gap-seconds', type=float, default=0.1)
    parser.add_argument('--level-dbfs', type=float, default=-36.0)
    args = parser.parse_args()
    try:
        report = generate(args.output, args.seconds_per_channel, args.gap_seconds, args.level_dbfs)
    except (ValueError, OSError, RuntimeError) as exc:
        parser.exit(1, f'ERROR: {exc}\n')
    print(json.dumps(report, indent=2))


if __name__ == '__main__':
    main()

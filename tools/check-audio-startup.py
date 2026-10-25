#!/usr/bin/env python3
"""Check the two AUX fixture tracks before their existing 100 ms tone check.

Domain: capture_fixture --gpu-aux, channel zero is a 0.15-amplitude 300 Hz
tone with a single 0.8 impulse at source sample time 200 ms. The fixture's
separate source-clock oracle checks the impulse's A/V position. Here that
impulse locates the tone onset, independent of codec and container delay.
"""

import array
import json
import math
import subprocess
import sys

clip, = sys.argv[1:]
metadata = json.loads(subprocess.check_output([
    'ffprobe', '-v', 'error', '-select_streams', 'a', '-show_streams', '-of', 'json', clip
]))
if len(metadata['streams']) != 2 or any(
        stream['channels'] != 16 or int(stream['sample_rate']) != 48000
        for stream in metadata['streams']):
    raise ValueError('not the two 16-channel, 48 kHz AUX fixture tracks')
results = []
for track in range(2):
    raw = subprocess.check_output([
        'ffmpeg', '-v', 'error', '-i', clip, '-map', f'0:a:{track}',
        '-af', 'pan=mono|c0=c0', '-f', 'f32le', '-'
    ])
    samples = array.array('f')
    samples.frombytes(raw)
    if sys.byteorder != 'little':
        samples.byteswap()
    if len(samples) != 48000 or not all(math.isfinite(x) for x in samples):
        raise ValueError('not one complete finite second of the fixture')
    impulse = max(range(len(samples)), key=lambda i: abs(samples[i]))
    if abs(samples[impulse]) < 0.3:
        raise ValueError('missing source impulse; onset inference is invalid')
    # Start 5 ms after the inferred source onset. Ten ms contains three cycles;
    # quadrature tolerates the source's phase and small codec/resampler offsets.
    start = max(0, impulse - 9600) + 240
    window = samples[start:start + 480]
    if len(window) != 480:
        raise ValueError('startup window is outside the decoded stream')
    sine = sum(x * math.sin(2 * math.pi * 300 * i / 48000) for i, x in enumerate(window))
    cosine = sum(x * math.cos(2 * math.pi * 300 * i / 48000) for i, x in enumerate(window))
    amplitude = 2 * math.hypot(sine, cosine) / len(window)
    results.append({'track': track, 'impulse_sample': impulse, 'window_start': start,
                    'amplitude': amplitude, 'passed': abs(amplitude - 0.15) <= 0.04})
print(json.dumps(results, indent=2))
if not all(result['passed'] for result in results):
    raise SystemExit('early source tone was lost or materially altered')

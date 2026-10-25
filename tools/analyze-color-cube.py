#!/usr/bin/env python3
"""Check rgb_encode_probe's 8x8-patch RGB cube through an independent decoder.

This measures decoded RGB code error, including codec/filter effects. It is not
a lossless-conversion claim or a perceptual-quality score. Matrix/range metadata
select the inverse transform; BT.709 primaries and sRGB transfer stay unchanged.
"""
import argparse
import json
import math
import subprocess

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('clip')
parser.add_argument('--levels', type=int, required=True)
args = parser.parse_args()
levels = args.levels
if not 2 <= levels <= 256:
    parser.error('8-bit cube levels must be between 2 and 256')
metadata = json.loads(subprocess.check_output([
    'ffprobe', '-v', 'error', '-select_streams', 'v:0', '-show_streams', '-of', 'json', args.clip
]))['streams'][0]
width, height = metadata['width'], metadata['height']
if ((width, height) != (levels * 8, levels * 8) or metadata['pix_fmt'] != 'yuv420p'
        or metadata['color_primaries'] != 'bt709' or metadata['color_transfer'] != 'iec61966-2-1'):
    raise ValueError('unsupported cube geometry, pixel format, primaries or transfer')
kr, kb = {'bt709': (.2126, .0722), 'bt2020nc': (.2627, .0593)}[metadata['color_space']]
limited = {'tv': True, 'pc': False}[metadata['color_range']]
frame_bytes = width * height * 3 // 2
absolute = squared = bias = maximum = 0.0
per_channel = [0.0, 0.0, 0.0]
unclipped_max = 0.0
samples = 0
with subprocess.Popen(['ffmpeg', '-v', 'error', '-xerror', '-i', args.clip,
                        '-map', '0:v:0', '-fps_mode', 'passthrough', '-f', 'rawvideo', '-'],
                       stdout=subprocess.PIPE) as decoder:
    for blue in range(levels):
        frame = decoder.stdout.read(frame_bytes)
        if len(frame) != frame_bytes:
            raise ValueError('missing or partial decoded frame')
        for green in range(levels):
            y = green * 8 + 4
            for red in range(levels):
                x = red * 8 + 4
                yy = frame[y * width + x]
                cb = frame[width * height + (y // 2) * (width // 2) + x // 2]
                cr = frame[width * height * 5 // 4 + (y // 2) * (width // 2) + x // 2]
                Y = (yy - 16) * 255 / 219 if limited else yy
                Cb = (cb - 128) * 255 / 224 if limited else cb - 128
                Cr = (cr - 128) * 255 / 224 if limited else cr - 128
                r, b = Y + 2 * (1 - kr) * Cr, Y + 2 * (1 - kb) * Cb
                g = (Y - kr * r - kb * b) / (1 - kr - kb)
                expected = [index * 255 // (levels - 1) for index in (red, green, blue)]
                for channel, (actual, wanted) in enumerate(zip((r, g, b), expected, strict=True)):
                    unclipped_max = max(unclipped_max, abs(actual - wanted))
                    error = min(255, max(0, actual)) - wanted
                    absolute += abs(error)
                    squared += error * error
                    bias += error
                    per_channel[channel] += error
                    maximum = max(maximum, abs(error))
                    samples += 1
    if decoder.stdout.read(1):
        raise ValueError('unexpected extra frame data')
    if decoder.wait():
        raise ValueError('independent decoding failed')
print(json.dumps({'levels': levels, 'colors': levels ** 3, 'samples': samples,
                  'matrix': metadata['color_space'], 'range': metadata['color_range'],
                  'rgb_mae': absolute / samples, 'rgb_rmse': math.sqrt(squared / samples),
                  'rgb_max': maximum, 'rgb_unclipped_max': unclipped_max,
                  'rgb_bias': bias / samples,
                  'channel_bias': [n / (samples // 3) for n in per_channel]}, indent=2))

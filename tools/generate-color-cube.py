#!/usr/bin/env python3
"""Stream rgb_encode_probe's 8-bit BGRA cube without storing full raw frames."""
import argparse
import sys

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--levels', type=int, required=True)
args = parser.parse_args()
if not 2 <= args.levels <= 256:
    parser.error('8-bit cube levels must be between 2 and 256')
values = [i * 255 // (args.levels - 1) for i in range(args.levels)]
for blue in values:
    for green in values:
        row = b''.join(bytes((blue, green, red, 255)) * 8 for red in values)
        sys.stdout.buffer.write(row * 8)

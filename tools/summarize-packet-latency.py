#!/usr/bin/env python3
"""Observed public submit-to-receive delays; these are not pure GPU durations."""

import json
import math
import re
import statistics
import sys
from pathlib import Path

rows = []
for line in Path(sys.argv[1]).read_text(encoding="utf-8").splitlines():
    match = re.fullmatch(r"PACKET_LATENCY pts=(-?\d+) ns=(\d+) bytes=(\d+) pending=(\d+)", line)
    if match is None:
        raise ValueError(line)
    rows.append(tuple(map(int, match.groups())))
if not rows:
    raise ValueError("no measured packets")
delays = sorted(row[1] for row in rows)
print(json.dumps({
    "count": len(rows),
    "mean_ns": statistics.mean(delays),
    "median_ns": statistics.median(delays),
    "p99_observed_ns": delays[math.ceil(len(delays) * .99) - 1],
    "min_ns": delays[0],
    "max_ns": delays[-1],
    "first_packet_ns": rows[0][1],
    "max_pending": max(row[3] for row in rows),
}, indent=2))

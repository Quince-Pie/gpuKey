#!/usr/bin/env python3
"""Summarize measured replay lifecycles, excluding warmups and packet manifests."""

import csv
import json
import statistics
import sys
from collections import defaultdict
from pathlib import Path

root = Path(sys.argv[1])
groups = defaultdict(list)
with (root / "runs.tsv").open(newline="") as source:
    for row in csv.DictReader(source, delimiter="\t"):
        # Basenames keep archived evidence usable after moving its directory.
        path = root / Path(row["json"]).name
        groups[row["case"], row["variant"]].append(json.loads(path.read_text()))
summary = {}
for (case, variant), runs in sorted(groups.items()):
    measurements = {}
    for key in runs[0]:
        values = [run[key] for run in runs]
        measurements[key] = {
            "mean": statistics.mean(values),
            "sample_sd": statistics.stdev(values) if len(values) > 1 else None,
            "min": min(values),
            "max": max(values),
        }
    summary.setdefault(case, {})[variant] = {"runs": len(runs), **measurements}
print(json.dumps(summary, indent=2, sort_keys=True))

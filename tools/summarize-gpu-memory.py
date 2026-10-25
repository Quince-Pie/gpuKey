#!/usr/bin/env python3
"""Summarize matrix runs; CPU/throughput include startup and teardown.

DRM peaks are sampled, and shared bytes come from the same sample as total VRAM.
Producer-empty callbacks remain separate from recorder skips. Raw runs are kept
in summary.json. A single run has no estimable sample standard deviation.
Prefer completed-child CPU/RSS when present; older evidence uses the recorder's
report taken near exit. Keep that distinction in each machine-readable row.
"""

import json
import statistics
import sys
from collections import defaultdict
from pathlib import Path

if len(sys.argv) != 2:
    sys.exit("Usage: python3 tools/summarize-gpu-memory.py MATRIX_DIRECTORY")

groups = defaultdict(list)
for folder in Path(sys.argv[1]).iterdir():
    if not (folder / 'recording.json').exists():
        continue
    case, variant, run = folder.name.rsplit('-', 2)
    record = json.loads((folder / 'recording.json').read_text(encoding='utf-8'))
    producer = json.loads((folder / 'producer.json').read_text(encoding='utf-8'))
    memory = json.loads((folder / 'memory.json').read_text(encoding='utf-8'))
    if not record['success'] or memory['returncode'] or int(run) < 1:
        raise ValueError(f'failed recording: {folder}')
    samples = []
    for sample in memory['samples']:
        clients = sample['clients'].values()
        samples.append({k: sum(c[k] for c in clients) for k in
                        ['total-vram', 'shared-vram', 'resident-vram', 'purgeable-vram', 'total-gtt']})
    peak = max(samples, key=lambda s: s['total-vram'])
    groups[case, variant].append(dict(
        drm_sample_count=len(memory['samples']),
        drm_first_sample_ms=memory['samples'][0]['elapsed_ns']/1e6,
        drm_last_sample_ms=memory['samples'][-1]['elapsed_ns']/1e6,
        fdinfo_unavailable=(memory['unavailable_fdinfo'].get(str(memory['pid']), {})
                            if 'unavailable_fdinfo' in memory else None),
        cpu_scope='completed_child' if 'child_cpu_user_us' in memory else 'recorder_report',
        vram=peak['total-vram']/2**20, private=(peak['total-vram']-peak['shared-vram'])/2**20,
        gtt=max(s['total-gtt'] for s in samples)/2**20,
        cpu=(memory.get('child_cpu_user_us', record['cpu_user_us'])+
             memory.get('child_cpu_system_us', record['cpu_system_us']))/1000,
        rss=memory.get('child_max_rss_kib', record['max_rss_kib'])/1024,
        fps=record['submitted']/(record['elapsed_ns']/1e9),
        lease=record['source_lease']['max_ns']/1e6,
        lease_mean=record['source_lease']['mean_ns']/1e6,
        lease_p99_upper=record['source_lease']['p99_bucket_upper_ns']/1e6,
        submitted=record['submitted'], empty=producer['empty_processes'],
        coalesced=producer['coalesced_ticks'], skipped=record['skipped'],
        source=producer['peak_source_bytes']/2**20))
if not groups:
    raise ValueError('no recordings found')
print('case variant n | VRAM private GTT MiB | RSS MiB sd | CPU_ms sd | FPS sd | empty coalesced skipped | lease_mean max_ms')
for (case, variant), rows in sorted(groups.items()):
    mean = lambda key: statistics.mean(r[key] for r in rows)
    sd = lambda key: statistics.stdev(r[key] for r in rows) if len(rows)>1 else float('nan')
    print(f'{case:8} {variant:8} {len(rows)} | {mean("vram"):8.3f} {mean("private"):8.3f} {mean("gtt"):7.3f} | '
          f'{mean("rss"):7.3f} {sd("rss"):6.3f} | '
          f'{mean("cpu"):7.3f} {sd("cpu"):6.3f} | {mean("fps"):7.2f} {sd("fps"):5.2f} | '
          f'{mean("empty"):6.1f} {mean("coalesced"):6.1f} {mean("skipped"):5.1f} | '
          f'{mean("lease_mean"):7.3f} {max(r["lease"] for r in rows):8.3f}')
Path(sys.argv[1], 'summary.json').write_text(json.dumps(
    {f'{c}/{v}': rows for (c,v),rows in sorted(groups.items())}, indent=2, allow_nan=False)+'\n',
    encoding='utf-8')

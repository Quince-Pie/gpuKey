#!/usr/bin/env python3
"""Summarize complete private-recorder comparisons without hiding quality differences."""
import collections
import json
import statistics
import sys
from pathlib import Path

root = Path(sys.argv[1])
groups = collections.defaultdict(list)
for folder in sorted(root.iterdir()):
    if not (folder / 'pictures.json').exists():
        continue
    case, variant, run = folder.name.rsplit('-',2)
    memory = json.loads((folder / 'memory.json').read_text())
    source = json.loads((folder / 'producer.json').read_text())
    pictures = json.loads((folder / 'pictures.json').read_text())
    if memory['returncode'] or memory['stop_killed'] or pictures['corrupt_markers']:
        raise ValueError(f'invalid run: {folder}')
    engine_max = collections.defaultdict(int)
    values = []
    for sample in memory['samples']:
        counts = {key:sum(client[key] for client in sample['clients'].values())
                  for key in ('total-vram','shared-vram','resident-vram','total-gtt')}
        values.append(counts)
        for client, fields in sample['clients'].items():
            for key,value in fields.items():
                if key.startswith('engine-'):
                    engine_max[client,key] = max(engine_max[client,key], value)
    engines = collections.defaultdict(int)
    for (client,key),value in engine_max.items():
        engines[key] += value
    peak = max(values,key=lambda s:s['total-vram'])
    row = dict(run=int(run), cpu_ms=(memory['child_cpu_user_us']+memory['child_cpu_system_us'])/1000,
        rss_mib=memory['child_max_rss_kib']/1024,
        vram_mib=peak['total-vram']/2**20, shared_at_peak_mib=peak['shared-vram']/2**20,
        gtt_mib=max(s['total-gtt'] for s in values)/2**20,
        producer_buffers=source['peak_source_buffers'], producer_payload_mib=source['peak_source_bytes']/2**20,
        elapsed_seconds=memory['elapsed_ns']/1e9, source_frames=source['produced'],
        decoded_frames=pictures['decoded_frames'], unique_frames=pictures['unique_identities'],
        repeats=pictures['repeated_identities'], gaps=pictures['interior_missing_identities'],
        backwards=pictures['backward_transitions'],
        source_empty=source['empty_processes'], source_coalesced=source['coalesced_ticks'],
        patch_mae=pictures['patch_mean_absolute_error'],
        patch_max=pictures['patch_max_absolute_error'], sampled_engine_ms={k:v/1e6 for k,v in engines.items()},
        color_matrix=pictures.get('color_matrix'), color_range=pictures.get('color_range'),
        rgb_patch_mae=pictures.get('patch_rgb_mean_absolute_error'),
        rgb_patch_max=pictures.get('patch_rgb_max_absolute_error'),
        unavailable_fdinfo=memory['unavailable_fdinfo'])
    groups[case,variant].append(row)
if not groups:
    raise ValueError('no verified recordings')
print('case variant n | CPU ms ± SD | RSS MiB ± SD | sampled VRAM(shared) GTT MiB | source buffers/payload MiB | frames unique repeats gaps | patch MAE YUV / RGB')
for (case,variant),rows in sorted(groups.items()):
    mean=lambda k:statistics.mean(r[k] for r in rows)
    sd=lambda k:statistics.stdev(r[k] for r in rows) if len(rows)>1 else float('nan')
    rgb_error = mean('rgb_patch_mae') if all(r['rgb_patch_mae'] is not None for r in rows) else float('nan')
    print(f'{case:17} {variant:7} {len(rows)} | {mean("cpu_ms"):8.3f} ± {sd("cpu_ms"):6.3f} | '
          f'{mean("rss_mib"):7.3f} ± {sd("rss_mib"):6.3f} | '
          f'{mean("vram_mib"):7.3f} ({mean("shared_at_peak_mib"):7.3f}) {mean("gtt_mib"):7.3f} | '
          f'{mean("producer_buffers"):2.0f}/{mean("producer_payload_mib"):7.3f} | '
          f'{mean("decoded_frames"):7.1f} {mean("unique_frames"):7.1f} {mean("repeats"):5.1f} {mean("gaps"):5.1f} | '
          f'{mean("patch_mae"):.3f} / {rgb_error:.3f}')
(root/'summary.json').write_text(json.dumps({f'{c}/{v}':rows for (c,v),rows in sorted(groups.items())},indent=2)+'\n')

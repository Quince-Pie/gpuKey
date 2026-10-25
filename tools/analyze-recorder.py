#!/usr/bin/env python3
"""Decode the external fixture's image identities and constant-color patches.

This oracle is specific to --gpu-portal-source at >=256x64 with the scene pattern.
It measures visible repeats/gaps and unoccluded SDR patches, not general video
quality. Frame identities are taken from pixels, independently of mux timestamps.
"""

import argparse
import json
import statistics
import subprocess
from pathlib import Path


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('directory', type=Path)
    args = parser.parse_args()
    root = args.directory
    producer = json.loads((root / 'producer.json').read_text())
    metadata = json.loads((root / 'decoded.json').read_text())
    stream, = metadata['streams']
    width, height = producer['width'], producer['height']
    if (width < 256 or height < 64 or width % 2 or height % 2 or producer['pattern_width'] < width
            or producer['produced'] >= 2**32 or stream['width'] != width
            or stream['height'] != height
            or stream['pix_fmt'] not in ('yuv420p', 'yuv420p10le')):
        raise ValueError('input is outside this fixture oracle domain')
    if stream.get('color_primaries') != 'bt709' or stream.get('color_transfer') != 'iec61966-2-1':
        raise ValueError('source primaries or transfer do not match the fixture')
    kr, kb = {'bt709': (.2126, .0722), 'bt2020nc': (.2627, .0593)}[stream['color_space']]
    limited = {'tv': True, 'pc': False}[stream['color_range']]
    command = ['ffmpeg', '-v', 'error', '-i', str(root / 'clip.mkv')]
    markers = subprocess.run(command + ['-map', '0:v:0', '-vf',
        'crop=256:16:0:0,format=gray', '-fps_mode', 'passthrough',
        '-enc_time_base', 'demux', '-f', 'rawvideo', '-'], check=True, stdout=subprocess.PIPE).stdout
    stride = 256 * 16
    if not markers or len(markers) % stride:
        raise ValueError('incomplete marker frames')
    identities = []
    corrupt = []
    for frame in range(len(markers) // stride):
        values = []
        for row in range(2):
            value = 0
            for bit in range(32):
                patch = [markers[frame * stride + (row * 8 + y) * 256 + bit * 8 + x]
                         for y in range(2, 6) for x in range(2, 6)]
                level = statistics.median(patch)
                if 40 < level < 200:
                    corrupt.append({'frame': frame, 'row': row, 'bit': bit, 'level': level})
                value |= (level >= 128) << bit
            values.append(value)
        if values[0] ^ values[1] != 0xffffffff:
            corrupt.append({'frame': frame, 'identity': values[0], 'complement': values[1]})
        identities.append(values[0])
    if max(identities) >= producer['produced']:
        corrupt.append({'out_of_source_domain': max(identities)})
    differences = [b-a for a, b in zip(identities, identities[1:])]
    ten_bit = stream['pix_fmt'] == 'yuv420p10le'
    first = subprocess.run(command + ['-map', '0:v:0', '-frames:v', '1', '-pix_fmt',
        'yuv420p10le' if ten_bit else 'yuv420p', '-f', 'rawvideo', '-'],
        check=True, stdout=subprocess.PIPE).stdout
    sample_bytes = 2 if ten_bit else 1
    if len(first) != width * height * 3 // 2 * sample_bytes:
        raise ValueError('incomplete independently decoded color frame')
    # Source bars are BGR bytes. Use the declared model's Kr/Kb equations, not
    # either recorder's conversion implementation, for the constant-patch oracle.
    bars = [(255,255,255), (0,255,255), (255,255,0), (0,255,0),
            (255,0,255), (0,0,255), (255,0,0), (0,0,0)]
    colors = []
    source_y = max(height//4, 24)  # Below the identity strip, inside the color bars.
    for index, (b,g,r) in enumerate(bars):
        left = (index*producer['pattern_width']+7)//8
        right = min(width, ((index+1)*producer['pattern_width']+7)//8)-1
        if right-left < 8:
            continue
        source_x = None
        for x in ((left+right)//2, left+4, right-4):
            if abs(source_y-height//2) <= 36 and abs(x-width//2) <= 36:
                continue  # The moving center marker covers this patch.
            source_x = x
            break
        if source_x is None:
            continue
        luma = kr*r + (1-kr-kb)*g + kb*b
        expected = ([16+219*luma/255, 128+112*(b-luma)/(255*(1-kb)),
                     128+112*(r-luma)/(255*(1-kr))] if limited else
                    [luma, 128+(b-luma)/(2*(1-kb)), 128+(r-luma)/(2*(1-kr))])
        observed = []
        for plane in range(3):
            divisor = 1 if plane == 0 else 2
            pitch = width // divisor
            offset = 0 if plane == 0 else width*height + (plane-1)*width*height//4
            cx, cy = source_x//divisor, source_y//divisor
            if ten_bit:
                patch = [int.from_bytes(first[2*(offset + y*pitch + x):2*(offset + y*pitch + x + 1)], 'little') / 4.0
                         for y in range(cy-2,cy+2) for x in range(cx-2,cx+2)]
            else:
                patch = [first[offset + y*pitch + x] for y in range(cy-2,cy+2)
                         for x in range(cx-2,cx+2)]
            observed.append(statistics.mean(patch))
        yy = (observed[0]-16)*255/219 if limited else observed[0]
        cb = (observed[1]-128)*255/224 if limited else observed[1]-128
        cr = (observed[2]-128)*255/224 if limited else observed[2]-128
        rr, bb = yy+2*(1-kr)*cr, yy+2*(1-kb)*cb
        gg = (yy-kr*rr-kb*bb)/(1-kr-kb)
        decoded_rgb = [min(255,max(0,c)) for c in (rr,gg,bb)]
        colors.append({'bgr': [b,g,r], 'expected_yuv': expected, 'decoded_yuv': observed,
                       'absolute_error': [abs(a-b) for a,b in zip(observed,expected)],
                       'decoded_rgb': decoded_rgb,
                       'rgb_absolute_error': [abs(a-b) for a,b in zip(decoded_rgb,(r,g,b))]})
    result = {'decoded_frames': len(identities), 'source_frames': producer['produced'],
              'first_identity': identities[0], 'last_identity': identities[-1],
              'unique_identities': len(set(identities)),
              'repeated_identities': sum(d == 0 for d in differences),
              'interior_missing_identities': sum(max(d-1,0) for d in differences),
              'backward_transitions': sum(d < 0 for d in differences),
              'corrupt_markers': corrupt, 'identities': identities, 'color_patches': colors,
              'patch_count': len(colors), 'color_matrix': stream['color_space'],
              'color_range': stream['color_range'],
              'patch_mean_absolute_error': statistics.mean(e for p in colors for e in p['absolute_error']),
              'patch_max_absolute_error': max(e for p in colors for e in p['absolute_error']),
              'patch_rgb_mean_absolute_error': statistics.mean(e for p in colors for e in p['rgb_absolute_error']),
              'patch_rgb_max_absolute_error': max(e for p in colors for e in p['rgb_absolute_error'])}
    if len(identities) != int(stream['nb_read_frames']):
        raise ValueError('independent decoder frame counts disagree')
    with (root / 'pictures.json').open('x') as output:
        json.dump(result, output, indent=2)
        output.write('\n')
    print(json.dumps({k:v for k,v in result.items() if k not in ('identities','color_patches')}))
    if corrupt:
        raise ValueError('decoded markers are corrupt; cadence estimates are invalid')


if __name__ == '__main__':
    main()

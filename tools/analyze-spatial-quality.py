#!/usr/bin/env python3
"""Measure sampled scene-frame RGB error against independent fixture equations.

Run after analyze-recorder.py. This is a synthetic spatial fidelity diagnostic,
not a perceptual-quality score or a performance measurement. Only the decoded
frame identities select the source images; timestamps do not stand in for pixels.
"""
import argparse
import json
import math
import statistics
import subprocess
from pathlib import Path


def reference(width: int, height: int, pattern_width: int, identity: int) -> bytearray:
    # Written independently from the C producer: RGB output, visible width only.
    bars = [(255,255,255), (255,255,0), (0,255,255), (0,255,0),
            (255,0,255), (255,0,0), (0,0,255), (0,0,0)]
    top = b''.join(bytes(bars[x*8//pattern_width]) for x in range(width))
    gray = b''.join(bytes([(x+identity) % 256])*3 for x in range(width))
    bottom = []
    for phase in range(19):
        row = bytearray(gray)
        for remainder in (0,1):
            for x in range((remainder-phase) % 19, width, 19):
                row[x*3:x*3+3] = b'\0\0\0'
        bottom.append(row)
    stride = width*3
    image = bytearray(stride*height)
    for y in range(height):
        row = top if y < height//2 else gray if y < height*3//4 else bottom[(y+identity) % 19]
        image[y*stride:(y+1)*stride] = row
    left, right = max(0,width//2-32), min(width,width//2+32)
    marker = bytes([255 if identity % 2 else 0])*((right-left)*3)
    for y in range(max(0,height//2-32), min(height,height//2+33)):
        image[y*stride+left*3:y*stride+right*3] = marker
    for y in range(16):
        for bit in range(32):
            value = 255 if bool((identity >> bit) & 1) != (y >= 8) else 0
            image[y*stride+bit*24:y*stride+(bit+1)*24] = bytes([value])*24
    return image


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('directory', type=Path)
    parser.add_argument('--samples', type=int, default=16)
    parser.add_argument('--identities', type=int, nargs='+', help='compare these exact source frames')
    parser.add_argument('--label', default='spatial')
    args = parser.parse_args()
    if args.samples < 1:
        parser.error('samples must be positive')
    if not args.label.replace('-','').replace('_','').isalnum():
        parser.error('label must be a simple filename component')
    root = args.directory.resolve()
    source = json.loads((root/'producer.json').read_text())
    pictures = json.loads((root/'pictures.json').read_text())
    width, height = source['width'], source['height']
    identities = pictures['identities']
    if (width < 256 or height < 64 or source['pattern_width'] < width or not identities
            or pictures['corrupt_markers'] or pictures['backward_transitions']):
        raise ValueError('input is outside this scene oracle domain')
    frames = [0] if args.samples == 1 else sorted({
        i*(len(identities)-1)//(args.samples-1) for i in range(args.samples)
    })
    if args.identities is not None:
        if args.identities != sorted(set(args.identities)):
            parser.error('source identities must be distinct and increasing')
        frames = [identities.index(identity) for identity in args.identities]
    matrix = {'bt709': 'bt709', 'bt2020nc': 'bt2020'}[pictures['color_matrix']]
    input_range = {'tv': 'tv', 'pc': 'pc'}[pictures['color_range']]
    select = '+'.join(f'eq(n\\,{i})' for i in frames)
    # Both filter inputs use explicit RGB full range and the same sample clock.
    # No range-dependent YUV PSNR values are compared across profiles.
    graph = (
        f'[0:v]select={select},scale=flags=accurate_rnd+full_chroma_int:'
        f'in_color_matrix={matrix}:in_range={input_range}:out_range=pc,'
        'format=gbrp,setparams=range=full,settb=1/1,setpts=N[decoded];'
        '[1:v]format=gbrp,setparams=range=full,settb=1/1,setpts=N[reference];'
        '[decoded][reference]psnr=shortest=1:repeatlast=0:eof_action=endall,'
        f'metadata=mode=print:file={args.label}-frames.txt'
    )
    if any((root/f'{args.label}{suffix}').exists() for suffix in ('.json','.log','-frames.txt')):
        raise FileExistsError('spatial evidence already exists')
    command = ['ffmpeg','-hide_banner','-nostdin','-v','info','-i','clip.mkv',
               '-f','rawvideo','-pixel_format','rgb24','-video_size',f'{width}x{height}',
               '-framerate','1','-color_range','pc','-i','pipe:0',
               '-filter_complex',graph,'-frames:v',str(len(frames)),
               '-an','-fps_mode','passthrough','-f','null','-']
    with (root/f'{args.label}.log').open('x') as log:
        with subprocess.Popen(command, cwd=root, stdin=subprocess.PIPE,
                              stdout=subprocess.DEVNULL, stderr=log) as process:
            for frame in frames:
                process.stdin.write(reference(width,height,source['pattern_width'],identities[frame]))
            process.stdin.close()
            if process.wait():
                raise RuntimeError(f'spatial comparison failed; see {args.label}.log')
    mse = [float(line.split('=',1)[1]) for line in (root/f'{args.label}-frames.txt').read_text().splitlines()
           if line.startswith('lavfi.psnr.mse_avg=')]
    if len(mse) != len(frames) or any(not math.isfinite(value) or value < 0 for value in mse):
        raise ValueError('spatial comparison did not account for every requested frame')
    average = statistics.mean(mse)
    result = {'sampled_frames': frames, 'source_identities': [identities[n] for n in frames],
              'rgb_mse': average, 'rgb_psnr_db': 10*math.log10(255**2/average) if average else None,
              'exact_match': not average, 'worst_frame_mse': max(mse), 'frame_mse': mse,
              'matrix': pictures['color_matrix'], 'range': pictures['color_range']}
    with (root/f'{args.label}.json').open('x') as output:
        output.write(json.dumps(result,indent=2)+'\n')
    print(json.dumps(result))


if __name__ == '__main__':
    main()

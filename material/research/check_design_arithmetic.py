"""Small design probes; these are not a recorder implementation or GPU tests.

Checks numerical examples, rational timing, and the rotating-surface damage
invariant against a full-image reference. A deliberately broken incremental
method must fail so that a vacuous oracle cannot pass the suite.
"""
from fractions import Fraction
from itertools import product
import json
import random
from pathlib import Path

def footprints(w, h):
    # Even-sized tightly packed layouts only; modifiers/alignment are excluded.
    assert w % 2 == 0 and h % 2 == 0
    return {"rgbx_bytes": w*h*4, "nv12_bytes": w*h*3//2,
            "p010_bytes": w*h*3, "rgba16f_bytes": w*h*8}

def timing():
    rows = []
    for fps in [Fraction(60), Fraction(240), Fraction(60000,1001), Fraction(240000,1001)]:
        # Full day. Rounding each absolute timestamp avoids accumulated drift.
        n = int(86400*fps)
        t = Fraction(n*1_000_000_000, 1)/fps
        absolute_ns = (t.numerator + t.denominator//2)//t.denominator
        period = Fraction(1_000_000_000,1)/fps
        rounded_period = (period.numerator + period.denominator//2)//period.denominator
        assert abs(Fraction(absolute_ns)-t) <= Fraction(1,2)
        rows.append({"fps":str(fps),"frames":n,
                     "absolute_rounding_error_ns":float(Fraction(absolute_ns)-t),
                     "repeated_period_error_ns":float(Fraction(n*rounded_period)-t)})
    # Matroska signed16 relative block time. Positive span only is used here.
    spans = {str(scale):32767*scale/1e9 for scale in [1_000_000,100_000,10_000,1_000]}
    return rows, spans

def full_chroma(src):
    # Independent reference recomputes every 2x2 average from the full image.
    return tuple(sum(src[y*4+x] for y in range(by,by+2) for x in range(bx,bx+2))//4
                 for by in (0,2) for bx in (0,2))

def update_slot(slot, src, source_dirty):
    out = list(slot)
    affected = {(i//4//2)*2 + (i%4//2) for i in source_dirty}
    for j in affected:
        bx, by = (j%2)*2,(j//2)*2
        # Explicitly sum the exact contributors of each impacted output cell.
        out[j] = (src[by*4+bx] + src[by*4+bx+1] +
                  src[(by+1)*4+bx] + src[(by+1)*4+bx+1])//4
    return tuple(out)

def damage_probe():
    # 3 rotating output slots; arbitrary skipped submissions; producer damage
    # always consumed even for skipped submissions. Source is a complete image.
    rng = random.Random(20261019)
    src = [0]*16
    stamps = [0]*16
    slots = [full_chroma(src)]*3
    generations = [0]*3
    checked = skipped = 0
    for epoch in range(1, 20_001):
        changed = set(rng.sample(range(16), rng.randint(1,4)))
        for p in changed:
            src[p] = rng.randrange(256)
            stamps[p] = epoch
        if rng.randrange(4) == 0:
            skipped += 1
            continue
        j = rng.randrange(3)
        # Union of all damage newer than this particular destination's image.
        pending = {p for p in range(16) if stamps[p] > generations[j]}
        slots[j] = update_slot(slots[j],src,pending)
        generations[j] = epoch
        assert slots[j] == full_chroma(src)
        checked += 1
    # Broken control: reuse slot0 after slot1, updating just last-frame damage.
    src = [0]*16
    old0 = full_chroma(src)
    src[0] = 100
    _slot1 = full_chroma(src)
    src[15] = 200
    broken = update_slot(old0,src,{15})
    oracle = full_chroma(src)
    assert broken != oracle
    return {"seed":20261019,"source_updates":20000,"checked_submissions":checked,
            "skipped_submissions":skipped,"broken_control_detected":True,
            "broken_control_output":broken,"expected":oracle,
            "scope":"Scalar 4x4 source and 2x2 box-filter outputs; no GPU, transfer function, or driver validation."}

def information_loss_probe():
    # A 2x2 chroma checker and its spatial permutation have identical 420 result.
    a = (0,255,0,255)
    b = (255,0,255,0)
    assert a != b and sum(a)==sum(b)
    return {"distinct_chroma_fields":[a,b],"box_subsampled_value":sum(a)/4,
            "conclusion":"Increasing encoder bitrate cannot invert a many-to-one chroma subsampling operation."}

def limited_range_probe():
    # ITU BT.709 black/white endpoints with specified half-up quantization.
    def q(v): return (v.numerator*2+v.denominator)//(v.denominator*2)
    assert q(Fraction(64))==64 and q(Fraction(940))==940
    endpoints = {"10bit_black_Y":64,"10bit_white_Y":940,"10bit_neutral_C":512,
                 "10bit_chroma_min":64,"10bit_chroma_max":960,
                 "p010_black_word":64<<6,"p010_white_word":940<<6}
    assert all((endpoints[k]&63)==0 for k in ("p010_black_word","p010_white_word"))
    return endpoints

def main():
    f4 = footprints(3840,2160)
    rates = {"4k240_rgbx_to_p010_min_read_write_GBps":(f4["rgbx_bytes"]+f4["p010_bytes"])*240/1e9,
             "4k240_extra_rgbx_copy_read_write_GBps":f4["rgbx_bytes"]*2*240/1e9,
             "100Mbps_output_MBps":100/8,
             "1Gbps_output_MBps":1000/8,
             "16_frame_lookahead_at_60_ms":16/60*1000,
             "16_frame_lookahead_at_240_ms":16/240*1000}
    assert abs(rates["4k240_rgbx_to_p010_min_read_write_GBps"]-13.934592)<1e-12
    ts, spans = timing()
    result = {"status":"PASS","footprints_4k":f4,"rates":rates,"timing":ts,
              "matroska_positive_relative_span_seconds_by_scale_ns":spans,
              "damage":damage_probe(),"information_loss":information_loss_probe(),
              "range":limited_range_probe(),
              "scope":"Analytical arithmetic and deterministic abstract data reconstruction only. No measured recorder performance."}
    path = Path(__file__).with_name("design_arithmetic_results.json")
    path.write_text(json.dumps(result,indent=2)+"\n")
    print(json.dumps(result,indent=2))

if __name__ == "__main__": main()

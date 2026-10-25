# Conditional memory targets

These targets model the dense image, bitstream, and replay payload storage for
`gpuKey`'s two-reference AV1 recording pipeline. They supplement
[`README.md`](../README.md) and [`docs/validation.md`](validation.md), and are
not claims that any single configuration is universally optimal across all
workloads.

## What can be calculated

Resolution and codec alone do **not** determine a unique minimum allocation.
AV1 permits different reference structures; input formats, concurrency, driver
scratch, tiling and replay bitrate also matter. Static content can share an
unchanged input. Changing content cannot generally do so. An all-intra encoder
would have different memory and compression objectives. It is not a substitute
for the current two-reference AV1 plan in these comparisons.

For width `W`, height `H`, and 4:2:0:

```
YUV samples = W*H + 2*ceil(W/2)*ceil(H/2)
RGBX8       = 4*W*H bytes
NV12        = YUV samples bytes
P010        = 2*YUV samples bytes
packed b-bit sample payload = ceil(b*YUV samples/8) bytes
```

The last expression is an information-storage comparison. P010 actually stores
each ten-bit sample in a sixteen-bit word; it cannot use the packed number
without changing the representation and supported GPU operations. All image
numbers below exclude pitch/height alignment, modifiers, compression metadata
and firmware storage. They concern dense, fully materialized images, not a
codec-wide information-theoretic minimum over every possible architecture.

The material's bandwidth/storage and ownership sections give the same NV12,
P010 and RGBX arithmetic and require separate accounting of source, input,
reference and output lifetimes. The pinned FFmpeg VAAPI AV1 implementation
uses at most two past reference images. On the RX 9070 XT, Mesa's VCN 5 capability
path advertises two L0 references; the no-B-frame configuration uses those two.
`vaapi_encode_av1_init_picture_params` fills only their actual VA references.
Eight AV1 reference slots do not imply eight distinct allocated pictures here.

## VRAM target

Let `C` be distinct capture buffers, `D` distinct materialized converted inputs
and `R` past reference images. Queue capacity is not the same as `D`: when
completion retires an input before the next conversion, its backing is reusable
while spare surface handles remain available for overlap. Under ordered reuse
after the last encoder read, an image
payload target for this native pipeline is:

```
shared capture payload = C * RGBX8
private image payload  = (D + R + 1) * NV12_or_P010
pipeline union         = shared capture payload + private image payload
```

The extra reconstruction is the current picture. This assumes the consumed
oldest reference can be reused for a later ordered encode. Mesa's VA AV1 path
evicts unlisted DPB entries and reuses their backing buffers. A backend that
instead requires distinct reservations for every in-flight reconstruction needs
`R + encode_depth` reconstruction images; that additional cost must be counted,
not hidden in a claim about the codec. FFmpeg's retained VA surface handles are
not themselves evidence that every handle has a separate live GPU allocation.

For the reported **2560×2880, AV1 8-bit 4:2:0**, with two RGBX capture buffers,
one reusable converted input and two past references:

| Component | Bytes | MiB |
|---|---:|---:|
| One tightly packed RGBX image | 29,491,200 | 28.125 |
| One tightly packed NV12 image | 11,059,200 | 10.546875 |
| Two capture images, counted once across processes | 58,982,400 | 56.25 |
| One reusable converted input | 11,059,200 | 10.546875 |
| Two past reconstructions plus the current reconstruction | 33,177,600 | 31.640625 |
| **Private image payload target** | **44,236,800** | **42.1875** |
| **Pipeline image payload target** | **103,219,200** | **98.4375** |

This is the revised target when completed inputs can be reused between captures,
including with depth-two capacity. Two simultaneously materialized inputs instead
require **108.984375 MiB** of image payload, of which **52.734375 MiB** is private.
The implementation retains the second surface and materializes it when overlap
requires it; saturated cases must still account for that allocation. Forcing
depth one serializes conversion/encoding and is not an unconditional throughput
improvement. The opt-in direct-RGB AV1 path
has an **87.890625 MiB** image target *if* it keeps the same two
source buffers and reference count. Longer source leases could require another
RGBX buffer and erase that advantage. This is a conditional payload target,
not a universal minimum or a guarantee of lower process memory than VAAPI VPP.

A compatible producer that directly supplies correctly converted NV12 could
also eliminate the RGB capture images and the separate converted-input pool.
With two such source buffers and three reconstructions its conditional target
is **14.831543 MiB at 1920×1080**, or **52.734375 MiB at 2560×2880**. This is an
upstream representation/lifetime target, not a capability of the current Niri
RGB-only offer or the current capture implementation. Its source leases, color
conversion and static-repeat ownership would need independent validation.
Keep this alternative open rather than treating RGB intermediates as a codec
requirement.

The installed Niri 26.04 source confirms that restriction: its
`src/screencasting/pw_utils.rs::make_video_params` offers BGRA/BGRx with
ARGB8888/XRGB8888, without an NV12 alternative. The inspected source is
`/nix/store/bazsf2g3j9imcrqrw18p3zn9911s7lfm-source`, traced from the running
binary's derivation. Reaching the 52.734375 MiB producer-native target therefore
requires changing the producer/capture interface; selecting another current
recorder flag cannot achieve it. The live compositor was not modified.

| Resolution | 8-bit native, D=2 | 8-bit completed-input reuse, D=1 | 10-bit P010 native, D=2 |
|---|---:|---:|---:|
| 1920×1080 | 30.651855 MiB | 27.685547 MiB | 45.483398 MiB |
| 2560×2880 | 108.984375 MiB | 98.437500 MiB | 161.718750 MiB |
| 3840×2160 | 122.607422 MiB | 110.742188 MiB | 181.933594 MiB |

These are **payload floors under the stated lifetime/representation model**.
The engineering target is to approach them plus individually justified platform
costs, and to investigate every remaining allocation. They are not hard process
memory caps. Actual allocations may exceed them; static content may require
fewer distinct images. No downscaling, lower bit depth, dropped references or
quality reduction is allowed to masquerade as closing the gap.
These targets place the working images in GPU-local memory. Moving them to GTT
changes the RAM, bandwidth and latency costs; it does not remove their storage.
No positive physical-VRAM minimum follows from AV1 alone when such placement
changes are unconstrained.

The workspace audit separates another fixed implementation cost from those image
targets. In the tested Mesa AV1 tier-2 CQP configuration, the SDB payload is
947,200 bytes, default CDF 81,920 bytes, and session storage 131,072 bytes. The
previous slab classes reserved 3 MiB for those three single-owner resources.
Dedicated, page-rounded allocations need 1,163,264 bytes (1.109375 MiB), saving
1.890625 MiB without reducing any payload. The CDF/session pair needs
1.15625 MiB including its independently allocated SDB; a single whole workspace
uses 2 MiB to retain large-page eligibility. These are audited driver layouts,
not codec-wide lower bounds. Pre-encode has additional size-dependent storage.

At portrait/60, the dedicated layout measures 103.433594 MiB of recorder-visible
VRAM, including 57.5 MiB shared with the producer. The observed image allocations
total 100.390625 MiB: two 28.75 MiB source images, one 11.25 MiB converted image
and three 10.546875 MiB reconstructions. Thus 1.953125 MiB of the gap above the
98.4375 MiB dense target is image layout/alignment, and 3.042969 MiB is other
observed storage. Those remaining costs stay in the audit; they are not silently
reclassified as an unavoidable codec minimum. Sampled GTT is 23.347656 MiB.

The capacity audit distinguishes active overlap from retained spare allocations.
After an induced backlog, the prior policy retains another 11.25 MiB input image
in VRAM and another 21.15625 MiB coded-output buffer in GTT until shutdown.
Opt-in `--gpu-memory compact` releases unused capacity after submitted work drains,
returning to approximately **103.129 MiB VRAM and 23.348 MiB GTT** in that test.
It preserves the latest image, prepared input, external references and depth-two
capacity. It does not lower the simultaneous-overlap payload target or establish
a lower instantaneous peak. The 98.4375 MiB conditional target remains active.
Restoring capacity has allocation and latency costs; mixed comparisons keep
`retain` as the default. RSS, GPU-local VRAM, GTT and shared source storage must
remain separately accounted; summing process RSS with GTT can double-count pages.

The cross-recorder audit confirms that source-buffer negotiation is part of this
target. Under the same two-to-eight-buffer offer at 2560×2880, gpuKey requests two
source images (57.5 MiB allocated) while the inspected GSR requests no count and
receives eight (230 MiB). Its imported-image count varies independently. These
are measured fixture policies, not universal compositor requirements. Fixed
two-buffer comparisons preserve the same source payload for both recorders.
The latest forty-second replay pair measures 103.434/113.715 MiB recorder-visible
VRAM for gpuKey/GSR, both including 57.5 MiB shared, with 23.348/71.305 MiB GTT.
One long run per program does not establish a peak bound or general superiority.
The conditional 98.4375 MiB image target remains unchanged.

The direct-RGB Vulkan candidate is now implemented and tested with two source
allocations. At portrait60, final sampled VRAM is **91.554688 MiB**, including the
same 57.5 MiB of shared source storage. Its three reconstruction allocations are
11,087,872 bytes each, or 31.722656 MiB together. Actual images therefore total
**89.222656 MiB**: 1.332031 MiB above the dense 87.890625 MiB target. The remaining
approximately 2.332031 MiB is other observed storage and stays in the audit.
Command pools are created on demand; the unused download pool is removed. Merely
reserving three rather than nine video-session slots did not reduce observed
allocation, so that declaration is not counted as a memory saving.

The latest image remains leased for static repeats. The native tests verify
progress with two capture buffers; other producers and workloads can differ.

The optional full-range conversion profile uses the same two RGB inputs and
three 8-bit reconstruction images. Its model/range selector occupies one byte of
existing padding in the 24-byte x86-64 capability state. It adds no image, GPU
pass or packet field, so the conditional 87.890625 MiB payload target is unchanged.
Encoded history size remains content-dependent and must be measured separately.

Gallium encode-time processing remains an open alternative to the Vulkan route.
The first candidate had higher RSS and slightly higher constant-color error
than the VAAPI reference. The ACO-only driver removes the RAM regression in the
tested workloads; the color-precision difference remains. These costs are not
hidden in the VRAM improvement.

The static-key cache permits another conditional target: **zero unused coded
output buffers during an eligible static hold**, while retaining their full
capacity for subsequent allocation. With `--gpu-memory compact --static-interval 2`,
the tested portrait recorder returns 21.15625 MiB of GTT after key reuse takes
over. Sampled GTT falls from 23.242188 to 2.085938 MiB for RGB and from 23.347656
to 2.191406 MiB for VAAPI. Resume restores the allocation. This changes idle
retention, not the active capacity bound or the VRAM image targets. The remaining
pool metadata is retained; resume allocation and latency costs need separate
measurement.

## System RAM target

The RGB dependency audit found 25.180 MiB resident in an unused LLVM compiler
library. Building the same RADV version with ACO and without LLVM removes that
runtime dependency. The recorder executable and the tested encoded packets are
unchanged. Three-second mapping walks report RSS/PSS of 53.387/44.449 MiB with
LLVM, 27.000/18.014 MiB without it, and 38.586/20.558 MiB for VAAPI. These are
diagnostic observations, not atomic snapshots or peak bounds; PSS also depends
on other processes sharing those mappings.

In the fresh forty-second portrait replay comparison, ACO-only RGB peaks at
34.809 MiB RSS, versus 60.562 MiB for the earlier RGB driver and 45.844 MiB for
VAAPI. This removes an implementation dependency cost; it does not establish an
RSS minimum from the codec or screen dimensions. Raw CPU pixel storage remains
zero, encoded history remains content-dependent, and the 87.890625 MiB direct
image-payload target is unchanged.

The command-resource audit removes the unconditional 64 KiB hint array from
each command stream. The selected adaptive caches belong to the two command
contexts. For a context whose largest successful working set is `B` buffer
entries across all three lists, it needs no table while `B <= 8`; above that,
its nominal table payload is `2 * min(32768, bit_ceil(4*B))` bytes. Retained
capacity follows the high-water mark, not the current frame. Arithmetic here
describes the policy; implementation growth is bounded before multiplication.
Optional allocation failure may leave a smaller cache or linear lookup.

Both contexts together therefore reserve from zero through 128 KiB of table
payload, plus their pointer/mask bookkeeping. The bound is a cache policy, not a
limit on buffers. The original shared table used 64 KiB but failed the ownership
test. The independently owned fixed-table control uses 128 KiB. In the extracted
model, two 32-buffer contexts use 512 bytes of adaptive tables; exact per-list
maps use 256 or 512 bytes depending on list distribution. At 33,024 buffers per
context, the adaptive tables use 128 KiB and the exact maps use 512 KiB. Actual
whole-process RSS remains mixed in native tests; these are table payload counts,
not measured RSS reductions. This change does not reduce GPU image storage.

The header audit removes six persistent 2,856-byte raw OBUs, their count/alignment
and a 48-byte fragment record from the VAAPI AV1 context, replacing them with
32 bytes of actual metadata payload/flags. On the inspected x86-64 ABI the
derived persistent-context reduction is **17,160 bytes**, before allocator
rounding. A raw OBU now occupies 2,856 bytes of temporary stack storage during
its synchronous call; that transient cost must still be counted. Sequence state
remains owned by CBS, and required reference history is preserved.

The encoder also avoids its generic CBS writer's 1 MiB allocation, which was
mostly unused for short headers. This removes a reservation, not a demonstrated
1 MiB RSS saving: page commitment, allocator behavior and first-picture parsing
have separate costs. Unit/fragment buffers and unused redundant-header copies
are eliminated from the header path. Whole-process RSS remains workload-dependent
and mixed in the measurements. These changes do not reduce VRAM or the required
coded-output capacity.

The selected CPU converter's optional row cache is
`24 * round_up(ceil(width/2), 32)` bytes: 23,040 bytes at 1920 pixels and
30,720 bytes at 2560 pixels. Four rows follow from the vertical filter's support;
three planar uint16 channels retain exact horizontal sums. Its size is independent
of image height, and allocation failure selects the equivalent scratch-free
kernel. These are implementation costs, not codec lower bounds. CPU input still
has its output staging/upload lifetime; eliminating that intermediate remains an
open producer/consumer optimization.

First-picture metadata validation and replay key validation borrow encoded input
only when its owning buffer extent and zero padding are verified. The caller
retains it through parser teardown. Other inputs keep the parser's copying path,
including opaque mapped packet owners that cannot prove this extent. This avoids
unnecessary full-key copies without introducing an unchecked lifetime assumption.

Vulkan pooled buffers now need an owner whose lifetime spans every outstanding
buffer, including packets retained beyond codec teardown. Its x86-64 payload is
32 bytes (one device-reference pointer and three destruction function pointers),
plus a 24-byte AVBufferRef, once per pool. This is cold ownership metadata; no
per-packet field or GPU allocation is added. Keeping the entire encoder/filter
context alive would retain unrelated properties, queues and codec resources.
The small owner retains exactly the device and dispatch needed for destruction.

The native path's target for CPU copies of raw video is **zero bytes**. It
already processes imported GPU images without framebuffer readback. RAM for
encoded replay is content dependent even at fixed resolution, FPS and quantizer.

The RADV command writer now keeps a pending 32-bit word and its current fragment
length in eight aligned bytes. The position is derived from that length modulo
32; the consumer takes the length at flush. Removing the byte-index, unused
emulation-prevention fields, unused size counter and separate position reduces
`radv_enc_state` from 72 to 48 bytes on x86-64. The conventional-accumulator
comparison profile uses 56 bytes. These private object sizes are not measured
RSS savings or changes to video-image allocation. The default driver's text
section is 2,944 bytes smaller than the frozen byte-writer reference; an examined
inline variant added code and was not selected by those isolated timings alone.

Eligible static independent-key reuse adds one lazy `AVPacket`/`AVBufferRef`
cache and fixed media bookkeeping, not another raw image or reconstruction.
Replay references payload already retained by its budgeted history. Ordinary
Vulkan file recording instead copies one encoded key plus decoder padding into
CPU storage so that the cache does not pin a large mapped GPU output buffer.
That payload-sized RAM cost is separate from the saved encoding work; use
`--static-encoding always` to retain the previous policy at the same cadence.
The existing raw last-image pin is retained for correct fallback and resume.

Replay export compaction rewrites the existing per-stream index. It adds one
spare eight-byte video index and constant terminal-timestamp bookkeeping, with
no growth of the 32-byte live packet record. A terminal independent sample can
add one encoded payload to a one-picture clip for consumer compatibility;
compacting a longer identical-key run instead removes repeated file payloads.
Neither change is a new resolution-only RAM floor or a universal bitrate win.

For unchanged output bytes, the no-deduplication control stores the sum of
retained packet sizes, including the preceding independent keyframe and required
audio preroll. This is a concrete baseline target, not an information-theoretic
minimum: repeated byte-identical packet payloads can share storage while keeping
their original timing and flags. General lossless compression is another
content-dependent alternative. A bitrate estimate for the uninterned payload is:

```
payload bytes = aggregate encoded bits/second * retained seconds / 8
```

At an **illustrative**, unmeasured 5 Mbit/s, your 30-second window represents
18,750,000 bytes (17.881393 MiB), or 20,000,000 bytes (19.073486 MiB) with two
seconds of actual leading footage. A 300-second window represents 187,500,000 bytes
(178.813934 MiB). Including two seconds of actual leading footage raises this
to 188,750,000 bytes (180.006027 MiB). At 10 Mbit/s those numbers double.
The nominal keyframe interval is not a universal upper bound on leading footage:
static repeat settings, delivery gaps and scheduling affect actual key spacing.
Audio contributes to aggregate bitrate and also needs its codec/resampler state.

Target one copy per distinct immutable payload when exact equality and lifecycle
accounting make sharing worthwhile; measure its CPU/index cost against direct
packet adoption. Never substitute a merely similar compressed packet.
The current implementation shares consecutive exactly equal video keys, with a
24-byte owner descriptor per distinct key run and the existing 32-byte packet
records. It does not globally intern all equal packets. Independent reference
and copy controls remain selectable; this is progress toward the target, not
achievement of a universal minimum.
Add packet timing/index metadata, codec
configuration, runtime state and allocator overhead explicitly. Export can pin
old history while new history arrives; this is a separate lifetime cost, not a
reason to copy all payload at save time. The current 256 MiB live replay budget
is a user policy, **not** a theoretical minimum or an assurance that 300 seconds
at arbitrary CQP bitrate fits.

GPU-addressable system memory (GTT) must also be reported. The pinned FFmpeg
VAAPI encoder reserves `3*coded_width*coded_height + 65536` bytes for each coded
output buffer, with default AV1 dimensions rounded to 64. For 2560×2880 this is
**21.15625 MiB each**, or **42.3125 MiB at depth two**, before driver overhead.
That is an implementation reservation, not an AV1 lower bound or a proven
worst-case bitstream theorem; the source labels its sizing rule an assumption.
Shrinking it to an observed average packet size is not a safe optimization.
Collecting completed packets before the next encode can instead reuse **one**
coded buffer, targeting **21.15625 MiB** of this reservation at the reported
resolution without shrinking its capacity. Overlapping work can still require
two. Completion descriptors and the VA export protocol do not retain replay
payloads in these buffers: normal VA mapping and FFmpeg packet copying finish
before the buffer returns to its pool.
The final direct-RGB portrait replay uses a 21.15625 MiB coded buffer and samples
23.242188 MiB GTT overall. File recording can now finalize a pending packet at
the next input timestamp when all prior inputs have been collected, permitting
the same output-buffer reuse before another encode. Final portrait file tests
also sample 23.242188 MiB GTT, compared with 44.398438 MiB in the earlier candidate.
In-flight work and muxer retention can still need additional buffers. The generic
calculator's coded-buffer sizing remains the documented VAAPI rule; Vulkan uses
its queried coded-image and bitstream-size alignments, which coincide with that
number for this portrait case.

GTT uses system RAM. Do not blindly add GTT to process RSS, which may include
mapped backing, shared libraries and other differently attributed pages.

The mapping audit identifies a separate avoidable GTT allocation: eager CPU-image
probes during GPU frame-pool construction. Deferring that work to actual mapping
reduces the sampled portrait/60 GTT peak from 26.86328125 to 23.34765625 MiB in
the matched four-run series. The 21.15625 MiB coded-buffer reservation is unchanged.
This removes unused work; it does not shrink the encoder's output capacity or
move capture images into unreported producer storage. The remaining GTT gap and
VRAM alignment/workspace costs still need individual accounting.

## Direct-RGB standby after an independent key

Once an independent key replaces the prediction history, no issued encode is
pending, and static presentation comes from its retained CPU packet, one live
reconstruction is sufficient until real input resumes. Retaining the producer's
same `C` source buffers gives a different, conditional standby target:

```
direct-RGB standby image payload = C * RGBX + 1 * NV12_or_P010
one unused reconstruction adds   = 1 * NV12_or_P010
```

At **2560×2880, 8-bit**, these dense-image targets are **66.796875 MiB** with no
unused reconstruction or **77.34375 MiB** with one. They do not replace the
**87.890625 MiB** moving-image target for the same two-reference schedule. The
driver's actual reconstruction allocation is **11,087,872 bytes** here, so
reclaiming two unused images returns **21.1484375 MiB**. The initial real-recorder
probe measures **91.5546875 → 70.40625 MiB** VRAM and restores 91.5546875 MiB
when motion returns. Shared capture storage remains 57.5 MiB; it has not been
removed from the accounting. Fresh confirmation is recorded in the validation log.

The remaining static gap above the dense target is **3.609375 MiB**: 1.25 MiB
capture alignment, 0.02734375 MiB reconstruction alignment, and 2.33203125 MiB
of other observed storage whose complete attribution remains open. Session
memory remains bound.
A stream that starts static already grows only one reconstruction; this change
addresses retained capacity after motion. New image allocation and initial-layout
work on resume are real costs, and the one-unused-image alternative is measured
separately. `tools/memory-targets.py` prints both conditional standby targets.

## VAAPI standby input

With an independent CPU key owning static presentation, compact VAAPI can also
release its last converted input image. Keeping its existing reconstruction
capacity gives `C*RGBX + (R+1)*NV12_or_P010` dense bytes during standby. At the
portrait 8-bit configuration this is **87.890625 MiB**; it removes one
**10.546875 MiB** dense input from the active model. The actual input allocation
returned in the native probe is **11.25 MiB**, with recorder-visible VRAM after
motion falling **103.43359375 → 92.18359375 MiB**. Both shared source allocations
remain counted. This does not reclaim the VA driver's unused reconstruction
capacity or establish a codec-wide minimum.
The measured standby footprint remains **4.29296875 MiB** above this dense
model; that remainder includes alignment and driver storage. It is a target
gap to investigate, not evidence that all of those bytes can be removed.

Direct RGB has no separate converted input to free. Returning its latest source
lease earlier improves buffer availability while leaving the cached import and
producer allocation resident. The CPU key and required references remain owned.
Restoration on new input, optional cache setup and fatal later publication
failure have separate tests/costs.

## Uploaded GPU input and CPU/GTT staging

On the explicit CPU conversion/upload path (`--shm`), capture buffers reside in
shared host memory rather than GPU VRAM. For VAAPI, one converted input and
`R+1` reconstructions give `(1+R+1)*NV12_or_P010` dense active VRAM bytes
(**42.1875 MiB** at 2560×2880 8-bit, **84.375 MiB** at 10-bit). For uploaded
Vulkan, preserving `m->last` alongside one writable next-input spare without
per-frame allocation churn uses two converted inputs plus `R+1` queue-owned
reconstructions, or `(2+R+1)*NV12_or_P010` dense active VRAM bytes
(**52.734375 MiB** at 2560×2880 8-bit, **105.46875 MiB** at 10-bit), plus one
`NV12_or_P010` CPU conversion staging frame (**10.546875 MiB** at 8-bit,
**21.09375 MiB** at 10-bit).

In compact independent-key standby (`--gpu-memory compact --static-interval 2`),
releasing the converted input pool, unused reconstructions, temporary transfer
staging buffers and `m->staging` leaves one live GPU reconstruction
(**10.546875 MiB** dense VRAM at 2560×2880 8-bit, **21.09375 MiB** at 10-bit)
and **0 bytes** of CPU conversion staging or transfer GTT staging. In the
38-run confirmation and `smaps` series at 2560×2880:

- Uploaded Vulkan 8-bit active peak VRAM falls **166.742 → 56.852 MiB** and
  standby VRAM falls **166.734 → 13.184 MiB**; active peak GTT falls
  **90.173 → 33.793 MiB** and standby GTT falls **23.188 → 2.086 MiB**.
- Uploaded Vulkan 10-bit active peak VRAM falls **318.586 → 109.762 MiB** and
  standby VRAM falls **318.561 → 23.730 MiB**; active peak GTT falls
  **121.815 → 44.341 MiB** and standby GTT falls **44.281 → 2.086 MiB**.
- Uploaded VAAPI 8-bit and 10-bit peak/resumed GTT fall by **21.15625 MiB**
  (reusing one coded output buffer after pre-send collection), while standby
  `smaps` anonymous RAM falls by **10.551 MiB** (8-bit) and **21.102 MiB**
  (10-bit) on VAAPI and **10.602 MiB** (8-bit) and **21.152 MiB** (10-bit) on
  Vulkan when `m->staging` is released.
- `tools/memory-targets.py` reports these `conditional_uploaded_gpu_targets`
  alongside the existing native DMA-BUF targets.

The surface-owned VAAPI DPB profile (`vaapi-dpb-amd`) preserves cross-surface
reuse while letting idle surface destruction reclaim cached reconstructions.
Once the independent key completes, its conditional image-payload target is
`C*RGBX + 1*NV12_or_P010`, the same **66.796875 MiB** portrait/8-bit standby
target as the direct-RGB path. The moving VAAPI target is still **98.4375 MiB**;
it still needs its converted input during motion.

The revised prototype and 50-run confirmation series measure
**92.18359375 → 71.08984375 MiB** standby VRAM, with the active peak near
103.44 MiB. The native allocation trace identifies two **11,059,200-byte**
reconstructions restored on resume, matching the **21.09375 MiB** standby
difference. The two source images remain counted. The remaining standby gap
above the **66.796875 MiB** dense model is **4.29296875 MiB**.
`tools/memory-targets.py` reports the old retained-capacity target and this
surface-owned target separately.

Keeping one live GPU reconstruction is a policy, not a universal lower bound.
Closing the encoder while retaining the CPU key could target zero private encoder
images during a long static hold, but requires encoder initialization and a fresh
keyframe on resume. Its latency, compression and failure costs remain unmeasured.

## Reproduce and verify

```
nix develop -c python3 tools/memory-targets.py
nix develop -c python3 tools/memory-targets.py --width 3840 --height 2160 --bits 10
nix develop -c python3 tools/memory-targets.py --inputs 2
nix develop -c python3 tools/memory-targets.py --inputs 0
nix develop -c python3 tools/memory-targets.py --bitrate-mbps 5 --replay-seconds 300 --preroll-seconds 2
```

The calculator defaults to one materialized input, depth-two capacity and the
recorder's 30-second history, and also prints the full-depth input target.
With no bitrate it deliberately leaves
replay payload unknown.
Its arithmetic was checked against the material's three 3840×2160 examples and
odd-dimension chroma rounding. Those checks establish arithmetic, not achievable
hardware allocations. Measured results and unresolved gaps belong in
`docs/validation.md`.

Memory sampling follows the [Linux DRM fdinfo specification](https://docs.kernel.org/gpu/drm-usage-stats.html):
deduplicate descriptors by device/client ID; distinguish total, resident,
shared and purgeable bytes. A shared image is real storage but not an extra
copy in each importing process. For a static stream, a recorder may import
fewer buffers than the producer allocated. Compare the union of recording
allocations, including the producer's pool, as well as process counters.
Sampling reports observed peaks and can miss shorter-lived allocations.

Audited implementation mechanisms: FFmpeg `hw_base_encode.c`,
`vaapi_encode_av1.c`, `vaapi_encode.c`, `hwcontext_vaapi.c`; Mesa 26.1.8/26.2.5
`si_get.c`, `picture_av1_enc.c`, `radeon_vcn_enc.c`, `surface.c`.
Their pinned source locations are recorded in `docs/dependencies.md`.

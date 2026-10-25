# Implementation evidence

Validated on 20–21 October 2026 (UTC), Linux 7.2.10, x86-64, AMD Radeon RX 9070 XT.
The other exposed AMD render node does not advertise AV1 encoding. The supplied
`material/` is unchanged; every entry in its SHA256 manifest was checked.

## Cross-recorder comparison: 23 October

The comparison now uses the same synthetic private portal/PipeWire source with
independent decoded picture identities. It compares clean GSR 6.1.4, then the
upstream snapshot `ee2c6ca1e03e4432652fe9492b3a405f654b45ac`, checked on 23 October
2026 after the 6.1.5 release. Both clean builds use GCC 16.2 and FFmpeg 9.0.2.
gpuKey uses its unchanged capacity-milestone executable and patched FFmpeg 9.0.2.
Both use the same Mesa hints driver and RX 9070 XT. The separate editable GSR
checkout was never modified. There is no new recorder hot-path change in this
continuation; the delivered change is reproducible evaluation and a more accurate
test producer. Earlier Samply/Tracy captures cover the unchanged recorder
implementation. The current package's executable is byte-identical to the measured
capacity-milestone executable; its new package hash also includes updated tests.

Settings are AV1 8-bit, native quantizer 32, no B pictures, limited BT.709 with
the source's sRGB transfer tag, a requested two-second key interval and the same
frame-rate cap. GSR uses content cadence; gpuKey uses source-timed variable rate.
The scene continuously changes. GSR's normal depth three and a depth-two control
are both tested; gpuKey retains depth two. Source offers either permit eight
buffers or constrain both recorders to two. Requested settings do not establish
identical output: cadence, color error and clip durations are measured separately.

The 6.1.4 confirmation has 45 rotating eight-second runs: three alternatives,
five cases, three repetitions. The fresh upstream-snapshot confirmation has 18
runs at portrait/60 and HD/240 with a two-buffer source. Each alternative occupies
each ordering position; case order reverses between rounds. Builds, profilers and
other deliberate benchmark jobs do not overlap timing. CPU is the complete child
process, including initialization and teardown. External control-client CPU is
not included. Values below are means ± sample SD, three runs each.

| Latest-snapshot case | gpuKey CPU ms | GSR depth 3 CPU ms | GSR depth 2 CPU ms | gpuKey / GSR depth 3 RSS MiB |
|---|---:|---:|---:|---:|
| 2560×2880 / 60 | 93.009 ± 18.631 | 155.639 ± 15.803 | 192.231 ± 3.105 | 37.901 ± 0.313 / 68.803 ± 0.339 |
| 1920×1080 / 240 | 179.955 ± 22.920 | 258.141 ± 15.688 | 267.469 ± 2.312 | 36.693 ± 0.241 / 67.309 ± 0.340 |

gpuKey's decoded identities have zero interior gaps, repeats or backward
transitions in these runs. GSR has zero repeats/backward transitions and averages
0.7–1.0 interior missing identities per run. Startup and shutdown account for
additional source pictures absent from the files; those are not silently counted
as interior drops. The source has no empty-buffer callbacks or coalesced ticks in
the latest long replay controls. Eight-second runs include initialization, so
their frame count is not a steady-state encoder throughput limit.

The constant-patch oracle uses independent BT.709 equations. gpuKey's mean
absolute error across eight Y/Cb/Cr patches is 0.184 code values, versus 1.232 for
the inspected GSR sources. A separate 6.1.4 control corrects only GSR's limited-range
matrix constants; its error falls to 0.434. Fresh paired tests still favor gpuKey
CPU/RSS: portrait/60 CPU 99.077 ± 2.904 versus 196.715 ± 12.119 ms; HD240 CPU
167.482 ± 6.310 versus 281.035 ± 18.204 ms. This does not prove identical spatial
filtering or full-scene rate-distortion quality. No universal quality ranking is
claimed from eight patches.

At portrait/60 the eight-buffer offer produces 230 MiB of exported source images
for GSR and 57.5 MiB for gpuKey. GSR imports images as used, so only a varying
subset appears in its process's shared VRAM counter. The controlled two-buffer
offer removes that source-capacity difference. Raw total/resident/shared DRM
counters and actual exported source allocations are retained separately. Shared
bytes must not be counted twice, and process-visible totals alone cannot establish
the complete capture pipeline's physical footprint.

Fresh forty-second replay runs use a two-buffer source, a thirty-second history,
and two saves near seconds 35 and 38. Both exports independently decode and start
with keyframes at PTS zero; packet PTS strictly increases. All ten saved clips
across the 6.1.4 and current-snapshot controls have no corrupt, repeated or
backward picture identities. Their durations are approximately 30.8–31.9 seconds,
including the preceding keyframe. The current-snapshot pair measures:

| Forty-second replay | gpuKey | GSR upstream, depth 3 |
|---|---:|---:|
| Complete recorder CPU | 416.047 ms | 746.827 ms |
| Peak RSS | 44.754 MiB | 76.160 MiB |
| Sampled VRAM peak | 103.434 MiB | 113.715 MiB |
| Shared VRAM at that peak | 57.500 MiB | 57.500 MiB |
| Sampled GTT peak | 23.348 MiB | 71.305 MiB |

These are single long confirmations, not estimates of variance. gpuKey submits
all 2,399 source pictures, reports zero skips/FPS limiting and saves both clips.
Live replay payload peaks at 8,091,514 bytes. The recorder stop request is timed
from process launch for both applications. This differs from the earlier gpuKey
fixture's forty-second media-duration setting; their frame counts must not be
treated as identical timing domains.

The 6.1.4 portrait/depth-two runs have approximately 9.26/9.28 ms of sampled video
engine busy time per decoded picture for gpuKey/GSR. Total busy time differs with
the number of encoded pictures. Engine counters are optional and sampling can
miss trailing work before close. They are neither energy measurements nor
capture-to-packet latency; graphics and compute scheduling also have different
interference costs. Game impact, full-scene quality, static/HDR/audio comparisons,
other GPUs and matched end-to-end latency remain open.

Verification passes GCC/Clang, application ASan/UBSan, all sixteen x86-64 Nix
checks and native sanitizer capture/audio/replay integration. A directed mock
response now matches the real portal implementation. External-source mode uses
nonzero allocation extents and complete damage metadata; zero-size DMA-BUF cases
remain in the gpuKey regression suite. Source progression no longer depends on a
consumer requesting Header metadata. Independent negative controls detect eleven
repeated pictures and reject a corrupted complementary marker. Observer shutdown
tests cover graceful exit, an ignored interrupt requiring child-only termination,
and rejection of nonfinite timing arguments.

At small dimensions the identity strip and moving center marker can cover color
patches. The final oracle chooses only visible, unoccluded patches, using the
actual pattern width rather than assuming the allocation pitch equals the image
width. A separately generated 256×64 lossless control checks twelve distinct
identities and six color patches. A 320×128 native external-source run passes
fixture ASan/UBSan. The initial 256×64 native attempt was rejected by the encoder's
reported minimum size; that capability failure is retained. Rechecking both large
geometries, both recorders, the corrected-color control and negative controls
preserves their previous identity and color results exactly.

Initial failed compatibility runs are kept. The broadcast mock and zero-size
regression source were unsuitable performance inputs for GSR. Rawvideo/null
validation also initially rounded timestamps to a guessed frame-rate time base,
producing artificial DTS warnings at 240 Hz. The original packets had strictly
increasing PTS; retaining the demuxer time base removes those warnings and preserves
all 1,896 decoded pictures in the diagnostic control. These failures are not
reported as recorder performance losses. See the
[cross-recorder index](evidence/cross-index.txt) for pinned artifacts and commands.

## Idle GPU capacity: 23 October continuation

The selected packages add opt-in `--gpu-memory compact`. The default remains
`retain`: CPU, RSS and latency comparisons do not establish a universal winner.
Both preserve AV1 settings, source ownership, the latest static image and
depth-two encoding. Compact releases unused input/output allocations after a
backlog drains and restores capacity on demand. It does not reduce active
overlap's storage requirement or establish a lower instantaneous peak.

A private diagnostic VA interposer traces actual surface/coded-buffer creation
and destruction. One fifty-millisecond media-owner delay after completion 120
creates overlap: another input image and a 22,183,936-byte coded-output buffer
materialize. The fixed preceding implementation retains both until shutdown.
After three seconds, all 433 remaining control samples show 114.379 MiB VRAM and
44.504 MiB GTT. Compact returns to about 103.129/23.348 MiB: **11.25 MiB VRAM and
21.15625 MiB GTT released**. Seven repeated induced backlogs also complete with
two successful replay saves. These tracing/fault runs are not CPU benchmarks.

The initial three-run matrix is retained, including its invalid observer run.
A startup fdinfo read raised PermissionError; the observer now counts unavailable
reads rather than inventing zero-use DRM clients. Completed cases were validated
against fixed recorder/fixture/driver hashes before the missing cases resumed.
The exact cause of that transient permission error was not established.

Fresh confirmation runs four rotations of four alternatives across five cases
(80 runs), with every alternative in every ordering position. Paced/static runs
last eight seconds; the saturated HD case lasts twenty. No build or profiler
overlaps timing. All runs report zero recorder skips; producer misses under
overload remain separate. Whole-child CPU includes startup and teardown.

| Case | Preceding control CPU ms | New retain CPU ms | Compact return CPU ms | Compact explicit trim CPU ms |
|---|---:|---:|---:|---:|
| Static | 17.895 ± 0.524 | 17.626 ± 0.398 | 17.846 ± 0.457 | 17.486 ± 0.378 |
| 2560×2880 / 60 | 102.248 ± 8.166 | 92.096 ± 5.035 | 100.827 ± 9.910 | 93.819 ± 6.497 |
| 2560×2880 / 120 | 315.877 ± 26.367 | 303.883 ± 29.201 | 288.226 ± 35.878 | 294.870 ± 14.239 |
| 1920×1080 / 240 | 179.710 ± 18.716 | 173.655 ± 10.138 | 181.028 ± 9.475 | 178.096 ± 13.678 |
| 1920×1080 / 1000 offered | 486.621 ± 28.508 | 482.240 ± 19.106 | 487.006 ± 18.270 | 504.338 ± 8.408 |

Values are means ± sample SD, four runs each. Saturated throughput is
350.09 ± 2.89 FPS for control, 352.85 ± 3.05 retain, 349.62 ± 4.15 compact return,
and 351.77 ± 4.56 explicit trim. At portrait/60, control/compact-return maximum
source-lease times are 0.642/0.693 ms and RSS is 38.555 ± 0.431 /
38.611 ± 0.339 MiB. At HD240, compact-return RSS is 38.100 ± 0.409 versus
37.230 ± 0.487 MiB control. The mixed tails and memory are not hidden by a
CPU-only ranking. The return policy avoids a separate trimming lock; that source
mechanism is not proof of an end-to-end speedup in every case.

Pool tests compile the actual patched FFmpeg sources. Independent live-object
oracles, held clones, allocation failure, concurrent get/return/trim and reentrant
owner destruction pass ASan/UBSan and TSan. Removing the temporary lifetime pin
fails both buffer-pool and RefStruct negative controls. Native Clang sanitizer
checks cover both bit depths, prepared frames during collection, retained pixels
after codec/device-owner destruction, and configured capacity up to one million.

Allocation-failure injection exposed a separate accepted-output loss: eleven
accepted inputs yielded only ten packets because a global error flag suppressed
valid drain output. The final application distinguishes unsubmitted input failure
from output failure. It preserves all eleven accepted pictures in the corrected
native test, independently decodes them, and still exits with the original error.
Eight MP4/Matroska, audio/no-audio, invalid-image/duplicate-PTS cases pass the
sanitizers; the fixed prior media implementation fails the accepted-prefix oracle.
The audio oracle uses an independently initialized resampler with matching
format/rate/chunk/flush semantics. An initial raw-step expectation was invalid
because the configured resampler filters that transition; its failure and artifact
are preserved. This failure-path refinement postdates the 80-run comparison;
those numbers must not be described as measurements of an identical final binary.

The final compiler/sanitizer verification passes all 16 x86-64 Nix checks.
Native compact capture/audio/replay integration passes under Clang ASan/UBSan.
Actual final Samply and Tracy captures save independently decoded replay clips.
Samply records 91,156 microseconds of main-thread CPU deltas in its twelve-second
capture; remaining sampled work is distributed across encoder submission,
allocation, synchronization, headers and the event loop. This profile is not an
uninstrumented speed comparison. Tracy's six-second run contains two replay
exports and approximately 5.937 seconds in blocked portal-wait zones; those zones
measure wall time, not consumed CPU.

The final uninstrumented forty-second portrait/60 run submits and encodes all
2,400 pictures, with zero recorder skips, producer misses, coalesced ticks or
completion fallback. Both late replay saves decode independently after the
30-second window fills. Whole-child CPU is **420.770 ms**, RSS **44.359 MiB**,
sampled VRAM **103.434 MiB** (including **57.5 MiB shared**), and sampled GTT
**23.348 MiB**. Live replay payload peaks at 8,076,263 bytes. Source-lease mean is
0.165 ms, histogram p99 upper bound 0.262 ms, and maximum 0.638 ms. This is one
long confirmation; it does not establish variance, a lower busy peak, or a new
every-metric winner. There were no unavailable fdinfo reads in this run.

The [capacity index](evidence/capacity-index.txt) identifies pinned packages,
controls, raw results, failed experiments and reproduction commands. External
production libraries are not all sanitizer-instrumented. Cross-recorder
matched-quality, other-GPU, power and game-interference comparisons remain open.

## Portal CPU overhead: samply and Tracy

The reported PID initially consumed about 99% of one logical CPU. On this
32-logical-CPU host that is approximately 3.1% of the normalized machine total.
This was an application busy loop. Changing GLib's ready-time property wakes its
context; our wait returned on that internal wake and immediately rearmed it.
Previous low-CPU PipeWire benchmarks bypassed this portal integration path.

The corrected wait uses one owner-only deadline word in a custom GSource and
does not generate a wake merely to update its deadline. It returns for capture,
control, the deadline, revocation or stop, and retries internal context wakes.
The [idle-wait regression fails against the original
code](evidence/overhead-idle-control.log). Both the default and the simpler
corrected ready-time implementation (`PORTAL_READY_TIME=1`) pass that regression.

Five alternating repetitions of each variant used private portal authorization,
2560×2880 tiled DMA-BUF capture, GPU VPP, AV1, a 60 FPS cap, 30-second replay
setting, two exports and teardown. The changing source offers about 50 updates/s;
the static source offers one image and exercises legal owned-image repeats.
Each run lasts about four seconds, including startup, and every exported clip
is independently decoded. No profiler or concurrent build ran during these
comparisons.

| Workload | Original CPU time, mean ± sample SD | Corrected CPU time, mean ± sample SD | Original / corrected CPU (% of one CPU) |
|---|---:|---:|---:|
| Static | 4040.319 ± 1.873 ms | 38.308 ± 0.922 ms | 99.904 / 0.947 |
| Changing | 4008.451 ± 2.676 ms | 73.342 ± 4.314 ms | 99.063 / 1.813 |

That is approximately **99.1% / 98.2% less process CPU**, respectively.
The corrected normalized machine percentages are about **0.030% / 0.057%** on
32 logical CPUs. All runs report zero capture skips and zero FPS-limited frames,
and both saves succeed. These are measured synthetic workloads, not a promise
for every desktop, game or GPU.

The corrected ready-time control measures 38.581 ± 0.636 ms static and
74.159 ± 0.330 ms changing. Its full-session difference from the default is within
variation. The default is retained because it removes the avoidable context
notification calls and their work. Raw [runs](evidence/overhead-matched.tsv),
[summary](evidence/overhead-matched.log) and
[binary hashes](evidence/overhead-matched-binaries.sha256) are retained.
Peak RSS remains around 81–82 MiB on these simple synthetic images, with no
established memory improvement. Encoder/capture pools and replay limits were
not reduced. GPU memory and whole-system compositor/kernel overhead are not
established by these process measurements.

The samply [before profile](evidence/overhead-before.json.gz) and
[symbols](evidence/overhead-before.json.syms.json) capture the original main
thread's GLib reference, lock, iteration and ready-time churn. The
[after profile](evidence/overhead-after.json.gz) and
[symbols](evidence/overhead-after.json.syms.json) shift the small remaining sampled
CPU cost toward startup/linker/device initialization. Both
[before](evidence/overhead-before-summary.json) and
[after](evidence/overhead-after-summary.json) summaries include CPU-delta weighting
and sample counts; the [analysis script](evidence/overhead-samply-summary.py.txt)
is retained. User-space stack sampling is not a complete kernel-callchain profile.

Tracy captures cover both [static](evidence/overhead-static.tracy) and
[changing](evidence/overhead-changing.tracy) portal paths. The
[static zones](evidence/overhead-static-zones.csv) contain 12 waits totaling
4.003 seconds; the [changing zones](evidence/overhead-changing-zones.csv) contain
208 waits totaling 3.925 seconds. These are **blocked wall-time zones**, not CPU
consumption. [Static](evidence/overhead-static-recording.json) and
[changing](evidence/overhead-changing-recording.json) statistics preserve frame,
save and allocation accounting.

[GCC/Clang, ASan/UBSan and the 14-check Nix matrix](evidence/overhead-verification.log),
[native GPU/Tracy/sanitizer integration](evidence/overhead-tracy-gpu-sanitizers.log)
and [the ready-time alternative](evidence/overhead-ready-time-check.log) pass.
The suite now exercises private portal → PipeWire → GPU → replay, alongside
idle waiting, readable FDs, closure, cancellation, audio and stop.
The user's desktop was not captured for these tests. Samply's initial interactive
attach affected terminal job control; those stopped-process samples were excluded,
and the reproducible profiling script launches private children instead.

Reproduction, inside `nix develop`:

```sh
make PROFILE=release CFLAGS=-Werror
make PROFILE=release PORTAL_READY_TIME=1 CFLAGS=-Werror
bash tools/benchmark-portal.sh /tmp/gpukey-before-overhead-fix/gpukey \
  build/gcc-release-portal-ready-time/gpukey build/gcc-release/gpukey \
  build/gcc-release/tests/capture_fixture /tmp/new-portal-comparison
bash tools/profile-portal-samply.sh build/gcc-release/gpukey \
  build/gcc-release/tests/capture_fixture /tmp/new-portal-samply \
  --gpu-portal-replay-static
```

`GPUKEY_SAMPLER` can select a sampler executable/wrapper. This host's restricted
perf configuration required its existing user-space sampler wrapper; the kernel
settings were not relaxed. The default development shell now includes samply.
Use `tools/profile-replay.sh` with `--gpu-portal-replay[-static]` for Tracy.

Fresh 40-second uninstrumented confirmations exercise the 30-second retention
window beyond startup. [Static](evidence/overhead-long-static.json) uses 44.305 ms
of process CPU (0.111% of one CPU); [changing](evidence/overhead-long-changing.json)
uses 348.994 ms (0.871% of one CPU), accepting all 2,001 offered updates.
Both report zero skips/FPS-limited frames and two successful decoded exports.
These are single longer confirmations, not additional five-run averages; the
exports occur early, while capture/retention continue for the full 40 seconds.
The [execution log](evidence/overhead-long-confirmation.log) is retained.

## Frozen recordings from missing video timestamps

The supplied `gpukey-replay-20261020-093411-388187074.mkv` was inspected without
modification. [Packet/decoded-image evidence](evidence/timing-clip-analysis.json)
shows 18 AV1 pictures spanning 18.568 seconds, with timestamps one second apart.
The same two decoded hashes alternate throughout; adjacent pictures have
71.899 dB PSNR. FFmpeg reports no decoding errors. This is consistent with
re-encoding the owned first image at the recorder's static-repeat interval.

The installed Niri 26.04 source writes frame sequence numbers but leaves header
PTS unset. Our old admission logic accepted zero as a timestamp and discarded
every subsequent update as the same FPS slot. A [saved-control
reproduction](evidence/timing-missing-pts-control2.log) records **one image and
rejects 50 updates as FPS-limited** from a private changing source.

The corrected capture path chooses timing before admission. Auto prefers
populated source PTS and otherwise reports and uses monotonic receive time from
the first frame. `--video-clock source|cycle|receive` provides explicit control;
source mode supports a valid zero epoch. An identical source (PTS, sequence)
pair is a duplicate; repeated sequence numbers with advancing PTS remain valid.
Nonduplicate stopped/reset clocks fail explicitly. Receive time is sampled
before diagnostics and copies. Its source capture latency remains unknown.

A graph-first candidate was rejected: a positive initialized graph timestamp
does not prove that the producer updates its clock. Switching from that stale
value to receive time after the first image can introduce an arbitrary initial
offset. For example, an untouched 1.000 s graph value and image receipts at
1.500/1.520 s produce a false 520 ms first interval instead of 20 ms. This is a
counterexample, not a hardware latency measurement. Reliable graph clocks remain
available through explicit selection, without a mid-recording clock switch.

The delivered version passes:

- [GCC/Clang, ASan/UBSan and all 14 applicable Nix
  checks](evidence/timing-final-verification.log).
- [Strict release checks, native GPU integration, actual Tracy capture and the
  native ASan/UBSan replay regression](evidence/timing-final-release-gpu-tracy.log).
  Cases cover omitted PTS, explicit graph time, zero-based source time, repeated
  sequence numbers, loss of source/graph clocks, source reconfiguration and
  removal, static content, FPS caps and independent audio tracks.
- Private **2560×2880 tiled DMA-BUF replay with Niri-style empty PTS and two
  16-channel audio tracks**. Each clip is independently decoded and checked for
  visible black/white scene changes, not just packet counts.
- The final [Tracy recording statistics](evidence/timing-final-tracy-recording.json)
  report 201 offered/submitted frames, zero skipped/FPS-limited frames, two
  successful saves and zero extra replay packet-copy bytes. The
  [trace](evidence/timing-final.tracy) and
  [zone data](evidence/timing-final-tracy-zones.csv) are retained.
  A separate decode of the first saved clip finds **76 pictures and 75 scene
  transitions**. These are functional measurements on a synthetic workload,
  not a universal performance claim.
- [Headless mpv playback](evidence/timing-final-mpv.log) of that fresh clip reaches
  EOF normally. Brave and the user's interactive desktop were not driven.

No new frame allocation or GPU conversion stage was added. The unused frame
sequence field was removed: the frame descriptor shrank from 128 to 120 bytes
and its capture record from 152 to 144 bytes on this x86-64 build
([control layout](evidence/timing-control-layout.log),
[new layout](evidence/timing-layout.log)). Clock state is per stream.
The original clip checksum still matches. Its discarded motion cannot be
reconstructed; the correction applies to new recordings.

The pre-fix source/binary remains at `/tmp/gpukey-before-timing-fix`, with its
[source manifest](evidence/pre-timing-implementation.sha256) preserved.
To repeat the full-resolution profiling case in `nix develop`, use
`--gpu-replay-niri` as the fourth argument to `tools/profile-replay.sh`.
The dependency/platform limits below remain in force.

## Startup negotiation and empty-encoder shutdown

The reported `replay 30 --display --fps 60` failure was reproduced and fixed.
The [user-core backtrace](evidence/startup-core-backtrace.log) shows zero submitted
frames and a crash inside FFmpeg hardware drain. Its saved media format used
modifier zero, while the [final PipeWire format recovered from that
core](evidence/startup-core-format.log) was 2560×2880 with tiled modifier
`0x0200000000082305`. The capture callback had published an unfixed modifier
choice as a final configuration. The producer's actual selection was consequently
rejected as a change, and draining the unused encoder triggered a separate
FFmpeg 9.0.2 null dereference.

Capture now waits for producer fixation before publishing a configuration.
Media close skips drain when no input was accepted, preserves an earlier error,
and otherwise returns `ENODATA`. After encoder initialization, the main loop
rechecks capture errors before announcing readiness. No new per-frame work,
buffer copies, fields or worker threads were added.

Verification for this correction:

- The saved pre-fix objects reproduce [exit 139 on empty VAAPI
  close](evidence/startup-empty-control.log). The saved executable also rejects
  [the private producer's valid two-stage negotiation](evidence/startup-negotiation-control2.log).
- [Empty file/replay and prepared-but-rejected first-frame
  tests](evidence/startup-empty-fixed.log) now close without a crash on software,
  VAAPI and Vulkan. The regression also checks original-error preservation,
  idempotent close and absence of a replay output file.
- [Private PipeWire GPU integration](evidence/startup-gpu-integration.log) passes
  with immediate linear formats and two-stage tiled formats, ordinary recording
  and replay, 320×240 and the reported **2560×2880** geometry, and two independent
  16-channel audio tracks. Both triggered clips are independently decoded.
  Actual midstream color changes still return failure and leave a decodable
  completed segment; they are not ignored as startup negotiation.
- [GCC and Clang, both ASan/UBSan builds, and all 14 applicable Nix
  checks](evidence/startup-verification.log) pass. [Additional VAAPI
  sanitizer checks](evidence/startup-vaapi-sanitizers.log) cover empty close and
  the full-resolution tiled replay. The aarch64 checks were not run.
- [Strict release checks and an actual Tracy run](evidence/startup-release-tracy.log)
  pass. The [trace](evidence/startup.tracy), [zones](evidence/startup-tracy-zones.csv)
  and [recording statistics](evidence/startup-tracy-recording.json) retain the
  2560×2880 tiled run: 201 submitted frames, zero skipped frames, two completed
  clips and zero extra replay packet-copy bytes. This synthetic four-second
  integration run is not a general performance comparison.

The native source and audio were private synthetic producers. The user's desktop
was not recorded. The recoverable pre-fix source/binary is in
`/tmp/gpukey-before-startup-fix`; the original source manifest is
[preserved](evidence/pre-startup-implementation.sha256).

An additional Vulkan-only teardown check found **3,170,720 bytes in 27
allocations** reported by LeakSanitizer across four open/free cycles. A
[standalone FFmpeg control](evidence/startup-vulkan-control.c.txt), without
gpuKey code, video submission or drain, reproduces
[the same report](evidence/startup-vulkan-control.log).
[Full allocation stacks](evidence/startup-vulkan-leaks.log) identify AV1
coded-bitstream initialization and Vulkan-driver allocations. The pinned
`vulkan_encode_av1_close` omits cleanup of its coded-bitstream context/fragment.
These dependency leaks remain unresolved and unsuppressed; Vulkan passes the
crash regression but is **not claimed leak-free**. The default VAAPI checks above
pass with leak detection enabled.

Reproduce the new GPU and profiling cases in `nix develop`:

```sh
GPUKEY_TEST_GPU=1 make PROFILE=release CFLAGS=-Werror integration
./build/gcc-release/tests/media_empty_test vaapi
make PROFILE=release TRACY=1 CFLAGS=-Werror
bash tools/profile-replay.sh "$PWD/build/gcc-release-tracy/gpukey" \
  "$PWD/build/gcc-release/tests/capture_fixture" /tmp/new-startup-trace \
  --gpu-replay-portrait
```

## Rolling replay

`gpukey replay [SECONDS]` continuously retains encoded media in RAM; Enter or
`gpukey save [FILE]` exports a clip while capture continues. Audio selections,
channel layouts, display selection and FPS controls are shared with ordinary
recording. The design follows material §13.4. A clip starts at a verified AV1
random-access point and ends at the latest completed common audio watermark;
the requested window is not an arbitrary frame-exact edit.

The replay tests exercise:

- Twenty logical seconds of independently generated video, with a 1.1-second
  window. Export is deliberately paused while later recording evicts its original
  live history. Fresh libdav1d and audio decoders check picture values, timestamps,
  exact PCM/Opus sample counts, two separate tracks and a known nonzero-cut impulse.
  Both Matroska and fMP4 are covered, with CPU-reference and all-copy storage.
- Sub-millisecond PCM prefixes/tails and Opus tails retain their 12 samples.
  A 2,000 FPS video case fails export when intermediate Matroska timestamps
  collapse, rather than losing reference-dependent packets. Only a terminal
  picture with no representable duration may be omitted.
- Sixty-five consecutive saves, including publication observed before the worker
  finishes notifying and an immediate stop after the final accepted request.
  Refcounts pin sealed blocks; no job mutex is shared with the capture owner.
  Stop waits for the accepted export and its sink to finish.
- A 1 MiB live budget, invalid/non-key first packets, contradictory timestamps,
  existing files, injected ENOSPC, a busy exporter and continued encoding after a
  failed save. The accounting bounds are checked while export is paused.
- Private Unix triggers: Enter, completion replies, session collisions, stale
  sockets, private-directory permissions, malformed messages, missing output
  directories, extension errors, repeated saves, SIGINT/SIGTERM and restart.
  Foreground Enter/Ctrl-C were also exercised through an actual pseudo-terminal;
  a background job with a terminal stdin correctly leaves that input alone.
- Private PipeWire clips with mono/surround tracks, sparse static video and source
  removal after a save. Native DMA-BUF replay additionally uses **two 16-channel
  AUX tracks**. Fresh FFmpeg decoders open every saved clip. The portal mock checks
  that control readiness wakes its event loop without a new video frame.
- Vulkan's mapped bitstream output is explicitly copied and independently decoded
  with two audio tracks. Buffered and io_uring exports both finish with `--durable`.

The pre-correction replay verification logs are [GCC/Clang, ASan/UBSan and the 14-check Nix
matrix](evidence/replay-verification.log), [strict GCC release](evidence/replay-release.log),
[strict Clang release](evidence/replay-clang-release.log),
[native GPU integration](evidence/replay-gpu.log) and
[Vulkan exports](evidence/replay-vulkan.log). No host desktop or microphone content
was captured. The aarch64 packages and Intel/NVIDIA hardware were not executed.
The integrated eventfd alternative also passes
[release checks](evidence/replay-eventfd-check.log),
[ASan/UBSan](evidence/replay-eventfd-sanitizers.log) and
[targeted ThreadSanitizer checks](evidence/replay-eventfd-tsan.log).

[Targeted ThreadSanitizer checks](evidence/replay-tsan.log) cover the ring/sink,
media, audio, replay, trigger controller, CLI and private PipeWire integration.
A broader run also reports races inside the **uninstrumented GLib/GDBus** paths
used by the portal mock: [full log](evidence/replay-tsan-full.log).
The [saved pre-replay control](evidence/replay-tsan-portal-control.log) reproduces
the same report families. GLib's inspected `gthread-posix.c` uses internal atomic
and futex synchronization that this application-only TSan build does not observe.
These reports were not suppressed; complete sanitizer verification of the GLib
stack remains open.

The tests exposed three production issues and one faulty fixture interaction:
Tracy's pre-main threads invalidated main-thread-only signal routing, so stop now
uses an async-signal-safe eventfd notification. A sub-microsecond final frame
could quantize to exactly the stop tick in the encoder; terminal zero-duration
omission now covers both encoder and container precision, with regressions for
Matroska and MP4. When an incoming frame proved that a requested duration had
been reached, a later arrival-time estimate could incorrectly shorten that
endpoint. A controlled five-millisecond delivery-latency change reproduced
47,761 decoded samples instead of 48,000; the fixed path preserves the requested
source endpoint and independently decodes exactly 48,000 samples per track.
See the [control](evidence/replay-duration-control.log) and
[fixed run](evidence/replay-duration-fixed.log). In negative device-selection tests, the fixture tried to link
ports belonging to a client that had already rejected the selection. Those tests
now avoid unnecessary links and assert the specific rejection diagnostic, so a
startup timeout cannot pass as a valid rejection. Initial evidence is retained in
the [profiler verification](evidence/replay-initial-verification.log),
[stop boundary log](evidence/replay-stop-boundary-initial.log) and
[fixture log](evidence/replay-fixture-initial.log).

Memory figures are charged encoded allocations plus metadata and fixed reference
allowances, **not a process RSS guarantee**. Live history is bounded by the chosen
budget; one pinned export can retain that much again, plus one spare block.
Indices, codec/capture/output working sets and actual allocator overhead are
additional. Codec reconfiguration, arbitrary AV1 reorder/layers, exact arbitrary
cuts, simultaneous exports, physical A/V latency, power and game interference
remain outside the measured replay scope.

The synthetic picture generator now computes distinct row patterns once and
copies equivalent rows. The independent original scalar formula checks odd and
small dimensions, the 19-row period, and wraparound near `UINT64_MAX`. This removes
redundant fixture work; improvements to that synthetic workload are not attributed
to live capture or encoding. Live PipeWire comparisons use the same external
producer for the fixed control and delivered recorder.

### Matched replay measurements

The delivered release binary is
`552463462d060bab6df3a5b3133a4118e74ed3d07b0364cb05ea584e3ea02031`.
Four real encoded fixtures contain 20 logical seconds of 1080p60 video, with two
stereo tracks in the audio cases. Each run retains five seconds, requests a clip
at 16 seconds, continues admission and completes export/teardown. Input loading
and padded-packet construction are outside the timer. Replay setup, admission,
snapshot/index construction, muxing, writes, wrapper cleanup and shutdown are
inside it. The sink uses sixteen 64 KiB slabs; these are benchmark settings, not
requirements or the CLI's default output budget.

Seven alternating runs per variant follow a separate warm-up. The initial
mutex/scan control, final all-copy control, default futex build and integrated
eventfd build use the **same current input-preparation harness**. Every exported
video/audio stream is independently decoded; frame/sample hashes and timestamps
match across all 112 runs. Opus preparation restores the known continuous 20 ms
sample clock, checks serialized timestamps and retains final discard padding.

| Whole replay phase, mean ± sample SD (ms) | Initial mutex control | All-copy control | Default futex | Eventfd build |
|---|---:|---:|---:|---:|
| Static video | 0.787 ± 0.066 | 0.788 ± 0.073 | 0.738 ± 0.041 | 0.741 ± 0.055 |
| Changing video | 0.845 ± 0.084 | 1.089 ± 0.107 | 0.978 ± 0.081 | 0.941 ± 0.054 |
| Static + two Opus tracks | 1.563 ± 0.198 | 1.796 ± 0.144 | 1.407 ± 0.209 | 1.490 ± 0.183 |
| Static + two PCM tracks | 2.347 ± 0.230 | 3.338 ± 0.366 | 2.365 ± 0.173 | 2.406 ± 0.100 |

The default avoids respectively 1,027,620; 1,929,014; 1,807,806; and 16,387,620
extra copied bytes. Mean PCM-phase CPU is 3.479 ms versus 4.623 ms for all-copy;
the PCM wall-time maxima are 2.601 and 4.054 ms. These are scoped encoded-pipeline
results, not whole-recorder throughput or latency guarantees.

The old mutex control has the lowest changing-video mean in this series, but a
worker paused while holding that mutex can block the owner. The final variants
avoid that dependency. Neither final wake mechanism wins every case; the default
futex avoids one descriptor, and `REPLAY_EVENTFD=1` retains the alternative without
inventing a workload threshold. The initial endpoint-based block-rotation
experiment increased PCM memory and was rejected. Final PCM peak live charge is
6,090,394 bytes versus 6,906,130 in the initial control, with identical clip
contents. Opus retains its required preroll.

Raw [runs and maxima](evidence/replay-retention.tsv),
[complete records](evidence/replay-retention-runs.json) and
[binary hashes](evidence/replay-retention-binaries.sha256) are retained.
The [initial chunk experiment](evidence/replay-initial-chunks.tsv) and
[intermediate wake experiment](evidence/replay-intermediate-wake.tsv) remain
separate from confirmation. Seven samples do not establish a production tail
guarantee or universal optimality.

### Fixed-reference ordinary recording

The pre-replay control remains
`67ec452460e02ab18f5adcd5d29a56b05922ea44f6061c6d6353ebccd534ecfc`.
Five alternating complete 1080p/240-frame sessions per variant, with identical
quality and output settings, independently decode to matching video and audio:

| Synthetic session, mean ± sample SD | Pre-replay control | Delivered |
|---|---:|---:|
| Static video | 807.399 ± 6.138 ms | 797.603 ± 2.747 ms |
| Changing video | 996.870 ± 30.059 ms | 903.117 ± 9.947 ms |
| Static plus stereo PCM | 811.143 ± 7.895 ms | 802.027 ± 1.523 ms |

Changing-source CPU falls from 870.517 to 234.706 ms because of the equivalent
row-generation optimization described above. That is a **fixture improvement**.
The actual private PipeWire/DMA-BUF comparison uses the same external producer:
mean recorder CPU is 36.887 ± 0.936 ms for the control and 37.166 ± 1.570 ms for
the delivered build; mean session times are 1035.355 and 1035.114 ms. No clear
live-capture speed change is established by these samples. Raw
[synthetic runs](evidence/replay-file.tsv),
[capture runs](evidence/replay-capture.tsv) and their JSON records/hashes preserve
the complete results, including memory and latency-bucket measurements.

### Profiling and reproduction

The [Tracy capture](evidence/replay.tracy) records native GPU capture, two
16-channel audio tracks and two concurrent-with-capture exports. Its
[zone CSV](evidence/replay-tracy-zones.csv) has 200 video-retention zones
(0.658 µs mean), 402 audio-retention zones (0.518 µs mean), two snapshots
(2.079 µs mean) and two exports (0.775 ms mean). These instrumented host/API
measurements are distinct from uninstrumented confirmation and calibrated GPU
timestamps. Disabled builds have no Tracy symbol references.

```sh
nix develop
make PROFILE=release CFLAGS=-Werror replay-controls \
  build/gcc-release/tests/replay_bench
make PROFILE=release CFLAGS=-Werror REPLAY_EVENTFD=1 \
  build/gcc-release-replay-eventfd/tests/replay_bench
bash tools/benchmark-replay.sh "$PWD/build/gcc-release/gpukey" \
  "$PWD/build/gcc-release/tests/replay_bench" /tmp/new-replay-comparison \
  "$PWD/build/gcc-release/tests/replay_bench_scan" \
  "$PWD/build/gcc-release-replay-eventfd/tests/replay_bench"
```

Use a new output directory. `tools/benchmark-audio-extension.sh` and
`tools/benchmark-capture-extension.sh` accept the fixed recorder control and
candidate paths; `tools/check-replay-vulkan.sh` and `tools/profile-replay.sh`
reproduce the hardware/export and profiling checks.

## Audio, FPS and interface extension

The pre-replay audio extension passed the complete 14-check Nix matrix, GCC/Clang ASan/UBSan,
strict release warnings, ThreadSanitizer integration and native AMD DMA-BUF
integration. The combined GPU test records two **16-channel AUX tracks** alongside
AV1. No host audio or desktop content was captured. The earlier evidence below is
retained; this section describes the extension and its fixed-reference comparisons.

New checks cover:

- Independent decoding of simultaneous mono, stereo, 5.1, 7.1 and 16-channel AUX
  tracks at negotiated 8/44.1/48/96 kHz input rates. Output lengths are exact at
  48 kHz. Deliberately reordered surround/AUX inputs verify channel identity and
  ordering, including the AUX label tag and Opus mapping-family bytes.
- Actual private PipeWire mono input plus surround monitor, and two independent
  16-channel AUX sources, using both Opus and PCM. Each decoded channel carries
  a distinct value/frequency. Known source-clock impulses land within 96 output
  samples (2 ms) of their independently calculated A/V timeline positions.
  This tests the graph-cycle/latency fallback, rather than just an audio API stub.
  Descriptor validation also covers 64 AUX channels; 64-channel capture/encoding
  itself was not executed.
- Source removal preserves a decodable prefix and returns failure. Missing,
  duplicate and source/monitor-mismatched devices fail without creating an output.
  Read-only discovery sees the private input/monitor nodes. The fake portal checks
  exact monitor/window type requests and single-source selection.
- A producer supplying 50 Hz despite a negotiated 25 Hz cap is filtered before
  copy/GPU work. Rational slot arithmetic, a final update that quantizes to the
  stop tick, positional/default paths, extension inference, repeatable audio,
  invalid arguments and nonoverwrite behavior are checked.

Executed logs: [verification](evidence/audio-extension-verification.log),
[ThreadSanitizer](evidence/audio-extension-tsan.log),
[combined GPU integration](evidence/audio-extension-gpu.log).
The [C23 format probe](evidence/c23-width-format.txt) records why `PRIu64` remains
necessary for strict format checking with the pinned Clang 21.

### Matched existing workloads

Fixed control binary SHA256:
`1eaa920e232e8be9e9ddae7d6466549458a068a7024c04438e7eb096d06bebd8`.
Delivered release binary SHA256:
`67ec452460e02ab18f5adcd5d29a56b05922ea44f6061c6d6353ebccd534ecfc`.
Five alternating runs per variant, same stock Mesa 26.1.8, input, quantizer,
output settings and measurement controls, with no concurrent builds:

| 1080p, 240 frames; full session time | Control mean ± sample SD | Extension mean ± sample SD |
|---|---:|---:|
| Static DMA-BUF fixture | 810.848 ± 11.296 ms | 806.490 ± 5.213 ms |
| Changing DMA-BUF fixture | 1009.019 ± 25.498 ms | 1006.974 ± 36.397 ms |
| Static fixture plus stereo PCM | 812.037 ± 4.592 ms | 812.705 ± 5.678 ms |

Independent decoded video hashes/timestamps match in all three comparisons;
decoded stereo samples are byte-identical. These synthetic runs include fixture
generation/upload and device lifetime. They do not establish a clear general
speed change. The separate, paced, actual PipeWire GPU comparison used
36.971 ± 1.051 ms CPU for the control and 37.220 ± 1.627 ms for the extension.
Maximum session times were 1042.051 and 1049.050 ms respectively; each run had
51 frames and zero capture skips. Source-lease p99 bucket upper bounds were
4.194303 ms in both variants. These small samples do not establish a tail guarantee.

Raw [media runs](evidence/audio-extension-media.tsv) and
[capture runs](evidence/audio-extension-capture.tsv) retain individual CPU,
memory, latency-bucket and byte measurements. Complete JSON records and executable
hashes are retained beside them. The earlier tone-only capture series is also
retained; the final series includes the source-clock impulse oracle.

### Usable audio cost choice

Two 16-channel tracks, one-second private recordings, identical source tones and
clock events, five alternating Opus/PCM runs. This is a fidelity/storage tradeoff,
not an equal-quality codec contest. Video uses the software control in these rows.

| Matroska audio | Mean CPU ± sample SD | Maximum CPU | Mean file bytes | Mean peak RSS |
|---|---:|---:|---:|---:|
| Opus, independent AUX channels | 99.499 ± 2.601 ms | 104.035 ms | 317,710 | 73,100.8 KiB |
| PCM (`--audio-codec pcm`) | 36.395 ± 1.556 ms | 39.076 ms | 6,149,258 | 74,024.8 KiB |

PCM avoids Opus encoding and used about 63% less CPU here, with about 19.4 times
the file size and slightly higher observed RSS. Both preserve channel identity
and sample count; Opus is lossy, while PCM preserves the samples after the declared
rate/drift conversion. No aggregate preference or universal superiority is inferred.

The [Tracy capture](evidence/audio-extension.tracy) contains 471 host zones over
1.26 seconds for GPU capture plus two 16-channel tracks. The
[CSV](evidence/audio-extension-zones.csv) contains 102 audio-copy zones (3.994 µs
mean, 13.515 µs maximum observed) and 102 resample/encode zones. This instrumented
trace is separate from uninstrumented comparisons and does not measure calibrated
GPU timestamps or physical device latency.

To reproduce against the saved control in this workspace:

```sh
nix develop
bash tools/benchmark-audio-extension.sh /tmp/gpukey-before-multiaudio/gpukey \
  "$PWD/build/gcc-release/gpukey" /tmp/gpukey-new-media-comparison
bash tools/benchmark-capture-extension.sh /tmp/gpukey-before-multiaudio/gpukey \
  "$PWD/build/gcc-release/gpukey" "$PWD/build/gcc-release/tests/capture_fixture" \
  /tmp/gpukey-new-capture-comparison
```

Each output directory must be new. Physical MT48 latency, interactive portal UX,
editor support for AUX labels/discrete Opus, other GPUs, energy and game
interference remain unmeasured. Direct connector-name selection, audio mixing and
multiple-video composition remain outside the implemented interface.

## Actual checks

| Check | Executed coverage |
|---|---|
| Nix matrix | GCC 16.2.0 and Clang 21.1.8, each with GNU ld, mold and wild; six package builds, six binary compiler/linker identity checks, Tracy and formatting checks. Package checks include the private PipeWire integration suite. aarch64 is declared in the flake but was not built here. |
| CPU correctness | GCC and Clang AddressSanitizer/UBSan runs. ThreadSanitizer covered the concurrent ring, buffered/io_uring sinks and private PipeWire capture/removal/static-source integration. Dependencies and GPU memory are not made sanitizer-instrumented by these builds. |
| Ownership and I/O | One million FIFO token transfers crossing 32-bit counter wrap; full/empty behavior; existing-file refusal; seven-byte short writes; injected ENOSPC; ordered completed-prefix accounting; successful file/directory synchronization. |
| Portal | Private D-Bus portal emits responses before returning method handles; tests stable serial selection, owned FD extraction, revocation, denial, cancellation and session cleanup. The host's interactive desktop chooser was not exercised. |
| Real PipeWire | A private server and synthetic producer exercise shared-memory capture, source removal during recording, a single static frame, and actual DMA-BUF capture. The DMA-BUF producer deliberately advertises zero maxsize/chunk size. Output is independently decoded. No personal desktop or microphone was recorded. |
| Media | libdav1d independently decodes AV1. Black/white/neutral chroma endpoints, every serialized packet boundary across VFR gaps, a long final static interval, and exact decoded PCM/Opus lengths are checked. A 1000 ppm synthetic audio clock drift produces exactly 240,240 output samples from 240,000 input samples over 5.005 seconds, with resampling rather than merely padding the entire difference. |
| GPU | Native VAAPI AV1 and persistent imported RGB surfaces; 8/10-bit conversion against scalar BT.709 equations; odd 321×241 dimensions; 60000/1001 timing; hardware output decoded through FFmpeg. Vulkan AV1 upload also encoded and decoded; NVIDIA/Intel hardware was unavailable. |
| Matched output | Transient versus pooled VPP parameters produce identical independently decoded pixels and timestamps in the benchmark cases. |
| Tracy | A real network capture was saved, exported to CSV, and retained under `docs/evidence/`. These are host/API zones, including completion waits, not calibrated GPU timestamp queries. Disabled builds have no Tracy client symbol references. |
| Supplied models | All three checks were rerun in a temporary copy. Arithmetic and output models passed; expected broken controls still produce counterexamples. The output model checks 748 complete traces and 1,833 prefixes. These finite models do not prove C weak-memory or GPU behavior. |

A failed source-removal run is an expected negative test: the recorder returns
failure and its already-recorded prefix decodes. Success means codec drain, mux
finalization and accepted writes all completed; enqueue alone is never success.
Durability is reported separately. Fatal io_uring retirement failure quarantines
payload memory until process exit if synchronous cancellation cannot establish
quiescence. That rare path favors lifetime safety over reclaiming the bounded pool;
it has not been fault-injected into the kernel.

## A verified library correction

The hardware color oracle exposed a real representation mismatch in Mesa's
`vl_csc_get_rgbyuv_matrix`: destination range coefficients used the source format's
component depth. RGB8 → P010 therefore used an eight-bit normalized range for the
sixteen-bit destination storage. Both installed Mesa 26.1.8 and stock pinned Mesa
26.2.5 exhibit the discrepancy on the tested path.

The optional `mesa-color` package changes only that range calculation to use the
destination component depth. It adds no image pass, intermediate surface or
per-frame allocation. This is a local patch, not an upstream-reviewed release.

| Driver, same seven uniform RGB probes | Maximum 8-bit error | Maximum 10-bit error |
|---|---:|---:|
| Stock Mesa 26.2.5 | 0.4406 codes | 3.0000 codes |
| Patched Mesa 26.2.5 | 0.4406 codes | 0.6747 codes |

The native VA P010 contract explicitly ignores the bottom six storage bits. The
oracle compares the meaningful upper ten bits; requiring zero padding would test
a stronger CPU-format convention than the native API promises. These probes cover
uniform colors, not all spatial/chroma filters, HDR or every driver operation.

The patched driver also passed the real DMA-BUF PipeWire path and an odd-dimension,
10-bit, rational-rate, audio, io_uring and durable-finalization recording: 90 frames,
1.501500 seconds on both tracks, 72,072 decoded stereo samples, and 590,315 bytes
accepted, written and synchronized in that recorded run. See the saved logs for
exact tested configurations. The optional driver is loaded only for the recorder
process; no system graphics settings or drivers are replaced.

## Performance comparisons and their limits

`tools/benchmark.sh` alternates candidate order, runs five repetitions per case,
checks submitted/encoded counts and byte completion, and compares decoded hashes.
The transient control is preserved as `make PROFILE=release VPP_CONTROL=1`.
The ordinary implementation pools the VPP parameter buffer; timing arrays and
clock reads are omitted unless `--stats` is requested. The transfer queue uses
one-word generation/index tokens, and audio/video counters have separate cache
lines on the tested platform.

Raw runs, binary hashes and a TSV are in `docs/evidence/`. They measure synthetic
complete paths with the same input, native quantizer, timing, output and lifecycle.
They do not measure a game, compositor overhead, GPU engine utilization, energy,
physical audio synchronization, or photon-to-display latency. The changing-image
fixture includes its own CPU pattern generation and upload; those costs are not
attributed to real DMA-BUF capture.

The initial five-run comparisons overlapped substantially. Pooling removes known
allocation/registration work, but these runs do not establish a general throughput
or latency win. Ordinary io_uring likewise did not show a decisive advantage over
the dedicated buffered writer at the tested encoded rates. Buffered output remains
the default; both controls remain runnable. Percentiles in JSON are explicitly
logarithmic bucket upper bounds. Sink timing measures ordered buffer retirement,
and reserved-byte peaks are conservative producer-credit bounds.

A fresh confirmation table is saved separately when rerunning the final binary;
initial runs are retained rather than silently replaced. No universal SOTA,
lock-free pipeline, lossless desktop fidelity or cross-vendor quality ranking is
claimed. Native NVIDIA arrays, AMD direct RGB Vulkan encoding, Intel VPL, direct
compositor capture, explicit-sync early release, HDR, replay and multiple video sources
remain unimplemented candidates from the supplied paper, not measured losers.

## Reproduce

```sh
nix develop
make check
make integration
GPUKEY_TEST_GPU=1 make integration
make hardware-check
make check SANITIZE=address,undefined
nix flake check

make PROFILE=release
make PROFILE=release VPP_CONTROL=1
# Run without concurrent builds or other intentional benchmark interference.
tools/benchmark.sh build/gcc-release-vpp-control/gpukey \
  build/gcc-release/gpukey /tmp/gpukey-fresh-benchmark

nix build .#mesa-color --out-link result-mesa-color
LIBVA_DRIVERS_PATH="$PWD/result-mesa-color/lib/dri" make hardware-check
```

The strict color improvement is specific to the tested AMD VPP route. The default
recorder continues to use the host's chosen driver unless its environment selects
the optional patched driver. Intended players/editors still need acceptance tests,
particularly floating-point PCM in MP4 and Matroska's nominal DefaultDuration:
actual VFR timing is expressed by timestamps and explicit block durations, not an
average frame-rate claim.

### Final confirmation

The final series uses session wall time including device creation and teardown
(the earlier media-interval series excluded those two operations). Five alternating
runs per variant, stock host Mesa 26.1.8, release `-O2`, native quantizer 32, no
concurrent builds or intentional competing workload:

| Case | Transient VPP, mean ± population SD | Pooled VPP, mean ± population SD |
|---|---:|---:|
| 320×240 static, 240 frames | 194.464 ± 12.756 ms | 184.108 ± 8.267 ms |
| 1920×1080 changing, 180 frames | 736.935 ± 21.066 ms | 764.854 ± 7.591 ms |
| 3840×2160 static, 120 frames | 1332.853 ± 8.918 ms | 1332.335 ± 8.621 ms |

The separate 1080p output comparison was 752.107 ± 15.447 ms buffered versus
766.887 ± 2.811 ms ordinary io_uring. There is no robust general winner here.
All final runs submitted and produced the requested number of video packets,
completed their accepted bytes, and the three VPP comparison cases had identical
decoded frame hashes/timestamps. Full per-run CPU, memory and tail-bucket data is
in [final-benchmark-runs.json](evidence/final-benchmark-runs.json); the directly
comparable table and executable hashes are [retained](evidence/final-benchmark.tsv).

The `source_lease` JSON histogram describes retirement of the image handed to
processing, starting after the capture metadata/copy snapshot is ready. In the
shared-memory plan that image is an owned copy: the original PipeWire wrapper has
already been returned. Initial capture-copy time is visible in the capture Tracy
zone. This metric is not the original source's complete acquisition-to-release
interval or end-to-end display latency.

A further size-boundary probe encoded three 7680×4320 frames through the actual
DMA-BUF/VPP/AV1 path and independently decoded all three at that resolution. This
is a functional size check, not an 8K frame-rate guarantee.

The portal negative tests also reject an invalid returned FD index. An explicitly
selected audio track with no samples returns failure even for a short recording.
The untested NVIDIA control uses an encoder-owned input-buffer copy and requests
forced IDR pictures; it does not rely on asynchronous host-upload lifetime or
assume that an arbitrary forced intra picture is a random-access point.

In this final series, pooled parameters reduced small-static wall time but were
slower for the changing 1080p fixture; 4K was effectively tied. The changing fixture
also performs substantial CPU source generation/upload, so the totals must not be
presented as isolated VPP allocation costs. The pooled default is a reversible
choice to remove steady parameter allocation, not a demonstrated winner across
all objectives. `VPP_CONTROL=1` preserves the competing implementation.

## VRAM follow-up and memory targets — 20 October 2026

The fixed pre-change reference is preserved in `/tmp/gpukey-before-vram-fix`;
its source archive is `evidence/vram-reference-source.tar.gz` and its executable
SHA256 is `9add64c7e376b21128e86ea7535ef6123770c4c60bb61b62592dada900d62e64`.
The user's 122 MiB snapshot included 67.5 MiB of shared memory. Its exact counters
match our 1080p reference. An initial explanation based on an older 2560×2880
capture log was corrected; that old log did not establish the current session's
resolution. The supplied GPU Screen Recorder command used HEVC and direct DP-1
capture, so its number is not an AV1/portal comparison at matched settings.

Changes keep AV1, Q=32, bit depth, source resolution, reference structure, color
interpretation and replay behavior. The application prefers two source buffers,
negotiates additional buffers when required, and derives the VAAPI input count
from encoding depth. The optional `amd` package uses process-local Mesa 26.2.5
with the previously tested P010 correction, smaller sparse allocator slabs and
deferred graphics-ring allocation. It does not replace the system driver.

Two fresh confirmation series used five alternating repetitions per case and
variant after exploratory runs. The final series compares the fixed executable,
the compact allocator, and the allocator plus deferred ring. Each run includes
the private portal, PipeWire, native DMA-BUF capture, VPP, hardware AV1, two replay
exports, independent decoding and teardown. The synthetic producer runs in a
separate process; its CPU generation/upload cost is not included in recorder CPU.
DRM memory is sampled every 20 ms and cannot establish an unsampled instantaneous
high-water mark. Tests ran on the live desktop without changing clocks, kernel
permissions or other applications.

| Workload | Reference VRAM, mean MiB | New AMD profile, mean MiB | Reference CPU ms, mean ± sample SD | New CPU ms, mean ± sample SD |
|---|---:|---:|---:|---:|
| 1920×1080 changing scene, 240 Hz | 122.709 | 38.348 | 115.400 ± 2.861 | 110.491 ± 1.736 |
| 2560×2880 changing scene, 60 Hz | 330.384 | 116.648 | 86.164 ± 7.508 | 82.627 ± 6.676 |
| 2560×2880 changing scene, 120 Hz offered | 330.784 | 116.648 | 103.792 ± 9.578 | 94.891 ± 11.048 |
| 2560×2880 static from startup | 108.102 | 66.090 | 40.525 ± 0.785 | 37.557 ± 1.373 |

These four-second session costs include startup and teardown. The 1080p runs
encoded all 960 in-window pictures; the 60 Hz portrait runs maintained cadence.
The 120 Hz portrait case saturated at about 104 encoded frames/s on **both**
implementations. Recorder skips were zero, but source-empty callbacks were not:
the final reference averaged 57.6, and the candidate 60.4 per run. Counting only
recorder skips would conceal upstream pressure. The producer's timestamp oracle
was corrected during exploration to advance on missed timer/buffer opportunities,
rather than slow presentation time when it could not supply a frame.

At saturation, mean source lease fell from 38.917 ms to 7.317 ms and worst observed
lease from 75.141 ms to 17.173 ms. At 1080p/240 Hz, both had a worst per-run p99
histogram upper bound of 0.262143 ms; worst individual leases were 4.496 ms and
4.234 ms. No universal latency or throughput superiority follows from those
fixtures. CPU differences that overlap run variation are not declared wins.

Shared producer buffers are real storage. Static capture may import only one of
the producer's allocated buffers, so process VRAM alone understates the complete
source pool. Producer allocation was separately measured: 230 MiB for eight
portrait buffers versus 57.5 MiB for two, including actual allocation padding.
The smaller pool is negotiated, not merely hidden from the recorder's counter.
At 1080p/240 Hz, sampled GTT decreased from 19.156 MiB to 15.332 MiB.

The LLVM-capable newer-driver profile in this series increased process peak RSS:
82.247 ± 0.399 MiB to 90.016 ± 0.565 MiB at 1080p/240 Hz. Other cases showed a
similar 7–8 MiB increase. This is a material tradeoff, distinct from GTT and VRAM;
it is not hidden in a claim that every memory metric improved. Removing the
unused LLVM compiler initialization is a separate candidate under evaluation.

That candidate subsequently passed graphics, 8/10-bit color and native recording
checks. A final five-repetition series kept the same fixed executable reference
and compared the LLVM-capable profile with an ACO-only build. The `amd` package
now selects the ACO-only profile; the earlier profiles remain reproducible.

| Final comparison | Reference VRAM MiB | ACO VRAM MiB | Reference RSS MiB, mean ± SD | ACO RSS MiB, mean ± SD | Reference CPU ms, mean ± SD | ACO CPU ms, mean ± SD |
|---|---:|---:|---:|---:|---:|---:|
| 1080p, 240 Hz | 123.909 | 38.348 | 82.838 ± 0.349 | 55.365 ± 0.453 | 125.518 ± 4.539 | 111.592 ± 3.890 |
| Portrait, 60 Hz | 330.784 | 116.648 | 83.452 ± 0.783 | 57.187 ± 0.229 | 78.965 ± 4.042 | 73.828 ± 5.567 |
| Portrait, 120 Hz offered | 330.784 | 116.648 | 83.518 ± 0.383 | 56.451 ± 0.716 | 109.684 ± 7.248 | 101.249 ± 12.715 |
| Portrait, static | 107.702 | 66.090 | 81.129 ± 0.513 | 54.460 ± 0.469 | 40.991 ± 0.525 | 27.466 ± 0.478 |

All five 1080p/240 Hz ACO runs submitted 960 frames without recorder skips,
source-empty callbacks or coalesced ticks. The first exploratory ACO run had one
source-empty callback; it is not silently included as a clean confirmation.
The first 120 decoded scene frames also match the reference byte for byte.
The portrait 120 Hz case remains saturated at about 104 frames/s; its source
misses remain visible. Small source-lease means did not improve in every case:
the 60 Hz mean was 0.265 ms versus 0.244 ms, and the 1080p mean 0.133 ms versus
0.129 ms. Those observations remain part of the active latency objective, not
a blanket nonregression guarantee. The Nix-reported driver closure fell from
704,644,152 to 121,419,048 bytes after unused compiler/tool dependencies were
removed; this is a closure-size measurement, not a download-size claim.

`--encode-depth 1` remains an explicit Pareto alternative. It uses one VAAPI
input and one coded-output buffer, but exploratory saturated portrait throughput
was lower than depth two. It was not selected as the general default. Producers
requiring three buffers and explicit `--capture-buffers 3` both pass; extra
source buffering remains available when desired.

The first 120 independently decoded frames of the changing portrait scene are
byte-for-byte identical across the reference, allocator-only and deferred-ring
variants. The comparison covers full decoded planes, not just center markers.
The optional driver's 8/10-bit scalar conversion checks pass at depths one and
two, including refusal to overwrite a separately pinned input. A matched
headless graphics probe passed 32 context creations on eight threads, 256 checked
draws and destruction with queued GPU work under GCC and Clang. Injecting the
10.5 MiB ring allocation failure produced 16 failed allocation attempts and
successful concurrent context-failure cleanup.

The final packaged ACO AMD executable also completed a 40-second 1080p/240 Hz recording:
9600 submitted/encoded frames, zero recorder skips, zero source-empty callbacks,
zero coalesced timer ticks, and two independently decoded 31.3-second clips saved
after the 30-second history had advanced. Sampled peak VRAM remained 38.348 MiB.
CPU was 861.051 ms over 40.015 seconds, approximately 2.15% of one logical CPU;
peak process RSS was 62.070 MiB with the longer encoded history and exports.
This is a generated scene on this machine, not a measurement of every desktop
or game. This longer run tests retention eviction and late saves, unlike the
earlier overhead run whose exports occurred near startup.

Samply captures compare the saved reference, application changes, and final driver
profile. They launch private children rather than stop/attach to an interactive
job. CPU-delta-weighted summaries are retained; startup and library work remain
substantial, and raw sample counts are not used as CPU fractions. A real Tracy
capture contains conversion, encode, replay-export and portal-wait zones. Wait
zones include sleeping wall time. The full GCC/Clang, ASan/UBSan and 14-check Nix
matrix passed, and the native GPU integration suite passed under ASan/UBSan with
the optional driver. Aarch64, Intel/NVIDIA runtime, protected graphics, broad game
interference, energy and complete source-to-packet latency remain unverified.

The active goal's calculated targets are in [memory-targets.md](memory-targets.md).
With the current full-image RGB/VPP model they are 30.652 MiB at 1080p and
108.984 MiB at 2560×2880, before driver/alignment costs. The observed gaps are
about 7.70 and 7.66 MiB. Producer-native YUV, exact encoded-payload sharing and
other representations remain open; these conditional payload targets are not
universal mathematical minima.

Evidence: `evidence/vram-media-confirmation.tar.gz` contains individual raw runs;
`evidence/vram-confirmation-matrix.tar.gz` isolates application, newer-driver and
allocator changes. Summaries, source hashes, driver hashes, long-run counters,
graphics/failure logs, decoded-plane hashes, buffer-allocation traces, samply
profiles and `evidence/vram.tracy` accompany them. `tools/benchmark-gpu-matrix.sh`,
`tools/summarize-gpu-memory.py`, `tools/check-gpu-driver.sh`, and
`tools/memory-targets.py` reproduce the measurements or calculations.
The final ACO series is in `evidence/vram-aco-confirmation.tar.gz`; its long-run
logs and counters use the `vram-aco-long-` prefix. Earlier LLVM-capable results
remain preserved under their original names.

## Exact key-payload sharing (20 October 2026 follow-up)

The original user clip contains nine byte-identical 1,764,416-byte AV1 keyframes.
A fresh static scene independently reproduced identical keys. This motivated
sharing already encoded immutable payloads, with per-packet timing preserved.
The optimization does not repeat or alter codec output. `auto` shares equal
consecutive keys, `reference` adopts each packet independently, and `copy`
retains the original copying control. Payload comparison is exact; there is no
hash collision assumption. Packet records remain 32 bytes. Unique shared owners
cost 24 bytes each; live and export-owned budgets charge their payload once.

`tests/replay_sharing_test.c` checks alternating key runs, sub-buffer offsets,
smaller replacement allocations, a paused export while live history expires,
reclamation after export, stopping with an accepted export, malformed bytes,
changed extradata, full-budget failure ownership, and the exact admission edge.
Independent libdav1d decoding checks saved black/white output. Custom buffer
destructors verify prompt duplicate release and one final release per backing
allocation. Existing replay tests cover audio cuts, invalid random-access flags,
stream timing and export errors. These tests are coverage, not a formal proof.

The fixed pre-change source is `evidence/replay-unshared-control.c.txt` and its
recorder executable was saved in `/tmp/gpukey-before-retention-layout`. A common
benchmark loads identical real packets before timing retention, export and
teardown. It uses a five-second window and one save after 16 seconds; these are
test scales, not production limits. Each variant gets a separate warmup, followed
by seven repetitions with rotating order. Every exported packet's bytes, timing,
flags and side data match the control. Full decoded planes and both audio tracks
also match for the first repetition of each variant/input. The final confirmation
followed a cost audit replacing a second eviction scan with the existing key bitmap.

| Fixed input | Control / auto peak live MiB | Control / auto lifecycle CPU ms (sample SD) |
|---|---:|---:|
| User's duplicate-heavy clip | 6.800 / 1.751 | 4.400 (0.213) / 3.889 (0.255) |
| Fresh static scene | 0.409 / 0.152 | 0.593 (0.061) / 0.548 (0.064) |
| Changing scene, 240 FPS | 1.938 / 1.938 | 4.204 (0.140) / 4.153 (0.205) |
| Static video + two Opus tracks | 1.025 / 0.769 | 1.936 (0.186) / 1.876 (0.098) |
| Static video + two PCM tracks | 5.808 / 5.552 | 3.634 (0.313) / 3.785 (0.301) |

These are bounded allocation charges, not process RSS. The changing scene has
96 additional live metadata bytes and no sharing hits. CPU differences near
the reported variation are not established speedups or regressions. In the
large-key user input, total admission time rises from 0.860 to 1.379 ms, and the
largest observed admission rises from 0.420 to 1.195 ms even though complete
lifecycle CPU falls. Keeping fewer allocations reduces later retirement work;
exact equality still reads both payloads. `reference` remains a useful explicit
alternative when this admission cost matters more than retention RAM. No
all-metric dominance or universal deduplication optimality is claimed.

Reproduce with `make PROFILE=release replay-controls`, then
`bash tools/benchmark-replay-sharing.sh BENCH UNSHARED_CONTROL NEW_DIR CASE INPUT ...`
and `python3 tools/summarize-replay-sharing.py NEW_DIR` inside `nix develop`.
The raw run JSON and hash/decode manifests are archived with the final evidence.

Three alternating live comparisons use the same opt-in ACO driver and the fixed
pre-sharing executable. Each static run lasts 40 seconds, includes the full
30-second rolling window and two late exports, and submits 40 valid repeat
pictures from one source update. Peak live/owned replay charge falls from
1,811,884 to 377,092 bytes (79.2%); 19 keys share their payload. Mean peak RSS
falls from 57.112 (SD 0.590) to 55.806 (0.164) MiB. Full-session CPU falls from
35.835 (0.856) to 33.870 (0.512) ms. Saved clips independently decode and retain
the same encoded image payload. One candidate run has an extra sampled 64 KiB
GPU slab, so this change is not credited with a VRAM improvement.

Six-second changing-scene controls and candidates each submit all 360 in-window
frames at 60 FPS, with zero recorder skips, source-empty callbacks or coalesced
ticks. VRAM stays at 38.348 MiB. CPU is 78.267 (6.892) versus 80.259 (2.880) ms,
and RSS is 54.837 (0.870) versus 55.104 (0.228) MiB; these differences are within
the observed run variation. There are no identical-key hits. The reported
lifecycle FPS includes setup/teardown time; the in-window count is the cadence
check. This does not measure every game, encode rate or latency regime.

The full GCC/Clang, both ASan/UBSan builds, and all 14 x86-64 Nix checks pass.
The targeted ownership/eviction test also passes Clang ThreadSanitizer. Fresh
native GPU integration also passes under ASan/UBSan with the ACO driver, including
multichannel audio, static/changing replay, source revocation, and depth-one reuse.
samply captures launch both saved control and candidate on the same moving scene;
all clips independently decode. The candidate main-thread CPU-delta-weighted
profile attributes about 28% inclusively to GPU preparation/conversion and 37%
to submission. These overlapping sampled estimates identify further audit areas;
they are not precise CPU-time partitions or evidence that an unsampled routine
costs zero. Startup/dynamic loading is still represented.

A fresh Tracy static capture includes six retention calls (10.858 µs mean,
40.056 µs maximum), two snapshots (3.321 µs mean), and two exports (0.606 ms mean).
Two keyframes share storage. Portal waiting spans about six seconds of sleeping
wall time, not six seconds of CPU. These instrumented timings are diagnostic,
separate from the uninstrumented comparison table.

Evidence uses the `replay-sharing-` prefix: fixed-input and native run archives,
JSON summaries, profiler archives, Tracy zone CSV, compiler logs, thread-sanitizer
log, and the archived source/executable control. The source audit states lifetime
and publication invariants. Intel/NVIDIA runtime, global interning, packed arenas,
energy, game interference and complete source-to-packet latency remain open.

## VPP conversion follow-up (20 October 2026)

Samply identified conversion and encoder submission as larger remaining CPU
costs than replay retention. The saved recorder/source control is
`/tmp/gpukey-before-fused`; the fixed driver control is the prior Mesa 26.2.5
ACO profile. `nix run .#amd-reference` preserves that profile with the current
recorder. `nix run .#amd` selects the new VPP driver. Ordinary builds retain the
system driver. Both wrappers honor an existing `LIBVA_DRIVERS_PATH` override.

The new path combines luma and chroma conversion in one compute dispatch,
reuses source-owned sampler views where their semantics and context match, and
caches a 64-byte constant record. Other VPP operations retain the general path.
Shader dumps confirm `vl:rgb_yuv_pair` is actually compiled and used; merely
getting a successful API return was not treated as proof of path selection.

The original scalar color/pinned-input probe passes at 8/10 bits and depths one
and two. Raw pre-encode bytes and independent AV1 decoded patterns match the
control for native/linear inputs and both output depths. The general transition
probe compares **576 complete raw outputs**: four RGB formats, three dimensions
(including 78×66), both output depths, changed source content, full/reduced range,
matrix and chroma-location changes, rotation, crop/scale, alpha blending, and
transfer/primaries conversion. These include returning from general processing
to the optimized path in the same context. GCC and Clang probes pass.

A separate fault-only driver zeros discarded constant storage before simulating
a failed map, and injects failures at several shader-construction positions.
Map failure and initial fast-shader failure fall back with all 576 outputs
matching. Partial general-shader failures produce the expected one operation
error, then all 575 subsequent/other outputs match. The tests exercise retry
and cleanup; the injection code is compiled out of the normal driver. These
cases also pass under ASan/UBSan. Source audit additionally preserves temporary
views for other contexts, missing view getters and single-component swizzles;
the VAAPI image tests do not claim coverage of arbitrary foreign Gallium callers.

The native capture/integration suite passes under ASan/UBSan, including replay,
multichannel audio, revocation, reconfiguration, static/changing input and
depth-one reuse. Headless graphics passes 32 contexts on eight threads, 256
checked draws and destruction with queued GPU work. These are separate checks:
the graphics test does not substitute for the VPP image/lifetime tests.

The encoder-side RGB alternative remains open. An immutable-source probe verifies
that native conversion is reachable through Mesa's combined encode-processing
path. Native and linear GFX12 inputs produce identical decoded results; disabling
native conversion reproduces ordinary VPP exactly. Native conversion changes
rounding and filtering, including white 235 to 234 in the tested 8-bit pattern.
The probe's Q=1 decode includes quantization, so it cannot alone rank conversion
fidelity. Eight-bit RGB to ten-bit output falls back to VPP. The pinned FFmpeg
AV1 VAAPI path rejects Q=0's inferred lossless transform mode; that pre-existing
limitation was recorded rather than hidden by calling Q=1 lossless. A complete
direct-input recording path still needs source-lease/static-repeat integration.

The performance comparison alternates equal driver configurations on the same
recorder, private portal, PipeWire producer, AV1 settings and replay/export path.
It includes static portrait, portrait at 60/120 FPS, and 1080p at 240/1,000 FPS.
The saturated cases run for ten seconds; the others run for four. These are test
scales, not production limits. Source-empty callbacks and coalesced producer ticks
remain distinct from recorder skips. A launcher ordering error in an early
two-variant extension was stopped and excluded; fresh alternating runs replaced
it. Raw variance and maxima are retained, and throughput differences within run
variation are not presented as established gains.

The final packaged-build series has four alternating repetitions per case. The
table reports means (sample SD for CPU and RSS); source-lease maxima include
startup and scheduling stalls. CPU covers the complete recorder process lifetime.

| Input | Profile | CPU ms | Peak RSS MiB | Source lease mean / largest ms |
|---|---|---:|---:|---:|
| Static 2560×2880, 4 s | reference | 28.870 (1.062) | 54.207 (0.218) | 3.963 / 4.420 |
| | paired | 24.286 (0.608) | 51.532 (0.299) | 0.550 / 0.608 |
| 2560×2880 at 60 FPS, 4 s | reference | 74.445 (6.182) | 56.492 (0.365) | 0.269 / 4.780 |
| | paired | 65.757 (4.367) | 54.352 (0.372) | 0.177 / 0.862 |
| 1920×1080 at 240 FPS, 4 s | reference | 114.503 (2.983) | 55.375 (0.421) | 0.239 / 9.827 |
| | paired | 110.063 (5.187) | 53.004 (0.365) | 0.183 / 5.481 |
| 2560×2880, 120 FPS offered, 10 s | reference | 199.666 (6.975) | 57.639 (0.273) | 7.659 / 22.084 |
| | paired | 184.157 (28.131) | 55.451 (0.617) | 7.740 / 22.207 |
| 1920×1080, 1,000 FPS offered, 10 s | reference | 292.489 (23.329) | 56.747 (0.681) | 4.643 / 13.272 |
| | paired | 290.605 (16.276) | 54.845 (0.446) | 4.676 / 14.232 |

At 60 FPS every in-window frame is submitted in both profiles, with no source
misses or recorder skips. At 240 FPS the candidate submits all 960 frames in
each run; the reference has one source-empty callback on average. At saturation,
portrait throughput is 101.29 (SD 2.09) versus 100.57 (0.71) FPS, and 1080p
throughput is 301.44 (2.85) versus 299.73 (2.02) FPS. These results **do not
establish a throughput improvement**. Some saturated lease means/maxima increase;
the prior profile remains available. An earlier six-run series likewise showed
similar saturated throughput, not an all-metric win.

Sampled VRAM is 38.285 MiB at 1080p and 116.586 MiB at 2560×2880, 64 KiB below
the reference. Image pools and AV1 references are unchanged, so the calculated
image-payload targets remain 30.652 and 108.984 MiB. The large remaining gap is
not solved by this kernel. System-memory bitstream reservations are unchanged.
RSS savings are about 1.9–2.7 MiB in these tests; they are not mislabeled as VRAM.

The final AMD package also records 40 seconds of 1080p/240 FPS with all 9,600
submitted frames, no recorder skips, no source-empty callbacks and no coalesced
ticks. Both late exports independently decode after the 30-second window advances.
Sampled VRAM remains 38.285 MiB; CPU is 863.966 ms over 40.017 s, with 59.359 MiB
peak RSS including longer encoded history. This is a generated scene on this
machine, not a promise for all desktop/game workloads.

The first 120 complete decoded frames of matched live portrait/60 FPS clips
also have identical hashes. Fresh samply captures show conversion consuming
about 29% versus 19% of sampled main-thread CPU deltas; encoder submission is
now a larger remaining sampled cost. These overlapping stack estimates include
startup and are diagnostic, not precise performance partitions. Tracy records
360 VPP submissions (66.692 µs mean), 360 source-read waits (64.094 µs mean),
two exports and sleeping portal waits. Instrumented timings remain separate
from the uninstrumented comparison table.

Evidence uses the `paired-vpp-` prefix, including the final package runs,
earlier comparisons, raw-image hashes, failure logs, source/driver manifests,
native sanitizer logs and profiler captures. `tools/benchmark-vpp.sh`,
`tools/check-vpp-driver.sh` and `tools/check-vpp-failures.sh` reproduce the relevant
scopes inside the flake. The failed initial Q=0 and incompatible Tracy-fixture
invocations are retained as evaluator findings; neither is counted as a passing
run. All-metric superiority, energy, game interference, other-vendor runtime and
complete source-to-packet latency remain unestablished.

## Completed-output collection and memory reuse

The fixed pre-change source/executable is preserved in the `packet-readout-`
evidence, alongside its original implementation manifest. The new FFmpeg option
allows a completed VAAPI picture to leave a partially filled asynchronous queue.
The AMD driver exports the coded buffer's writer fence; the recorder integrates
its persistent descriptor into its existing wait. A successful VA completion
query retires the codec's input reference before reuse. No extra input, encoder
flush, lowered bit depth, reduced reference count or periodic polling timer is
used to obtain output.

A single-picture probe first waits for authoritative GPU completion, then asks
the actual replay consumer to save. The original depth-two queue still returns
EAGAIN; the completed-output path saves successfully at the same depth. A pinned
input forces use of the other surface, and releasing that pin permits reuse of
the original surface. Both `available` and `events` also save two independently
decoded real private-portal clips at approximately 174 and 374 ms, before the
first one-second static repeat. The ordinary system Mesa 26.1.8 driver passes
the same early-save test through automatic demand-readout fallback.

The public API tracer measures **accepted encoder submission to packet retrieval**,
not source capture time, pure GPU execution or disk/mux availability. It has no
libavcodec dependency of its own, preventing the diagnostic preload from injecting
a different codec library. On the static four-packet test, buffered delays are
997.164–1001.615 ms; events give 1.708–2.312 ms. Four observations do not establish
a tail distribution. On the 240-packet portrait/60 FPS test, buffered mean/observed
p99 are 16.705/19.015 ms; demand readout gives 16.472/17.714 ms; events give
10.072/11.279 ms. Ordinary file output still needs the next video timestamp to
finalize a VFR packet's displayed duration. These are not claims of corresponding
end-to-end file-write latency.

The first complete comparison uses four alternating repetitions per case against
the saved VPP implementation, matched AV1 settings, producer, capture/replay
contract and teardown. CPU is whole-process time including startup; FPS includes
startup/teardown, so a complete 240-frame four-second capture reports slightly
under 60. RSS includes replay data. Parentheses are sample SD.

| Input | Profile | CPU ms | Peak RSS MiB | Submitted FPS |
|---|---|---:|---:|---:|
| Static portrait, 4 s | control | 21.410 (0.244) | 52.551 (0.490) | 0.99 |
| | events | 14.902 (0.590) | 35.369 (0.483) | 0.99 |
| Portrait/60 FPS, 4 s | control | 55.583 (6.349) | 54.361 (0.607) | 59.70 |
| | events | 51.555 (4.736) | 38.107 (0.462) | 59.61 |
| 1080p/240 FPS, 4 s | control | 80.133 (0.999) | 53.533 (0.367) | 239.20 |
| | events | 79.212 (1.932) | 37.241 (0.418) | 239.31 |
| Portrait/120 FPS offered, 10 s | control | 138.154 (6.651) | 56.204 (0.705) | 104.63 |
| | events | 146.680 (15.803) | 39.887 (0.181) | 104.60 |
| 1080p/1,000 FPS offered, 10 s | control | 197.480 (6.562) | 54.940 (0.377) | 315.26 |
| | events | 196.096 (5.902) | 38.565 (0.648) | 309.32 |

Both variants submit every in-window frame at 60/240 FPS without source misses
or recorder skips. Saturated 1080p throughput is about 1.9% lower in this series;
this is **not** an all-metric win. Source-lease means/maxima and producer-empty
callbacks are retained in the machine-readable summaries. A separate three-way,
rotating-order comparison of the compact library's `buffered`, `available` and
`events` policies gives saturated 1080p means of 311.50 (SD 5.60), 311.28 (3.65)
and 311.84 (2.75) FPS. It does not isolate a notification-specific throughput
penalty or establish equivalence. At portrait/60, demand readout uses 48.186
(4.168) ms CPU versus events' 51.532 (3.907), with the same sampled memory.
Keep the controls; do not hide these tradeoffs with a weighted score.

At portrait/60 the sampled recorder counters fall from **116.586 to 105.328 MiB
VRAM**, **48.020 to 26.863 MiB GTT**, and about **54.4 to 38.1 MiB RSS** in the
four-run series. The VRAM difference is one aligned input backing; the GTT
difference is exactly one 21.15625 MiB coded-output reservation. Shared source
images remain counted in recorder VRAM. Producer allocations are also sampled
separately; they have not been shifted out of the accounting boundary.
Saturated runs retain both buffers and their original VRAM/GTT peaks. A queue
slot is capacity, not evidence that distinct image backing is always required.
The revised conditional image target is 98.4375 MiB with one reusable input,
or 108.984375 MiB with two, plus justified platform overhead. Neither is a
universal codec minimum or a process memory cap.

The broad FFmpeg rebuild initially increased RSS despite unchanged configured
features: `/proc` maps showed about 10 MiB in file PMDs where the fixed reference
used small pages. No kernel memory policy was changed to conceal this cost.
The recorder-specific configuration removes unused library families, preserves
runtime CPU dispatch and every advertised backend/container/audio consumer, and
passes its configuration-selected FATE tests and explicit linked-feature probe.
Full command-line ffmpeg/ffprobe tools remain separate dependencies. The full
original library and previous driver remain runnable controls.

Validation includes 48 native empty/pending/collected/capability-fallback/EMFILE
open/close cycles with unchanged descriptor counts, CLOEXEC checks, input pinning,
and inactive/error/fd-reuse portal tests. EMFILE is induced with a process-local
soft descriptor limit around the actual duplicate call, then restored at release.
ASan/UBSan native integration covers capture, source removal/reconfiguration,
multichannel audio, replay and stop. The complete GCC/Clang/sanitizer and 14-check
x86-64 Nix matrix passes. The linked Vulkan AV1 path also passes replay with
mapped output copied to RAM, both buffered/io_uring sinks, durable completion,
and independent video/two-audio-track decoding. Intel/NVIDIA hardware is not
claimed verified by the build-feature registry.

The new driver matches all 576 raw VPP transitions, both bit depths, linear/native
layouts, scalar color endpoints and independently decoded patterns. The first
120 complete decoded frames of matched live portrait clips have identical hashes.
This preserves the tested pixel contract; it is not an exhaustive spatial or
perceptual quality study. Native completion notification also passes the failure
and teardown probes; driver-loss behavior remains a separate unverified gate.

Fresh confirmation of the final Nix package uses two alternating runs per case
against the previous fixed Nix package. Portrait/60 again uses 105.328 MiB VRAM
and 26.863 MiB GTT versus 116.586/48.020 MiB. Peak RSS is 38.209 (SD 0.500) versus
54.287 (0.135) MiB. CPU is 49.569 (6.240) versus 59.173 (5.607) ms per complete
four-second capture. Static CPU is 14.586 (0.048) versus 21.666 (0.532) ms.
At 1080p/240, both submit all 960 frames, but CPU is 80.444 (2.789) versus
77.100 (1.514) ms. Saturated 1080p throughput is 313.38 (4.15) versus 311.79
(4.88) FPS, reversing the earlier series' direction within substantial variation.
Its largest source-lease observation is 9.435 versus 6.839 ms. This confirmation
supports the resource reduction and preserves the observed mixed CPU/tail results;
it does not establish universal nonregression or throughput superiority.

Two longer final-package runs complete all 2,400 portrait/60 frames and all 9,600
1080p/240 frames over 40 seconds. Neither has source-empty callbacks, coalesced
producer ticks, recorder skips or completion fallback. Both late saves decode
after the 30-second window has advanced. Portrait VRAM/GTT remain
105.328/26.863 MiB; CPU is 403.657 ms over 40.025 s and peak RSS is 45.777 MiB
including the longer history. The 1080p run uses 38.285/15.332 MiB VRAM/GTT,
639.484 ms CPU over 40.015 s, and 44.504 MiB peak RSS. These are two long
confirmations, not additional repeated-run averages.

Fresh samply captures launch the fixed and final unwrapped packages on the same
six-second portrait scene. They contain 234/214 active main-thread sample weights
and reported CPU deltas of 64,677/50,576 µs. Encoder submission remains a leading
sampled cost; candidate completion collection accounts for about 16.6% and VPP
submission about 14.7% of sampled main-thread CPU deltas. These are overlapping
stack estimates including startup, not precise performance partitions or a
replacement for uninstrumented timings. No live user process was attached or stopped.

The actual Tracy capture contains 360 conversions, 360 completion-collection
zones, 360 retained packets and two exports. Collection averages 28.520 µs;
encode/submission averages 66.454 µs; source-read waiting averages 102.589 µs.
The 728 portal-wait zones include blocked wall time. These instrumented host/API
zones are not calibrated GPU execution timestamps. The capture, CSV and sampler
profiles are retained with the measurement scope intact.

Evidence is in [the complete run archive](evidence/packet-readout-evidence.tar.gz),
[fixed source/executable control](evidence/packet-readout-control.tar.gz),
[final package summary](evidence/packet-readout-final-summary.txt),
[verification log](evidence/packet-readout-verification.log), and
[Tracy capture](evidence/packet-readout.tracy). The archive index identifies
authoritative final checks separately from exploratory/failed invocations.
`tools/benchmark-readout.sh`, `tools/check-completion.sh`,
`tools/profile-packet-latency.sh`, the existing sampler/Tracy launchers and the
memory calculator reproduce the corresponding scopes. Compile the diagnostic
packet tracer with headers and `-ldl` only, without linking libavcodec:

```sh
nix develop -c bash -c 'cc -std=c23 -O2 -fPIC -shared $(pkg-config --cflags libavcodec) tools/packet-latency-trace.c -ldl -o /tmp/gpukey-packet-trace.so'
```

The active goal remains open: universal all-metric superiority, power/game
interference, other-vendor runtime, full source-to-file latency, complete quality
comparisons, and the remaining justified-allocation gap have not been established.

## Lazy CPU images and checked synchronous transfers

The next fixed control is the completed-output package, with source/executable
saved before this extension and its starting manifest verified. The first audit
traces actual BO allocations and the two eager `vaDeriveImage` calls from FFmpeg
frame-pool initialization. A call-site-restricted diagnostic suppresses only those
probes on the immutable control library. Its 120-frame portrait comparison remains
identical with explicit timestamp passthrough. This diagnostic is not linked into
the delivered recorder.

The integrated lazy policy derives images when mapping is requested. A capability
failure selects copied implicit writes through one atomic hint, while explicit
direct maps still test their actual surface. The cacheless variant is preserved
as a control: it would repeat an unsupported derivation on every upload. Tests
verify one learned failure, subsequent copied writes, and later successful direct
access after an earlier incompatible result. Normal CPU-read copying remains.

The four-run packaged comparison of that policy against the fixed control covers
static portrait, portrait at 60/120 FPS, 1080p at 240/1,000 FPS, and unpaced CPU
uploads at 320×240, 1920×1080 and 2560×2880. Each upload run has 240 independently
decoded AV1 frames. Matching decoded hashes include presentation times. No
profiler or concurrent build runs during the performance series. New measurements
collect child resource usage after exit, including final reporting/cleanup; older
archives contain the recorder's near-exit counters. Every comparison uses the
same measurement scope for both variants, recorded in the summary.

At portrait/60, sampled VRAM changes from 105.328 to 105.324 MiB and GTT from
26.863 to 23.348 MiB. The meaningful storage reduction is GTT. At 1080p, GTT
changes from 15.332 to 14.270 MiB. Saturated portrait still requires two coded
buffers: GTT is 44.504 versus 48.020 MiB. No reference count, bit depth, quality
setting or coded-output capacity is reduced. Static whole-child CPU is
13.890 ms (SD 0.187) versus 15.388 (0.244) per four-second session.
At portrait/60 the CPU means are 53.015 (6.953) versus 50.081 (3.874) ms;
saturated 1080p throughput is 307.49 (1.98) versus 310.08 (4.99) FPS.
Some RSS and source-lease observations also increase. These mixed results are
retained, not hidden behind a universal improvement claim.

Source audit then found that the original synchronous upload logs late unmap,
writeback and image-release errors while returning zero. The negative control
reproduces all three. The new implementation uses borrowed stack plane views for
synchronous transfers, eliminating their temporary frame/map/reference graph.
Persistent maps retain their owned references. Cleanup status reaches transfer
callers, failed unmap suppresses writeback, and failed copy validation cancels
copied writeback. A clean download destination remains unpublished after a late
failure. Preallocated device bytes can have changed before an error; no rollback
transaction is claimed. Recorder preparation errors are terminal.

The recorder-level injected tenth-upload failure gives a concrete before/after:
the control exits successfully with 20 submitted/decoded frames despite the logged
failure. The checked path exits unsuccessfully after nine submissions, preserves
the partial file and independently decodes its seven-frame valid prefix. This
does not claim that all accepted frames are preserved after a terminal error.

Mapping tests cover NV12/P010, even and odd dimensions, write/overwrite/read/direct
flags, copied fallback, concurrent direct and copied maps, and a live map after
the original frame, frame-context and device owners close. Readback storage is
poisoned, so recycled host pixels cannot fake a successful transfer. The constructor
refactor initially lost pan-scan data; a new metadata regression caught that.
Setting the actual source display extent before property copying preserves
size-dependent metadata, and cropped displays use full allocation capacity in
the tests. The corrected path passes pixels, metadata, cropped extents, lifetime,
capability and cleanup-failure checks.

The full GCC/Clang, ASan/UBSan and 14-check x86-64 Nix matrix passes. Native
sanitizer integration also passes the mapping/transfer regressions, GPU color
oracles, private PipeWire capture, multichannel audio, replay, source loss,
reconfiguration and stop. Dependencies and GPU storage are not made sanitizer-
instrumented by the application/probe builds.

Fresh final-package confirmation uses two alternating runs per workload. It
reproduces the GTT reduction, with 105.324/23.348 MiB VRAM/GTT at portrait/60.
Static CPU is 13.876 ms (SD 0.303) versus 15.533 (0.567); portrait/60 CPU is
48.317 (5.824) versus 47.355 (4.371) ms. Peak portrait/60 RSS is 38.348 (0.586)
versus 38.041 (0.273) MiB. All 240 and 960 frames are submitted in the 60/240 FPS
cases without source misses or recorder skips. At saturated 1080p, mean throughput
is 312.54 (1.42) versus 309.43 (1.50) FPS, but the largest source lease is
10.106 versus 6.431 ms. This supports the storage reduction, not uniform CPU,
throughput or tail-latency superiority.

The CPU-upload confirmation is likewise mixed. At 1080p the final package gives
178.34 (SD 1.40) versus 182.32 (2.29) FPS against the original eager control.
A separate four-run comparison isolates the borrowed transfer path from lazy
initialization: 1080p means are 182.56 (2.81) versus 179.13 (5.10) FPS, portrait
49.71 (0.12) versus 49.84 (0.09), and small images 1182.42 (17.22) versus
1198.22 (62.22). The control in this isolation series still has the old ignored
cleanup errors. Removing its temporary wrappers is a source-audited reduction
in work; these end-to-end measurements do not establish a general speedup.

The final package also records 40 seconds of portrait/60 video: all 2,400 frames,
no source-empty callbacks, coalesced ticks, recorder skips or completion fallback,
and two independently decoded late replay saves. Sampled VRAM/GTT remain
105.324/23.348 MiB. Whole-child CPU is 396.819 ms and peak RSS is 45.609 MiB with
the longer history. Vulkan replay passes again with two audio tracks, both sinks,
mapped-packet copying and durable completion.

Real private shared-memory capture with VAAPI encoding, mono/5.1 audio and two
replay exports also passes. The first attempted fixture was 64×48, below this
device's advertised 320×128 minimum; it is retained as an invalid test setup,
not a product regression. Geometry can now be selected independently of the
fixture's fixed audio/cadence oracle. The corrected 320×240 case is included in
hardware integration, and its software/audio timing expectations are unchanged.

Fresh samply captures cover the actual CPU-upload path at 1080p/60 for six seconds
on both immutable binaries. Reported main-thread CPU deltas are 1,989,800 versus
1,940,523 µs; swscale accounts for roughly 90.5%/90.6% of these sampled deltas.
Scalar chroma conversion/filtering dominates this control workload. Temporary
mapping-wrapper work is smaller. This identifies a remaining optimization target;
sampled stack proportions are overlapping diagnostics, not isolated speedup
measurements. The real Tracy capture contains 360 preparation zones averaging
5.405 ms and 360 encode/submission zones averaging 70.137 µs. Profiling remains
separate from the uninstrumented performance series.

The final [run archive](evidence/lazy-mapping-evidence.tar.gz) includes immutable
binary hashes, allocation traces, all comparisons, negative controls, profiler
data and the index identifying authoritative checks. The
[fixed source/executable control](evidence/lazy-mapping-control.tar.gz),
[final comparison summary](evidence/lazy-mapping-final-summary.txt),
[verification log](evidence/lazy-mapping-verification.log) and
[Tracy capture](evidence/lazy-mapping.tracy) are also retained directly. The final
package and Tracy executable are byte-identical to the measured versions after
the integration-fixture update. `tools/benchmark-gpu-pair.sh` reproduces matched
native and CPU-upload workloads; the map/failure probes use the real VAAPI driver.
The broad optimization goal remains open, including unused workspace capacity,
CPU conversion, power/game interference, cross-vendor runtime and stronger
representation alternatives.

## AV1 workspace allocation and failure recovery (21 October 2026)

The fixed pre-change recorder is
`/nix/store/64lbdzi0r249br2d6l2aipc04s2nnc85-gpukey-0.1.0/bin/gpukey`; the control
driver is `mesa-gpukey-events-26.2.5`. Only the driver changes in these comparisons.
Dedicated singleton allocations, a packed CDF/session pair, and a whole 2 MiB
workspace preserve dimensions, bit depth, reference structure, QP, output capacity
and capture policy. Their source/ownership reasoning is in `dependencies.md`;
their sizes are separate from codec/image lower bounds in `memory-targets.md`.

The first matrix has three rotating-order repetitions across static portrait,
moving portrait/60, saturated portrait/120, 1080p/240 and saturated 1080p/1000.
It compares the control, dedicated buffers and the packed pair. A fresh matrix
uses the same cases and rotation for the control, dedicated buffers and the
whole workspace. Ordinary runs last four seconds; saturation runs last ten.
Source-empty callbacks at saturation are reported separately from recorder skips.
Builds and profilers are excluded from those series. The recorder executable and
producer hashes are fixed, and every replay export passes independent decoding.

At portrait/60, sampled total/private VRAM is 105.324/47.824 MiB for the control,
103.434/45.934 for dedicated buffers, 103.480/45.980 for the packed pair and
104.324/46.824 for the whole workspace. GTT is 23.348 MiB. The second matrix's
whole-child CPU means (sample SD) are 53.773 (1.271), 53.755 (10.532) and
59.364 (0.936) ms for control/dedicated/whole. All submit 240 frames with no
producer misses or recorder skips. This proves the measured allocation saving;
it does not establish a uniform CPU or latency improvement.

At saturated 1080p, the second matrix gives 312.46 (SD 2.97), 309.60 (4.18) and
314.54 (3.32) FPS for control/dedicated/whole. The largest source lease is
7.241, 6.415 and 6.745 ms respectively. At 1080p/240, one whole-workspace run
materializes only one input instead of two. Its lower sampled VRAM/GTT reflects
that real completion/reuse outcome, not a different configured queue capacity.
Raw per-run input/storage outcomes remain available; means are not hard caps.

Allocation fault injection reproduces an exit-139 crash in the control at the
FCB allocation. The candidate validates auxiliary storage before picture-command
emission and retains initialized session backing through the normal destructor.
Dedicated and packed layouts pass CDF allocation/map, SDB and FCB failures;
the dedicated layout also tests its separate session allocation. The whole
layout tests its shared allocation, CDF map and FCB. Every case performs eight
failed/healthy encoder lifetimes in one process and checks descriptor balance.
Diagnostic hooks stay outside the production driver/recorder.

Four-worker tests exercise independent 8/10-bit AV1 streams and repeatedly close
encoders, with independent dav1d pixel/order checks. H.264 I/P/B and HEVC I/P,
plus AV1 CQP and QVBR, produce matching independent decode hashes for 120 frames
per case across the four drivers. QVBR exercises the whole-layout fallback for
a larger pre-encode workspace. The initial 320×240 HEVC attempt failed the
device's advertised minimum width; the corrected matched case is 640×480. That
invalid setup remains in the evidence and is not counted as a product failure.
An added picture-type assertion also caught an incorrect HEVC B-frame test
assumption: this device/driver produces I/P, and FFmpeg's capability-dependent
GOP initialization had silently substituted that structure for `-bf 2`. The
corrected test explicitly requests HEVC I/P and verifies actual decoded types;
H.264 still exercises B-frame order. HEVC B-frame runtime remains untested here.

The separate three-run, twenty-second saturation confirmation reverses the
earlier ranking: control/dedicated/whole throughput is 309.62 (SD 4.48),
312.58 (0.75), and 306.64 (4.76) FPS. Whole-child CPU is 424.538 (34.601),
402.164 (21.862), and 429.820 (39.300) ms. Largest source leases are 11.650,
9.714 and 11.652 ms. These results do not establish a consistent throughput
advantage for the larger block. The default `amd` package selects dedicated
buffers for their repeatable fixed storage reduction; all three controls remain
runnable. This is a scoped engineering choice, not all-metric superiority.

Final verification passes GCC 16, Clang 21, their ASan/UBSan application suites,
all 14 x86-64 Nix checks and formatting. The final barrier-based encoder probe
holds four initialized sessions concurrently, then checks 16 lifetimes and 128
ordered 8/10-bit decoded frames per driver. Encoded-payload SHA-256 and decoded
frame hashes match across all four drivers in the corrected codec matrix.
The selected driver also passes 576 raw VPP transition comparisons, both bit
depths/layouts, independent decoded patterns and 32 concurrent graphics contexts
with 256 checked draws and in-flight teardown. These tests exercise the pinned
RX 9070 XT driver; they do not certify other devices or firmware-failure paths.

The native Clang ASan/UBSan run also passes CPU/GPU capture, multichannel audio,
replay, source loss/reconfiguration and stop, including the concurrent encoder
probe. The fault interposer and repeated recovery probe pass under Clang as well
as GCC. Dependencies and GPU storage are not sanitizer-instrumented by those
application/probe builds. The final packaged executable is byte-identical to the
fixed recorder used in all driver comparisons; its wrapper selects the dedicated
workspace driver and respects explicit driver overrides.

Fresh twelve-second native samply captures report main-thread CPU deltas of
105,352 µs for the control and 96,058 µs for dedicated buffers. Active weighted
sample counts are 274 and 327, so these profiles identify work rather than prove
a CPU speedup. Command-buffer resource insertion, AV1 header generation and
handoff/wait operations are distributed contributors. A separate real Tracy
capture has 360 frames: mean VPP submission 36.954 µs, source-read completion
wait 102.451 µs, encode/submission 50.347 µs and completed-output collection
25.901 µs. These are instrumented wall-time zones, not isolated CPU costs.

The final packaged 40-second portrait/60 recording submits all 2,400 frames,
with no producer-empty callbacks, coalesced ticks, recorder skips or completion
fallback. Both late replay exports pass independent decoding after the rolling
window has evicted old history. Peak VRAM/GTT remains 103.434/23.348 MiB;
whole-child CPU is 433.675 ms and peak RSS 45.605 MiB with the longer history.
The original user-supplied frozen clip remains unchanged.

The [evidence archive](evidence/workspace-evidence.tar.gz) contains the index,
raw runs, faulty test setups, negative control, profiles and immutable hashes.
The [fixed control](evidence/workspace-control.tar.gz),
[fresh matrix summary](evidence/workspace-confirmation-summary.txt),
[longer saturation summary](evidence/workspace-saturation-summary.txt),
[full verification log](evidence/workspace-verification.log),
[long-run counters](evidence/workspace-long-summary.json) and
[Tracy capture](evidence/workspace.tracy) are also retained directly. The
all-metric goal remains active; these are measured allocation/correctness gains,
not proof of universal performance, compression or power optimality.

## Conversion, geometry and consumer confirmation — 23 October 2026 UTC

The fixed pre-change executable and source are retained. The selected implementation
filters RGB with four small rows, then applies the BT.709 matrix once per output
sample. Its optional row cache is 23,040 bytes at 1080p and 30,720 bytes at the
reported portrait width; allocation failure selects a bit-identical scratch-free
kernel. There is no per-frame allocation. The integer coefficient/range analysis
bounds conversion error below 0.505 output codes. Independent double-precision
tests exercise 208 shape/format combinations, including odd and one-pixel edges,
8/10-bit planar/semiplanar output, padding and invalid input. The exact generated
portable, x86-64-v3 and x86-64-v4 bodies pass separately under GCC and Clang.
Lossless AV1 decoding also checks moving chroma impulses at 64×48 and 63×47.
Eleven forced row-cache allocation failures pass the media/lifecycle suite.

The GPU correction keeps sample spacing independent of rounded chroma-plane
dimensions. The independent raw oracle covers four RGB formats, both output
depths, direct and DMA-BUF-imported surfaces, partial tiles and 1×1 inputs. The
previous workspace driver fails that oracle. All 576 existing even-size VPP
transitions remain byte-identical. Two shader-creation and two constant-map
failure/retry cases each verify that the intended fault fired and that the
fallback still produces correct pixels. This does not instrument every possible
driver failure. At 641×481/10-bit, the corrected native near-lossless probe has
U/V MSE 0.07086/0.00697; the earlier ratio-derived grid had much larger errors.

The encoded first picture now supplies validated codec configuration and standard
presentation cropping. A rejected attempt to change AV1 coded dimensions while
retaining firmware padding accumulated prediction errors; its patch and failed
outputs are preserved, and it is absent from runnable packages. The selected
path keeps the valid coded image. mpv 0.41 presents and exports 641×481 from a
648×482 coded picture. Brave 155 ignores that crop in tested Matroska, MP4 and
WebM playback, including the default GPU path. This compatibility limit remains.
Replay exports publish their known duration: Brave's tested static clips end at
the exact saved endpoints. Unknown-duration ordinary Matroska playback in Brave
still ends at the last PTS rather than honoring the final BlockDuration. MP4 and
the explicitly sized replay exports avoid that observed case.

First-output bootstrap, observed configuration, checked parser borrowing and
Matroska duration options are tested independently. The borrowed parser input
stays alive through parser teardown; inputs without proven extent and zero
padding keep the copy path. Vulkan replay passes independent video/two-track
audio decoding with both buffered and io_uring durable output. Separate 8/10-bit
641×481 VAAPI replay exports preserve crop metadata and both audio tracks.

### Matched performance and quality

The final packaged comparison rotates order over three runs per case, each with
240 deterministic frames at the same quantizer, source geometry, bit depth and
selected driver. CPU is completed-child user plus system time, including startup
and teardown. Builds, profilers and quality evaluation run outside the timing
series. These are synthetic upload/conversion/encode workloads, not native
desktop-capture CPU percentages. Means and sample standard deviations follow.

| CPU-input workload | Control CPU ms | Selected CPU ms | Control FPS | Selected FPS |
|---|---:|---:|---:|---:|
| 320×240, 8-bit | 91.524 ± 0.375 | 58.183 ± 0.693 | 928.11 ± 46.52 | 1001.71 ± 46.09 |
| 1920×1080, 8-bit | 1535.238 ± 12.690 | 646.767 ± 18.248 | 153.75 ± 2.49 | 289.30 ± 1.40 |
| 2560×2880, 8-bit | 5346.201 ± 17.731 | 2505.979 ± 42.689 | 44.27 ± 0.45 | 90.91 ± 0.75 |
| 1920×1080, 10-bit | 1571.259 ± 13.590 | 755.923 ± 9.135 | 151.56 ± 1.28 | 280.32 ± 4.12 |

Sampled VRAM is unchanged in those upload cases; RSS means are lower by roughly
0.8–1.7 MiB. The odd-size control is timed but rejected by the presentation oracle,
so it is not presented as an equivalent correct result. The selected odd-size
file independently decodes and scores all 240 presented frames.

The stronger swscale control uses the corrected full-chroma-input flag with the
same current application, compiler flags and driver. Another three-run matrix
measures 1080p CPU at 1174.443 ± 10.675 ms versus 629.006 ± 11.769 ms for the
selected kernel; portrait CPU is 4253.077 ± 16.533 versus 2481.148 ± 13.976 ms.
Ten-bit 1080p is 1216.566 ± 4.914 versus 744.681 ± 5.172 ms. At 320×240 CPU
falls from 77.303 to 57.570 ms, while throughput is 995.75 ± 19.53 versus
992.33 ± 33.09 FPS. Small-image throughput is not a proven win. The modern
swscale frame API retained the same implementation in the inspected cases;
the pinned operations backend rejects these subsampled/semiplanar formats.
RGB upload followed by GPU processing remains an unevaluated integrated challenger.

All three payload hashes match within each case/variant. The independent dav1d
quality comparison uses the declared filters and applies only advertised container
crop. It does not crop malformed control files into apparent correctness. For
1080p/8-bit, selected Y/U/V PSNR is 55.568/60.867/61.222 dB versus
55.560/60.850/61.171; payload is 378,511 versus 376,211 bytes. For 1080p/10-bit,
it is 61.029/58.756/61.205 versus 60.966/58.323/61.807 dB, with 421,100 versus
420,446 bytes. The V-channel loss and small payload increases are explicit
tradeoffs, not hidden in an invented aggregate score. The corrected swscale
control's payloads equal the scored legacy payloads on these even-boundary
patterns; the separate impulse tests expose the old siting error.

The native three-run matrix retains 103.434 MiB total / 45.934 MiB private VRAM
and 23.348 MiB GTT at 2560×2880/60. Its 240 submitted frames have no producer
misses or recorder skips. Control/selected whole-child CPU is 97.098 ± 11.290 /
100.708 ± 3.664 ms. Saturated 1080p throughput is 300.07 ± 0.59 / 297.13 ± 4.83
FPS; mean source leases are 3.441 / 4.206 ms, with maxima 7.534 / 8.461 ms.
The other cases and bucketed tail estimates are retained in the raw summary.
These results do not establish a native performance improvement or universal
nonregression. The correctness fixes and CPU-input gains are separate conclusions.

### Actual profiles and verification

Fresh samply captures use private recorder children, never attachment to the
user's session. In the four-second 1080p shared-memory/two-audio-track capture,
main-thread CPU deltas are 1,392,187 µs for the control and 698,427 µs for the
candidate. Scalar chroma work dominates the control; the selected converter
accounts for about 60.5% of candidate main-thread CPU, with copy/upload and audio
still present. These sampled profiles identify costs; the unprofiled comparison
above measures the gain. A separate twelve-second native profile records
210,934 µs main-thread CPU, distributed across driver submission, allocation,
copying and synchronization. It is not a matched native speedup measurement.

Real Tracy captures retain two independently decoded exports each. The six-second
native capture contains 360 frames: mean VPP submission 68.911 µs, source-read
completion wait 122.238 µs, encode/submission 105.714 µs and completed-output
collection 75.610 µs. The four-second CPU-input capture contains 201 frames and
two audio tracks; mean prepare/upload is 2.594 ms. These are instrumented wall-time
zones, not isolated CPU costs, and include their relevant waits.

Final verification passes GCC 16, Clang 21, ASan/UBSan application suites, all 14
x86-64 Nix checks and formatting. Native Clang ASan/UBSan passes hardware color,
first-output bootstrap, concurrent encoder lifetimes, raw chroma and the full
private CPU/GPU/audio/replay/source-loss integration. Application sanitizers do
not instrument the driver or GPU allocations. The supplied material and the
original user clip remain unchanged.

The [evidence index](evidence/conversion-index.txt),
[full archive](evidence/conversion-evidence.tar.gz),
[fixed source/control](evidence/conversion-control.tar.gz),
[packaged comparison](evidence/conversion-confirmation-summary.txt),
[corrected swscale comparison](evidence/conversion-fullinput-summary.txt),
[native comparison](evidence/conversion-native-summary.txt) and
[verification log](evidence/conversion-verification.log) preserve the mechanisms,
failed experiments and actual measurements. The all-metric goal remains active;
these results do not establish universal quality, power or performance optimality.

## AV1 header ownership and construction — 23 October 2026 UTC

The selected library removes unused CQP field tracing, writes single packed OBUs
directly into caller-owned buffers and avoids a redundant-header cache that the
VAAPI consumer never uses. Generic CBS callers retain their behavior. Required
reference state, non-CQP bit offsets, metadata order and the existing packed-header
capacity remain. Raw sequence/frame/metadata objects no longer persist in the
encoder context. Their exact layout reduction is derived as 17,160 bytes on the
tested ABI, before allocator rounding; a 2,856-byte raw object is now temporary
stack storage during a synchronous call. The unused persistent 1 MiB generic
write-buffer reservation also disappears. This is not a claim of a 1 MiB RSS
saving. The [memory accounting](memory-targets.md) prices these costs separately.

The independent control is the fixed preceding package with the same driver and
codec settings. Eighteen early hardware recordings and six final packaged cases
cover 640×480, 641×481 and 2560×2880 at both depths, 120 frames each. Encoded
payload hashes and independent decoded frame hashes match the control exactly.
The public VAAPI probe additionally compares CQP/QVBR, repeated keys, 8/10-bit
input and changing MDCV/CLL metadata subsets. A diagnostic interposer observes
the exact packed bytes and rate-control offsets before they enter the driver;
all match. No interposer is part of production.

The isolated writer oracle compares the ordinary fragment writer with the direct
writer at five OBU-size-field settings, with both ordinary and hidden/show-existing
frame streams. It checks exact bytes, offset callbacks, reference-state fields,
buffer canaries and independent dav1d output. It overwrites borrowed raw input
after every call, checks all capacities from zero through 63 bytes, injects a
sequence-owner allocation failure and closes failed contexts. GCC and Clang
ASan/UBSan pass. These checks exercise the show-existing mechanism without
claiming hardware AV1 B-frame validation on a device that does not expose that
tested configuration. The actual picture/reference settings remain unchanged.

An initial metadata assertion was wrong for the fixed driver: the first decoded
picture lacks metadata, while subsequent pictures contain it. The corrected
probe separately checks metadata handed to VAAPI and compares decoded properties
against the fixed control. The failing original setup is retained. The recorder
does not advertise HDR recording, and this continuation does not claim to fix
that first-picture metadata delay.

### Matched measurements

The first rotating three-run matrix compares the unchanged library, CQP tracing
elimination alone and the initial direct writer. Results are mixed. At portrait/60
the control uses 52.038 ± 4.379 ms whole-child CPU, tracing-only 60.052 ± 3.467,
and direct 57.935 ± 5.456. Saturated 1080p is 283.816 ± 7.229, 274.272 ± 5.317
and 275.989 ± 0.767 ms respectively. This matrix did not establish an all-case
CPU improvement and is retained rather than discarded.

The final compact variant then receives fresh three-run confirmation, rotating
order, with eight-second paced/static cases and twenty-second saturated cases.
CPU includes complete child startup/teardown. Builds, profilers and independent
analysis are outside timing runs. Values below are means and sample standard
deviations; three runs do not establish universal nonregression.

| Native workload | Control CPU ms | Compact CPU ms | Control FPS | Compact FPS |
|---|---:|---:|---:|---:|
| Static portrait | 15.083 ± 0.376 | 14.500 ± 0.397 | 1.00 | 1.00 |
| Portrait, 60 Hz | 93.361 ± 2.348 | 90.900 ± 5.249 | 59.81 ± 0.01 | 59.81 ± 0.01 |
| Portrait, offered 120 Hz | 305.529 ± 12.806 | 284.319 ± 10.558 | 104.65 ± 0.08 | 104.66 ± 0.04 |
| 1080p, 240 Hz | 158.718 ± 7.040 | 157.709 ± 10.537 | 239.66 ± 0.02 | 239.62 ± 0.04 |
| 1080p, offered 1000 Hz | 418.141 ± 7.278 | 411.087 ± 2.732 | 311.05 ± 0.33 | 310.65 ± 0.90 |

The CPU means improve by roughly 0.6–6.9%; the smaller differences are within
observed variation. Throughput is effectively unchanged in these measurements.
Mean source leases at portrait/60 are 0.163 ms for both; saturated 1080p is
4.575 versus 4.576 ms. The corresponding largest observed leases are 0.578/0.601
and 7.280/7.577 ms. Neither tails nor latency are claimed to improve uniformly.
The paced cases have no producer misses or recorder skips; overloaded producers
report their misses separately. VRAM/GTT is unchanged: portrait/60 remains
103.434/23.348 MiB, including 57.5 MiB of shared source storage. RSS is mixed,
including 38.240 ± 0.393 versus 38.434 ± 0.487 MiB at portrait/60. Full per-run
results, bucketed tail estimates, context switches and page-fault counts remain
in the evidence. The structural allocation reduction is not substituted for
measured process-memory results.

### Final verification and profiles

GCC 16, Clang 21, application ASan/UBSan, all 14 x86-64 Nix checks and formatting
pass. Native Clang ASan/UBSan passes GPU color, first-output bootstrap, concurrent
encoder lifetimes, the CQP/QVBR metadata probe and private CPU/GPU/audio/replay
integration. The standalone CBC writer code itself is sanitizer-instrumented;
the normal production FFmpeg/driver libraries are not made instrumented by the
application sanitizer build. Vulkan replay also passes both durable output sinks
and independent decoding of its video and two audio tracks.

Fresh twelve-second samply captures report 106,897 µs main-thread CPU for the
control and 109,917 µs for the compact build, with active sample weights 732 and
548. These profiles locate work; they do not demonstrate a CPU speedup. Driver
command submission, allocation and synchronization remain distributed costs.
The actual six-second Tracy capture contains 360 encoded frames: mean VPP
submission 35.980 µs, source-read completion wait 103.767 µs, encode/submission
50.978 µs and completed-output collection 24.592 µs. These are instrumented wall
zones, including waits. Two replay exports decode independently.

The final packaged executable is byte-identical to the compact binary used in
confirmation. Its forty-second portrait/60 replay submits all 2,400 frames with
no recorder skips, producer-empty callbacks, coalesced ticks or completion
fallback. Both late saves decode after the thirty-second live window has evicted
old history. Whole-child CPU is 420.074 ms, peak RSS 45.043 MiB and peak live
encoded history 8,076,263 bytes for this particular synthetic scene. VRAM/GTT
remains 103.434/23.348 MiB. These are fixture measurements, not predictions for
arbitrary desktop content. The supplied material and original user clip remain
unchanged.

The [index](evidence/headers-index.txt), [raw archive](evidence/headers-evidence.tar.gz),
[fixed source/control](evidence/headers-control.tar.gz),
[initial matrix](evidence/headers-comparison-summary.txt),
[fresh confirmation](evidence/headers-confirmation-summary.txt),
[verification log](evidence/headers-verification.log) and
[long-run counters](evidence/headers-long-summary.json) preserve the evidence.
Default packages select the compact implementation; `amd-headers-reference`,
`mapping` and `header-trace` retain the unchanged and simpler controls. The
all-metric goal remains active, including the remaining native submission costs,
memory gap, broader hardware, quality, power and game-interference comparisons.

## Command-resource ownership and compact hints — 23 October 2026

The preceding header milestone is the fixed reference. One identical packaged
executable and fixture compare the old driver with four corrected drivers:
independent fixed tables, adaptive context-owned hints, exact per-list maps and
atomic shared hints. The selected AMD profile now uses adaptive hints. Exact,
fixed, shared and unchanged-driver profiles remain available; this selection is
not a claim that one algorithm wins every regime.

The original shared array fails a deterministic paused-worker membership test
and a ThreadSanitizer race test. The checks extract the actual lookup/growth/add
helpers from the pinned and patched Mesa sources. Independent expected object
indices check membership, duplicate IDs, all three buffer types, low-bit
collisions, 15-bit index wrap, growth, allocation failure, concurrent publication,
context reuse and teardown through 66,048 objects. Corrected candidates pass
Clang ASan/UBSan and TSan. These are instrumented helper/lifecycle-model checks,
not an instrumented execution of all Mesa. Actual queue publication/completion
ordering is audited separately. The additional acquire fence follows the native
completion wait before a context's worker-written state is reused.

All four corrected drivers pass native color and odd-chroma checks, four-worker
encoder lifetime tests, and 32 graphics contexts across eight threads with
in-flight teardown. The selected driver also matches 576 raw VPP transitions
and both bit-depth/layout decoded comparisons against the old driver. No codec
configuration or buffer-reference ownership is weakened.

The first rotating three-run matrix is retained and has small/mixed differences.
Fresh confirmation uses eight-second paced/static and twenty-second saturated
sessions, three runs per variant, reversing case order and rotating driver order.
No builds or profilers overlap the performance series. CPU includes complete
child startup/teardown; values are mean and sample standard deviation:

| Native workload | Old driver CPU ms | Adaptive CPU ms | Old FPS | Adaptive FPS |
|---|---:|---:|---:|---:|
| Static portrait | 15.860 ± 0.440 | 15.634 ± 0.412 | 1.00 | 1.00 |
| Portrait, 60 Hz | 114.227 ± 9.306 | 109.289 ± 6.982 | 59.83 ± 0.01 | 59.81 ± 0.01 |
| Portrait, offered 120 Hz | 332.447 ± 22.076 | 283.557 ± 12.645 | 103.67 ± 1.44 | 104.68 ± 0.05 |
| 1080p, 240 Hz | 184.139 ± 14.298 | 171.965 ± 11.749 | 239.56 ± 0.08 | 239.60 ± 0.06 |
| 1080p, offered 1000 Hz | 477.358 ± 44.223 | 458.311 ± 70.515 | 312.03 ± 0.42 | 311.97 ± 0.36 |

The lower CPU means do not establish universal nonregression: several differences
are within variation, the first matrix is mixed, and three rotations do not
balance every position among five variants. Source-lease maxima are also mixed.
Old/adaptive portrait/60 maxima are 1.206/1.199 ms; saturated portrait maxima are
20.916/17.271 ms; saturated 1080p maxima are 6.959/6.955 ms. Static first-source
leases increase from a 0.569 to 0.614 ms mean. All recordings report zero recorder
skips; overloaded producers report their own missed opportunities separately.
Full per-run counts, bucketed tails and all four candidates remain in the data.

RSS is mixed: old/adaptive portrait/60 means are 38.284/37.816 MiB, while 1080p/240
means are 36.885/37.540 MiB. Adaptive portrait/60 VRAM/GTT stays 103.434/23.348 MiB.
Some old/fixed/shared portrait/60 runs report a higher sampled peak consistent
with spare-input materialization; this is not a measured table-layout VRAM saving.
Saturated portrait needs that overlap storage with every variant. The conditional
98.4375 MiB dense-image target and remaining platform gap are unchanged.

The isolated complete-batch comparison includes insertion, two reordered lookups
per object, reference release, cache reset and a separately reported cold start.
It uses actual helper code but synthetic 64-byte BO records and sequential batches;
it does not measure concurrent worker contention or whole-driver cost. Three
rotations show adaptive/exact times of 391/418 ns at 32 mixed, collision-free
objects and 51.5/63.1 µs at 4,096. With low-bit collisions at 4,096 mixed objects,
the ranking reverses to 2.30 ms/66.4 µs. At 33,024 entries per context, exact maps
are faster but use 512 KiB of table payload versus adaptive's capped 128 KiB.
The exact profile makes that advantage usable; its larger storage is not hidden.

Retained evaluator failures include a TSan runtime crash with C11 thread creation
(corrected to intercepted pthread creation), and a native graphics harness that
mixed system Mesa GBM with the candidate EGL driver. The corrected harness sets
GBM, GL, EGL and VA driver paths consistently and checks loaded module provenance.
A new benchmark diagnostic exposed Clang 21's incomplete `%w64u` format checking;
an explicit `unsigned long long` conversion fixes the diagnostic without changing
the measured helpers. The original measured harness is saved and hash-verified.

GCC, Clang, application ASan/UBSan, formatting and all **15** x86-64 Nix checks
pass. The new `command-hints` check reproduces the original deterministic failure
and validates all corrected helper variants without requiring a GPU. The final
packaged recorder executable is byte-identical to the one used in confirmation;
the process-local driver selection is the change. Native application ASan/UBSan
passes hardware checks and the full private capture/audio/replay integration,
including source reconfiguration and serial/depth-two operation. Four CQP/QVBR,
8/10-bit AV1 packet files also match the old driver exactly and decode correctly.

Fresh twelve-second samply sessions report 107,204/121,643 µs of main-thread CPU
for old/adaptive, with active sample weights 565/670. These profiles do not show
a speedup. Allocation, dynamic linking, locks and buffer submission remain
distributed costs; the new-driver profile includes more loader work. A real
six-second Tracy capture contains 360 encoded frames. Mean VPP submission,
source-read wait, encode/packet submission and completed-output collection are
41.306, 96.365, 58.375 and 29.720 µs respectively. These instrumented wall zones
include waits, not just CPU execution. Both profilers' replay clips decode.

The final 40-second packaged replay submits all 2,400 frames with zero recorder
skips, producer-empty callbacks, coalesced ticks or completion fallback. Both
late exports decode after history eviction. Whole-child CPU is 437.883 ms,
peak RSS 43.953 MiB and peak retained encoded payload 8,076,263 bytes for this
particular fixture. Sampled VRAM/GTT peaks are **114.379/44.504 MiB**, including
57.5 MiB shared VRAM. Most VRAM samples are 103.121 MiB; 92 of 1,921 reach the
higher peak. This is consistent with materializing the spare input/output
capacity late in the run; individual allocations were not traced in this run.
It is not legitimate to substitute the smaller paced-run peak for that observed
maximum. The next memory audit must distinguish live overlap from retained
high-water capacity. No lower VRAM claim is made for this bookkeeping change.

The [index](evidence/hints-index.txt), [raw archive](evidence/hints-evidence.tar.gz),
[fixed control](evidence/hints-control.tar.gz),
[confirmation](evidence/hints-confirmation-summary.txt),
[helper comparison](evidence/hints-core-summary.txt),
[verification](evidence/hints-verification.log) and
[long-run counters](evidence/hints-long-summary.json) preserve the evidence.
The original material and supplied clip remain unchanged. The broad goal is
active; remaining driver allocation-failure paths and memory high-water retention
are concrete follow-ups, alongside cross-recorder/hardware/quality/power testing.

## Direct RGB AV1 and lifetime verification — 23 October 2026

The opt-in `rgb-amd` package now records native RGB DMA-BUFs through Vulkan AV1.
The ordinary VAAPI package remains the default. This follows the direct-input
plan in `material/research/chapter_gpu.md`: capture storage is also encoder input,
with separate source, reconstruction and encoded-output lifetimes. It uses the
advertised [Vulkan RGB conversion extension](https://docs.vulkan.org/refpages/latest/refpages/source/VK_VALVE_video_encode_rgb_conversion.html),
with exact modifier queries, native channel-order views, BT.709 limited-range
conversion, two past references and three queue-owned reconstruction images.
This is tested on the RX 9070 XT; it is not an all-hardware or every-metric win.

The final package is `/nix/store/4fsykvy59bv48gl14plaplbgrpizkq9q-gpukey-rgb-amd`.
Its process-local RADV driver is
`/nix/store/0jq2axwnmxcargkdlmpjjv02m7g4aggx-mesa-gpukey-rgb-26.2.5`.
The fixed reference is the earlier `q7rkq342smywmjd4c3iv4syh1qw9lvrz` AMD package,
using the same previously verified Gallium producer/VA driver. Measurements use
unwrapped GCC 16 Nix executables with matching hardening settings. Early results
using a development executable are retained as exploratory evidence.

Three rotating eight-second runs per program and workload use the same private
portal source, two source buffers, AV1 CQP 32, no B frames, depth-two capacity,
geometry, cadence and color declarations. CPU values include startup and teardown.
Memory values are sampled process counters, not instantaneous peak bounds.

| Workload | Program | CPU ms, mean ± SD | RSS MiB | VRAM MiB | GTT MiB |
|---|---|---:|---:|---:|---:|
| 1080p60 | Fixed VAAPI | 61.632 ± 0.405 | 36.249 | 32.633 | 8.230 |
| 1080p60 | Direct RGB | 55.777 ± 2.074 | 51.789 | 28.844 | 8.082 |
| Portrait60 | Fixed VAAPI | 81.145 ± 2.906 | 37.840 | 103.434 | 23.348 |
| Portrait60 | Direct RGB | 82.896 ± 0.471 | 52.716 | 91.555 | 23.242 |
| 1080p240 | Fixed VAAPI | 148.805 ± 2.312 | 36.723 | 36.391 | 14.270 |
| 1080p240 | Direct RGB | 123.419 ± 6.339 | 51.522 | 28.848 | 14.078 |

All decoded identities are monotonic, with no interior missing or repeated
identities in these runs. Final boundary counts vary slightly; the retained
per-run results include source and decoded counts. Portrait CPU is mixed rather
than an established improvement. Direct RGB has higher RSS. Its constant-patch
mean/max absolute errors are 0.500/1.441 eight-bit codes, compared with
0.184/0.441 for the reference. These patches do not establish full-scene quality,
compression efficiency, game interference or energy use.

Container bytes per submitted input are approximately 1,563/1,556 at 1080p60,
3,710/3,773 at portrait60 and 815/815 at 1080p240 (reference/candidate). The portrait
candidate is about 1.7% larger in this source pattern. These include mux overhead
and are not a quality-matched rate-distortion comparison.

Mean portrait source leases are 0.166 ms for VAAPI and 16.742 ms for direct RGB,
which intentionally retains the latest source for repeats. These are ownership
durations, not packet latencies. One 1080p240 candidate run observes two producer
callbacks without an available source buffer; all other measured runs report
zero. Consecutive decoded identities therefore do not prove zero producer
backpressure or universal throughput superiority.

A fresh forty-second portrait60 replay pair advances beyond the 30-second window
and saves at approximately 35 and 38 seconds. Both clips decode independently.
The first saved clips each contain 1,856 distinct consecutive source identities.

| Program | CPU ms | RSS MiB | VRAM MiB | GTT MiB |
|---|---:|---:|---:|---:|
| Fixed VAAPI | 385.837 | 45.406 | 103.434 | 23.348 |
| Direct RGB | 300.627 | 61.730 | 91.555 | 23.242 |

This is one longer pair, not another repeated-run mean. Both VRAM counters include
57.5 MiB shared with the producer. RSS and GTT must not be blindly added: pages
can be attributed to both. The dense direct-input image target remains
87.890625 MiB under the stated two-source/two-reference model. Native reconstruction
allocations are 11,087,872 bytes each, compared with 11,059,200 dense bytes.
Together with the two source allocations, images occupy 89.222656 MiB; other
observed storage remains in the audit. See [memory targets](memory-targets.md).

The final implementation removes unused work across both ends of the pipeline:

- Capture imports persist by allocation generation. A last-image pin and encoder
  references retain source leases; no CPU raw-pixel copy or converted input image
  is introduced. Producer-fence waiting services audio and control events.
- Reconstruction ownership follows actual reference use on one encode queue,
  independently of the longer CPU picture-wrapper lifetimes. Ordered barriers
  preserve previous reads before storage reuse.
- Frame execution pools are allocated on first use. The unused download pool and
  duplicate command-buffer array are removed. Release/acquire publication and a
  constructor mutex preserve complete initialization and retry after failure.
- A completed file-recording packet can be finalized at the next input timestamp
  when no older input remains in flight. This releases its mapped output buffer
  before a new one is requested. In-flight work keeps the receive-order path.
  The earlier portrait file candidate used 44.398 MiB GTT; final measurements use
  23.242 MiB. Overlap or muxer retention can still require additional capacity.

The dependency audit also corrected forced-keyframe rate-control declarations,
AV1 level timing, input channel-order views, initial non-host-copy image layout
transitions, partial-picture list unlinking, GPU view/output-buffer leaks and CBS
teardown. Pool destruction now accounts for partially constructed children and
is idempotent. [Destroying a Vulkan command pool frees its command buffers](https://docs.vulkan.org/refpages/latest/refpages/source/vkDestroyCommandPool.html),
so a duplicate handle array and individual free pass are unnecessary. A native
allocation-failure trace exposed RADV cleanup walking a list before it had been
initialized. The local patch establishes destructor-visible state first. The old
path crashes in `radv_destroy_cmd_buffer`; the patched path returns a clean error.
A failed first input-pool construction subsequently retries and encodes/decodes
60 pictures without validation errors.

Validation includes 250-frame ordinary/queue-owned reconstruction controls with
identical decoded hashes, forced keyframes, portrait and ten-bit RGB probes,
submission/export/view failures, native multichannel audio (16+16), replay,
static repeats, source reconfiguration and missing timestamps. The exact pool
helper is extracted from the delivered patch for GCC, ASan/UBSan and TSan tests
of failure/retry, publication and a paused initializer. Those tests model pool
construction, not arbitrary concurrent Vulkan command execution; the recorder
has one media owner. Native ASan/UBSan checks keep RADV loaded for leak inspection.
Unloading it leaves a separate 256-byte report; the earlier 1,054,312-byte FFmpeg
leak is fixed. Neither the complete driver nor FFmpeg is sanitizer-instrumented.

The startup-audio regression uses a 30 ms readiness deferral for the first DMA-BUF.
This models event-loop ownership while actual producer writes have completed; it
is not a forced hardware-write-fence test. Existing later-tone checks passed even
when early samples were lost. The new independent AUX check uses the known
200 ms impulse to locate a 5–15 ms startup window. Earlier candidates have zero
amplitude there; the fixed recording preserves approximately 0.15 on both tracks.
Audio is now kept queued until the video epoch is known, including while the
first producer buffer remains pending. The default backend receives this fix too.

Fresh Samply captures and a Tracy trace are included. Symbolication uses the
sampler's sidecar tables, including ELF debug identifiers when code IDs are absent.
In the candidate main thread, `vulkan_encode_issue` accounts for about 30.6% of
sampled CPU deltas and `radv_enc_headers_av1` for 18.7%; these inclusive stack
fractions overlap. They include startup and are not precise partitions of total
CPU time. Tracy records 360 submissions/retentions and two exports; average
submission/packet handling is 73.778 microseconds and collection is 5.777
microseconds across 724 collection zones. Portal waits include sleeping time.
A separate public-submit-to-receive trace observes 360 packets per program:
reference mean/observed-p99 9.346/9.884 ms and candidate 9.297/9.779 ms. This excludes
capture/conversion before submission and mux/disk completion; it does not prove
full capture-to-packet latency superiority.

The RGB validation-layer fork repairs profile-chain tracking and the RGB query
allowlist. It is a diagnostic fork, not an unmodified-upstream validation pass.
Its negative profile control still detects VUID 04854. Unsupported range/model
requests fail before output creation. The noncoherent readback invalidation
branch is source-audited, not independently exercised on this GPU. CUDA interop,
other physical GPUs, full-scene quality and device-loss recovery remain outside
this native verification scope.

Evidence and reproduction are indexed in [rgb-index.txt](evidence/rgb-index.txt).
The default remains VAAPI because the measured results retain meaningful RAM and
color-precision tradeoffs. The active goal continues; VA encode-time RGB processing,
minimal video-only device setup, and moving private reconstruction initialization
onto the encode queue remain open alternatives, not disproved designs.

## ACO-only RGB driver — 23 October 2026

The next RAM audit identified an avoidable dependency in the optional RGB driver.
Its `libvulkan_radeon.so` linked LLVM although this recorder uses the hardware AV1
path and RADV's ACO compiler. The Mesa build files make LLVM optional. Applying
the already-tested ACO configuration to the RGB driver removes LLVM and unused
compiler tooling. `mesa-rgb-reference` preserves the previous driver exactly.
The new driver is `/nix/store/896yc4wnz6wn804kdbl1638gl243vskz-mesa-gpukey-rgb-26.2.5`;
the wrapper is `/nix/store/4ils1mnbzaj2s92dkwwnyfwzplzv7yk7-gpukey-rgb-amd`.
The recorder executable is byte-identical to the preceding RGB package.

The observer now supports opt-in `--smaps-after SECONDS`, or
`GPUKEY_RECORDER_SMAPS_AFTER` through the comparison launcher. It records mappings,
RSS, PSS and private/shared pages separately. Unavailable data is reported as an
error rather than zero, and a permission-denied control confirms that the child
continues normally. The [kernel's smaps documentation](https://docs.kernel.org/filesystems/proc.html)
defines RSS as resident mapped memory and PSS as proportionally shared memory.
Mapping walks can race changes and do not establish instantaneous peak bounds.
They are diagnostic runs, separate from CPU measurements.

At approximately three seconds into portrait60 recording:

| Variant | RSS MiB | PSS MiB | Anonymous MiB |
|---|---:|---:|---:|
| VAAPI reference | 38.586 | 20.558 | 4.637 |
| Previous RGB driver | 53.387 | 44.449 | 7.996 |
| ACO-only RGB | 27.000 | 18.014 | 4.719 |

LLVM accounts for 25.180 MiB resident in the old RGB process. The new driver has
neither the LLVM dynamic dependency nor a loaded LLVM mapping. PSS is conditional
on sharing with other processes; it is not a portable process-memory minimum.

The native RGB matrix passes again. Four 250-frame cases (ordinary/queue-owned
reconstruction, ten-bit RGB and portrait, including forced keyframes) have
identical compressed packet hashes and decoded frame hashes across the two
Vulkan drivers. The tested AV1 capabilities, formats and modifiers also match.
This is output equality on those inputs, not proof about every possible shader
or source. LLVM shader compilation is omitted from this specialized profile;
the old profile remains available.

Twenty-seven fresh eight-second runs rotate three variants and three workloads.
The same binary runs both Vulkan variants; only the driver changes. Source,
geometry, cadence, CQP 32, depth two, two source buffers and output settings match.
No mapping walk, compiler or profiler overlaps these timing runs.

| Workload | Variant | CPU ms, mean ± SD | RSS MiB | VRAM MiB | GTT MiB |
|---|---|---:|---:|---:|---:|
| 1080p60 | VAAPI | 62.374 ± 2.352 | 36.633 | 32.633 | 8.230 |
| 1080p60 | RGB with LLVM | 58.073 ± 0.029 | 51.581 | 28.844 | 8.082 |
| 1080p60 | RGB with ACO | 52.381 ± 2.268 | 24.891 | 28.844 | 8.082 |
| Portrait60 | VAAPI | 81.786 ± 13.073 | 38.167 | 103.434 | 23.348 |
| Portrait60 | RGB with LLVM | 79.601 ± 5.972 | 52.522 | 91.555 | 23.242 |
| Portrait60 | RGB with ACO | 64.387 ± 6.812 | 26.342 | 91.555 | 23.242 |
| 1080p240 | VAAPI | 140.769 ± 5.765 | 36.816 | 36.391 | 14.270 |
| 1080p240 | RGB with LLVM | 120.140 ± 10.093 | 51.720 | 28.848 | 14.078 |
| 1080p240 | RGB with ACO | 117.301 ± 6.755 | 25.245 | 28.846 | 12.079 |

The last GTT mean contains two 14.078 MiB runs and one 8.082 MiB run; it reflects
whether overlap materialized another output buffer, not a smaller capacity rule.
All these runs have zero repeated/interior-missing decoded identities and zero
producer-empty/coalesced callbacks. Final boundary counts vary slightly and are
retained in the per-run data. The initial partial series is excluded: the input
hash gate caught a diagnostic-tool edit made while a run was active. Confirmation
restarted with frozen inputs; the failure is retained.

Fresh forty-second portrait60 replay runs save twice after the 30-second history
window advances. Both clips decode; the first clips each contain 1,856 distinct
consecutive source identities.

| Variant | CPU ms | RSS MiB | VRAM MiB | GTT MiB |
|---|---:|---:|---:|---:|
| VAAPI | 403.268 | 45.844 | 103.434 | 23.348 |
| RGB with LLVM | 287.213 | 60.562 | 91.555 | 23.242 |
| RGB with ACO | 296.057 | 34.809 | 91.555 | 23.242 |

These are single longer runs. CPU compared with the previous Vulkan build is
mixed, not an established steady-state gain. RAM improves in both the repeated
file runs and longer replay, while VRAM and output semantics remain unchanged.
The RGB color-patch error remains 0.500 mean/1.441 maximum eight-bit codes versus
VAAPI's 0.184/0.441. Full-scene rate-distortion and game interference remain open.

Fresh Samply profiles confirm that encoder submission remains a leading sampled
cost. Inclusive main-thread `vulkan_encode_issue` CPU-delta fractions are about
32.7% with LLVM and 35.7% with ACO; these overlap other frames and include startup.
They do not establish a CPU regression or improvement. Tracy records 360 input and
retention events, two exports, 69.523 microsecond mean submission/packet handling,
and 4.769 microsecond mean completion collection. Separate 360-packet API traces
observe mean/p99 9.304/9.813 ms with LLVM and 9.320/9.861 ms with ACO. These are
instrumented submit-to-receive delays, not pure GPU or end-to-end capture latency.
No latency superiority claim follows.

Native multichannel audio, replay, static repeats, reconfiguration, missing-PTS,
ASan/UBSan, Vulkan validation, injected allocation failure/retry, and all 17
x86-64 checks pass. The sampler/Tracy runs launch private processes. The recorder,
reference executable and old driver remain unchanged. The supplied research
material and original clip remain intact. Evidence and exact commands are indexed
in [rgb-aco-index.txt](evidence/rgb-aco-index.txt).

VA encode-time RGB processing was inspected but not implemented or benchmarked in
this continuation. Its eager nominal-output allocation is an open optimization,
and portable capability signaling needs care: the public processing attribute
found is for decoding. The RAM result above removes a dependency cost; it does not
prove the Vulkan architecture universally superior or complete the broader goal.

## Color and quantizer audit — 23 October 2026

The current continuation separates an explicit quantizer request from the actual
bitstream. On the RX 9070 XT, the previous VAAPI `--qp 0` path failed on its first
frame because the packed header selected a transform mode inconsistent with
coded-lossless AV1. The previous Vulkan path silently clamped the request to
QIndex 1; an independent `trace_headers` pass confirms the effective value.
Neither was a valid lossless-conversion control.

The VAAPI patch now rejects an effective zero I/P/B QIndex during setup when the
driver lacks the required `ONLY_4X4` capability. The header writer selects that
inferred mode for zero QIndex when it is supported. Vulkan rejects explicit
requests outside its reported range and constrains derived picture quantizers to
that range. The native device reports [1, 255]. Both patched recorder packages
reject `--qp 0` before creating an output file. QIndex 1 and 32 recordings from
both backends independently decode. The traced QIndex 1 runs use 1 throughout;
the QIndex 32 runs use the existing derived I-picture value 26 and P-picture
value 32. Software libaom Q=0 still encodes and decodes. This GPU cannot test
the positive VAAPI lossless-capability branch, and coded-lossless YUV would not
make the preceding RGB-to-4:2:0 conversion lossless.

The new cube probe emits one uniform 8x8 patch per RGB tuple, sampling the center
after independent decoding. At 256 levels it covers all 16,777,216 eight-bit RGB
tuples in 256 2048x2048 frames. The oracle independently inverts the declared
matrix and range; it keeps BT.709 primaries and the sRGB transfer function.
All cases use QIndex 1, 60 FPS, GOP 120, and no B pictures. The VPP control streams
the same BGRA corpus through CPU upload and GPU conversion; this is a color
diagnostic, not a capture-performance comparison.

| Conversion | RGB MAE | RGB RMSE | Maximum absolute error | Signed mean error |
|---|---:|---:|---:|---:|
| VPP BT.709 limited | 0.6689 | 0.9264 | 7.3291 | +0.0310 |
| Direct RGB BT.709 limited | 1.3948 | 1.6967 | 8.7067 | −1.3196 |
| Experimental direct RGB BT.2020 matrix, full range | 0.8486 | 1.0936 | 6.5068 | −0.6749 |

Errors are in eight-bit RGB code values after clipping to [0, 255], including
codec/filter effects. This is full coverage of color tuples at uniform-patch
centers, **not** coverage of arbitrary images, edges, motion, perceptual quality,
or every codec state. The corrected 64-level corpus produces different errors
because its spatial/color steps differ: mean errors are 0.5361 for VPP, 1.3509
for direct limited range and 0.7791 for experimental full range.
VPP retains the lowest mean error in both corpora. The experimental pair improves
the direct path's mean error but does not establish an every-metric winner.

Mesa documents only BT.709/limited and BT.2020/full as working firmware pairs on
VCN 5. The private prototype advertises only the second pair, avoiding the
unsupported Cartesian product. It changes the YCbCr matrix and range without
relabeling source primaries or transfer. It is **not integrated into the recorder**:
consumer compatibility, spatial quality, compression and complete lifecycle costs
remain acceptance gates. The current `rgb-amd` profile stays BT.709/limited.

The installed Niri 26.04 source was also traced from its executable's derivation.
Its format offer is RGB-only. The 52.734375 MiB producer-native NV12 target requires
an upstream interface change; the current direct path's conditional image target
remains 87.890625 MiB. No user's compositor or recording was changed. This work
does not introduce new CPU, memory or latency performance measurements.

The first native reconstruction comparison exposed stale input in the standalone
probe. Mesa's derived-image unmap path flushes submitted copies without recording
that fence on the VA surface, so `vaSyncSurface()` alone was not a valid external
producer-completion oracle. The recorder already polls the DMA-BUF before
encoding; the probe now does the same for every exported object. Twenty fresh
250-frame cases across both reconstruction modes, plus two using the fixed
pre-change FFmpeg library, have identical decoded frames. The native Vulkan
validation/fault matrix passes, as do both backends' capture, 16+16-channel audio,
replay, static-repeat, reconfiguration and missing-PTS tests. The native sanitized
QIndex-0 failure path has no sanitizer report. The final C23 verification suite
passes, including the x86-64 Nix checks; aarch64 remains unbuilt.

The color table uses fresh, fenced producer runs. Initial unfenced cube results
are retained but excluded from conclusions. The initial quantizer-patch enum
spelling build failure and a nonexecutable-script invocation are also retained;
corrected builds and explicit Bash invocations pass. Evidence and reproduction
details are indexed in [color-quantizer-index.txt](evidence/color-quantizer-index.txt).

## Full-range RGB integration — 24 October 2026 UTC

The previously private conversion alternative is now usable through
`rgb-full-amd`. Its process-local driver exposes only BT.2020/full. The recorder
selects only an advertised pair, retains source primaries/transfer, applies that
plan to codec/frame metadata, and checks actual first-picture matrix/range before
publishing the container header. The ordinary RGB profile and default VAAPI path
remain available. A one-byte pair selector fits existing padding: the cold import
state remains 24 bytes on x86-64. No GPU image or packet field is added.

Native full/limited capture passes video, 16+16-channel audio, replay, static
repeats, reconfiguration and missing-PTS checks. The original fixture's black/white
oracle rejected full-range pixels because it assumed 16/235 endpoints. It now
checks the decoded range and its corresponding 0/255 or 16/235 endpoints, retaining
the same three-code tolerance; unknown ranges fail. Both profiles were rerun.
The full-range RGB8/RGB10, portrait, reconstruction-reuse, forced-key and injected
failure probes pass. RGB validation uses the previously documented repaired
validation-layer fork. Native full-range ASan/UBSan also passes.

Thirty-six fresh eight-second runs rotate four variants across three workloads.
The fixed VAAPI reference and preceding RGB executable remain unchanged. The two
current profiles use an identical recorder executable, differing in driver
conversion policy. Input, CQP 32, encoding depth two and the two-to-eight-buffer
producer offer match. Builds, players and profilers finish before timing starts.

| Workload | Variant | CPU ms, mean ± SD | RSS MiB | VRAM MiB | GTT MiB |
|---|---|---:|---:|---:|---:|
| 1080p60 | Fixed VAAPI | 61.447 ± 3.738 | 36.924 | 32.633 | 8.230 |
| 1080p60 | Fixed RGB | 50.692 ± 1.690 | 25.255 | 28.844 | 8.082 |
| 1080p60 | Current limited RGB | 52.220 ± 2.980 | 24.811 | 28.844 | 8.082 |
| 1080p60 | Full RGB | 52.181 ± 2.110 | 25.151 | 28.844 | 8.082 |
| Portrait60 | Fixed VAAPI | 81.424 ± 14.724 | 37.737 | 103.434 | 23.348 |
| Portrait60 | Fixed RGB | 61.870 ± 4.959 | 26.171 | 91.555 | 23.242 |
| Portrait60 | Current limited RGB | 60.503 ± 4.952 | 26.168 | 91.555 | 23.242 |
| Portrait60 | Full RGB | 66.699 ± 12.513 | 26.107 | 91.555 | 23.242 |
| 1080p240 | Fixed VAAPI | 139.681 ± 2.882 | 36.592 | 36.391 | 14.270 |
| 1080p240 | Fixed RGB | 112.879 ± 2.231 | 25.266 | 28.848 | 14.078 |
| 1080p240 | Current limited RGB | 113.717 ± 2.350 | 25.283 | 28.848 | 14.078 |
| 1080p240 | Full RGB | 115.152 ± 3.966 | 24.638 | 28.848 | 14.078 |

All 36 outputs have distinct consecutive decoded identities, with zero observed
interior gaps or corrupt markers. Boundary frame counts differ slightly and are
retained per run. Full range preserves the sampled VRAM/GTT footprint; these runs
do not establish a CPU improvement over limited RGB. The 87.890625 MiB conditional
portrait image target remains unchanged. Replay payload size is a separate cost.

On the eight constant patches, independent RGB-code MAE/max is 0.688/2.944 for
full range, 0.837/3.080 for limited RGB and 0.209/1.142 for VAAPI. These normalized
RGB errors can be compared across ranges; raw YUV-code errors cannot. Sixteen
matched source identities from a separate 640x480 scene give RGB PSNR
30.094 dB full versus 30.000 dB limited. This includes conversion, subsampling and
codec effects. It is a synthetic spatial diagnostic, not a natural-scene or
perceptual-quality result. The earlier full-color-cube findings remain separately
scoped to their QIndex-1 uniform-patch corpus.

Consumer validation now checks actual frames rather than advancing playback time.
mpv 0.41's image-output route exports 60 consecutive identities for both profiles.
Headless Brave 1.97.56 displays four advancing identities and exposes the expected
WebCodecs color tags. Its compositor screenshot and canvas readback have the same
observed patch values. The reported renderer is SwiftShader with software decode
and compositing; accelerated desktop playback remains unverified in this work.

The consumer pixel checks exposed conversion approximations. In mpv's default
fast zimg image-export path, full-range patch MAE/max is 2.792/9 despite correct
sRGB output tags. `--zimg-fast=no` gives 0.625/3 and `--sws-allow-zimg=no` gives
0.667/3. The encoded YUV values do not change. Brave's software BT.709 conversion
gives 2.708/16, matching libyuv's bounded blue coefficient; WebCodecs raw YUV
samples agree with FFmpeg. Its full-range conversion gives 0.792/4. These are
consumer-path findings, not reasons to falsify source metadata or claim every
player will behave identically. Source mechanisms are recorded in
[dependencies.md](dependencies.md#advertised-full-range-rgb-profile-and-consumers).

The raw patch values were checked again at decoded frames 0, 1, 15, 30 and 120;
they remain stable. This rules out the suspected per-frame color drift for those
samples. Consumer conversion results must not be mistaken for encoder errors or
for the behavior of every rendering path.

Fresh Samply and Tracy captures save and independently decode both replay exports.
Samply's ordinary kernel-inclusive request is denied by the host's existing
`perf_event_paranoid=2`. The installed compatibility wrapper retries with kernel
and hypervisor events excluded; that shim is removed from the recorder environment.
No system setting is changed. Paired 50-FPS, six-second API traces contain 300
packets each. Full/limited mean submit-to-receive delay is 9.296/9.336 ms, observed
p99 is 9.762/9.766 ms, and maximum is 9.953/10.135 ms. These instrumented API delays
do not establish a latency gain or measure pure GPU/end-to-end capture latency.

Twelve fresh forty-second portrait60 replay runs repeat each variant three times,
with rotating order and two exports after the 30-second window advances. All
exports independently decode. The first clips contain 1,856 consecutive identities
except one fixed-reference boundary case with 1,857; none repeat or skip an
interior identity. The initial four runs remain included in these totals.

| Variant | CPU ms, mean ± SD | RSS MiB, mean ± SD | VRAM MiB | GTT MiB |
|---|---:|---:|---:|---:|
| Fixed VAAPI | 375.968 ± 18.132 | 45.846 ± 0.637 | 103.434 | 23.348 |
| Fixed RGB | 283.762 ± 17.026 | 34.460 ± 0.828 | 91.555 | 23.242 |
| Current limited RGB | 301.657 ± 6.019 | 34.556 ± 0.560 | 91.555 | 23.242 |
| Full RGB | 310.293 ± 16.625 | 35.169 ± 0.521 | 91.555 | 23.242 |

CPU means are higher than the fixed RGB reference; three runs do not establish a
general regression bound or a speed win. Full range also uses more encoded
storage: the first matched replay's peak live payload is 8,445,133 bytes versus
8,223,125 for current limited RGB, and aggregate export bytes increase about 3%.
This is a measured precision/storage tradeoff at the same native quantizer, not
a matched-distortion compression win. No default is switched on the strength of
these results. Both profiles remain usable, with VAAPI retained for its better
independent mean color precision in these corpora.

Commands, per-run measurements, failures, raw clips, consumer screenshots and
profiler captures are indexed in [full-range-index.txt](evidence/full-range-index.txt).
The broader goal remains active. Accelerated desktop-player behavior, natural
scene rate/distortion, game interference, energy and cross-vendor behavior remain
open; the current data does not establish state-of-the-art superiority on every
metric.

## Independent static-key reuse — 24 October 2026 UTC

The static path now replaces an eligible requested independent refresh with the
same verified key bytes when the encoder is fully collected and no real input
has intervened. `--static-interval 2` permits that path; the one-second default
is unchanged. The native constant-image probe produced three distinct decoded
reconstructions across I/P pictures, so a P-picture is not treated as an arbitrary
repeatable key. `--static-encoding always` preserves ordinary encoding at the
same cadence. Actual encoder outputs and reused samples have separate counters.

The first candidate, show-existing of an already displayed key, was rejected
against the AV1 rule. dav1d tolerated the invalid test stream; libaom rejected it.
A second candidate, a single independent picture held for 30 seconds, decoded
but immediately ended playback in headless Brave. A final independent sample
repairs that consumer behavior. The seek fixture initially lacked HTTP range
responses; after correcting it, both sparse and periodic controls seek to the
requested positions. A two-image transition test verifies the old image at
14.9 seconds and the new image at 15 seconds for both Matroska and MP4.

Replay export now compacts byte-identical independent key runs in its existing
flat index and preserves a terminal sample. It does not mutate snapshot blocks
or suppress dependent packets. PCM timing and Opus handling retain their existing
sample/codec rules. The new ownership test pauses export, replaces the media's
cache with real input, evicts the old key from live history, and then resumes
export. The saved old picture and exact PCM endpoint remain valid. Injected
packet-descriptor and buffer-reference failures fall back to ordinary encoding.

The native pause/resume comparison records 240 real source pictures plus three
unchanged-image samples. All 243 decoded pictures have identical hashes between
the fixed reference and candidate. The candidate uses 241 encoder submissions
and two reused keys; no real source identity is lost or reordered. Native VAAPI
and Vulkan capture, multichannel audio, replay, reconfiguration and missing-PTS
checks pass with two-second static refreshes.

The MP4 long-hold probe exposed movenc's signed `INT_MAX` duration validation,
which is narrower than the unsigned container field. Compaction now retains
checkpoints for that actual consumer bound. The corrected 6,000-second MP4 has
four samples, preserves the full duration, and independently decodes with libaom;
Matroska uses two samples. The failed wider-duration probe remains in evidence.

GCC/Clang, ASan/UBSan and the x86-64 Nix checks pass. ThreadSanitizer initially
crashed in the generated converter IFUNC resolver before `main`, also on the
existing sharing test. Both ownership tests pass with `GK_RGB_PORTABLE`, which
avoids that resolver while retaining instrumentation of the converter body.
The initial sharing test's fixed three-packet expectation was replaced by checks
for unchanged pixels, contiguous display intervals and the exact snapshot end.

Confirmation uses the preceding immutable VAAPI/RGB packages as fixed references,
the new package with `--static-encoding always`, and its automatic reuse policy.
All use `--static-interval 2`, quantizer 32, depth two, 2560x2880 at 60 Hz, and the
same two-to-eight source-buffer offer. Thirty eight-second file runs and eighteen
forty-second replay runs completed successfully. Each variant has three runs;
static variant order rotates, while the moving control follows the candidate.
No build, profiler, player or heavy analysis overlaps those timing runs. Input
hashes pass. Startup and shutdown are included in CPU/RSS measurements.

| Eight-second file workload | Variant | CPU ms, mean ± SD | RSS MiB, mean ± SD |
|---|---|---:|---:|
| Static RGB | Fixed | 19.538 ± 0.319 | 25.758 ± 0.815 |
| Static RGB | Always encode | 19.542 ± 0.557 | 26.228 ± 0.430 |
| Static RGB | Automatic reuse | 18.257 ± 0.306 | 26.151 ± 0.412 |
| Static VAAPI | Fixed | 14.574 ± 0.135 | 37.505 ± 0.235 |
| Static VAAPI | Always encode | 15.133 ± 0.207 | 37.320 ± 0.437 |
| Static VAAPI | Automatic reuse | 13.801 ± 0.416 | 37.460 ± 0.424 |
| Moving RGB | Fixed | 69.646 ± 4.981 | 26.048 ± 0.326 |
| Moving RGB | Automatic reuse | 73.118 ± 4.520 | 26.392 ± 0.297 |
| Moving VAAPI | Fixed | 84.020 ± 5.776 | 37.398 ± 0.439 |
| Moving VAAPI | Automatic reuse | 85.148 ± 9.451 | 38.286 ± 0.305 |

Static file runs submit one encoder input and reuse three keys, versus four
encodes in both controls. Each file decodes four pictures with identical scene
identity and unchanged patch error. Moving runs have 479–480 consecutive pictures,
zero interior gaps/repeats and zero reuses. The higher moving CPU means overlap
the observed run variation; these three-run results neither demonstrate a dynamic
speedup nor establish a general nonregression bound. File-cache RAM is not hidden:
the automatic RGB mean RSS is about 0.39 MiB above the fixed reference here.

Each static replay run saves twice after the thirty-second history window advances:

| Backend | Variant | CPU ms, mean ± SD | RSS MiB, mean ± SD | Actual encodes / reused keys | Two exports, bytes |
|---|---|---:|---:|---:|---:|
| RGB | Fixed | 32.080 ± 0.151 | 27.803 ± 0.273 | 20 / 0 | 9,742,142 |
| RGB | Always encode | 27.827 ± 1.325 | 26.085 ± 0.240 | 20 / 0 | 1,218,884 |
| RGB | Automatic reuse | 19.612 ± 0.325 | 25.723 ± 0.375 | 1 / 19 | 1,218,884 |
| VAAPI | Fixed | 23.011 ± 0.540 | 38.652 ± 0.343 | 20 / 0 | 9,720,286 |
| VAAPI | Always encode | 19.029 ± 0.455 | 37.397 ± 0.169 | 20 / 0 | 1,216,152 |
| VAAPI | Automatic reuse | 15.472 ± 0.472 | 37.486 ± 0.543 | 1 / 19 | 1,216,152 |

The always-encode control isolates export compaction from key reuse. Automatic
RGB replay copies 304,608 encoded bytes rather than 6,092,160; VAAPI's native
packet-adoption path adds no replay copy in either variant. Peak budgeted live
history is unchanged: existing exact-key sharing already removes duplicate
payload storage. The new exports contain two timed video samples rather than
sixteen, preserving the same image and clip duration. This is a static-content
storage improvement, not a natural-scene compression result.

Sampled encoder-engine time over forty seconds is 4.062 ms with RGB reuse versus
98.051 ms for its fixed reference; VAAPI is 4.814 versus 106.212 ms. These fdinfo
counters are GPU work observations, not end-to-end latency or energy measurements.
Mean whole-run CPU corresponds to 0.0490% of one core for RGB reuse and 0.0387%
for VAAPI reuse on this idle fixture; neither is a promise about a live desktop.

All variants have unchanged sampled VRAM within their workload: static RGB
41.636719 MiB and VAAPI 53.578125 MiB, including one imported 28.75 MiB source;
moving RGB 91.554688 MiB and VAAPI 103.433594 MiB, including two source images
totalling 57.5 MiB. The producer allocates two buffers even in the static case.
The lower static recorder-visible count must not be presented as a new memory
optimization or as the complete producer/recorder union. Sampled GTT stays
23.242188 MiB for RGB and 23.347656 MiB for VAAPI. The image-payload targets in
[memory-targets.md](memory-targets.md) remain unchanged.

A final-package Samply capture and a current Tracy capture both save and decode
their replay exports. The user-space Samply main thread has only sixty samples;
43.2% of its CPU-delta weight includes `gk_gpu_open`, making startup a material
part of this short static profile. Inclusive weights overlap and do not prove a
steady-state hotspot ranking. Tracy records one actual encode and one reuse in
its four-second test. Its 6,573 ns reuse zone is a single instrumented observation,
not a latency distribution or a GPU speedup claim. The earlier Samply local-build
capture is retained but excluded from final-package conclusions. Neither profiler
changes the host's kernel policy or the user's desktop session.

Final native full-range capture/audio/replay checks also pass, with one actual
encode and one reused key in the short static case. A final-package forty-second
MP4 replay uses one encode and nineteen reuses. Its actual export and the matched
MKV export independently decode with libaom and pass headless Brave playback,
seeks to 5/15/29.5 seconds and image-identity checks. The one-picture-plus-terminal
export control passes the same checks in both containers. These checks do not
establish ordinary accelerated desktop playback behavior.

The first MP4 harness requested `.mkv` save filenames and was correctly rejected
by the recorder's container check. The corrected harness uses `.mp4` paths;
the failed run remains in evidence. No application semantics were weakened to
make that test pass. The fixed 201-file implementation, all supplied material
and the original user clip retain their hashes. Packages, complete results,
commands and retained failures are indexed in
[repeat-index.txt](evidence/repeat-index.txt). The broader goal remains active.

## Idle coded-output reclamation and pool ownership — 24 October 2026 UTC

`--gpu-memory compact` now supports VAAPI and Vulkan. During ordinary capture it
keeps at most one unused coded-output buffer; when an eligible static independent
key takes over, it releases all unused coded buffers. New input restores the
one-spare policy. The full output capacity is retained as an allocation target,
and AV1 reference/capture images remain live. Vulkan upload-image capacity stays
unchanged, avoiding repeated allocation from a one-image input pool.

Eight initial native portrait probes verify the memory transition and resumed
pixels. RGB GTT falls from 23.242188 to 2.085938 MiB; VAAPI falls from 23.347656
to 2.191406 MiB. Each returns 21.15625 MiB during the eligible static hold and
restores it on resume. Every decoded hash in each 243-picture pause/resume pair
matches, including all 240 source images. VRAM does not fall with this change.

The allocation audit found a separate lifetime defect: pooled Vulkan buffers used
an encoder/filter-owned `FFVulkanContext` in their destruction callback. Retaining
a packet past encoder close reproduces a crash in `ff_vk_unmap_buffers` with freed
memory poisoning. Each pool now retains an explicit owning device reference and
three destruction functions. This costs 32 bytes plus a 24-byte AVBufferRef once
per pool on x86-64; no packet field, GPU image or copy is added. Nine native
release-order cases cover imported RGB8, ordinary RGB10 upload and forced RGB10
staging. Packet bytes and decoded pixels survive encoder/device-owner teardown;
the device is destroyed after the last packet is released. Old-library controls
fail as expected. This uses FFmpeg's documented delayed pool destruction, checked
against the pinned source rather than its documentation's lock-free description.

Debugger injection reaches actual staging and encoder pool constructor failures
after ownership acquisition; both return ENOMEM and release the device completely.
The first preload-only second-pool test did not reach its target: libavutil uses
symbolic internal binding. Its disassembly and the failed test are retained.
No production linker binding was changed for the tests.

A real resumed-buffer allocation failure exposed an unsafe drain of a failed
encoder's partially updated reference graph. Media now latches actual video API
failure separately, avoids further encode/drain calls on that codec, finalizes
collected packets and returns the original error. Capture/input failures still
drain accepted work. Sanitizer-enabled native failures on both backends preserve
64 independently decoded pictures and return failure. The new software regression
covers send/receive errors in MKV/MP4, exact pixels, contiguous timestamps and
the endpoint; the same test fails against the preceding source. The added flag
fits padding: `struct gk_media` remains 2,280 bytes on the tested x86-64 ABI.

Final confirmation uses the preceding immutable VAAPI/RGB packages and current
retain/compact policies, all with CQP 32, depth two and a two-second static
interval. Thirty-six file runs repeat every backend/policy/workload combination
three times, rotating policy order. Moving files last eight seconds; pause/resume
files last ten, with producer updates paused from seconds 1 through 7. Startup
and stop are included in CPU/RSS. All decode with no corrupt markers, backward
transitions or interior identity gaps. Terminal source/decoded counts vary by
one frame and are retained. No build, profiler, player or heavy analysis overlaps
these uninstrumented runs; the frozen input checks pass.

| Workload | Backend | Fixed CPU ms, mean ± SD | Current retain | Current compact |
|---|---|---:|---:|---:|
| Moving | RGB | 72.380 ± 13.945 | 80.809 ± 3.908 | 69.171 ± 4.394 |
| Moving | VAAPI | 99.237 ± 5.791 | 98.597 ± 13.110 | 93.519 ± 11.029 |
| Pause/resume | RGB | 44.869 ± 2.564 | 48.922 ± 4.893 | 48.477 ± 3.373 |
| Pause/resume | VAAPI | 58.445 ± 8.941 | 50.685 ± 4.825 | 55.622 ± 14.528 |

CPU and RSS comparisons are mixed; three runs do not establish a general speed
win or a nonregression bound. Moving compact RSS means are 25.914 MiB RGB and
37.340 MiB VAAPI; pause/resume means are 26.516 and 38.018 MiB. Moving sampled
VRAM stays 91.554688 MiB RGB / 103.433594 MiB VAAPI, including 57.5 MiB shared
source storage. GTT peaks remain 23.242188 / 23.347656 MiB because active capacity
is still needed. During the independently chosen 5.5–6.5 second idle window,
every compact run instead samples 2.085938 / 2.191406 MiB GTT.

Four forty-second replay runs, one per backend/policy, save twice after the
thirty-second window advances. Both exports decode and preserve the static image
and duration. Compact GTT stays at the lower value during seconds 10–34. One
run per policy is functional/resource evidence, not a CPU variance estimate.
Static recorder-visible VRAM includes one imported 28.75 MiB source although the
producer still allocates two; do not report that as a new physical-VRAM saving.

Twelve separate traced resume runs measure the public encoder API boundary.
The first input after the known long timestamp gap identifies the resumed frame;
each trace has one such gap and exactly the actual encoder packet count.

| Backend | Retain first-resume API ms, mean ± SD | Compact | Runs per policy |
|---|---:|---:|---:|
| RGB | 10.2266 ± 0.5815 | 11.2535 ± 0.1258 | 3 |
| VAAPI | 9.7177 ± 0.3141 | 9.7469 ± 0.4988 | 3 |

RGB compact has about one millisecond more observed API delay in these runs.
This includes submission, allocation, GPU work, completion and scheduling;
it does not isolate pure GPU time or complete display-to-file latency. Retain
therefore remains the default. Full per-run observed p99/max values are preserved
in `confirmation-analysis.json`; a three-resume sample is not a tail bound.

Fresh final-source Samply and Tracy captures each save two independently decoded
exports. Tracy observes one idle-release zone at 32,442 ns; this single instrumented
zone is not a latency distribution. Kernel profiling policy is unchanged. GCC,
Clang, ASan/UBSan and the final applicable x86-64 flake checks pass. Commands,
packages, profiler data, raw comparisons and retained harness failures are indexed
in [idle-index.txt](evidence/idle-index.txt). The broader goal remains active.

## 24 October 2026: reclaim unused Vulkan reconstructions in standby

The fixed reference is the preceding 209-file implementation and RGB package
`brpdzw51wrc3w3w9px0by8kxxx3ngvm5`. The first candidate is retained as
`y8y8603lzw8prli5nlgbid9p6cxy4kaq`; the final candidate removes its unnecessary
ordinary-picture retirement scan. Final RGB/full-RGB packages are
`90sv0rykd64v4fmqr252d0ax735aq55k` and
`nd09hqrk60hv0larib5ha85rxbxldjp8`. All use the same AV1 settings, source-buffer
count and audited driver. The one-unused-reconstruction control changes only
standby retention in the corresponding library. No quality, reference-count,
FPS or output-capacity reduction is used to obtain the memory saving.

The source/lifetime mechanism is recorded in [dependencies](dependencies.md).
Only formerly active, retired slots are deactivated in the existing encode
submission. After successful completion/readout and an empty issued-output FIFO,
compact standby destroys unused queue-owned views/frames. It retains the live
key reconstruction and restores other images on demand. Fatal submission/readout
failure skips this path and uses full close. No new persistent record, per-packet
field or extra retirement submission is introduced. A small exhaustive check
covers all 26 three-slot states with at least one available slot.

Native validation runs compare retention with reclamation using 250 deterministic
inputs and forced/periodic independent pictures. Both 8-bit and uploaded 10-bit
controls create three reconstruction views and retire none before close; both
reclaiming runs create eleven and retire eight, returning to three live views
after resumed motion. Every decoded picture hash matches its control. Both
replacement-view failures occur after actual reclamation and close without
validation errors. Portrait DMA-BUF, submission/fence-export failure and packet
release before/after encoder/device-owner close also pass. These tests use the
pinned repaired validation layer; they do not infer correctness from successful
encoding alone.

Final recorder checks pass limited/full RGB capture, two 16-channel audio tracks,
replay, reconfiguration, missing-PTS capture, static repeats and ASan/UBSan builds.
An actual post-standby 11,087,872-byte reconstruction allocation is faulted under
ASan/UBSan; the recorder returns failure while preserving 64 decoded pictures.
The application/probe is sanitizer-instrumented; this does not claim that the
entire driver/library stack is instrumented. GCC, Clang and applicable x86-64
flake checks pass; ARM execution is not covered.

Two exploratory harness defects are preserved with their logs. Selecting the
FFmpeg derivation's empty main output did not load the one-spare library; its
separate `lib` output is required. Preloading through a nested host `env` also
failed its glibc version requirement. Corrected wrappers select the actual
library and preload only the recorder. Fresh confirmation wrappers avoid extra
`cat` child processes so their CPU cost does not vary with the selected policy.

Final-source Samply and Tracy captures cover motion, standby, resumed motion and
two independently decoded exports. Samply attributes 45.7% of sampled main-thread
CPU delta to the Vulkan receive path and 32.2% to encode issue, with overlapping
inclusive stacks; those percentages must not be added. The main thread has 212
samples and 46,142 microseconds of recorded CPU delta. Its largest sampled leaf
is Mesa's `radv_enc_output_one_byte` at 8.0%; this is a lead for further command
generation analysis, not proof of a universal bottleneck. Tracy observes one
idle-release call at 163,530 ns, 241 source handoffs and two reused keys. A single
instrumented release is not a latency distribution. Kernel profiling policy is
unchanged.

Final uninstrumented portrait/60 file comparisons rotate order over three
repetitions per case. They use QP 32, depth-two capacity, the same two source
buffers and two-second static cadence. No build, profiler, player or heavy
analysis overlaps these timing runs. Motion lasts eight seconds; pause/resume
lasts ten, with source updates paused between seconds one and seven.

| Scenario / policy | CPU ms, mean ± SD | Peak RSS MiB, mean ± SD |
|---|---:|---:|
| Moving, fixed reference | 77.359 ± 1.508 | 26.152 ± 0.243 |
| Moving, preceding candidate | 72.233 ± 9.071 | 26.290 ± 0.299 |
| Moving, final candidate | 67.796 ± 2.057 | 26.280 ± 0.402 |
| Pause/resume, fixed reference | 44.588 ± 2.791 | 26.564 ± 0.753 |
| Pause/resume, one unused reconstruction | 46.966 ± 3.372 | 26.841 ± 0.249 |
| Pause/resume, no unused reconstruction | 48.185 ± 5.936 | 26.224 ± 0.248 |

The final moving mean is lower, but pause/resume is mixed. The first comparison
had a higher candidate moving mean, and the preceding candidate varies widely
in the final series. Removing the scan is established by the code; attributing
the entire CPU difference to it is not. These runs establish neither universal
CPU superiority nor a nonregression bound. Frame counts vary at the independent
wall-clock stop boundary; all runs pass the actual decoded-identity oracle with
no interior gaps, backward transitions or corrupt markers. Native deterministic
inputs separately establish exact decoded-picture equality.

During the 5.5–6.5 second idle phase, total and resident VRAM agree in every final
file run:

| Reconstruction policy | Idle VRAM MiB | First resumed API delay, ms ± SD |
|---|---:|---:|
| Retain all, fixed reference | 91.5546875 | 10.4878 ± 0.5470 |
| Keep one unused image | 80.98046875 | 11.2430 ± 1.3753 |
| Reclaim all unused images | 70.40625 | 10.8582 ± 1.2669 |

The nine API-traced runs are separate from CPU comparisons, three per policy.
The first resumed packet is identified by the single long input-PTS gap, and
each trace's count equals the actual encoder packet count. Mean observed p99
delays are 9.8743 / 9.8869 / 9.8201 ms respectively; mean per-run maxima are
12.3450 / 12.4946 / 12.5218 ms. The second resumed packet means are
9.4894 / 9.4816 / 9.5739 ms. Full per-run values remain in the evidence.
These public API delays include allocation, GPU completion and scheduling;
three resumes do not establish a tail bound or pure-GPU latency. The one-spare
control saves half the VRAM but shows no clear first-resume advantage here.
Compact therefore reclaims both unused images; retain remains the default.

The final three forty-second replay runs save twice after the moving frames
leave the thirty-second window. Both exports preserve the held source image and
timing, and each policy writes 1,315,916 bytes across the two clips. Idle total
and resident VRAM agree: 91.5546875 / 80.984375 / 70.40625 MiB, respectively.
The one-spare replay run has an additional observed 4 KiB allocation (including
at its 91.55859375 MiB active peak); its complete attribution is unresolved.
CPU totals are 25.428 / 27.633 / 29.684 ms and peak RSS is
26.625 / 26.477 / 26.406 MiB. One long run per policy is resource/functional
evidence, not a variance estimate. All policies' idle GTT is 2.0859375 MiB;
file runs return to 23.2421875 MiB GTT and 91.5546875 MiB VRAM on motion.

Both shared capture images remain counted at 57.5 MiB. Reclaiming two
11,087,872-byte reconstructions returns **21.1484375 MiB**, or **23.1%** of
the preceding portrait idle total. The conditional dense standby target is
66.796875 MiB, distinct from the active 87.890625 MiB target. No new whole-system
physical-memory minimum or general state-of-the-art claim follows.

All 30 final runs, 27 preceding-candidate comparisons, required compiler/native
checks and final profiles complete successfully. Frozen input hashes pass.
Reproduction scripts, actual measurements and retained harness failures are
indexed in [dpb-index.txt](evidence/dpb-index.txt). The broader goal remains active.

## 24 October 2026: resume-cost audit and RADV header writing

The fixed control is the 212-file reconstruction checkpoint, recorder
`90sv0rykd64v4fmqr252d0ax735aq55k`, and driver
`896yc4wnz6wn804kdbl1638gl243vskz-mesa-gpukey-rgb-26.2.5`.
The initial audit separates resumed output allocation from mapping: three
instrumented runs measure 514.5 ± 21.7 microseconds in Vulkan allocation,
21.35 ± 1.70 microseconds in mapping, and 0.54 ± 0.11 microseconds in binding.
Nested logging is included in the allocation diagnostic; these are not
uninstrumented estimates. The first libdrm-only map hook missed RADV's direct
mmap path. Corrected Vulkan dispatch hooks and the failed assumption are retained.
All three first-resume producer polls were already ready, leaving no measured
producer wait to hide allocation in these fixtures. Output capacity is unchanged.

The next lead came from Samply's driver-header samples. Source inspection found
a bytewise writer, unused state and repeated command-memory read/modify/write
access. Tests compared a conventional accumulator, a length-marked accumulator,
and direct in-place writes. The last looked competitive on cached memory but
lost badly on mapped WC GTT and visible VRAM. Those memory-specific results are
algorithm evidence, not recorder throughput. Two accumulator drivers then passed
native AV1 packet-byte/decoded-picture equality, Vulkan validation, forced keys,
10-bit input, portrait DMA-BUFs and failure cleanup. H264 and HEVC shared-writer
checks also pass with exact packets and pictures.

The first 12-second recorder comparisons were noisy. A separate rotating series
of thirty-second motion recordings measured total CPU of
199.642 ± 14.797 ms for the fixed driver, 178.509 ± 12.082 ms for the conventional
accumulator and 189.153 ± 7.403 ms for the length-marked accumulator, three runs
each. The conventional accumulator had the lowest mean. Additional diagnostics
around the actual `vkCmdEncodeVideoKHR` call measured about 16.8 microseconds CPU
for the original writer and 3.2–5.3 microseconds for those candidates, excluding
the startup picture. These small diagnostic samples do not establish end-to-end
latency or a universal ordering between accumulator designs.

The selected compact refactor changes the consumers as well as the writer:
flush returns the current fragment length and clears the fragment. Position is
then derivable from that length modulo 32. The independent source-based oracle
checks this contract against the old consumers' cumulative-count subtraction at
every flush. All 4,224 boundary cases and 10,000 stateful sequences pass under
GCC/Clang and ASan/UBSan. Native AV1, H264 and HEVC packets/pictures remain exact
matches, with clean validation and teardown. The conventional accumulator is
retained as `rgb-word-amd`; no claim that compact is universally faster replaces
the observed tradeoff.

The compact encoder state is 48 bytes versus 72 originally, and its hot writer
record is eight aligned bytes. The conventional alternative uses 56/12 bytes.
The compact driver's text section is 2,944 bytes smaller than the control. An
additional inline/restrict experiment passed correctness and reduced some
diagnostic times but added 4,416 bytes of text relative to the control; it was
not selected from those observations alone. Restrict in that experiment was
confined to the leaf writer's disjoint host-state and GPU-command allocations.

The investigation also repaired a real test-harness defect: standalone
`! grep` does not trigger Bash errexit. Validation, sanitizer, packet-trace and
two related negative gates now use explicit failures. A new GPU-free regression
rejects injected positive/cleanup errors, admits a clean log to the next stage,
and rejects the saved old checker. Earlier DPB logs were independently searched
and contained no validation errors. The initial stock full-FFmpeg H264/HEVC
control had synchronization/view-lifetime VUIDs; using the audited FFmpeg patches
repaired that control before comparing drivers. Both failures remain in evidence.

Final packages are `hz02jpvisd33dlv5l4cd5x94g96g0762` (RGB),
`aml3iin3mml8kvdlzzl249mwiwzncwmk` (full RGB) and
`dni1bqj73yysrlvc43jdylsdmx23cw7r` (conventional writer).
All C23 compiler/linker checks, GCC/Clang sanitizers and applicable x86-64 flake
checks pass, including the three new source-extracted writer checks. Final native
limited/full RGB, multichannel audio, replay, reconfiguration and static tests
pass. A first-resume reconstruction-allocation failure preserves 64 decoded
pictures. ARM execution and an instrumented entire driver/library stack are not
claimed.

Final-source Samply and Tracy captures each save two independently decoded clips.
Samply records 178 main-thread samples and 39,868 microseconds of CPU delta;
34.6% is attributed inclusively to Vulkan receive and 25.4% to encode issue.
Inclusive percentages overlap. Tracy observes 241 handoffs, two reused keys,
two exports and one idle-release call at 80,002 ns. These instrumented samples
are not an uninstrumented speedup or a latency distribution. Kernel profiling
policy remains unchanged.

Final uninstrumented comparisons rotate the fixed and selected drivers over
three repetitions. They use the same final recorder binary, FFmpeg, private
producer, source-buffer count, QP 32, depth-two capacity and two-second static
cadence. No builds, profilers, players or heavy analysis overlap the timings.

| Workload | Fixed CPU ms ± SD | Compact CPU ms ± SD | Fixed / compact peak RSS MiB, means |
|---|---:|---:|---:|
| Portrait/60 motion, 30 seconds | 214.671 ± 9.280 | 199.266 ± 10.592 | 26.833 / 26.471 |
| Portrait pause/resume, 10 seconds | 46.448 ± 5.276 | 37.859 ± 3.545 | 26.250 / 26.617 |
| Requested 1080/1000 FPS, 10 seconds | 157.433 ± 18.961 | 108.396 ± 2.201 | 25.445 / 24.815 |

The compact means are lower by approximately 7.2%, 18.5% and 31.1% respectively
in this series. RSS is mixed; reduced private-state size is not a universal RSS
reduction. Motion and pause/resume have no interior identity gaps or corrupt
markers. The overload case intentionally permits dropping; its actual generated
and delivered rates, skips and API-delay observations are reported separately
below. A requested 1,000 FPS is not evidence of 1,000 produced/encoded frames.

Both portrait variants peak at 91.5546875 MiB VRAM (57.5 MiB shared) and
23.2421875 MiB GTT. Both overload variants peak at 28.84765625 MiB VRAM
(16.875 MiB shared) and 14.078125 MiB GTT. This change does not reduce video
image allocation or coded-output capacity. It reduces command-generation work
while retaining the preceding standby-memory mechanism.

The overloaded producer actually generated 254.208 ± 1.615 FPS with the fixed
driver and 256.154 ± 2.232 FPS with compact. Submitted rates were
254.175 ± 1.580 and 256.087 ± 2.230 FPS, respectively, using recorder elapsed
time. These are whole-fixture observations, not maximum hardware throughput.
Each variant had zero or one missing interior source identity per run, no
backward transition and no corrupt marker. The application-level skipped count
was zero; the exact earlier handoff/admission location of those few missing
identities was not traced. Motion/resume runs had no interior gaps, and every
decoded file count agrees with collected/reused packets minus boundary omissions.

Six separate API traces measure first-resume delay at 10.4193 ± 0.8529 ms fixed
and 10.6311 ± 1.3646 ms compact, three runs each. Second-resume means are
9.3669 / 9.4291 ms. Mean per-run observed p99 values are 9.7527 / 9.8261 ms,
and mean per-run maxima are 12.5904 / 12.1164 ms. The values overlap in variation;
no general latency improvement or tail bound is established. These delays
include allocation, GPU completion and scheduling, not pure hardware encoding.
Idle total/resident VRAM remains 70.40625 MiB in both policies, preserving the
preceding reconstruction-saving behavior.

All 24 final runs complete, frozen input hashes pass, and measured alternatives,
failed harness assumptions, source-based tests and final profiler captures are
indexed in [encode-index.txt](evidence/encode-index.txt). This is a verified CPU
and representation improvement against the fixed reference, with mixed RSS and
unresolved broader latency/throughput superiority. The all-metric goal stays active.

## 24 October 2026: retire redundant raw input in static standby

The fixed control is the preceding 219-file checkpoint and RGB package
`hz02jpvisd33dlv5l4cd5x94g96g0762`. Both backends use the same frozen RGB-capable
FFmpeg build and process-local drivers in the quantitative comparisons. The
first prototype released raw input only after reusing a cached key. The selected
candidate transitions when a requested static key actually completes, after
the CPU key and any pending-packet replacement have been committed successfully.

The configured independent-refresh mode is separate from the one-second default,
which retains P-picture refinement. New regressions exercise shorter requests
in explicit independent mode, actual black/white output and durations, a prepared
but unsubmitted next image, and a later packet-reference failure with a retained
raw fallback. Both flags use existing padding: media/options remain 2,280/192
bytes on the inspected x86-64 ABI.

Initial portrait probes show VAAPI returning an actual 11.25 MiB input allocation:
after motion, idle VRAM is 92.18359375 MiB rather than 103.43359375 MiB. RGB retains
the same 70.40625 MiB standby allocation but reaches it at the first completed
static key, rather than waiting for another refresh. In the long-pause fixture,
the longest RGB source lease falls from about six seconds to two. That is earlier
resource retirement, not a claim that GPU encoding took six seconds. The short
pause also reaches standby and restores capacity on motion. GPU source
allocations remain counted, including unimported producer storage for a stream
that starts static.

Full C23 compiler/linker, GCC/Clang sanitizer and applicable x86-64 flake checks
pass. Final native VAAPI, limited/full RGB and sanitizer capture suites pass,
including replay, reconfiguration, missing PTS, and a new static GPU replay with
two 16-channel tracks. Final packages are `asy315yaqbdrwcfd8arv7wj7d8yarzf8`
(VAAPI), `mmv53c8sr03p2swc49bwrzjyr15943sg` (RGB) and
`nzcipyjqijlpqna8nfp34p7z7kz7aw6y` (full RGB).

Native sanitizer faults distinguish optional setup from committed state.
Failure to create/reference the CPU key before commit keeps the raw fallback;
both backends resume successfully with 301 decoded pictures in the short-pause
fixture. A later key-reference failure after retirement returns an error and
preserves both completed pictures, while the retained-raw control re-encodes and
continues to four pictures. Failing physical allocation of the next VAAPI input
reaches VPP begin and returns an input error, preserving 64 decoded pictures.
These injections do not claim whole-driver sanitizer instrumentation.

Final-source Samply and Tracy captures each save two independently decoded
exports. Samply records 200 main-thread samples and 38,398 microseconds of CPU
delta; encode issue accounts for 24.1% inclusively and Vulkan receive 32.6%.
Tracy observes one idle-release zone at 120,910 ns, two cached-key repeats and
two exports. Single instrumented samples do not establish timing distributions.
No kernel profiling policy or live desktop settings were changed.

Fresh confirmation rotates the fixed and selected recorders over three
repetitions per backend and workload: eight seconds of motion, a ten-second
run paused from seconds one to seven, and an eight-second run paused from one
to four. All use 2560×2880 at 60 FPS, QP 32, depth two, two source buffers,
two-second static refreshes and compact memory policy. FFmpeg and each backend's
driver are held fixed. Builds, profilers, players and heavy analysis do not
overlap the uninstrumented comparisons. All 36 file runs pass independent AV1
identity/color checks without interior gaps, backward transitions or corrupt
markers. Boundary frame counts can differ with actual source delivery.

| Workload | Backend | Fixed CPU ms ± SD | Candidate CPU ms ± SD | Fixed / candidate peak RSS MiB, means |
|---|---|---:|---:|---:|
| Motion | Direct RGB | 55.086 ± 13.621 | 65.708 ± 6.010 | 26.277 / 25.839 |
| Motion | VAAPI | 93.103 ± 13.354 | 90.348 ± 5.295 | 37.988 / 37.757 |
| Long pause/resume | Direct RGB | 40.517 ± 4.299 | 41.589 ± 4.006 | 26.203 / 26.484 |
| Long pause/resume | VAAPI | 52.806 ± 6.812 | 52.921 ± 7.933 | 37.977 / 38.236 |
| Short pause/resume | Direct RGB | 43.246 ± 6.910 | 41.855 ± 0.309 | 25.745 / 25.751 |
| Short pause/resume | VAAPI | 53.863 ± 2.709 | 58.708 ± 6.304 | 37.630 / 37.349 |

These CPU and RSS observations are mixed. They do not establish CPU
nonregression or an all-metric improvement. The selected behavior is confined
to the explicit compact policy; retain remains available without this input
retirement/reallocation tradeoff. In particular, the moving RGB CPU mean is
higher in this series even though its image allocation is unchanged.

At seconds 3.3–3.7 of either pause, candidate RGB uses 70.40625 MiB VRAM and
2.0859375 MiB GTT versus 91.5546875/23.2421875 MiB for the control. VAAPI uses
92.18359375/2.19140625 MiB versus 103.43359375/23.34765625 MiB. At seconds
5.5–6.5 of the long pause, the control RGB has also reached 70.40625 MiB;
VAAPI retains the 11.25 MiB difference. The short-pause candidate reaches
standby before motion resumes at second four. Active RGB returns to
91.5546875 MiB. Some VAAPI candidate runs peak at 103.44140625 MiB versus
103.43359375 MiB for the control, an observed additional 8 KiB whose attribution
is unresolved. Sampling can miss shorter-lived peaks.

Maximum paused RGB source leases are 6,018.608 ± 0.229 ms fixed versus
2,014.497 ± 0.669 ms candidate for the long pause, and 3,017.524 ± 1.246 ms
versus 2,015.145 ± 1.894 ms for the short pause. These are retained-image
lifetimes, not GPU encoding latency. Both producer images remain allocated;
the after-motion shared capture allocation is 57.5 MiB in both versions.

Twenty-four separate API-traced runs also pass. The first resumed real packet
is selected using both timestamp gaps: the first belongs to the static key
near second three; the second belongs to resumed input near second four or
seven. Choosing only the largest gap would misidentify the short-pause packet.

| Resume | Backend | Fixed send-to-packet ms ± SD | Candidate send-to-packet ms ± SD |
|---|---|---:|---:|
| Long pause | Direct RGB | 10.5842 ± 1.1636 | 11.5415 ± 0.2416 |
| Long pause | VAAPI | 10.5881 ± 0.7544 | 10.1686 ± 0.4422 |
| Short pause | Direct RGB | 8.4474 ± 0.7287 | 8.7049 ± 0.0312 |
| Short pause | VAAPI | 7.0101 ± 0.0712 | 7.3817 ± 0.0267 |

Across all encoded packets in those traced runs, the following are means of
the three per-run observed quantiles/maxima, not pooled population estimates:

| Workload/backend | Fixed / candidate observed p99 ms | Fixed / candidate maximum ms |
|---|---:|---:|
| Long pause, direct RGB | 9.8221 / 9.7343 | 12.5620 / 13.0275 |
| Long pause, VAAPI | 10.0588 / 10.0342 | 11.4097 / 11.9342 |
| Short pause, direct RGB | 9.7325 / 9.7269 | 13.0762 / 11.9672 |
| Short pause, VAAPI | 9.9006 / 9.9001 | 10.4949 / 11.2636 |

These samples include scheduling and GPU completion, but VAAPI input preparation
occurs before `avcodec_send_frame`. The separate publication-timestamp series below
therefore measures that earlier work as well. Neither series establishes a tail
bound or compositor-to-disk latency.

Four additional forty-second replay runs, one per backend/version, save twice
after the moving portion has left the rolling window. Both exports decode
correctly in every run. Total exported bytes match within each backend:
1,315,916 for RGB and 1,313,236 for VAAPI. The maximum RGB source lease is
38,984.943 ms fixed versus 2,016.310 ms candidate. CPU is 24.378/24.138 ms
fixed/candidate for RGB and 26.771/25.976 ms for VAAPI; peak RSS is
25.609/26.695 MiB and 37.738/38.086 MiB respectively. With one repetition,
these are functional/resource observations, not performance distributions.

Four default-cadence runs, both versions on both backends, retain the one-second
refinement behavior: six encoded pictures, no cached substitutions, and exact
decoded hashes within each backend. The 64 primary confirmation runs and this
default compatibility check are indexed in [raw-index.txt](evidence/raw-index.txt).

The first attempt to include pre-send work used the fixture's nominal source
PTS as a physical clock origin. That analysis is rejected: the paced fixture
anchors its first timestamp after image generation, then advances by timer
ticks. Subsequent frames can publish before their nominal timestamp because
their generation finishes at a different timer phase. All 24 clips pass
functional checks, but negative pre-send intervals expose the invalid latency
origin. Those runs and the failed interpretation are preserved in the evidence.

The corrected diagnostic records `CLOCK_MONOTONIC` immediately before the
private producer's `pw_stream_queue_buffer`, copying metadata before ownership
transfer. It matches real encoded packet PTS to unique publication records;
synthetic static keys are excluded from the resumed-real-input observation.
This boundary includes PipeWire scheduling, input restoration/preparation,
encoder submission, GPU completion and wakeup. It excludes source generation,
compositor presentation, muxing and disk completion, and adds tracing overhead.

All 24 corrected runs pass independent decoding and timestamp/order checks.
First resumed-picture observations, three repetitions per configuration:

| Resume/backend | Fixed publication-to-packet ms ± SD | Candidate publication-to-packet ms ± SD | Fixed / candidate publication-to-send ms, means |
|---|---:|---:|---:|
| Long pause, direct RGB | 10.7296 ± 0.6727 | 11.6866 ± 1.4013 | 0.1734 / 0.2193 |
| Long pause, VAAPI | 9.7710 ± 0.9715 | 10.4233 ± 0.1731 | 0.2342 / 0.3506 |
| Short pause, direct RGB | 8.1862 ± 0.1102 | 9.7246 ± 1.8761 | 0.1204 / 0.0893 |
| Short pause, VAAPI | 8.4456 ± 2.0562 | 9.0942 ± 2.3583 | 0.2112 / 0.2997 |

The candidate's mean first-resume publication-to-packet delay is higher in all
four comparisons in this diagnostic series. VAAPI's pre-send mean is also
higher, consistent with restoring the retired input, although this boundary
includes scheduling as well as allocation and does not isolate causality.
The recorded variation and small sample count do not support a universal
latency ordering. Compact saves standby resources; retain remains available
to keep capacity ready. The evidence does not establish every-metric superiority.

Together, the 64 primary runs and 24 corrected publication runs provide 88
final comparisons. The additional rejected-origin runs are retained as
functional evidence only. The default-cadence check is separate. Memory target
arithmetic passes for portrait 8-bit, 4K 10-bit and odd one-pixel dimensions.

The local `build/gcc-release/gpukey` was stale despite the current Nix packages.
It has now been rebuilt and passed the release unit/CLI checks and all seven
native capture cases, including static multichannel replay and reconfiguration.
The optional absolute packet-time tracer builds with GCC and Clang. A final
`nix flake check` passes on applicable x86-64 checks; ARM execution remains
unverified. The source, fixed control, failed analyses, final profiles and
comparison artifacts are preserved under the raw checkpoint evidence index.
The broad optimization goal remains active.

## 24 October 2026: reclaim reusable VAAPI reconstructions

The fixed reference is the 220-file raw-input checkpoint and package
`asy315yaqbdrwcfd8arv7wj7d8yarzf8`. The design audit found separate FFmpeg
surface-pool and Mesa reconstruction-cache lifetimes. A four-way experiment
compared the original, driver-only ownership, library-only trimming, and both.
Library-only trimming left standby at 92.18359375 MiB. Naively keeping one
buffer per retained surface reduced combined standby to 71.2265625 MiB but
raised active peak to 135.2109375 MiB, versus about 103.44 MiB fixed. That
prototype lost the original cross-surface reuse mechanism and was rejected
for active memory; its patch, packages and results remain saved.

The revision retains that reuse and records an idle buffer's owning surface ID
in the unused order-hint field. Active entries retain the codec order hint.
There is no new per-entry field or allocation. Surface destruction clears its
borrowed cache entry; context destruction leaves the surface-owned backing
for the surface's eventual destructor. The corrected three-way probe returns
active peak to about 103.44 MiB and reaches 71.08984375 MiB standby when combined
with idle surface-pool trimming. Driver-only remains at the original footprint.

Final source removes redundant current/retired surface lookups. The actual-source
ownership oracle preserves signed loop indices, byte-sized counters and full
32-bit IDs. GCC/Clang normal and ASan/UBSan runs pass for all eight reference
slots plus the current image, self/cross-surface reuse, transfer into an empty
slot, failed allocation, invalid/live-foreign contexts and both destruction
orders. The old-source negative control rejects the new surface-ownership
property; this is not a general correctness failure of the old driver.

The oracle's first extraction stopped at an inner `break`, then a strict compiler
rejected an uninitialized test array. A later strict warning prompted a simpler
unconditional current-surface lookup in the real implementation. The failed
build logs and corrected source operations are preserved. Tests do not substitute
a model with wider counters or unsigned indices for the actual library types.

Final release and application-ASan native suites pass all seven capture cases,
including multichannel audio, replay, reconfiguration and missing PTS. Four
concurrent workers complete sixteen encoder lifetimes with repeated standby and
independent 8/10-bit pixel checks. Full C23 compiler/linker, sanitizer and
applicable x86-64 flake checks pass, including both new source-based checks.
ARM execution and a sanitizer-instrumented entire driver stack remain unverified.

A native allocation trace separates the 11,796,480-byte input image,
22,183,936-byte coded buffer and two 11,059,200-byte reconstruction restores.
Failing the first or second reconstruction allocation after standby exits with
an explicit error and preserves 64 or 65 independently decoded pictures,
respectively. The latter includes the first resumed picture. The app sanitizer
logs remain clean. An unchecked auxiliary allocation in the driver constructor
is also guarded; that exact `calloc` site was not separately faulted natively.

Final-source Samply and Tracy runs each save two independently decoded replay
exports. Their instrumented timings are separate from the uninstrumented
comparisons. Kernel profiling policy and live desktop configuration are unchanged.

The deterministic control comparison matches all 384 decoded pictures, AV1
packet bytes, timestamps, durations and flags across sixteen 8/10-bit streams
with repeated standby. Samply records 294 main-thread samples and 45,625
microseconds of CPU delta; hardware receive accounts for 35.8% inclusively,
VA EndPicture for 20.5%, and VAAPI issue for 20.3%. Inclusive values overlap.
Tracy records two key reuses and two exports, with one standby-output release
zone at 68,670 ns. These single profiles do not establish latency distributions.

Fresh confirmation uses a fixed private producer, two source buffers, QP 32,
depth two, compact policy and two-second refreshes. The recorder and its pinned
library/driver pair rotate against the frozen control over three repetitions.
No builds, profilers, players or heavy analysis overlap the timed comparisons.
The following runs have **recorder statistics enabled**, including its optional
timing histograms; they have no external profiler. A separate series removes
`--stats` to measure recording without those timers.

| Workload | Fixed CPU ms ± SD | Candidate CPU ms ± SD | Fixed / candidate peak RSS MiB, means |
|---|---:|---:|---:|
| Portrait/60 motion, 30 s | 296.444 ± 0.935 | 292.040 ± 7.688 | 38.310 / 38.495 |
| Long pause/resume, 10 s | 53.171 ± 3.878 | 53.889 ± 3.890 | 38.359 / 38.312 |
| Short pause/resume, 8 s | 56.701 ± 11.870 | 61.458 ± 10.961 | 37.786 / 38.103 |
| Requested 1080p/1000 FPS, 10 s | 212.617 ± 5.005 | 211.198 ± 3.664 | 36.591 / 37.022 |

CPU and RSS remain mixed; these samples do not establish general CPU
nonregression. Moving portrait peak VRAM is exactly 103.43359375 MiB in both
variants. During both pause lengths, the candidate reaches **71.08984375 MiB**
versus **92.18359375 MiB** fixed: **21.09375 MiB**, or about 22.9%, less.
Shared capture storage stays 57.5 MiB. Paused GTT stays 2.19140625 MiB. On
resume, the candidate's sampled peak is 103.4375 MiB versus 103.44140625 MiB
fixed; that 4 KiB difference is not a claimed image-storage improvement.
One separately traced run has an additional 8 KiB in standby. Sampling may
miss shorter-lived peaks.

Actual overloaded submitted rates are 354.215 ± 1.339 FPS fixed and
354.474 ± 0.961 FPS candidate; produced rates are 355.659 ± 1.123 and
355.658 ± 0.685 FPS. Both peak at 36.390625 MiB VRAM. Missing interior image
identities average 7.667 ± 8.145 fixed and 5.000 ± 3.464 candidate per run.
The encoder-skip counter is zero, so the exact earlier handoff location is
untraced. No backward identity transition or corrupt marker occurs. Motion
and pause/resume runs have no missing interior identity. Requested 1,000 FPS
is not delivered throughput or a hardware limit inferred from this fixture.

The twelve separate latency runs match actual producer publication to the
first resumed encoded packet, including input preparation and reconstruction
restoration. They exclude source generation, compositor presentation and disk
completion, and include diagnostic overhead.

| Resume | Fixed publication-to-packet ms ± SD | Candidate publication-to-packet ms ± SD |
|---|---:|---:|
| Long pause | 10.4570 ± 0.7773 | 11.1617 ± 1.1751 |
| Short pause | 9.0401 ± 2.4472 | 8.6299 ± 1.0856 |

Mean pre-send intervals are 0.3527/0.3518 ms fixed/candidate for the long pause
and 0.3013/0.3323 ms for the short pause. Means of per-run observed API p99
are 9.9306/9.9646 ms and 9.9134/9.9234 ms, respectively. Means of per-run API
maxima are 10.6882/11.8830 ms and 11.3097/10.7751 ms. This mixed, small sample
does not establish a latency win or tail bound; restoring freed images is real
work. The fixed driver/profile remains available.

Two forty-second replay runs save twice after motion has left the rolling
window. Both exported clips decode correctly in each run, with matching total
output size of 1,313,236 bytes. CPU is 27.642/27.996 ms fixed/candidate and peak
RSS 37.656/37.582 MiB. With one repetition, these are functional/resource
observations. All 38 primary runs pass, and their frozen input hashes verify.

Twelve additional rotating motion and long-pause comparisons omit `--stats` to
measure recording without the application's optional timing histograms, while
retaining the same external memory observer and independent decoded-output
oracle. All twelve pass with zero interior identity gaps, backward transitions
or corrupt markers:

| Workload without `--stats` | Fixed CPU ms ± SD | Candidate CPU ms ± SD | Fixed / candidate peak RSS MiB, means |
|---|---:|---:|---:|
| Portrait/60 motion, 30 s | 284.142 ± 5.618 | 289.888 ± 5.822 | 38.008 / 37.759 |
| Long pause/resume, 10 s | 51.312 ± 5.580 | 53.270 ± 5.505 | 38.466 / 37.853 |

Moving peak VRAM remains 103.43359375 MiB in both variants; paused VRAM reaches
71.08984375 MiB in the candidate versus 92.18359375 MiB fixed, with resumed
peaks at 103.4375 and 103.44140625 MiB respectively. Together, the 38 primary
and 12 `--stats`-free runs provide 50 fresh confirmation comparisons. Target
arithmetic passes for portrait 8-bit, 4K 10-bit and odd one-pixel dimensions.
Final formatting and applicable x86-64 flake checks pass. The pinned packages,
controls, failed prototypes and reproduction artifacts are indexed in
[vaapi-dpb-index.txt](evidence/vaapi-dpb-index.txt). The broad optimization goal
remains active.

## 24 October 2026: synchronous Vulkan transfers, lazy upload pools, and standby staging reclamation

The fixed reference is the 227-file VAAPI-DPB checkpoint (`/tmp/gpukey-before-upload`),
with fixed packages `4dfa9hh26w59r20pwhcsl8841d667c1c-gpukey-rgb-amd` and
`qz9znwylcb8jf5q9z4i85w1n1d95q72f-gpukey-vaapi-dpb-amd`. Auditing the CPU
staging and Vulkan upload path exposed three distinct representation and
lifetime costs:

1. In `libavutil/hwcontext_vulkan.c`, `vulkan_transfer_frame` waited for `exec`
   on downloads (`!upload`) but returned immediately after queue submission on
   uploads (`upload`), attaching a clone of `swf` (`sw_frame_deps`) and the
   host-mapped or pooled `FFVkBuffer` (`buf_deps`) to the two-slot
   `FRAME_TRANSFER` execution pool until each slot wrapped two transfers later.
   That left `swf->buf[0]` with `refcount > 1` on return (forcing per-frame
   multi-megabyte reallocations for callers using `av_frame_make_writable`),
   pinned two simultaneous GTT host-pointer imports (`21.10 MiB` at 8-bit
   portrait or `42.19 MiB` at 10-bit portrait) or two `fp->tmp` buffers, and
   retained both across static standby even after `m->staging` was freed.
2. In `src/media.c`, uploaded Vulkan (`!direct`) set `fc->initial_pool_size` to
   `surfaces` (`8`), omitted `queue_dpb = 1` when supported, and excluded
   `GK_VULKAN` from `compact_inputs` and `release_idle_inputs`. That eagerly
   allocated eight NV12/P010 input images (`84.59 MiB` at 8-bit portrait,
   `169.19 MiB` at 10-bit portrait) plus per-picture reconstructions, and
   retained all of them in standby.
3. Because uploaded Vulkan preparation preserves `m->last` (`m->pool[i] != m->last`),
   a naive zero-spare policy freeing every writable frame `!= m->last` after
   each packet causes per-frame `VkImage` allocation and destruction churn. In
   addition, `m->staging` (`10.546875 MiB` at 8-bit portrait, `21.09375 MiB` at
   10-bit portrait) remained allocated in RAM throughout compact static standby
   on both VAAPI and Vulkan, and encodings completing during multi-millisecond
   CPU conversion/upload were not collected before the next pre-send pending
   check, allocating a second `21.15625 MiB` coded output buffer.

`nix/ffmpeg-vulkan-transfer.patch` waits for `exec` completion on both transfer
directions before returning, omits the redundant execution-context software
frame and buffer dependency clones, and trims `fp->tmp` in
`vulkan_frame_free_cb`. `src/media.c` sets `fc->initial_pool_size = 0`, enables
`queue_dpb = 1` on uploaded Vulkan when advertised, extends `compact_inputs`
and `release_idle_inputs` to `GK_VULKAN` (`!direct`) with one writable spare
while `m->last && !m->prepared` and zero spares in standby, frees `m->staging`
in compact standby when `!m->prepared` and restores it via `ensure_staging`,
and collects completed output before the pre-send check on uploaded paths
(`!m->source.dmabuf`). No struct field is added: `struct gk_media` and
`struct gk_media_options` remain `2,280` and `192` bytes on x86-64.

A three-way `av_hwframe_get_buffer` / `av_buffer_pool_trim` trace across a
239-picture portrait 8-bit pause/resume run separates the three designs:

- **Fixed control**: `247` total frame gets (`8` input gets plus `hwframe_pool_prealloc`,
  `239` reconstruction gets), `6` trimmed entries (coded buffers only), and
  `166.734 MiB` standby VRAM.
- **Naive zero-spare challenger**: reaches `13.184 MiB` standby VRAM, but
  performs `129` input-frame allocations and `131` trims (`134` total gets) due
  to per-frame `VkImage` churn during active capture.
- **Selected one-spare active / zero-spare standby candidate**: `9` total frame
  gets (`4` input gets: `2` during initial motion and `2` restored after
  standby; `5` reconstruction gets: `3` initial and `2` restored) and `5`
  standby trims, eliminating per-frame churn while reaching `13.184 MiB`
  standby VRAM.

Source-extracted GCC and Clang ordinary and ASan/UBSan checks
(`nix/vulkan-upload-check.nix`, `tests/vulkan_upload_probe.c`,
`tools/extract-vulkan-upload.py`) verify synchronous transfer completion,
single-owner `swf` refcounts, `fp->tmp` trimming on frame free, one-spare
active / zero-spare standby pool transitions, and `ensure_staging` failure
cleanup; the unpatched transfer source fails the single-owner check as a
negative control. All 24 x86-64 flake checks, 250-frame 8/10-bit Vulkan
validation-layer DPB retirement/restoration checks, forced staging packet-hold
checks, and 16 concurrent encoder lifetimes pass. Native ASan/UBSan and release
fault injections failing `ensure_staging` (VAAPI and Vulkan) or resumed upload
image allocation after standby exit with code `1`, pass sanitizers, and
preserve all completed pre-standby pictures (`22` under ASan, `64` in release).

Final-source Samply and Tracy captures on `--shm-portal-replay` each save two
independently decoded replay clips. Samply records `1,796` main-thread samples
and `1,551,637 us` of CPU delta (`gk_rgb_to_yuv_rows` `96.7%` inclusive,
`vulkan_transfer_frame` `1.0%`, `receive_video` `0.5%`). Tracy records `4`
`media.allocate-input` zones (`154,678 ns` mean), `1`
`media.release-idle-output` zone (`124,426 ns`), `2`
`media.reuse-independent-key` zones (`9,989 ns` mean), and `391`
`media.release-idle-inputs` zones.

Fresh uninstrumented confirmation rotates the fixed (`4dfa9hh26w59r20pwhcsl8841d667c1c`
and `qz9znwylcb8jf5q9z4i85w1n1d95q72f`) and candidate
(`93sawk9pg2bp1im42906lbizz55zfp3i` and `livs4j7rr8n41rfxrakys6fnz80j99g9`)
recorders across six 2560×2880 / 60 FPS cases (three repetitions each, 36
lifecycle runs) plus two 40-second rolling replay runs (38 primary runs total),
with eight separate `smaps` standby snapshots:

| Case (2560×2880 / 60) | Fixed CPU ms ± SD | Candidate CPU ms ± SD | Fixed → candidate peak VRAM MiB | Fixed → candidate standby VRAM MiB | Fixed → candidate peak / standby GTT MiB |
|---|---:|---:|---:|---:|---:|
| Uploaded Vulkan 8-bit pause/resume, 10 s | 2141.136 ± 25.645 | 2138.413 ± 28.077 | 166.742 → **56.852** | 166.734 → **13.184** | 90.173 / 23.188 → **33.793 / 2.086** |
| Uploaded Vulkan 10-bit pause/resume, 10 s | 2292.715 ± 71.534 | 2318.306 ± 21.153 | 318.586 → **109.762** | 318.561 → **23.730** | 121.815 / 44.281 → **44.341 / 2.086** |
| Uploaded Vulkan 8-bit motion, 8 s | 4224.340 ± 137.884 | 4275.399 ± 75.654 | 166.755 → **56.852** | — | 86.656 → **33.793** (active) |
| Uploaded VAAPI 8-bit pause/resume, 10 s | 2370.659 ± 75.740 | 2375.970 ± 98.633 | 57.133 → 57.125 | 13.227 → 13.219 | 58.566 / 5.707 → **37.410** / 5.707 |
| Uploaded VAAPI 10-bit pause/resume, 10 s | 2691.778 ± 97.676 | 2810.297 ± 4.921 | 110.020 → 110.016 | 24.082 → 24.078 | 72.629 / 9.223 → **51.473** / 9.223 |
| Native DMA-BUF RGB Vulkan pause/resume, 10 s | 45.747 ± 4.519 | 45.331 ± 4.812 | 91.555 → 91.555 | 70.406 → 70.406 | 23.242 / 2.086 → 23.242 / 2.086 |

Resumed active GTT also falls from `72.552 ± 12.215` to `23.2421875 ± 0.000 MiB`
on uploaded Vulkan 8-bit, `107.750` to `23.2421875 MiB` on uploaded Vulkan
10-bit, `48.020` to `26.863 MiB` on uploaded VAAPI 8-bit, and `51.535` to
`30.379 MiB` on uploaded VAAPI 10-bit. Mean `encode_and_receive` times on
uploaded Vulkan fall from `135.18 → 69.94 us` (8-bit pause), `189.60 → 88.02 us`
(10-bit pause), and `139.84 → 66.47 us` (8-bit motion) because `queue_dpb`
reuses persistent reconstruction views. Whole-process CPU and lifetime peak RSS
remain mixed across the CPU-conversion workloads; no universal CPU speedup is
claimed.

In the diagnostic `smaps` snapshots taken at `5.5 s` during standby, anonymous
RAM and resident set size drop by the released `m->staging` allocation:

- Uploaded VAAPI 8-bit: RSS `161.523 → 150.730 MiB` (`-10.793 MiB`), anonymous
  `71.359 → 60.809 MiB` (`-10.551 MiB`).
- Uploaded VAAPI 10-bit: RSS `171.980 → 151.121 MiB` (`-20.859 MiB`), anonymous
  `82.059 → 60.957 MiB` (`-21.102 MiB`).
- Uploaded Vulkan 8-bit: RSS `150.852 → 140.250 MiB` (`-10.602 MiB`), anonymous
  `71.656 → 61.055 MiB` (`-10.602 MiB`).
- Uploaded Vulkan 10-bit: RSS `161.566 → 140.414 MiB` (`-21.152 MiB`), anonymous
  `82.371 → 61.219 MiB` (`-21.152 MiB`).

Both 40-second uploaded Vulkan 8-bit replay runs save twice after the initial
moving frames age out of the 30-second window; both exports decode
independently and match at `1,745,859` total bytes (`3267.106` vs
`3168.409 ms` CPU, `166.746 → 56.852 MiB` peak VRAM, `166.742 → 13.184 MiB`
standby VRAM, and `65.500 / 23.188 → 33.793 / 2.086 MiB` peak / standby GTT).
All 36candidate confirmation clips have zero interior missing identities,
backward transitions or corrupt markers (one fixed `vk10-pause` run had one
interior missing identity). Complete artifacts, controls and reproduction
commands are indexed in [vulkan-upload-index.txt](evidence/vulkan-upload-index.txt).
The broad optimization goal remains active.

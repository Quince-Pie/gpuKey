# gpuKey implementation brief

## Authority and explicit requirements

Replace the C23 template with a usable GPU recorder strictly grounded in
`material/`; rename the project; use the Nix toolchains and Tracy; inspect source
when integrating libraries; verify consequential choices and report material limits.
No execution deadline was supplied. The design paper is authoritative research;
its provisional operating profiles and future experiments are not new user mandates.
No performance-superiority claim may substitute for actual comparison.

The active goal includes the conditional memory targets in
[docs/memory-targets.md](docs/memory-targets.md): at 2560x2880, 8-bit AV1 with
two source buffers and two past references, approach 98.4375 MiB of dense image
payload for VAAPI VPP, or 87.890625 MiB for direct RGB, plus justified driver
storage. A producer-native NV12 alternative targets 52.734375 MiB but requires
a producer interface change. Raw CPU video copies target zero; replay RAM must
account for actual distinct encoded payload, timing records and export lifetimes.
Independent-key standby targets are 66.796875 MiB for direct RGB with one live
reconstruction and 87.890625 MiB for VAAPI with its current reconstruction
capacity and no converted input. These are dense payload models, excluding
alignment and other driver storage; restoration on motion can allocate.
Resolution and codec do not determine a universal RAM or physical-VRAM minimum.

## Current extension brief

Synchronous Vulkan transfers, lazy upload pools, and standby staging
reclamation: fixed source is the 227-file VAAPI-DPB checkpoint in
`/tmp/gpukey-before-upload` (archived in `docs/evidence/vaapi-dpb-control.tar.gz`
and `docs/evidence/vaapi-dpb-evidence.tar.gz`, verified by
`docs/evidence/vaapi-dpb.sha256`); new experiments belong under
`/tmp/gpukey-upload`. Auditing the explicit CPU conversion and Vulkan upload
path identified three avoidable resource costs:

1. `libavutil/hwcontext_vulkan.c::vulkan_transfer_frame` returned from uploads
   without waiting for `exec`, retaining a software-frame clone (`sw_frame_deps`)
   and the imported host-pointer or staging `FFVkBuffer` (`buf_deps`) across two
   `FRAME_TRANSFER` execution slots until wraparound or teardown. That left
   `swf->buf[0]` with `refcount > 1` on return, pinned two simultaneous GTT
   host-pointer imports (`21.10 MiB` at 8-bit portrait, `42.19 MiB` at 10-bit
   portrait) or two `fp->tmp` staging buffers, and leaked them across standby.
2. `src/media.c` preallocated `fc->initial_pool_size = surfaces` (`8`) on
   uploaded Vulkan (`84.59 MiB` 8-bit / `169.19 MiB` 10-bit at portrait),
   omitted `queue_dpb = 1` when supported, and excluded `GK_VULKAN` from
   `compact_inputs` and `release_idle_inputs`.
3. `m->staging` (`10.55 MiB` 8-bit / `21.09 MiB` 10-bit CPU RAM) remained
   allocated during compact static standby on both VAAPI and Vulkan, and
   encodings completing during multi-millisecond CPU conversion/upload were not
   collected before the pre-send pending-packet check.

`nix/ffmpeg-vulkan-transfer.patch` waits for `exec` completion on both transfer
directions before returning, omits the redundant execution-context software
frame and buffer dependency clones, and trims `fp->tmp` in
`vulkan_frame_free_cb`. `src/media.c` sets `fc->initial_pool_size = 0`, enables
`queue_dpb = 1` on uploaded Vulkan when advertised, extends `compact_inputs =
options->compact_gpu && pipeline && !direct` and `release_idle_inputs` to
`GK_VULKAN` with one writable spare while `m->last && !m->prepared` and zero
spares in standby, frees `m->staging` in compact standby when `!m->prepared`
and restores it via `ensure_staging`, and collects completed output before the
pre-send check on uploaded paths (`!m->source.dmabuf`). A three-way trace
rejects the naive zero-spare active policy (`129` input-frame gets and `131`
trims across 239 frames due to per-frame churn) in favor of the one-spare
active / zero-spare standby policy (`4` input-frame gets, `5` reconstruction
gets, `5` standby trims).

All 24 x86-64 flake checks (including `vulkan-upload` and `vulkan-upload-clang`),
250-frame 8/10-bit Vulkan validation-layer DPB checks, packet-hold checks, 16
concurrent encoder lifetimes, ASan/release post-standby staging and upload-image
allocation faults, Samply/Tracy profiles, 38 rotating confirmation runs, and 8
standby `smaps` snapshots pass. Uploaded Vulkan 8-bit active/standby VRAM falls
`166.742 → 56.852 MiB` / `166.734 → 13.184 MiB` and GTT falls
`90.173 / 23.188 → 33.793 / 2.086 MiB`; 10-bit active/standby VRAM falls
`318.586 → 109.762 MiB` / `318.561 → 23.730 MiB` and GTT falls
`121.815 / 44.281 → 44.341 / 2.086 MiB`; uploaded VAAPI 8/10-bit peak/resumed
GTT falls by `21.15625 MiB`; and standby `smaps` anonymous RAM falls by
`10.55–10.60 MiB` (8-bit) and `21.10–21.15 MiB` (10-bit). Final packages are
`93sawk9pg2bp1im42906lbizz55zfp3i` (`rgb-amd`),
`sbk285yp9hk1zrmj74d4hr9kgsnb74sd` (`rgb-full-amd`),
`livs4j7rr8n41rfxrakys6fnz80j99g9` (`vaapi-dpb-amd`), and
`8r7cxxwjh9v617122xdmq8yqy9lnj3py` (`amd`), indexed in
`docs/evidence/vulkan-upload-index.txt`.

### Preceding VAAPI-DPB checkpoint

VAAPI reconstruction investigation: fixed source is the 220-file raw-input
checkpoint in `/tmp/gpukey-before-va-dpb`; experiments belong under
`/tmp/gpukey-va-dpb`. The preceding goal turn made verified progress. Current
source inspection finds two independent owners: FFmpeg's reconstruction-surface
pool and Mesa's cached DPB buffers, detached from retired surfaces. Trimming only
the former cannot release the latter's image storage.

Compare the unchanged control, surface-owned DPB reuse, and that ownership with
explicit idle pool trimming. Preserve bitstream semantics and inspect completion,
cross-context handling, late surface destruction, allocation failure and resume.
An alternative that shuts down the encoder in standby stays open: it can remove
the remaining live reconstruction/workspace, but requires reinitialization and a
fresh keyframe on resume. Keeping encoder state is an implementation policy, not
an external requirement. Price its compression/latency cost before rejecting it.
No candidate is selected from source inspection alone. The active paper contract
is `material/research/chapter_gpu.md`, especially separate input/reconstruction/
output lifetimes and complete-path comparisons. The first ownership candidate
adds no per-frame allocation or persistent record field; that is a candidate
property, not an externally mandated architecture restriction.

The first four-way probe passes decoded-image checks but rejects the naive
surface-ownership representation for active memory: driver-only standby grows
to 123.9609375 MiB and combined active peak to 135.2109375 MiB, versus about
103.44 MiB active fixed. FFmpeg retains more surface handles than Mesa needs
physical reconstructions. Combined standby does reach 71.2265625 MiB. Preserve
Mesa's cross-surface buffer reuse in the revision. For an inactive DPB entry,
`order_hint` has no codec reader and can carry the owning surface ID instead;
active entries retain their actual order hint. That avoids a new record field.
Initial patch/packages and all four probes remain saved as controls.

The reuse-preserving revision restores the original active footprint in its
three-way probe: combined standby is 71.08984375 MiB, active peak about
103.44 MiB. Native tracing identifies two 11,059,200-byte reconstructions
restored after standby. The final source removes redundant surface lookups and
passes GCC/Clang actual-source ownership checks with ASan/UBSan, final native
release/ASan capture suites, 16 concurrent encoder lifetimes with repeated
standby and 8/10-bit pixels, deterministic 384-picture/packet equality against
the fixed reference, plus first/second reconstruction allocation faults that
preserve 64/65 decoded pictures. Full C23 verification and final Samply/Tracy
captures pass. All 38 primary confirmation runs (24 uninstrumented lifecycles,
12 actual-publication latency traces and two forty-second replays) and 12
additional `--stats`-free motion/pause comparisons pass, confirming the
21.09375 MiB standby VRAM reduction (92.18359375 to 71.08984375 MiB) while CPU,
RSS and resume latency remain mixed. The old control remains
asy315yaqbdrwcfd8arv7wj7d8yarzf8; the delivered profile is
qz9znwylcb8jf5q9z4i85w1n1d95q72f with driver dmvpki16f3xkriasqsj8wadjnyfbxr84,
indexed in docs/evidence/vaapi-dpb-index.txt.

### Preceding raw-input checkpoint

Verified static raw-input checkpoint: the preceding 219-file control verifies. Its
fixed source is saved in `/tmp/gpukey-before-raw`; evidence belongs under
`/tmp/gpukey-raw`. The implementation releases redundant raw input only after an
independent CPU key safely owns static presentation. It preserves submitted GPU
reads, prepared input, future real-input restoration, replay exports, stop and
error-prefix behavior. Default retain remains the capacity-retaining alternative.
Compact trades raw fallback storage for an explicit error if later key
publication cannot allocate; both paths are tested and documented.

The first prototype releases raw storage only after key reuse; the stronger
candidate transitions after a static key has actually completed. A declared
independent-repeat policy preserves the one-second refinement default and all
existing internal callers. Optional cache/reference setup failure keeps the raw
fallback. After successful standby, later publication failure returns an error
and finalizes the completed prefix. Native failure probes exercise that path:
the compact candidate preserves two decoded pictures; the retained-raw control
continues by re-encoding and produces four. Existing repeat/export tests and new
short-cadence policy/fault tests pass.

Final portrait comparisons confirm that compact standby returns 11.25 MiB of
VAAPI input storage (103.43359375 to 92.18359375 MiB after motion). RGB reaches
its existing 70.40625 MiB standby at the first completed static key instead of
the next refresh. Maximum paused-source lease is 2,014.497 ± 0.669 ms versus
6,018.608 ± 0.229 ms in three long-pause runs. A short pause also reaches standby
and restores capacity on motion. CPU and RSS are mixed; this is a memory/lifetime
improvement, not an all-metric win. CPU staging/upload-pool reclamation remains
a separate unmeasured alternative; retained capacity is not a universal floor.

Full C23 and applicable flake checks pass. Final VAAPI/RGB/full-RGB/ASan native
capture, audio, replay, reconfiguration and static tests pass, including the new
static GPU replay with two 16-channel tracks. Both added flags use existing
padding: media remains 2,280 bytes and options 192 bytes. A published key can repeat
while a newer prepared image remains unsubmitted; the regression checks the
actual displayed black/white timeline and independent-key flags.

Final ASan faults verify both boundaries. Cache setup/reference failure before
commit retains the raw fallback and records the resumed source successfully.
Later cached-key reference failure exits with the original error and two decoded
completed pictures. Failing physical allocation of the next VAAPI input reaches
VPP begin, returns an input error and preserves 64 pictures. The old compact
control, which retains raw input, continues through the late-reference fault
by re-encoding. The fixed 219-file source and all initial/early alternatives
remain recoverable. Final Samply/Tracy captures each save two independently
decoded exports. All 64 primary and 24 corrected publication confirmation runs
pass. The latter include input restoration before the encoder API boundary;
candidate first-resume means are higher in all four comparisons, with substantial
variation. Full latency/CPU/RSS tradeoffs and limits are in `docs/validation.md`.
Nominal source PTS is unsuitable as a physical latency origin in the paced
fixture: its initial generation phase can put later PTS ahead of publication.
Keep that rejected analysis and use actual publication timestamps for the
corrected publication-to-packet diagnostic. The previously stale
`build/gcc-release/gpukey` is rebuilt and passes its unit/CLI and seven native
capture cases. Final flake checks and GCC/Clang diagnostic builds pass.

The broad goal stays active. Remaining priorities include the VAAPI reconstruction
capacity retained in standby, the 4.29296875 MiB gap above its dense target,
CPU staging/Vulkan upload-pool lifetimes, and resume allocation/CPU costs. Retain
the existing alternatives and fixed controls; no all-metric winner or universal
minimum is established by this checkpoint.

### Preceding command-writer checkpoint

The selected implementation is now integrated: a compact RADV header writer
returns fragment lengths to its consumers, derives position from the count, and
emits complete words from an eight-byte host record. The conventional accumulator
remains available through `rgb-word-amd`; the original driver is retained as
`mesa-rgb-byte-reference`. This improves the original command-generation stage
without changing AV1 settings, image capacity or ownership/synchronization rules.
The compact choice has less state/code; no universal ordering against the retained
CPU alternative is claimed.

Final packages are hz02jpvisd33dlv5l4cd5x94g96g0762 (RGB),
aml3iin3mml8kvdlzzl249mwiwzncwmk (full RGB), and dni1bqj73yysrlvc43jdylsdmx23cw7r
(conventional writer). Full C23/flakes checks pass, including actual-source bit
oracles and the repaired validation-gate regression. Native limited/full capture,
16+16-channel audio, replay, reconfiguration, static behavior and resumed-allocation
failure pass; the latter preserves 64 pictures. Final Samply/Tracy captures and
independent exports pass. All 24 final motion, pause/resume, overload and latency
comparisons pass. Mean CPU is lower by about 7.2%, 18.5% and 31.1% in the three
uninstrumented workloads. RSS remains mixed; first-resume API delay is
10.6311 +/-1.3646 ms versus 10.4193 +/-0.8529 ms fixed. Actual overloaded delivery
is about 256 versus254 FPS, not the requested1,000; zero/one missing identity per
run is recorded, with its exact earlier handoff location untraced. Image memory
is unchanged, including70.40625 MiB in standby. Evidence is indexed in
docs/evidence/encode-index.txt. Final integrity/archive checks close this checkpoint.

Scope/limits: the fixed reference is the preceding 212-file checkpoint. Native
packet/picture equality is established for tested AV1/H264/HEVC cases. GPU image
and coded-buffer targets are unchanged. Overload requested FPS is not achieved
throughput; actual offers, delivered pictures and drops must be reported. Process
RSS and API-delay variance remain separate from private-state/code sizes. ARM
runtime, whole-stack sanitizer coverage and universal every-metric superiority
remain unproved. The broader goal stays active.

### Investigation record

Resume-cost continuation: the preceding reconstruction checkpoint is verified;
its 212-file manifest passes. Fixed source is saved in `/tmp/gpukey-before-map`,
with new evidence under `/tmp/gpukey-map`. Preserve AV1 output, source timing,
queue/reference lifetimes, full output capacity and failure cleanup. First
measure new coded-buffer allocation, CPU mapping and submission/readout costs.
Compare the existing eager mapping with useful overlap or deferred mapping only
when no CPU prefix write needs the mapping. Keep the source-arrival timing
boundary; moving work outside a measured call is not a gain. Policy/API choices
remain provisional until measurements establish where the cost lies.

Three instrumented portrait pause/resume runs put resumed coded allocation at
514.5 +/- 21.7 microseconds, CPU mapping at 21.35 +/- 1.70 microseconds, and
binding at 0.54 +/- 0.11 microseconds. These are diagnostic API durations, not
uninstrumented latency estimates. The first libdrm-only tracer missed mapping:
RADV uses a direct mmap path on native hardware. The corrected tracer hooks the
encoder's Vulkan dispatch and preserves the failed measurement assumption.
Deferred mapping is legal if host access still waits for device completion,
but its observed available overlap is small relative to allocation. Keep it
open while auditing the larger allocation and producer-readiness dependency.

All three readiness probes found the first resumed source already ready. The
current profile therefore offers no measured producer wait to hide allocation.
The pinned RADV profile also does not advertise reliable insufficient-bitstream
range detection; smaller-buffer retry would require separate detection and
reference-recovery work, not an assumed safe fallback.

The next sampled CPU lead is RADV's bitstream-template writer. Its current
emulation-prevention mode is never enabled, `bits_size` has no reader, and byte
alignment is an unused helper. Extracted original helpers and three word-oriented
representations pass 4,224 boundary cases and 10,000 stateful sequences against
an independent bit-level oracle under GCC/Clang ASan/UBSan. Cached-memory results
favor different candidates by field width. Audit the actual command-buffer
memory type before choosing; repeated read/modify/write stores may have a very
different cost on write-combined GPU mappings. These are prototype results, not
yet a delivered driver speedup.

The GPU-mapped tests resolve an important representation cost: direct in-place
bit updates retain read/modify/write accesses to command memory. On the tested
write-combined GTT/VRAM mappings those are much slower than accumulating in host
state and emitting complete words. Both accumulator candidates preserve every
tested output; the sentinel representation stores pending bits and total length
in one aligned eight-byte record, while the conventional accumulator uses twelve
bytes. Native candidate drivers for both are building. The original mapped-memory
writer remains the fixed control; native byte equality, lifecycle checks, CPU,
memory and latency measurements still decide adoption.

Both native accumulator drivers now pass validation, exact AV1 packet/picture
equality (8-bit, uploaded 10-bit and portrait), DPB retirement/restoration and
failure cleanup. H264/HEVC also produce exact packet/picture matches when the
control uses the audited FFmpeg patches. The stock full FFmpeg control had
existing synchronization/view-lifetime validation errors, so those attempts are
retained and excluded from a clean-driver claim.

That run exposed a real harness defect: a standalone `! grep` does not trigger
Bash errexit. Negative gates now use explicit failure branches; a new regression
checks both positive/cleanup error rejection and clean-log admission, and rejects
the old checker. Earlier saved DPB logs were searched directly and contain no
validation errors. Recorder comparisons are now running with the fixed binary,
FFmpeg library, producer, settings and rotating driver order, without builds or
profilers. No production writer is selected yet.

The initial whole-recorder series was noisy; a longer 30-second series measured
199.642 +/-14.797 ms CPU for the control, 178.509 +/-12.082 for the conventional
accumulator, and 189.153 +/-7.403 for the sentinel. Per-command CPU diagnostics
independently measured about 16.8 us in the control versus 3.2--5.3 us in the
accumulator candidates. These are separate observations, not a universal speedup.

A producer/consumer reformulation removes the remaining representation tradeoff:
each consumer needs a fragment length. Flush now returns that length and clears
the fragment, so its pending position is derivable from the required bit count
modulo 32. This combines the conventional accumulator with an eight-byte record,
removes the callers' cumulative-count/subtraction bookkeeping, and reduces the
encoder state to 48 bytes. All eight H264/HEVC consumers and the AV1 consumer
were updated; native packet/picture equality, validation, cleanup, GCC/Clang
ASan/UBSan and the source-extracted independent oracle pass. Its matched cost
comparison is running before final selection and integration.

### Preceding reconstruction checkpoint

Reconstruction-storage continuation: the preceding idle-output checkpoint made
verified progress and its 209-file manifest passes. Its fixed source is saved in
`/tmp/gpukey-before-dpb`; new evidence belongs under `/tmp/gpukey-dpb`. Preserve
AV1 quality/reference structure, capture leases, all submitted GPU reads, resume
and failure cleanup. Investigate unused reconstruction images after an independent
key, against retained capacity and a one-unused-image alternative. Static storage
targets must not be reported as lower active-overlap requirements.

The candidate marks retired Vulkan slots inactive in their existing encode
submission, using bits in the existing temporary initial-layout word. It only
deactivates previously active retired slots; Vulkan forbids deactivating an
already inactive slot. Encode reference lists stay separate from BeginVideoCoding
deactivation entries. With successful readout and an empty issued-output FIFO,
compact standby destroys only unused queue-owned views/frames and trims their
frame pool. No additional GPU submission or persistent state field is introduced.
Native validation, decode equality and restoration tests must decide adoption.

The native checkpoint now passes for both 8/10-bit input: three retained images
versus eleven created/eight retired across repeated forced/periodic keys, exact
250-picture decode equality, failed replacement-view creation, packet ownership,
Vulkan validation and ASan/UBSan. The actual recorder passes capture, 16+16-channel
audio, replay, reconfiguration and static checks in limited/full RGB and ASan
builds. Faulting the first resumed 11,087,872-byte reconstruction allocation
preserves 64 decoded pictures and exits with the original error. GCC, Clang and
the x86-64 flake checks pass; ARM execution was not performed.

The first repaired recorder comparison measures idle VRAM of 91.5546875 MiB
retained, 80.98046875 MiB with one spare, or 70.40625 MiB with no spare. Moving
capacity restores to 91.5546875 MiB in every case. The conditional dense standby
target is 66.796875 MiB, separate from the 87.890625 MiB active image target.
The initial one-spare harness incorrectly selected FFmpeg's empty main output
rather than its `lib` output; a nested preload also reached the host's older env
binary. Those attempts are retained and excluded from comparisons. Corrected
runs select the actual library and preload only the recorder. Final-source
Samply/Tracy captures and all 30 final comparisons pass. The first candidate's
27 comparisons are retained too. The final patch removes an ordinary-picture
retirement scan; a 26-state model checks its mask against the declarative set.
Final moving CPU is 67.796 +/- 2.057 ms versus 77.359 +/- 1.508 ms for the fixed
reference over eight seconds, but pause/resume CPU remains mixed. First resumed
API delay is 10.8582 +/- 1.2669 ms versus 10.4878 +/- 0.5470 ms fixed and
11.2430 +/- 1.3753 ms with one spare. These three-run samples establish no
general latency bound or clear one-spare advantage. Select zero unused
reconstructions for explicit compact standby; retain/one-second defaults remain.

Final RGB/full packages are 90sv0rykd64v4fmqr252d0ax735aq55k and
nd09hqrk60hv0larib5ha85rxbxldjp8. The native ASan/Vulkan checks, full capture/audio/
replay checks and resumed-allocation failure are repeated on them. All final
recordings preserve frame identities without interior gaps; all long replays
save twice after history turnover. Evidence is indexed in
docs/evidence/dpb-index.txt. The one-spare long replay contains an additional
observed 4 KiB allocation, recorded rather than rounded away. The broad goal
stays active; no universal state-of-the-art result is claimed.

Retain the reference-preserving design as the current comparison contract, not
as a permanent user constraint. A further standby alternative could discard the
last reference and force an independent picture on resume, pricing its extra
keyframe, quality/compression, latency and slot-reset costs. It has not been tested
or ruled out. Do not call the current one-live-reference target a universal floor.

### Preceding idle-output checkpoint

Idle GPU-memory continuation: the preceding static-replay checkpoint made verified
progress, and its 204-file manifest passes. The fixed source is saved in
`/tmp/gpukey-before-idle`; evidence belongs under `/tmp/gpukey-idle`. First audit
the 21.15625 MiB portrait coded-output reservation and remaining image/workspace
gap. Compare retained capacity with explicit reclamation while preserving AV1
state, source timing, queued/external packet ownership, resume and failure cleanup.
Do not reduce capacity to an observed packet size or count shared imports as freed
physical storage. The memory policy and API remain provisional until tested.

The source audit found a prerequisite lifetime defect: Vulkan pooled buffers use
an encoder/filter-owned FFVulkanContext as their destruction callback's opaque
data. A native retained-packet probe passes when release precedes encoder close,
but crashes afterward in ff_vk_unmap_buffers under allocator free poisoning.
The implemented repair gives each pool an explicit device reference and three
destruction function pointers (32 bytes plus a 24-byte AVBufferRef on x86-64).
It adds no per-packet record, raw image or copy. Imported RGB8, uploaded RGB10
and forced staging RGB10 pass release-before-close, release-after-codec-close
and release-after-device-owner-close checks. Old-library controls fail. Debugger
faults reach both staging and encoder pool constructors and verify clean device
teardown; preload-only injection cannot reach libavutil's symbolically bound
internal calls. Preserve those failed harness assumptions in the evidence.

Compact mode now retains at most one unused Vulkan coded buffer during active
capture, and VAAPI keeps its existing single-idle return policy. On an eligible
static-key cache transition both release all unused coded buffers, restoring one
spare on new encoder input. Vulkan upload-image capacity stays unchanged to avoid
allocation churn. Eight real recorder probes pass: RGB GTT falls from 23.242188
to 2.085938 MiB and VAAPI from 23.347656 to 2.191406 MiB during the static hold.
Both resume correctly, and all 243 decoded picture hashes match their retain
controls. VRAM and the active output-capacity reservation are unchanged.

The resumed-allocation fault exposed another real defect: after a fatal encoder
error, draining its partially updated reference graph can assert. Media now
latches actual video API failure separately from capture/mux failure, skips later
encode/drain calls on that failed codec, and finalizes the collected prefix while
preserving the error exit. Both native backends preserve 64 decoded pictures in
the measured failure cases. A new independent software regression covers send
and receive failures in MKV/MP4 and fails against the saved pre-change source.
The new flag consumes existing padding: the x86-64 media object stays 2,280 bytes.

Compiler, sanitizer and final x86-64 flake checks pass. Final-source frozen Samply
and Tracy captures, 36 rotating file comparisons, four forty-second replay runs
and twelve traced resume runs are complete. No build, profiler, player or heavy
analysis overlaps the uninstrumented timings. Frozen-input hashes pass. Compact
RGB resume API delay is 11.2535 +/- 0.1258 ms versus 10.2266 +/- 0.5815 ms retain;
VAAPI is 9.7469 +/- 0.4988 versus 9.7177 +/- 0.3141 ms, three runs per policy.
These are observed public API delays, not pure GPU/end-to-end latency. CPU/RSS
results are mixed; no general speed win or nonregression bound is established.
Keep the preceding
pciv8gf03n8a2lfa8yb5wwf61vwg1904 VAAPI and habl3gdyjmc58ahgwgplfxlniipcmlyv RGB
packages as fixed references. Defaults remain retain and a one-second static
interval until the measured tradeoffs justify another choice.

The frozen packages are dcivbls3xff4gw8m0mfgkgnh5r8davf9 (VAAPI),
brpdzw51wrc3w3w9px0by8kxxx3ngvm5 (RGB), and i7mlsc466swj5jvlladkfn0hjh93b01z
(full RGB). Evidence is indexed in docs/evidence/idle-index.txt. All test/profile
processes are terminal at this checkpoint; the broad optimization goal stays active.
Profile the resumed allocation/map path next: test overlap of optional output
mapping with already-submitted GPU work when no CPU header write needs the mapping.
Keep the full source-arrival latency boundary so moving work cannot fake a gain.

The 32-byte destruction-dispatch cache is a chosen representation, not a lower
bound: a device-owned destruction helper could let each pool retain only the
device reference and share the device's existing function table. Compare its
allocation/callback costs before claiming the current cold owner is optimal.

Next consequential VRAM probes remain open: after an independent key resets
reference state, determine whether compact mode can release unused reconstruction
images. Validate Vulkan DPB-slot invalidation and every encoder/reference lease
before destruction, preferably recording any required invalidation in the real
key's existing submission. Preserve the key reconstruction and restore images on
demand; do not confuse a static-retention target with the moving-image floor.
Also audit eager Vulkan upload-image allocation against lazy growth that retains
the latest/prepared images and every in-flight input without per-frame churn.

### Preceding static-replay checkpoint

Static-repeat continuation: the preceding turn made verified progress and the
201-file manifest passes. The fixed control is `/tmp/gpukey-before-repeat` and
new evidence belongs under `/tmp/gpukey-repeat`. Preserve AV1, source timing,
audio, replay ownership/export bounds and usable player behavior. The AV1 spec
invalidates the initial idea of showing an already displayed keyframe through
show-existing: such a key is not showable, and a showable hidden key can be shown
that way at most once. Do not emit that invalid shortcut. Compare VFR independent
holds and validated independent-key reuse against normal encoding. A held-frame
snapshot must trim presentation/audio correctly and remain seekable; ordinary
files cannot assume a known final duration. Reusing independent keys requires
the encoder and decoder reference state to remain equal before the next real
input, compatible sequence syntax, no intervening encoded dependent pictures,
and fully accounted payload lifetime. Prototype these mechanisms before choosing
an interface or changing the existing static cadence policy.

Current repeat result: pure single-picture 30-second holds decode but Brave ends
them immediately. A terminal independent sample repairs playback and exact seeks
in Matroska and MP4; a two-image transition also seeks to the correct image on
both sides of the boundary. The first seek fixture lacked HTTP range support;
that oracle was corrected and both controls rerun. The invalid show-existing-key
probe is tolerated by dav1d but rejected by libaom, matching the AV1 prohibition.

The implementation now reuses only an eligible independent key when the encoder
is fully collected and no real input has intervened. Its cache is invalidated on
the next accepted encoder input. One-second defaults stay unchanged: ordinary
P-pictures can refine a static image (the native static probe produced three
reconstructions), so reuse only replaces requested independent refreshes, e.g.
`--static-interval 2`. `--static-encoding always` retains ordinary encoding at
the same cadence. Replay references existing retained CPU payload; Vulkan file
recording copies one encoded key to avoid pinning a large GPU output allocation.
This cache cost must be measured separately. No raw source is read for reuse.

Replay export compacts byte-identical independent key runs in its existing flat
index and retains a terminal sample. The 32-byte live packet record is unchanged.
The stream's first/last display intervals and audio timing remain explicit.
Long MP4 holds retain checkpoints for movenc's actual signed INT_MAX duration
limit, rather than the wider unsigned container field. A 6,000-second probe
exports/decode-checks successfully after correcting that bound.

Core tests cover resumed input, paused export through cache invalidation and
eviction, exact PCM clocks, and injected cache descriptor/ref-allocation failure.
Native VAAPI/Vulkan capture, audio and replay checks pass. The native paused-source
comparison has 243 identical decoded pictures, including all 240 real images;
the candidate avoids two GPU encodes. GCC/Clang, ASan/UBSan and the x86-64 flake
checks pass. TSan initially crashed in the generated CPU-dispatch IFUNC before
main, including in the existing sharing test. Both ownership tests pass with the
existing GK_RGB_PORTABLE build control; this leaves converter instrumentation
enabled and avoids instrumenting the pre-runtime resolver.

Candidate packages are pciv8gf03n8a2lfa8yb5wwf61vwg1904 (VAAPI),
habl3gdyjmc58ahgwgplfxlniipcmlyv (RGB), and f00635yzsjgcq0z6f5gzqkbx17pcym5r
(full RGB). Native ASan, a real Tracy trace and final-package Samply pass. The
earlier local-build Samply capture is retained but excluded from final claims.
All thirty file and eighteen replay confirmation runs pass with frozen inputs.
At the same two-second static cadence, forty-second RGB replay CPU falls from
32.080 +/- 0.151 ms to 19.612 +/- 0.325 ms, actual encodes from twenty to one,
and two exports from 9,742,142 to 1,218,884 bytes. The always-encode control
isolates export compaction from encode reuse. VRAM and peak live history are
unchanged. Moving CPU means are slightly higher with overlapping run variation;
these results establish neither a dynamic speedup nor a nonregression bound.

Final full-range native capture/audio/replay checks pass. Actual final-package
MKV/MP4 replay exports independently decode with libaom and pass headless Brave
playback, seek and identity checks. The initial MP4 harness used a .mkv filename;
the recorder correctly rejected it, and a corrected .mp4 run passes. The 201-file
fixed implementation, all material files and the original user clip retain their
digests. Preserve jp6jclgf9ppzgzj94gykfp770xza0vbg and
3gj45q5p8h34ds037ahvnwh66pv5gar1 as controls. Evidence is indexed in
[repeat-index.txt](docs/evidence/repeat-index.txt). The broad goal remains active;
interactive workload latency, energy, game interference and cross-vendor
superiority remain unestablished.

## Earlier checkpoints

Full-range integration continuation: the preceding turn made verified progress;
the 197-file manifest passes and its control is saved in
`/tmp/gpukey-before-full-range`. New evidence belongs under
`/tmp/gpukey-full-range`. Preserve the fixed VAAPI and RGB binaries, source timing,
audio/replay, producer waits, source retirement and AV1 reference structure.
The material requires honoring advertised conversion pairs. The recorder must
therefore continue using BT.709/limited on the existing driver; a separately
selected process-local driver profile may advertise only the already-probed
BT.2020/full pair. Never request an unadvertised pair or expose invalid cross-pairs.
Carry the negotiated matrix/range into frame, codec, bitstream validation and mux
metadata while retaining the source's BT.709 primaries and transfer. Compare the
usable path on spatial sources, playback, replay and complete resource costs
before making any default-selection claim. New profile selection is provisional;
source/consumer evidence can reopen it.

The capability-driven path is now implemented. A one-byte linked-pair selector
uses existing padding in the 24-byte x86-64 cold import state; there is no added
GPU image or per-packet record. Native full/limited capture, audio, replay,
reconfiguration and static checks pass. The fixture's limited-only black/white
oracle was corrected to use the decoded range, with both profiles rerun. Native
full-range ASan/UBSan and the full/10-bit Vulkan probe/fault checks pass. Headless
Brave renders advancing identities and exposes the expected matrix/range while
retaining BT.709/sRGB; mpv's image-output route emits 60 correct consecutive
identities. A matched 16-frame 640x480 scene check reports RGB PSNR 30.094 dB full
versus 30.000 dB limited, not a perceptual or natural-scene quality theorem.
Fresh user-space Samply, Tracy and API latency traces are complete. Those paired
50-FPS diagnostic runs show no established latency gain (about 9.3 ms mean and
9.76 ms observed p99). The default Samply attempt hit the host's kernel profiling
restriction; the installed wrapper excludes kernel/hypervisor events and is
removed from the recorder environment. No system policy was changed.

Performance confirmation is complete: 36 rotating eight-second runs and twelve
forty-second replay runs retain the fixed VAAPI/RGB references and current
limited/full variants. No build, player or profiler overlaps those timings, and
the recorded input hashes pass. All clips independently decode with no interior
identity gaps/repeats. Portrait replay means (CPU ms / RSS MiB) are
375.968/45.846 VAAPI, 283.762/34.460 fixed RGB, 301.657/34.556 current limited RGB,
and 310.293/35.169 full RGB. All RGB variants retain 91.554688 MiB VRAM and
23.242188 MiB GTT. Full range improves tested color precision but increases
encoded replay storage about 3%; CPU superiority over limited RGB is not shown.
The default stays VAAPI, and both RGB profiles remain explicit options.

Consumer checks identify distinct conversion paths. Brave's WebCodecs YUV samples
match FFmpeg; its software canvas/compositor conversion reflects libyuv's bounded
BT.709 blue coefficient. Full-range patch MAE/max is 0.792/4 versus 2.708/16 for
limited RGB there. The reported renderer is software SwiftShader, not the user's
accelerated desktop. mpv 0.41's default fast zimg image export changes full-range
values (2.792/9); `--zimg-fast=no` produces 0.625/3 without changing the recording.
No source metadata or user settings are changed to compensate. Ordinary mpv GPU
display behavior remains unverified by these image-output tests.

Final native, GCC/Clang, sanitizer and x86-64 flake checks pass. Runnable packages
are `3gj45q5p8h34ds037ahvnwh66pv5gar1-gpukey-rgb-amd` and
`sj4p2950dhrx1b3vf83i1ap0r0ii3qkd-gpukey-rgb-full-amd`; the default AMD wrapper is
`jp6jclgf9ppzgzj94gykfp770xza0vbg-gpukey-amd`. Evidence is under
`/tmp/gpukey-full-range` and indexed in `docs/evidence/full-range-index.txt`.
No benchmark or profiler is left running at this checkpoint. The goal remains
active; this is a usable precision/storage alternative, not an every-metric win.

Next consequential probes remain open. In particular, avoid re-encoding an
unchanged image by investigating AV1 show-existing syntax and cached independent
keys. Preserve equality of encoder/decoder reference state at every new real
input, validate sequence constraints, retain ordinary coding for incompatible
streams, and account for cached payload ownership and exports. Creating a cache
lazily from the pending independent packet may avoid work on dynamic scenes.
This is an unimplemented hypothesis, not a validated optimization. Also audit
unused FFmpeg initialization/dependencies and the remaining image alignment and
driver allocation gap before tuning small fields.

Status checkpoint: the color audit has verified all 16,777,216 eight-bit RGB
tuples at the centers of uniform 8x8 patches. At QIndex 1, the experimental
BT.2020-matrix/full-range driver has RGB MAE 0.8486 versus 1.3948 for the existing
BT.709/limited RGB path. Primaries remain BT.709 and transfer remains sRGB. This
does not test arbitrary spatial scenes or establish player compatibility; the
alternative is still a private probe, not a new default or a delivered full-range
recorder. VPP has lower average error in both the 262,144-color and full-domain
controls (full-domain MAE 0.6689). During verification, the standalone probe
exposed an upload race: derived-image unmapping can submit producer work without
updating the VA surface fence. The probe now waits on each DMA-BUF's writer fence,
as the actual recorder already does. Twenty current-library and two fixed-library
250-frame cases then have identical decoded frames. Color confirmation was
restarted with this corrected producer; the earlier unfenced results are retained
but excluded from conclusions. The native capture/audio/replay matrix passes on
both backends, the Vulkan fault/validation matrix passes, and the final C23 build
and sanitizer suite passes. Native sanitized unsupported-QIndex teardown passes.

The hardware quantizer fix is built and passes native setup tests: unsupported
QIndex 0 fails before output creation on both tested backends. QIndex 1 and 32
recordings independently decode, and software AV1 Q=0 remains available. The
Vulkan path rejects out-of-range requests instead of silently clamping them;
VAAPI checks the advertised lossless transform capability and writes the required
inferred transform mode when it is supported. That supported-hardware branch is
not exercised on this RX 9070 XT. The installed Niri source offers RGB only, so
the producer-native 52.734375 MiB image target requires an upstream interface
change. The current direct-RGB target remains 87.890625 MiB; observed total VRAM
remains 91.554688 MiB. The broader goal is active, with no universal superiority
claim and no new CPU/latency performance claim from this diagnostic work.

Color/producer continuation: the previous dependency audit made verified progress;
the 192-file manifest passes. Its fixed implementation is saved in
`/tmp/gpukey-before-color`, with new evidence under `/tmp/gpukey-color`. Preserve
AV1, source timing, audio, replay, source leases and the fixed VAAPI/RGB references.
First distinguish conversion error from codec quantization using effective
bitstream parameters and independent color oracles. Also recheck the actual Niri
producer's format offer before treating its RGB intermediates as unavoidable.
The complete RGB input domain and real producer/consumer lifetime costs matter;
eight uniform patches do not establish full-scene quality or a universal ranking.

Encode-time processing continuation: the preceding RGB turn made verified
progress; its 192-file manifest passes. The fixed implementation is saved in
`/tmp/gpukey-before-efc`, with new evidence under `/tmp/gpukey-efc`. Preserve the
original VAAPI reference and final RGB package. First separate private/anonymous
RAM from shared mapped libraries; RSS alone does not identify the cause of the
increase. Then test Gallium's encode-time RGB processing with correct color and
reference semantics before selecting its integration. Its nominal VPP output is
currently allocated eagerly; that implementation boundary is not a requirement.
The Vulkan minimal-device/initial-layout route remains open.

The steady mapping snapshots identify 25.180 MiB resident in LLVM in the RGB
driver. Its optional LLVM compiler dependency is unnecessary for this recording
path; Mesa's source supports an ACO-only build. The new driver has no LLVM dynamic
dependency or loaded LLVM mapping. Three-second snapshots show RSS/PSS of
38.586/20.558 MiB for VAAPI, 53.387/44.449 for the old Vulkan driver and
27.000/18.014 for ACO-only Vulkan. These diagnostic runs are not CPU timings.
The recorder executable is byte-identical. Four 250-frame probes, including
ten-bit, portrait and forced keys, have identical encoded packet hashes and
decoded frames across the two Vulkan drivers. A matched three-way comparison is
complete: 27 fresh rotating eight-second runs and one forty-second replay per
variant. Portrait file RSS means are 26.342 MiB ACO, 52.522 MiB old Vulkan and
38.167 MiB VAAPI; CPU means are 64.387/79.601/81.786 ms. The longer replay uses
34.809/60.562/45.844 MiB RSS and 296.057/287.213/403.268 ms CPU respectively.
CPU relative to the previous Vulkan build remains mixed; RAM improves and the
tested output is unchanged. Native audio/replay, sanitizer, allocation-failure,
capability and all 17 x86-64 checks pass. Fresh Samply/Tracy captures are retained.
The ACO driver is `896yc4wnz6wn804kdbl1638gl243vskz`; its `rgb-amd` wrapper is
`4ils1mnbzaj2s92dkwwnyfwzplzv7yk7`. The old driver remains byte-identical and
selectable as `mesa-rgb-reference`. Evidence is archived and indexed in
`docs/evidence/rgb-aco-index.txt`; the broader goal stays active. The RAM
regression is removed in the tested workloads. Color precision, broader latency
and throughput regimes, and the remaining GPU allocations are still open work.

The VA encode-time path remains open: the inspected Mesa frontend handles
processing parameters in an encode context and its EFC predicate accepts these
RGB inputs, but it eagerly allocates the nominal NV12 processing output. The
public libva processing attribute found is for decoding; encode-time support here
must be treated as an implementation-specific capability. No native EFC prototype
or superiority result has yet been established.

Direct-RGB continuation: the previous comparison turn made verified progress;
the 174-file manifest passes and no recorder/benchmark job remains. The fixed
implementation is saved in `/tmp/gpukey-before-rgb`, with new evidence under
`/tmp/gpukey-rgb`. Preserve source timing, AV1 quality/color, static repeats,
replay/audio behavior, retained-buffer ownership and overlap while comparing
native direct input against the current conversion control.

Verified source observations: the currently preferred AMD modifier has its DCC
bit set. Gallium EFC rejects a nonzero legacy metadata offset, but that is not a
universal DCC prohibition: the native RADV probe accepts this exact modifier on
GFX12, where compression is transparent to userspace. Direct-input compatibility
must be queried for each modifier. VAAPI can attach processing
parameters to an encode context, and the Gallium encoder has an EFC fast path.
The VA frontend nevertheless materializes its nominal output before testing that
path. Raw RGB without those processing parameters defaults to full-range output,
which does not match the recorder's limited-range contract. Vulkan has an explicit
RGB profile/session conversion interface, with BT.709/limited advertised on VCN 5.
The native probe reports RGB8/RGB10 source formats, NV12/P010 reconstructions,
BT.709/limited, horizontal cositing, two references and the actual imported
modifier's compatibility. No API route is committed or rejected by familiarity.

Status checkpoint, 23 October 2026: direct RGB is now integrated in the opt-in
`rgb-amd` package and `rgb`/`rgb-clang` development shells. The default remains
VAAPI. Persistent imports share capture storage; an image pin preserves static
repeats, and dense source tokens retire only after encoder and last-image pins
are gone. Producer-fence waiting shares the event loop with audio/control;
codec/mux calls run outside the PipeWire lock. Modifier negotiation uses complete
queried lists rather than a four-element copied array.

The queue-owned reconstruction pool and ordinary allocation control encode and
decode 250 320x240 frames with identical decoded hashes, including forced I
pictures at 17, 41 and 121. Portrait and ten-bit RGB probes also decode. Native
capture passes multichannel (16+16) audio, replay, static-repeat and reconfiguration
checks. Submission/export/view-creation fault injections verify cleanup. The
dependency patch fixes keyframe rate-control declarations, cross-queue upload
barriers, GPU view/output-buffer leaks, failed-picture list unlinking and the
AV1 CBS teardown leak. The noncoherent readback path now invalidates GPU-written
memory; that branch is source-audited, not independently exercised on this GPU.

The RGB validation-layer diagnostic fork repairs profile-chain tracking; it is
not an unmodified-upstream validation pass. Native ASan/UBSan checks pass with
RADV kept loaded; ordinary unloading leaves a separate 256-byte driver report,
preserved alongside the now-fixed 1,054,312-byte FFmpeg leak. All default checks,
the candidate package checks and candidate Clang strict-warning checks pass.
The fixed source/executable is unchanged under `/tmp/gpukey-before-rgb`.
Matched end-to-end comparisons and fresh profiling are underway. The
87.890625 MiB image target remains conditional; no universal win is established.

Further native checks found and corrected two startup audio losses: the video
epoch must be established while its first producer fence is pending, and queued
audio must not be consumed/discarded before that epoch is known. The new AUX
startup oracle derives a 5–15 ms tone window from the fixture's known 200 ms
impulse. Earlier controls have zero amplitude there; the fixed output is about
0.15, including an injected 30 ms producer wait. Existing later-tone checks alone
did not cover this interval.

The 80 KiB allocation hypothesis was revised after reading RADV's command-stream
allocator: these are command buffers, not one CDF table per reserved reference.
Declaring only three session slots did not reduce measured VRAM. The current
candidate instead allocates frame execution pools on first use, stores only
atomic pool pointers in each frames context, and removes the unused download
pool and duplicated command-buffer array. Construction is serialized, completed
pools are published with release/acquire, and an already initialized pool remains
usable while another initializer is paused. The exact helper passes GCC,
ASan/UBSan and TSan checks with constructor failure/retry and paused-owner cases.

Native allocation failure exposed RADV cleanup before its list/base initialization
and FFmpeg's partial-pool lifetime errors. The process-local driver now initializes
destructor-visible state before fallible command-stream creation. FFmpeg records
zero-initialized owned slots before construction and makes pool destruction
idempotent. Native failure exits cleanly; failure at the first input's execution
pool then retries and encodes/decodes 60 frames. Non-host-copy image preparation
also now performs its required initial layout transition.

For file recording, when every earlier submitted input has been collected, the
next input timestamp determines the pending packet's duration. Finalizing that
packet before requesting another encoder output buffer can release its large
mapped allocation early. In-flight work retains the original receive-order path.
Fresh matched package comparisons now verify the complete implementation. At
portrait60, three eight-second file runs average 91.555 MiB VRAM and 23.242 MiB
GTT, versus 103.434/23.348 for the fixed VAAPI reference. CPU is mixed there
(82.896 versus 81.145 ms), while the candidate's 1080p60/240 CPU means improve.
RSS and constant-patch precision favor VAAPI. A fresh forty-second replay pair
saves two independently decoded late clips: candidate/reference CPU is
300.627/385.837 ms, RSS 61.730/45.406 MiB, and VRAM 91.555/103.434 MiB. Both first
clips contain 1,856 distinct consecutive identities. These are scoped results.
The final package is `4fsykvy59bv48gl14plaplbgrpizkq9q-gpukey-rgb-amd` with the
process-local `0jq2axwnmxcargkdlmpjjv02m7g4aggx` RADV driver. Earlier RGB timings
are exploratory. Fresh Samply, Tracy, API-latency and allocation traces are under
`/tmp/gpukey-rgb/*-verified`, archived and indexed in
`docs/evidence/rgb-index.txt`. All 17 x86-64 checks and final native regressions
pass; the research inputs and original clip are unchanged. The default executable
is rebuilt with the startup-audio fix. The broader goal remains active. The next
decisions concern RSS, precision/compression, and whether VA encode-time RGB or a
minimal video-only Vulkan device can improve the complete pipeline. Moving
private reconstruction initialization onto the encode queue is also still open.
Do not treat the current scoped VRAM result as an every-metric optimum.

Cross-recorder continuation: the preceding capacity milestone is verified and
archived; its 170-file manifest passes. The previous turn made concrete progress.
The fixed source/executable is saved in `/tmp/gpukey-before-cross`, with evidence
under `/tmp/gpukey-cross`. The installed GSR reports 6.1.3 and links FFmpeg 9.0.1.
Its separate editable checkout contains user changes and remains untouched. Clean
6.1.3/6.1.4 tag exports provide auditable upstream sources; the installed package's
removed derivation prevents proving its complete build provenance from that file.

First establish a common private portal/PipeWire producer and independent output
oracles. Preserve geometry, source cadence, AV1 bit depth, quantizer/GOP, color
contract and recording lifecycle; report actual decoded frame identities and
quality, not just requested FPS. Consumer-specific regression metadata must not
be imposed on another recorder. Source buffer preferences, timing policy, graphics
initialization, reference/input/output lifetimes and driver versions are measured
costs, not equalities assumed from similarly named settings. Source publication
must advance even when a consumer does not request Header metadata. Initial
comparisons are exploratory until contract and quality alignment are established.
The 98.4375 MiB conditional image-payload target and broader goal remain active.

The first cross-recorder attempts exposed fixture assumptions, not usable timing:
the mock broadcast replies while the real portal sends them to request->sender;
the regression source used zero-length DMA-BUF chunks that GSR treats as cursor
updates. The shared external-source mode now uses directed replies, explicit
allocation sizes, complete damage metadata and source progression independent of
Header requests. Existing zero-size regression cases remain intact. Frame pixels
carry a sequence identity and its complement; separate decoding counts actual
repeats/gaps and checks constant BT.709 patches. Known duplicate and corrupted
picture controls respectively report eleven repeats and reject a broken marker.

Forty-five rotating eight-second runs compare the unchanged gpuKey executable
with clean GSR 6.1.4 at its depth-three policy and depth two, on a common driver.
Cases cover HD60, portrait60, HD240, and source offers capped at two or eight
buffers. CPU/RSS means favor gpuKey in these cases; encode-engine time per actual
picture remains close. GSR requests eight source allocations when offered them,
while its currently imported source images vary, so a process counter alone does
not describe source storage. Constant-patch errors differ; a separately built GSR
control corrects its limited-range matrix before the fresh quality comparison.
No full-scene, game-interference, or universal superiority claim follows.

The live upstream check found release 6.1.5 and commit
ee2c6ca1e03e4432652fe9492b3a405f654b45ac. A clean GCC 16/FFmpeg 9.0.2 build of that
snapshot has eighteen fresh rotating runs and a forty-second replay confirmation
against the unchanged gpuKey binary. At portrait/60 with two source buffers,
eight-second CPU means are 93.009 ms gpuKey, 155.639 ms GSR depth three and
192.231 ms GSR depth two; variation and color differences remain explicit.
The final forty-second pair measures 416.047/746.827 ms recorder CPU,
44.754/76.160 MiB RSS, 103.434/113.715 MiB sampled VRAM, and 23.348/71.305 MiB
GTT. Both save two independently decoded clips after thirty seconds. These are
scoped measurements, not an every-metric proof. All sixteen x86-64 checks,
native application/fixture sanitizers and observer/oracle failure controls pass.
The fixed prior recorder implementation is unchanged; the new deliverable is
the reusable comparison framework and evidence, indexed in
`docs/evidence/cross-index.txt`. The goal remains active.

The next large representation challenger is direct RGB encoding. The pinned Mesa
RADV path exposes VK_VALVE_video_encode_rgb_conversion and emits hardware source
and output-format commands. Gallium's VCN encoder also has explicit RGB input
packing cases; inspect the VA frontend/capability and reconstruction split before
assuming a complete API replacement is necessary. Native modifier compatibility,
correct conversion, retained-source lifetime, static ownership and overlap must
be demonstrated. Removing one conversion image could lower the conditional image
target to 87.890625 MiB, but a longer source lease or hidden driver allocation can
erase that gain. The prototype described above is implemented; recorder
integration and whole-pipeline evaluation remain open.

GPU retention continuation: the command-hint turn made verified progress; its
160-file implementation manifest passes and no benchmark/profile process remains.
The fixed source/executable is saved in `/tmp/gpukey-before-capacity`, with new
evidence in `/tmp/gpukey-capacity`. Trace the higher long-run VRAM/GTT peak before
choosing a pool policy. Distinguish capacity, active work and retained high-water
allocation. Codec settings, accepted frames, static last-image ownership, source
retirement and two-deep overlap remain intact; forcing serial encoding is a
comparison control, not a way to claim the same throughput at less memory.

Verified source context: gpuKey retains its bounded input AVFrames; VAAPI backing
can materialize only on first use. FFmpeg separately pools coded-output objects.
The event loop collects completed output after waiting, while preparation chooses
the first input whose codec reference has retired. Investigate whether extra
capacity reflects genuine overlap, completion/readout ordering, or avoidable
retention. Diagnostic tracing is separate from performance measurements. Releasing
idle capacity, collecting before new materialization, and changing pool ownership
are open alternatives with allocation/resume-latency costs, not selected designs.

Diagnostic evidence: an ordinary traced forty-second run uses one input and one
coded buffer. An explicit fifty-millisecond media-owner delay after completion
120 creates a backlog; frame 122 uses another input and creates another
22,183,936-byte coded buffer. Input reuse returns to the original image on frame
123, but both extra allocations survive until close. After three seconds the
fixed control reports 114.379 MiB VRAM and 44.504 MiB GTT throughout the remaining
samples. This is retained capacity after real overlap, not an arithmetic guess.

The default packages now expose opt-in `--gpu-memory compact`, preserving the
ordinary retain policy as the default. It trims unleased FFmpeg pool
entries without new per-entry fields. A temporary pool reference spans free
callbacks, including reentrant owner destruction. VAAPI keeps one idle coded
buffer, reports completion-FD closure through a read-only generation, and the
event-loop owner clears cached watches before polling changed generations.
Input trimming waits until all submitted output has been collected and preserves
the last image plus any prepared-but-unsubmitted image. Handles can be restored
when overlap returns; allocation and resume latency must be measured. The first
version freed reusable inputs while other work remained active; the idle-only
guard is the revised control. Both traced versions return to 103.129/23.348 MiB
after the stall. Sampled maxima cannot establish a lower instantaneous peak.

The actual generic pool sources pass Clang ASan/UBSan and TSan with independent
lifetime checks, held clones, allocation failure, concurrent reuse and reentrant
destruction. Removing the temporary pin fails the early-owner-close oracle.
GCC exposed a multiple-declarator constexpr diagnostic in the new test harness;
the declarations are now separate and GCC passes. Both buffer and RefStruct
negative controls independently expose the missing lifetime pin. These checks
do not instrument all FFmpeg or the driver. Diagnostic tracing/fault injection
is confined to a private recorder child and is not a performance measurement.

A stronger implemented challenger enforces the single-idle-object policy during
the existing RefStruct return operation. It detaches the older cached object
under the existing mutex, then destroys it outside that mutex, using the
returning lease's existing pool reference as its lifetime pin. This removes the
separate per-output trim lock and adds no per-entry fields. ASan/UBSan and TSan
cover concurrent reuse and a destructor that uninitializes the owner and destroys
the just-returned object's storage in place. Input records now form a dense live
array, materialized on demand in compact mode instead of allocating every frame
record up to the configured limit. Native Clang sanitizer checks pass at both
depths, with prepared input protected during collection, retained images readable
after codec/device-owner teardown, and configured capacities through one million.

The first native timing matrix is mixed. A memory-observer PermissionError on a
startup fdinfo read invalidated one run; that run and failure log are retained.
Unavailable reads are now counted rather than converted into fake zero-use
clients. Existing runs were verified before the missing cases resumed, using
unchanged recorder/fixture/driver hashes and the same workload settings. Future
resumes also require a recorded workload description. The completed eighty-run
four-way confirmation rotates every variant through every ordering position,
with builds/profilers kept outside timing. CPU, RSS and latency remain mixed;
all runs have zero recorder skips. Retain remains the default. Compact uses the
single-idle return policy; explicit trimming remains a runnable comparison.

Input-admission failure exposed an independent output-lifecycle defect: the
global error latch discarded a valid queued packet during finalization. Errors
from unsubmitted input preparation now stop admission while permitting previously
accepted video and audio to drain; codec/output errors still stop unsafe writes.
The first error remains the exit result. Eight container/audio/failure cases pass
ASan/UBSan with independently decoded pictures and a separately initialized
resampler oracle. The original media implementation fails the accepted-prefix
check. The initial raw-step audio expectation was invalid because forced
resampling filters the transition; that failed oracle and its artifact are kept.
A native injected input-restoration failure preserves all eleven accepted
pictures, independently decodes them, and exits with the allocation error.

Keep a stronger coded-output alternative open before selecting an architecture:
the audited VAAPI output objects never escape the serial codec owner. A flat
arena of eight-byte VA-ID/FD records with stable indices and a parallel free-index
stack could remove generic pool locks and refcount objects. The non-independent
frame path can transfer its output owner to the saved-prefix slot instead of
duplicating ownership; error and close paths must preserve this lifetime. Dynamic
arena growth must not invalidate picture references. This alternative is not
implemented or rejected. The generic trim implementation is a control to beat,
not proof of optimality. Seven repeated induced backlogs complete with two replay
saves. Final GCC/Clang, application sanitizers and all sixteen x86-64 checks pass,
as do native compact capture/audio/replay integration and actual Samply/Tracy
captures. The final forty-second run submits and encodes all 2,400 frames, saves
both late exports, and has no recorder skips, producer misses or completion
fallback. Whole-child CPU is 420.770 ms, RSS 44.359 MiB, sampled VRAM/GTT peaks
103.434/23.348 MiB, and the source-lease maximum is 0.638 ms. This is one long
confirmation, not an estimate of variance or a universal peak bound. Evidence is
indexed in `docs/evidence/capacity-index.txt`. The final failure-path refinement
postdates the eighty-run comparison and must be distinguished from those measured
binaries. The broader optimization goal and conditional memory targets stay active.

Command-resource lookup continuation: the header milestone is verified and its
manifest passes. The fixed source/executable is saved in `/tmp/gpukey-before-hints`,
with new evidence in `/tmp/gpukey-hints`. The source audit finds that the frontend
and asynchronous submission worker can both update the same 64 KiB hint array:
the worker resolves slab/sparse backing buffers while the frontend records the
other context. A reset optimization must first establish table ownership. Test
the shared original against independently owned fixed tables, then compact
growing hints and an exact-index-map challenger. Preserve all three buffer kinds,
collisions, large indices, allocation failure, paused workers and teardown.

Initial implementation choice: keep each hint cache with its command context,
whose ownership transfers through the existing submission queue/completion fence.
Start small, rehash from owned buffer lists when growing, retain the old table on
optional allocation failure, and keep linear lookup as the correctness fallback.
The original maximum hash-table size remains a cache policy, not a BO-count limit.
Two full tables can cost more at large sizes; an alternative that resolves slab
backing indices before handoff could instead remove worker cache access. That
alternative changes refcount/fence bookkeeping and stage timing and remains open,
not rejected. No new driver is selected before correctness/performance evidence.

Continuation audit, 23 October: the preceding status-only turn did not change
the implementation. All four ordered driver builds are now present and pass
native color, odd-chroma, concurrent encoder, graphics and in-flight teardown
checks. Fresh eight-/twenty-second rotating comparisons are running against the
unchanged driver. Keep the first, mixed comparison rather than replacing it.

The original shared table fails an extracted-code paused-worker membership test
and an extracted-code ThreadSanitizer test. Independently owned fixed tables,
adaptive hints, exact per-list index maps and atomic shared hints with an
untrusted-negative worker path pass those controls. These are tests of actual
extracted helper functions in a lifecycle model, not a sanitizer run of all Mesa.
The initial C11 thread harness crashed the TSan runtime; the corrected harness
uses intercepted pthread creation, matching the dependency's native runtime.

The queue mutex publishes each submitted context. The pinned futex completion
wait can return through a relaxed load, so each candidate now places an acquire
fence after the wait before reusing worker-written lists/cache state. Existing
buffer references, GPU fences and exclusive context ownership remain the lifetime
mechanism. Hints never protect a BO. Destruction waits for the worker before
releasing either context. Optional cache-allocation failure retains exact lookup;
it does not fabricate successful submission or impose a new BO-count limit.

Open choice: adaptive bounded hints versus exact growing maps versus the fixed
and shared controls. The first native comparison shows small/mixed CPU and RSS
differences, not a universal winner. Add a complete-batch extracted-core comparison
covering small/large lists, cross-list and low-bit collisions, allocation, reset
and teardown; keep its artificial BO layout distinct from native results. No
measurement during this comparison includes concurrent builds or profiling.

Selection after confirmation: adaptive hints are the provisional compact default;
the exact, fixed, shared and unchanged-driver profiles remain runnable. The
complete-batch microbenchmark favors adaptive over exact on collision-free lists
of 32–4,096 objects, while exact maps win heavily colliding and larger lists.
The exact map's larger-list storage is a real tradeoff, not an invalid input.
At 33,024 entries in each context the two map payloads total 512 KiB versus
128 KiB for adaptive hints. The native three-run confirmation has lower CPU
means for adaptive in all five cases, but wide variation and the mixed first
matrix do not establish universal improvement. RSS and tails are also mixed.
No table variant changes the VRAM image representation or codec settings.
The selected driver has passed 576 raw VPP transition comparisons and both-depth
decoded layout comparisons against the unchanged driver. All 15 x86-64 Nix
checks, GCC/Clang, application and native sanitizers, actual samply/Tracy,
packet equivalence and long replay/eviction checks pass. The final forty-second
run submits all 2,400 frames, saves both late exports and reports no recorder
skips or completion fallback. Its sampled VRAM peak is 114.379 MiB, unlike the
103.434 MiB paced confirmation cases; most long-run samples are 103.121 MiB.
This is consistent with spare input/output capacity becoming backed late in
the run, not a measured table-layout VRAM change. Trace live versus retained
high-water allocations next. Also preserve the independently found, pre-existing
slab/sparse essential-list allocation-failure defects as follow-up work; optional
cache-failure tests do not prove those paths. Evidence is indexed under
`docs/evidence/hints-index.txt`; the goal remains active.

Native header continuation: the conversion milestone is verified and archived;
its current source manifest passes. The fixed source/executable is saved under
`/tmp/gpukey-before-headers`, with probes under `/tmp/gpukey-headers`. Native
profiles still show distributed submission/allocation/header costs, and the
saturated source-lease comparison did not establish nonregression. Audit those
costs before changing source lifetime or queue depth.

Verified source observation: VAAPI AV1 enables a per-field tracing callback to
locate rate-control bit offsets, although its CQP path never consumes those
offsets. It also serializes one OBU through an intermediate write buffer, a
refcounted unit buffer and a separately assembled fragment before copying into
the caller's bounded packed-header buffer. Compare the unchanged control, CQP
trace elimination, and caller-owned one-OBU serialization using the same syntax
writer. Preserve reference-state updates, non-CQP offsets, metadata, B-frame
repeat headers, failure/close behavior and accepted header capacity. No winner
was selected before comparison. The direct writer now passes exact packed-header,
rate-control-offset and packet comparisons for CQP/QVBR at both depths, including
MDCV-only, CLL-only and combined metadata. Independent CBC/decoder probes also
cover hidden/show-existing frames, short buffers, allocation failure, teardown
and immediate invalidation of borrowed raw input; GCC and Clang ASan/UBSan pass.

The final compact variant removes persistent raw sequence/frame/metadata OBUs.
Only coded header bytes and 32 bytes of metadata payload/flags persist; temporary
raw OBUs are local to their synchronous consumer. The first three-way timing
matrix is mixed. Longer confirmation lowers every CPU mean (roughly 1–7%), with
no established throughput/lease improvement and mixed process RSS. The exact
persistent-state reduction and eliminated serialization allocations are separate
claims from those measurements. Default packages now select this variant for
final verification; `mapping`, `amd-headers-reference` and `header-trace` retain
the fixed library and simpler challenger. Whole-project verification, all 14
x86-64 Nix checks, native sanitizers, fresh samply/Tracy captures and the final
40-second replay/eviction run now pass. The packaged executable is byte-identical
to the compact candidate used for confirmation. The forty-second run submits
2,400 frames with no skips or completion fallback and keeps VRAM at 103.434 MiB.

The header milestone identified the 64 KiB buffer-index hint table as its next
native audit; the current continuation above records the ownership fix and
measured alternatives. Hints remain indices, not lifetime protection. Omitting
the CBS rollback snapshot only in the explicitly terminal-failure writer is
still an unmeasured alternative.

Keep a stronger static-scene challenger open: certify an actual independent key
and its first predicted frame as a closed repeatable pair, rather than assuming
that an arbitrary encoded frame can be repeated. A candidate requires unchanged
admitted source content, CQP eligibility, parsed reference/refresh masks that keep
the predicted frame's dependencies intact, and compatible frame-ID/order-hint,
CDF, grain and timing state. Repeating a key alone could increase bitrate; an
eligible key/predicted pair might preserve the normal cadence while avoiding
re-encoding. On movement, the hardware encoder and decoder must be synchronized:
one possible transition is a hidden form of the seed predicted frame before the
new visible picture when the decoder last saw only the cached key. This requires
actual bitstream/decoder proof, not byte similarity. Seed owners must count toward
RAM budgets and survive exports; freeing idle GPU inputs additionally requires
proven encoder completion and pool ownership. Startup, resume latency, added
decode/packet work, quality and bitrate may lose in some regimes. This is an
untested proposal with unresolved prior art and integration, not a feature or a
claim of novelty. The existing two-second key/one-second repeat policies remain
revisable guarantees, not intrinsic codec requirements.

Conversion continuation: the workspace turn made verified progress, and its
source manifest passes. The fixed source/executable is saved under
`/tmp/gpukey-before-conversion`, with probes under `/tmp/gpukey-conversion`.
The prior CPU-input profile places about 90% of main-thread CPU in swscale.
Audit the complete conversion/upload path against the material's explicit
BT.709 limited-range, horizontal-left/vertical-center chroma contract before
selecting a faster path. Initial candidates are the current legacy scaler,
full-resolution chroma input, the modern frame API, a fused CPU conversion,
and RGB upload followed by GPU processing. Exact rounding/filter equivalence
is not assumed. The native direct-RGB alternative remains open under its
separate eligibility, color and producer-lifetime obligations.
Source inspection finds that SWS_FULL_CHR_H_INT controls RGB output, while this
path supplies RGB input. Legacy swscale may therefore halve chroma horizontally
before position-aware filtering and select scalar chroma routines. An impulse
and raw-output probe compares semantics before an end-to-end performance choice.
The integrated candidate filters RGB before applying the matrix, retaining four
small planar rows instead of full intermediates. Packed unsigned lanes preserve
exact filter sums; the fixed Q18 matrix and final rounding are bounded within
0.505 output code values of the independent double-precision oracle, including
ten-bit output. The per-plan scratch is 24*round_up(ceil(width/2),32) bytes.
Runtime x86 multiversioning retains a portable implementation and requires no
global native-ISA build. The old scaler remains a comparison build; correcting
its input flag fixes even-width positioning but does not fix its odd-size grid.
Independent matrix/filter checks cover 208 format/geometry combinations. A
lossless AV1 integration regression checks moving one-pixel chroma impulses at
64x48 and 63x47. GCC/Clang, sanitizers and all 14 x86-64 Nix checks pass for this
conversion implementation. Final native ASan/UBSan integration, packaged
performance comparisons and real samply/Tracy captures also pass.

The initial three-run upload/encode matrix shows substantial CPU reductions at
large sizes, but saturated 320x240 throughput is lower despite less CPU. The
fresh packaged comparison confirms large-size gains; a separate same-build
comparison against corrected swscale also wins on CPU, while small-image
throughput remains indistinguishable within observed variation. The decoded-quality
oracle found a separate pre-existing driver geometry bug: 641x481 is encoded
as 648x482 by all three binaries. This is a product failure, not grounds to crop
the oracle or narrow permitted inputs. Mesa writes its allocation-aligned size
into the in-band AV1 sequence/frame headers. The `mesa-geometry` candidate keeps
VCN 5's firmware alignment/padding while writing the requested dimensions. This
candidate was rejected: an unchanged MI grid is insufficient, because predicted
frames accumulate edge errors when coded picture dimensions change. The patch
and negative evidence are retained under the experiment directory; it is removed
from the runnable packages. Its workspace allocation layout is retained in the
selected driver, with the separate sampling-grid correction described below.

The replacement preserves the valid padded bitstream and uses Matroska PixelCrop
or MP4 clean-aperture metadata for presentation. It extracts the actual first
sequence header and verifies geometry/color before publishing the mux header.
A first-output option prevents asynchronous FIFO fill from delaying startup
configuration in buffered/Vulkan modes; ordinary VAAPI readiness stays asynchronous.
The first 641x481 probes now decode/present the source correctly with low error
through inter frames. mpv reports a 641x481 crop and exports 641x481 images.
Brave's tested headless software path advances frames/colors but reports the
padded dimensions; browser interoperability remains explicitly unresolved.
The default browser path and a WebM remux reproduce that crop limitation. mpv
honors the crop. Known replay duration is now written before export begins;
Brave then holds static replay pictures through the exact saved endpoint.
An opt-in Matroska muxer option writes actual video BlockDuration without the
old fictitious 1000 FPS DefaultDuration. The one-shot AV1 parser can borrow
verified, zero-padded immutable packets, avoiding a full key-payload copy; the
caller retains them through parser close, with copying retained for other inputs.

The native odd-size audit also found ratio-derived chroma spacing in the general
GPU conversion path. The `mesa-chroma` candidate extends the paired kernel to
guarded partial tiles, uses logical source bounds, and separates rounded output
coverage from exact 2x chroma spacing in the unit-scale NV12/P010 fallback. Its
independent raw oracle passes four RGB formats, both depths, direct/imported
surfaces and partial tiles; all 576 existing even-size transition outputs remain
byte-identical. Decoded odd-size chroma error falls sharply. Shader creation and
discard-map failure/retry tests pass with the corrected fallback. The AMD profile
now selects this driver; `amd-chroma-reference` retains the workspace-only driver.
Final packages, native sanitizers, Vulkan replay, odd-size replay with two audio
tracks, and fresh profiling pass. The three-run packaged CPU-input comparison
reduces whole-child CPU by 57.9% at 1080p and 53.1% at 2560x2880. The corrected
swscale challenger is also measured with identical application/compiler settings.
Native DMA-BUF recording retains 103.434 MiB VRAM at portrait/60; its throughput
and source-lease results are mixed, not a demonstrated all-metric improvement.
The independent decoded oracle reports separate channel errors and payload sizes;
one ten-bit chroma channel loses about 0.6 dB on the deterministic pattern.
These measured limits and the browser crop restriction remain visible.

Next probes: compare RGB upload plus GPU processing against the now-cheaper CPU
path, investigate native submission/header-generation overhead and saturated
source-lease tails, and continue the conditional memory targets in
`docs/memory-targets.md`. No prototype or result here rules out those alternatives.

Workspace continuation: the lazy/checked-transfer turn made verified progress;
the starting manifest passes. The fixed source/executable is saved under
`/tmp/gpukey-before-workspace`, with new evidence under `/tmp/gpukey-workspace`.
The current control keeps three singleton AV1 tier-2 workspaces in larger slab
classes. Candidate `mesa-workspace` allocates the complete SDB, CDF and session
payloads separately without suballocation, retaining their existing minimum
virtual-address alignment. Candidate `mesa-workspace-packed` shares a 256 KiB
CDF/session resource with a 128 KiB-aligned session offset and independently owned
references. Its extra padding/CPU visibility trade against one fewer BO. Other
codecs and legacy DPB paths retain their existing allocation policy. No bitstream
capacity, references, dimensions, quality or firmware payload is reduced.

Source audit also found an unchecked auxiliary-frame-buffer allocation during
VCN command construction. Required storage must be validated before command
emission; an initialized session must remain owned until its close operation.
The negative control reproduces exit 139 after an injected FCB allocation failure.
The fix prepares all tier-2 auxiliary storage after valid session initialization
but before picture-command construction, reuses the existing context-count field,
and preserves partial/live resources for the normal destructor. Both layouts now
pass CDF allocation/map, SDB, session (when separate), and FCB failures across
eight failed/healthy cycles per case with balanced descriptor counts. Both also
pass nominal 8/10-bit color checks.
No undocumented firmware command or reduced address alignment is used.
The first three-run matrix reduces portrait/60 VRAM from 105.324 to 103.434 MiB
with dedicated buffers and 103.480 MiB with the packed pair. CPU/throughput/tail
results are mixed. The next challenger packs all three workspaces into one
2 MiB-aligned BO, retaining large-page eligibility. It computes full SDB size
before choosing the layout and falls back to separate SDB + packed pair when
that size exceeds the reserved prefix. Both shared layouts retain owned aliases
through session close. All four drivers pass concurrent independent 8/10-bit
streams; H.264 I/P/B, HEVC I/P and AV1 CQP/QVBR decoded hashes match. A first HEVC
test at 320×240 was below the device's 384-pixel width minimum; the corrected
matched workload is 640×480. The whole layout also passes repeated workspace
allocation, CDF-map and FCB failure/recovery cycles. A fresh rotating-order
matrix compares the control, dedicated buffers and the whole layout.
The separate twenty-second saturation confirmation reverses the earlier ranking:
dedicated/control/whole means are 312.58/309.62/306.64 FPS. This does not establish
a throughput winner. Dedicated buffers are selected for their repeatable fixed
storage reduction; the original, packed-pair and whole layouts remain runnable
as `amd-workspace-reference`, `amd-workspace-packed` and `amd-workspace-whole`.
The `amd` package now selects dedicated buffers and the checked failure path.
Final formatting, GCC/Clang/sanitizer checks, all 14 x86-64 Nix checks, native
sanitized integration and fresh samply/Tracy captures pass. The final executable
is byte-identical to the fixed measured recorder; the AMD wrapper selects the
verified driver. The stricter codec oracle corrected a false HEVC B-frame
assumption: this device/driver substitutes I/P, now explicitly tested/reported.
H.264 still tests B-frame order. The final packaged 40-second portrait/60 replay
submits all 2,400 frames without skips/source misses and independently decodes
two late exports after window eviction. Peak VRAM/GTT are 103.434/23.348 MiB.
Evidence, immutable controls and reproduction tools are linked in validation.
This continuation makes verified progress; the broad all-metric goal remains
active. Next consequential costs include native command/header bookkeeping,
CPU-input conversion, and representation changes that can remove full images;
quality/compression, power/game interference and other vendors remain required
evaluation dimensions, not assumed wins.

Static/allocation continuation: the completed-output turn made verified progress.
The starting source manifest passes, and its source/executable control is saved
under `/tmp/gpukey-before-static`. Current exploratory evidence is under
`/tmp/gpukey-static-audit`. The verified lazy/checked mapping implementation is
selected by the default and AMD packages. `amd-readout-reference` retains the
previous readout build as a comparison control.

The paper's sections 8.3 and 9.7 retain native repeated presentation beside
ordinary encoding. Source C19's actual AV1 semantics require a showable reference,
and a keyframe can be shown by show-existing at most once. Shown keys are not
showable. With the current one-second repeat/two-second random-access policy,
ordinary P input is needed after each shown key before useful inter repeats.
This limits a show-existing shortcut under the current default; it does not
reject that family for shorter repeat intervals, wider key spacing or other
backends. The VAAPI API documents application-generated short headers and skipped
frame accounting, while the inspected VCN AV1 path exposes ordinary I/P types,
not its H.264/H.265 P-skip mapping. No arbitrary encoded packet replay is added.

Allocation stacks identify an avoidable consumer mismatch: FFmpeg eagerly derives
CPU images while initializing both the input and reconstruction pools, even for
a GPU-only recording. A diagnostic restricted to the immutable control library's
exact init call site suppresses only these two probes. It reduces sampled GTT
from 26.86328125 to 23.34765625 MiB on portrait/60, leaves steady VRAM essentially
unchanged, and preserves all 120 independently decoded comparison frames. This
interposer is a diagnostic, not the implementation.

The `ffmpeg-mapping`/`mapping` implementation moves derivation to actual CPU map
requests. They remove the throwaway init image. A single atomic policy hint
prefers copied writes after a capability failure, avoiding repeated failed
derivations on every upload. It never rejects an explicit direct map of another
surface, and it publishes no data/ownership; relaxed atomic operations suffice.
Direct and copied maps retain the real surface's lifetime and flags.
Successful incompatible images must be destroyed before fallback; failed teardown
must retain its handle for cleanup. Default read maps retain the existing copied
path for CPU cache behavior. The cacheless candidate is preserved separately;
it would retry unsupported derivation for each upload, while the selected hint
learns that policy once. Both supported and fallback paths now pass NV12/P010
transfer/direct-map oracles at 320x240 and odd 319x241, with poisoned readback
destinations, concurrent maps and retained mapping lifetime. Faults cover
unsupported and wrong-format derivation, direct-map rejection/recovery and failed
image destruction with cleanup retry. Native capture and CPU-input controls,
full compiler verification, matched lifecycle comparisons and fresh samply/Tracy
captures pass; their scopes and mixed performance results are recorded in
`docs/validation.md`. Memory sampling now also records post-exit
child resource usage, covering final reporting/cleanup beyond the recorder's
near-exit counters; the summary records which CPU measurement scope was used.

The mapping audit also found a checked-transfer problem: the original FFmpeg
upload returns success after logging unmap, writeback or image-release errors.
A native failure probe reproduces all three false-success returns. The selected
implementation separates persistent public maps from synchronous transfers. Transfers
borrow their caller's frame/context lifetime and use stack plane views, removing
the temporary AVFrame, HWMapDescriptor, source clone and buffer-reference graph.
Their cleanup returns status to the transfer caller; a failed unmap suppresses
writeback, and failed pixel-copy validation cancels copied writeback. Persistent
maps retain their owned descriptors and source references. Fallible metadata
copy precedes writable-map construction. Frame objects/references remain intact
on failure; pixel storage is not a rollback transaction (the pinned CUDA path
also permits earlier plane copies before a later failure). The recorder treats a
failed preparation as terminal and never submits or repeats that destination.
The negative control confirms the original path returns zero for all three
injected upload failures despite logging them. The new transfer regression also
checks late download failures leave a clean destination frame unpublished.
This distinguishes frame-object ownership from device-byte rollback; no new
rollback guarantee or extra image backup is silently added.
The metadata regression caught an ordering mistake in the first constructor
refactor: copying properties before setting the mapped display dimensions drops
size-dependent pan-scan data. The candidate now sets the source's actual display
extent before property copying, matching the current primary FFmpeg implementation
instead of exposing allocation padding. The regression includes cropped display
dimensions, correct full allocation capacity, metadata and a poisoned pixel oracle.

Next dominant costs are now better identified. CPU-upload sampling places about
90% of main-thread CPU in swscale, especially scalar chroma conversion/filtering;
borrowed transfer bookkeeping is a smaller cost. Compare complete conversion/
upload alternatives with identical color/siting, checked writeback and lifetime
rules, including their extra raw-byte traffic. The GPU allocation trace also
shows sparse media workspace classes: the AV1 SDB payload is 947,200 bytes,
default CDF 81,920 bytes, session storage 128 KiB, and per-reconstruction frame
context 25,088 bytes, with larger backing allocations in the current slab policy.
The dedicated/shared workspace probes are now evaluated above, including
alignment, large-page/TLB, concurrent-context and failure costs. They do not
justify shrinking firmware payloads or output reservations. Native
RGB and producer-native YUV remain larger representation alternatives. The broad
all-metric goal is still active.

Packet-latency continuation: the previous VPP turn made verified progress, and
the starting source manifest passes. Save the fixed control under
`/tmp/gpukey-before-packet-latency`. A public-API probe submits one AV1 image and
waits for Mesa's actual encoding fence. With depth two, an immediate replay save
still returns EAGAIN after GPU completion (~3 ms in the probe); depth one can save.
The pinned FFmpeg path requires another input or drain before reading a partially
filled asynchronous queue. The real recorder's next static repeat is normally a
second later. Reducing depth would trade parallelism and memory against latency;
test completion-aware readout while retaining depth instead.

Selected design: an opt-in FFmpeg VAAPI option checks only the oldest issued
picture when the input FIFO has capacity. A full FIFO retains the blocking path,
preserving send/receive progress; readiness is monotonic and no extra input or
end-of-stream is fabricated. The existing completion/output path owns retirement.
A save trigger collects ready output before testing replay availability. No
polling timer or worker is added. Persistent dma-buf completion notifications are
now integrated through a tested process-local Mesa extension and stable owner-only
GLib watches. Unsupported export falls back to demand readout; explicit controls
retain buffered, demand and notification policies. Watches are retired before
their codec descriptors close, and inactive descriptor errors terminate the wait
instead of spinning. The AMD package now selects this path; the full original
FFmpeg library and previous VPP driver remain runnable comparison profiles.
The public probe now saves the first GPU-complete frame at depth two; the
buffered control still returns EAGAIN. A coded-buffer export prototype also
passes dma-buf poll readiness plus an authoritative zero-timeout VA completion
check. The VA export lock must be released before mapping; retaining an owned
FD duplicate preserves the signal without violating that API exclusion. Real
static capture has four completion wakes for four encodes, with no fallback,
and saves at approximately 174 and 374 ms before the first static repeat.

Resource audit found two separate effects: coded buffers can be allocated with
the sharing flag before export, avoiding private-to-shareable reallocation; and
the rebuilt FFmpeg library maps about 10 MiB through file PMDs where the cached
reference used small pages. Its configuration feature set is unchanged. That
RSS increase is not attributed to the queue/FD records. Investigate the actual
library footprint and unused dependencies rather than hiding it in comparisons.
The recorder-specific FFmpeg build removes unused codec/network/filter families
while retaining all four AV1 backends, Opus/PCM, required parsers, both containers
and independent decoders. Full ffmpeg/ffprobe tools remain separate dependencies.
Its public registry probe passes. VAAPI input selection now reuses any completed,
uniquely owned input, including the last image; spare slots remain capacity and
are backed only when overlapping work needs them. Preparation failure remains
terminal. An exploratory portrait/60 FPS run submits all 240 frames with no
notification fallback, at 110,444,544 bytes VRAM, 28,168,192 bytes GTT and
40,016 KiB peak RSS. A four-run fixed-reference comparison confirms the memory
reduction at portrait/60, with about 16 MiB less peak RSS across the tested cases.
Saturated cases retain two buffers; one saturated 1080p series is about 1.9%
slower, while a three-policy isolation series does not establish a notification-
specific throughput penalty. All readout controls remain available. No weighted
score or universal superiority is claimed.

Verified gates for this extension: early replay saves at depth two, independent
input pin/reuse, 48 native repeated empty/pending/collected/fallback/EMFILE
lifecycles without descriptor leaks, inactive/error/fd-reuse portal behavior,
native ASan/UBSan integration, full GCC/Clang and 14-check x86-64 Nix verification,
Vulkan replay with two audio tracks and both sinks, 576 raw conversion transitions,
and matching first 120 complete decoded frames of live portrait clips. The AV1
settings and pixel contract are preserved. Public submit-to-packet retrieval is
about 2 ms instead of one second in the static four-packet probe; this is distinct
from capture time, GPU execution time and VFR file-write availability.

Explicit memory-goal revision: queue capacity does not force distinct backing.
For 2560×2880 8-bit AV1, two capture images, two past references and one reused
input give a **98.4375 MiB** conditional image-payload target, including
**42.1875 MiB** private images. Two simultaneously materialized inputs require
**108.984375 MiB** total image payload. Coded output has a separate **21.15625 MiB**
one-buffer reservation target under the pinned FFmpeg sizing rule; it is not a
codec lower bound. Native CPU raw-frame copies remain targeted at zero. Replay
payload stays content/bitrate/history dependent. These targets, assumptions,
stronger direct-RGB/producer-YUV alternatives and actual gaps live in
`docs/memory-targets.md` and its calculator; the broad optimization goal remains
active. Longer lifetimes, packaged-build confirmation and fresh profiler evidence
are recorded with the measured results in `docs/validation.md`.

Pipeline continuation: the previous replay-sharing turn made verified progress;
the current source manifest passes and its executable/source control is preserved
in `/tmp/gpukey-before-fused`. Samply places conversion and encoder submission
ahead of replay retention in CPU cost. The paper keeps direct RGB beside VPP as
a first-class candidate. Audited Mesa 26.2.5 also exposes encoder-side processing
through VAProcPipelineParameterBuffer, with native VCN format conversion when
eligible and shader fallback otherwise. Probe that mechanism before changing the
recorder interface. Preserve AV1 bit depth, transfer, BT.709 limited output,
chroma location and exact source ownership. DCC, source lease length, static
repeats, fallback allocations and hardware conversion precision are unresolved
eligibility/cost questions. Encoder-side conversion remains a diagnostic probe;
the verified compute improvement is now integrated in the optional AMD package.
The first immutable-source experiment confirms native encoder conversion at
8-bit. Linear and the tested GFX12 modifier give identical decoded results;
the source's generic meta_offset guard is not a blanket rejection of every DCC
modifier. `AMD_DEBUG=noefc` restores the separate-VPP result exactly. Native
conversion changes rounding (including white 235 to 234) and chroma filtering;
neither family is declared universally better. Eight-bit RGB to ten-bit output
uses the shader fallback. The Q=0 probe exposed the pinned FFmpeg AV1 VAAPI
transform-mode limitation; Q=1 probes are explicitly near-lossless, not exact
color oracles. Pre-encode raw readback is now available for compute comparisons
and is forbidden with the diagnostic encoder-processing interposer.

In parallel, the VPP source audit found per-frame sampler-view allocation and
two separate plane dispatches. Optional `mesa-views` and `mesa-vpp` candidates
compare source-owned views and a single 2x2-tile compute dispatch. The latter
preserves the existing four chroma samples/addition order, uses a 64-byte cached
constant buffer, and retains the general path for transforms/blending/scaling
or unsupported formats. Its shader and constant buffer have independent cleanup
because the general shader family need not be initialized. These remain
conditional optimizations verified by raw-image, fallback, lifetime and complete-
pipeline checks. `amd` now uses the paired kernel; `amd-reference` retains the
previous ACO profile. The ordinary system-driver build is unchanged.
The paired kernel's pre-codec bytes and independently decoded patterns match
the control. The extended VPP probe covers 576 outputs across four RGB layouts,
both output depths, boundary dimensions, changed content/range/matrix/chroma
siting, rotation, crop/scale, alpha blending, and transfer/primaries changes.
Fault-only builds exercise a failed discard-map and failed shader construction
with successful fallback/retry and matching later images. Source-owned view
reuse is restricted to matching contexts; other callers retain temporary views.
The delivered profile also cleans partial general-shader initialization and
invalidates cached constants before a potentially destructive map operation.

An initial five-run series shows lower RAM and source-release time, but saturated
portrait throughput varies. Fresh six-run and final packaged four-run series
retain that limitation. At 2560×2880/60 FPS, the final package averages 54.352 MiB
RSS versus 56.492, and source lease averages 0.177 versus 0.269 ms. Sampled VRAM
falls by 64 KiB to 116.586 MiB; image-payload targets are unchanged. Saturated
throughput and some saturated lease tails are not improved. The first
two-variant launcher combined rotation and reversal, inadvertently leaving the
control first; it was stopped and excluded. The corrected launcher alternates
two variants and a six-repetition confirmation balances their first position.
No benchmark ordering defect is treated as evidence for the candidate. The final
package completes 9,600 frames in a 40-second 1080p/240 FPS run, with no misses
or skips and two late exports. GCC/Clang, the full Nix matrix, native ASan/UBSan,
scalar/raw/decoded comparisons, fault recovery and graphics teardown pass.
Fresh samply and Tracy captures are saved. Larger remaining work includes
encoder submission/output-readiness latency, the remaining justified VRAM/GTT
allocations, sparse replay metadata and complete direct-input integration.
Energy, game interference and other GPU vendors remain unverified; the full
all-metrics goal is active rather than declared achieved.

Replay-RAM continuation: the prior goal turn made verified progress; preserve
its executable/source control in `/tmp/gpukey-before-retention-layout` and
`docs/evidence/pre-retention-layout.sha256`. Inspecting actual encoded packets
found nine identical 1,764,416-byte keyframes in the user's earlier clip, so
14,115,328 bytes are repeated key payload. This motivates exact key-payload
sharing, not a change to encoding, presentation, or random-access semantics.

Accepted mechanism: a cold shared-payload descriptor owns the existing CPU
buffer; packet records keep their 32-byte shape and original timing. Separate
owner-only live/owned reference counts charge each payload once across live
history and immutable export snapshots. Keyframe byte equality is checked
against the preceding verified key, whose block remains live; unequal keys keep
their normal storage. The original reference-adoption and all-copy modes remain
controls. No worker may observe changed packet records or a reclaimed payload.
Checks cover budget edges, oversized references, alternating keys, paused
exports with live eviction, errors, teardown, decoded equality, and measured
CPU/latency costs on both duplicate-heavy and changing inputs. This is a scoped
retention improvement, not a claim of global deduplication optimality.
The first independent confirmation passed byte/timing/flag comparisons for every
export and full decoded audio/video comparisons for each input/variant. A second
confirmation followed a cost audit that replaced the extra eviction packet scan
with the existing key bitmap. The saved pre-change source remains a fixed
control. Exact sharing lowers retained RAM but can increase individual large-key
admission times; the reference mode remains an explicit Pareto alternative.
Three live 40-second static comparisons reduce peak replay charge from 1,811,884
to 377,092 bytes while preserving the 30-second history and both late exports.
Mean RSS falls by 1.306 MiB; matched changing captures still deliver 360 frames
in six seconds with no source misses or recorder skips. The full GCC/Clang,
ASan/UBSan and Nix matrix passes; the sharing lifetime test also passes Clang
ThreadSanitizer. Fresh samply and Tracy evidence is retained. Samply points to
driver conversion and encoding submission as larger CPU costs than retention;
Tracy confirms sleeping portal waits and successful concurrent replay exports.

Next unresolved probes: sparse static history leaves mostly empty 256-record
blocks, so compare smaller/adaptive chunks and packed arenas with full snapshot
and admission accounting. Global interning may share nonconsecutive payloads;
its index, equality and reclamation costs remain unmeasured. The Vulkan copy
path currently copies before key comparison; avoiding duplicate copies needs its
mapped-buffer lifetime checked. Driver conversion/submission work and complete
source-to-packet latency remain larger pipeline questions. These are open
choices, not new user constraints or reasons to discard the verified artifact.

Active ongoing goal (20 October 2026): improve CPU, VRAM, RAM, throughput,
latency/tails, AV1 quality, lifecycle correctness and usability together;
retain measured Pareto alternatives where objectives conflict. The user's
subsequent addition makes calculated memory floors explicit optimization targets.
The calculation, scope and runnable model are in `docs/memory-targets.md` and
`tools/memory-targets.py`. For the reported 2560×2880 8-bit AV1 two-reference
pipeline, two source images, two converted inputs and three reusable
reconstructions contain 108.984375 MiB of tightly packed GPU image payload,
of which 52.734375 MiB is private. Completed-input reuse now targets 98.4375 MiB
even with depth-two capacity when captures do not overlap;
direct RGB without converted inputs would be 87.890625 MiB only if the same
source-buffer count suffices. Alignment, codec/driver scratch and different
reuse constraints are additional, individually accountable costs, not a
promise of those exact process footprints. Host raw-video-copy target: zero.
RAM replay payload target: one copy of actual retained encoded bytes, with
metadata, audio, GTT bitstream reservations and export-pinned history priced
separately. Resolution/codec alone do not determine a content-independent RAM
number. These are active targets; theoretical payload arithmetic does not
establish achieved state-of-the-art behavior or close the goal.
The one-copy packet sum is a control footprint, not an information-theoretic
RAM minimum. Exact payload interning and packed retention arenas remain strong
challengers; compare their CPU, metadata, export, accounting and failure costs
before choosing them. Repeated byte-identical keyframes may share storage
without modifying the output stream; similarity alone never permits substitution.
The stronger producer-native YUV alternative remains an explicit target:
two NV12 source buffers plus three reconstructions contain 14.831543 MiB at
1920×1080 or 52.734375 MiB at 2560×2880. Current Niri offers RGB; changing that
representation and safely retaining source pixels through encoding/repeats is
unimplemented. It must not be ruled out by our present interface. The targets
assume GPU-local full-image storage; trading VRAM for GTT is a separate decision.

VRAM follow-up: audit capture allocations/imports, owned conversion inputs,
encoder reconstructions and bitstream storage together; reduce actual allocation
without changing source resolution, AV1 quality, reference structure, or safe
reuse. Save a fixed control in `/tmp/gpukey-before-vram-fix`; retain its manifest
in `docs/evidence/pre-vram-implementation.sha256`. No changes are requested to
session conflict handling; the user resolved the previous duplicate process.

Verified live context: the supplied comparison runs GPU Screen Recorder on
1920×1080 DP-1 with HEVC. An older gpuKey log reports 2560×2880 AV1, but that
does not establish the current session's dimensions: its exact live counters
match our 1920×1080 control. The earlier resolution-mismatch explanation was
corrected. Carry both resolutions in the target model and matched tests.
DRM fdinfo for gpuKey reports 125244 KiB VRAM, including 69120 KiB shared;
duplicate FDs with the same DRM client ID must not be summed. Shared imports
still occupy real memory, but are not additional copies in every importing
process. Their lifetime and producer allocation remain part of the audit.

Accepted application mechanisms: allocate VAAPI inputs once in the bounded
owner pool, reuse the library's probe surface, and size that pool from encoding
depth. Fresh VAAPI buffers mark their surface-ID bytes read-only; GPU reuse
therefore tests sole buffer ownership after the audited encoder completion,
not CPU writability of the handle. A failed prototype exposed this distinction
through independent decoding before promotion. Depth one permits one in-place
input only after prior reads retire; depth two remains the throughput control.
Source negotiation ranks exact small counts before a compatible range, retaining
producers that need more buffers. Ring capacity is independent of actual source
count. Tests cover a producer requiring three and an explicit three-buffer limit.

The Mesa VA frontend allocates surface backing lazily, so reducing handle counts
alone is not credited with a resident-memory win. Actual buffer-object tracing
identified sparse 2 MiB allocator slabs and an unused 10.5 MiB graphics ring.
Optional process-local Mesa packages retain the original reclamation protocol,
provision smaller sparse slabs, and defer the graphics ring until a graphics
queue context is initialized under the existing lock. Compute queues skip its
references. Concurrent graphics and injected allocation-failure cleanup pass.
The final opt-in AMD profile uses ACO without the unused LLVM runtime and
non-Radeon compiler tools. An intermediate LLVM-capable profile raised process
RSS by 7–8 MiB; removing unused dependencies reversed that tradeoff. In five
1080p/240 Hz confirmations the final profile averaged 38.348 MiB VRAM and
55.365 MiB RSS versus 123.909 MiB and 82.838 MiB in the fixed reference, with all
960 in-window frames and identical decoded scene data. CPU session cost also
fell. Small source-lease means still vary or increase slightly in some full-rate
cases, so the all-metrics goal remains active. The alternative driver builds
remain available, and the generic executable continues to use the system driver.
An initial graphics probe mixed 26.1.8 GBM with 26.2.5 EGL and crashed on both
control and candidate; matching GBM/EGL/DRI libraries repaired that evaluator.

Two five-repetition confirmation matrices cover static, changing, full-rate and
saturated inputs; producer-empty callbacks are reported alongside recorder skips.
The first 120 decoded scene frames are byte-identical across the saved reference
and both driver modifications. Native sanitizer and profiler evidence is retained.
These are scoped improvements. Cross-vendor minimum pool requirements, broader
graphics interference and full end-to-end latency remain open probes.

CPU-overhead follow-up: sample the reported overhead with samply and Tracy,
remove unnecessary work, and compare the complete recording path to the saved
executable. The reported PID used approximately one logical CPU; this machine
has 32 logical CPUs. The control is `/tmp/gpukey-before-overhead-fix`; its source
manifest is preserved in `docs/evidence/pre-overhead-implementation.sha256`.

Verified cause: GLib `g_source_set_ready_time` wakes its context when its value
changes. Our wait returned on that internal wake and rearmed itself, continuously
spinning. Earlier low-CPU capture benchmarks bypassed the portal, leaving a real
integration-coverage gap. The new idle-wait test fails against that control.

The delivered source keeps one owner-only deadline word and uses GSource
prepare/check/dispatch. Internal wakes do not return to the recording loop until
an application FD, deadline, revocation or stop is ready. No new worker, timer FD,
per-frame allocation or media copy is needed. A corrected ready-time control
remains available as `PORTAL_READY_TIME=1`. Five alternating repetitions per
variant and static/changing case include the private portal, native capture,
GPU VPP, AV1, replay, exports and teardown. Both fixes remove the spin; their
full-session CPU difference is within measured variation. The default also
avoids the API's self-notification work by construction.

The fixture now exercises portal authorization and waiting alongside actual
private PipeWire/GPU work; output is decoded and checked for scene changes.
Make now tracks auxiliary fixture-header dependencies. Profiling launches private
children: samply's live attach uses SIGSTOP/SIGCONT and interfered with the user's
foreground job control. Those stopped-process samples are excluded from results.
Kernel profiling permissions were not changed. Process CPU time supplies the
before/after numbers; Tracy's wait zones include blocked wall time.

Frozen-replay follow-up: inspect the user's
`gpukey-replay-20261020-093411-388187074.mkv`, explain the advancing playhead with
unchanged images, and correct future capture. The original clip is preserved.
Verified: 18 packets spanning 18.568 seconds, with the same two decoded picture
hashes alternating throughout; adjacent pictures differ only slightly
(71.899 dB decoded PSNR). The installed Niri 26.04 source populates Header.seq but
does not populate Header.pts. The saved control in `/tmp/gpukey-before-timing-fix`
records one of 51 private-source updates and labels the other 50 FPS-limited.
This invalidates the assumption that any nonnegative header value is a usable
presentation clock.

Revisable policy: `--video-clock auto` prefers populated source PTS, otherwise
explicitly announced monotonic receive time from the first frame. An evaluated
graph-first candidate avoided the freeze but let an uninitialized graph timestamp
shift the initial timeline. It was rejected. `source`, `cycle` and `receive`
retain explicit control; a reliable graph clock remains available without
automatically trusting a single positive value. Nonduplicate frames with a
stopped/reset selected clock fail explicitly. Auto treats header zero
as unspecified; explicit source mode preserves valid zero-based input.
Receive timing cannot recover source capture latency and is identified in
diagnostics and JSON statistics. This is a necessary approximation for producers
that omit usable timestamps, not a claim of physical A/V timing accuracy.
Repeated sequence numbers alone do not discard new timestamped frames; duplicate
source (PTS, sequence) pairs still do. The unused per-frame sequence field was
removed; timing state remains per capture stream. No new frame allocation,
framebuffer readback or encoder work was added.

Acceptance covers absent PTS, a stationary/advancing graph clock, a valid zero
epoch, repeated sequence numbers, lost PTS, existing timestamped sources, and
full-resolution native replay. Private-source output is independently decoded
and checked for actual black/white scene transitions, rather than only counting
packets. The already discarded motion in the user's clip cannot be reconstructed.

Startup failure follow-up: the user's `replay 30 --display --fps 60` exited with
SIGSEGV after reporting a configuration change. Required outcome: accept normal
PipeWire startup negotiation and shut down safely even without a submitted frame;
preserve the explicit failure policy for real configuration changes.

Verified from the user's core: zero submitted frames; `gk_media_close` called
FFmpeg's hardware encoder drain; the saved media format had modifier zero while
PipeWire's final 2560×2880 format had modifier `0x0200000000082305`. The pinned
PipeWire parser marks a modifier choice as requiring producer fixation, and its
optional scalar parser leaves the modifier zero until fixed. FFmpeg 9.0.2's
empty hardware drain dereferences `pic_end` when it is null.

The saved control is `/tmp/gpukey-before-startup-fix`; its empty VAAPI teardown
reproduces exit 139. Reversible implementation choice: ignore provisional
modifier choices until the producer publishes the fixed format; skip draining
when no video input was accepted and return the existing error or `ENODATA`.
This adds no frame storage, copying or steady-state work. Acceptance probes cover
two-stage tiled DMA-BUF negotiation, live replay saves, empty/prepared/rejected
first-frame teardown, real midstream changes, and existing compiler/sanitizer
checks. The user's interactive desktop is not captured during verification.
Those probes now pass on the native VAAPI path, including the reported dimensions
and an actual Tracy capture. A supplementary Vulkan leak check exposed separate
FFmpeg AV1 bitstream/driver teardown allocations, reproduced by a standalone
dependency-only open/free control. They remain an explicit acceptance limit;
the Vulkan crash regression passes but a leak-free Vulkan stack is not claimed.

Replay extension (current): continuously retain the last requested interval and
save on demand while capture continues, preserving existing audio, display and FPS
controls. Material §13.4 requires encoded retention, byte/time bounds, independently
decodable starts, and immutable export ownership. Replay was absent at the start.
The pre-replay source and executable are saved in `/tmp/gpukey-before-replay`;
the executable SHA256 is `67ec452460e02ab18f5adcd5d29a56b05922ea44f6061c6d6353ebccd534ecfc`.

Revisable replay decisions: `gpukey replay 30` starts a RAM-backed session;
`gpukey save` is a shortcut-friendly trigger, with Enter in an interactive terminal
as the local trigger. No SIGUSR1 handler or desktop shortcut is installed.
One bounded export can run alongside capture. Export includes the preceding
verified AV1 random-access point; arbitrary frame-exact cuts are not implied.
Opus requires decoder preroll and a new pre-skip; PCM can be clipped by sample.
Finite retention/export budgets and busy/error responses remain explicit.

Primary-source finding: FFmpeg VAAPI/NVENC/libaom packets already own CPU copies,
so transferring their buffer references avoids a second encoded-data copy.
Vulkan packets can directly own mapped GPU bitstream buffers; replay must copy
those before releasing them. Compare this dispatch with an all-copy control.
Packet metadata is packed in flat blocks; export pins blocks, not raw surfaces.
Readiness includes every selected audio track through the exported endpoint.
The chosen endpoint is the common completed audio watermark, at or before the
trigger; this avoids flushing/restarting live codecs. Keyframe alignment can add
leading footage. These are explicit precision choices, not exact arbitrary cuts.
Tests must independently decode evicted-window exports, repeated saves, long
static holds, nonzero audio cuts, source loss, concurrent teardown, memory limits,
and save failures. These tests are now implemented, including a deliberately
paused export with later live eviction, injected ENOSPC, and stop during export.
The final evidence remains in `docs/validation.md`.
No state-of-the-art claim substitutes for these checks.

Verified replay structure: 32-byte packet records in flat 256-record blocks,
rotated at one second even when sparse; owner-only block handles and key bitmaps;
sealed snapshot tails, per-block references and an SPSC retirement queue.
One background remux job at a time bounds pinned history. CPU packet adoption and
the all-copy control preserve the same encoded data and decoder contract.
The final job handoff uses release/acquire state publication and a dedicated
worker's futex wait (material Q04); an exporter never holds a mutex needed by the
capture owner. Job status and shutdown share one aligned 32-bit word. Completion
keeps its eventfd for the owner's poll loop. This replaces an evaluated extra
eventfd with the Linux futex ABI, while preserving the pause/retirement contract.
The integrated `REPLAY_EVENTFD=1` build remains available as a Pareto alternative;
both builds share the same atomic state/stop protocol and CLI. No automatic
dispatch threshold is inferred from a small benchmark on one host.
Cached successor-key state removes repeated empty searches. Audio timestamp
normalization reuses an exactly integral 48 kHz duration where possible without
changing chunk boundaries. An endpoint-based chunk variant was rejected after
it increased PCM retention memory in the fixed-input comparison.

The profiler-enabled CLI test exposed a pre-main-thread assumption in stop
handling: Tracy starts threads before the main thread blocks signals. Stop now
uses an async-signal-safe eventfd write from whichever thread receives SIGINT or
SIGTERM. The one notification descriptor lives until process exit, preventing
close/reuse races with library threads. Both signals are exercised in CLI tests.

Explicit request: accept multiple audio devices and multichannel audio, expose FPS
and display selection, and make everyday recording and refinement discoverable.
Verified baseline: FPS is configurable; display choice uses the portal; audio is
one fixed stereo source. A recoverable source/binary control is retained at
`/tmp/gpukey-before-multiaudio` (binary SHA256
`1eaa920e232e8be9e9ddae7d6466549458a068a7024c04438e7eb096d06bebd8`).

Revisable choices: repeated audio selections produce independent named tracks,
preserving each negotiated channel layout and rate before conversion to the file's
48 kHz clock. Mixing is not implicit. One bounded SPSC pair per actual audio
producer, one shared mux owner, and allocation proportional to selected channels
avoid a global contended queue or a new thread per track. Source loss stops the
recording; no fallback to a different device. Ordinary invocations gain a positional
output, extension inference, focused help, display/window shortcuts and read-only
audio discovery. Existing options remain accepted.

Constraint verified against ScreenCast v6: source types and portal restoration are
available; a connector-name selector is not. A chosen display therefore means the
display chosen in the desktop dialog, not an invented mapping from DRM connectors
to portal streams. Restoring permissions is outside this extension's default scope.

Probes/acceptance: independently decode multiple tracks, distinguish channel
identities, verify native rates, exact endpoints, separate drift, source loss,
layout changes and bounded callback behavior. Exercise commands and failures on
an isolated PipeWire graph. Compare the unchanged single-source path to the saved
binary with matched release workloads; new functionality alone is not evidence
of universal throughput or latency superiority.

## Verified context and delivered behavior

- The initial workspace contained a Collatz template and no Git repository. The
  executable, package, overlay, documentation and toolchain probes are now `gpukey`.
- The host is x86-64 Linux 7.2.10 with RX 9070 XT (AV1 encode/VPP) and a second AMD
  render node without AV1 encoding. The host's driver is Mesa 26.1.8.
- Portal authorization, stable source targeting, PipeWire leases and removal,
  persistent exact-modifier imports, native VAAPI conversion/AV1, audio, VFR timing,
  mature muxing, bounded buffered/io_uring output and complete stop are implemented.
- Real private PipeWire tests cover shared memory, DMA-BUF, revocation, and static
  content without recording private desktop or microphone content.
- C23 concurrency/media/portal tests, GCC/Clang sanitizers, ThreadSanitizer, the six
  compiler/linker packages and their identity checks, Tracy and formatting pass.
  An actual Tracy capture and performance controls are retained.
- A tested optional Mesa 26.2.5 patch corrects RGB8→P010 output range normalization.
  It is process-local and optional, and is not claimed as an upstream-reviewed fix.
- The supplied material is unchanged and its integrity manifest passes. Its finite
  models were rerun from a temporary copy and retain their explicitly limited scope.

## Chosen defaults and costs (revisable)

- Single-source local SDR recording, AV1 4:2:0 8-bit, source-derived VFR capped at
  60 Hz, native quantizer 32, two preferred source buffers (capacity for eight if
  required) and two owned VAAPI encoder inputs at encoding depth two.
  Input handles are capacity: completion permits one backing image to be reused
  between captures, while overlap can materialize the second. These are
  configurable starting policies, not universal optimal settings.
- Missing hardware or unsupported source color/transform is reported; software,
  shared-memory upload, and other backends require explicit selection. `--shm`
  forces the upload path without changing the selected encoder.
- GPU source reads are submitted while removal is excluded; completion waits run
  outside the PipeWire lock. Failed work never becomes a fabricated release signal.
- Bounded SPSC tokens, owner-local pools, out-of-band stop/error notification,
  optional cold timing storage, and separate audio/video counters.
- fMP4 at 1 us with exact-length floating PCM audio, or Matroska at 1 ms with Opus
  pre-skip/discard padding. Both use explicit valid media endpoints. The patched
  Matroska muxer writes VFR BlockDuration without a nominal DefaultDuration;
  unpatched libraries retain the one-tick compatibility fallback.
- A one-second static repeat bounds fragment/seek availability; changing
  `--static-interval` prices that guarantee separately. No recapture/conversion is
  needed for these legal re-encodes of the owned last image. Eligible independent
  refreshes can now reuse the encoded key; `--static-encoding always` retains
  ordinary encoding at the selected cadence.
- Buffered output is the control/default. Ordinary io_uring remains available;
  tested rates did not establish a decisive general win. No polling/kernel tuning.
- Profiling is optional. Library-internal allocations, driver waits and kernel
  work remain costs; a bounded queue is not a whole-system progress theorem.

## Remaining acceptance gates

Next optimization probes for the active goal: audit the remaining alignment and
driver workspace gap; compare RGB upload plus GPU processing against CPU conversion;
compare compact retention arenas and broader exact packet interning against direct
buffer adoption; reduce the remaining encoder submission/completion costs and
saturated source-lease tails. Static independent-key reuse is implemented under
the material's bitstream/reference-state contract; broader static-sequence
alternatives must preserve quality refinement and valid encoder/decoder state.
Deferred mapping, consecutive-key sharing and completed-output readiness already
have measured implementations.
Any encoder-worker or native RGB challenger must preserve source retirement,
last-image ownership, snapshot accounting, teardown and publication ordering.
Existing stage boundaries are controls, not requirements. Keep current source,
executable hashes and matched workload evidence as cumulative references.

Intel/NVIDIA hardware, real interactive portal/player/editor combinations, physical
A/V synchronization, driver-loss fault injection, game interference, power, broad
spatial color-quality tests and full cross-vendor Pareto comparisons remain open.
HDR, transforms, automatic reconfiguration into new segments, multiple
video sources, direct compositor capture and the paper's advanced array plans
remain unsupported. The optional direct-RGB route is implemented for its
advertised compatible formats; it does not imply support for those other plans.

Concrete source decisions are recorded in [docs/dependencies.md](docs/dependencies.md).
Executed checks, comparison controls, numerical results and their limits are in
[docs/validation.md](docs/validation.md). The implemented controls do not establish
universal state-of-the-art superiority or make unimplemented alternatives inferior.

The current extension is implemented: repeatable device selection, independent
named audio tracks with negotiated channels/rates, read-only discovery, positional
and dated output paths, extension inference, concise/complete help, display/window
shortcuts and capture admission rate limiting. The initial control remains saved.
Private PipeWire testing exposed metadata loss through audio mixing; capture now
selects a source-header or graph-cycle clock per stream generation and accounts
for the documented delay units. Stop quiesces producers and drains admitted audio
copies to the common endpoint before finalizing. Neither a toy driver with an
uninitialized clock nor a registry property absent from its filtered snapshot was
treated as a valid oracle.

A later read-only graph inspection found 16-channel AUX nodes on the actual host,
alongside the mono and stereo nodes. This invalidated the initial speaker-only
audio scope. Discrete layouts are now explicit, retain source order and AUX
identities, and use Matroska Opus mapping family 255 or opt-in PCM. The MP4 muxer's
layout restriction is reported, not bypassed by invented speaker assignments.
Source hardware audio has not been recorded during development.

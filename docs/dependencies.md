# Dependency and mechanism audit

Design authority: the supplied paper and its source registry. Implementation lookups
used the pinned dependency sources and the paper's primary references. No external
text was treated as authorization. Runtime driver versions are distinct from build
headers. Build versions below come from `flake.lock` (nixpkgs
`09063bd1f2b36fd9fd793c36dfade7bd40a93dfc`).

Startup correction: PipeWire's `doc/dox/internals/dma-buf.dox`,
`spa/param/video/raw-utils.h`, `src/pipewire/stream.c` and
`src/examples/video-src-fixate.c` establish the two-stage modifier protocol.
`DONT_FIXATE` produces `SPA_VIDEO_FLAG_MODIFIER_FIXATION_REQUIRED`; the optional
Long parser does not turn that choice into an allocation layout. The producer
publishes a preferred fixed EnumFormat followed by its original offers, causing
a second Format callback. Capture now publishes its immutable configuration only
after fixation. Final format/color changes still fail explicitly. FFmpeg 9.0.2
`libavcodec/hw_base_encode.c:hw_base_encode_send_frame` dereferences `pic_end`
on empty drain before its caller checks for an empty picture list. Media teardown
therefore bypasses drain when no input was accepted, then frees the codec and
owned resources in the ordinary order. This is a local integration workaround,
not a claim that upstream FFmpeg's empty-drain bug is fixed.
Vulkan's `vulkan_encode_av1_close` additionally omits
`ff_cbs_fragment_free`/`ff_cbs_close` for the encoder's initialized AV1 bitstream
state. A standalone public-API open/free probe reproduces the LeakSanitizer
report without gpuKey or a drain call. This dependency cleanup defect and
additional driver allocation reports remain documented in
[validation](validation.md#startup-negotiation-and-empty-encoder-shutdown);
no private FFmpeg ABI access or sanitizer suppression was introduced.

Capture timing correction: the running compositor's Nix derivation identifies
Niri 26.04 source `/nix/store/bazsf2g3j9imcrqrw18p3zn9911s7lfm-source`.
Its `src/screencasting/pw_utils.rs:mark_buffer_as_good` updates Header flags and
sequence, but not PTS; its stream listener also does not publish a graph clock.
PipeWire `stream.h:pw_buffer.time` and `stream.c:copy_position`/
`impl_node_process_input` describe and implement per-buffer graph-cycle times.
An initialized graph time can remain stationary when a driver never updates it,
as the private reproduction confirmed. A graph-first candidate then introduced
a startup offset before detecting the stationary clock. Auto now uses
`CLOCK_MONOTONIC` receive time from the first frame when source PTS is absent,
and explicitly identifies that approximation in diagnostics/statistics.
Explicit clock selection retains the graph-clock path for producers that
publish it correctly, and a strict source-PTS path.

Portal wait audit: GLib 2.88.3 `glib/gmain.c:g_source_set_ready_time` calls
`g_wakeup_signal` on its attached context whenever the deadline changes.
`g_main_context_iteration` can return after an internal wake without dispatching
an application source. Repeatedly rearming a deadline around one iteration
therefore spins. The new owner-only GSource uses public prepare/check/dispatch
hooks and a single deadline word. The owner retries internal wakes against that
same deadline. Capture/control FDs remain readable until their owners consume
them; session closure and stop still wake the same loop. No private GLib fields,
additional polling thread or timer FD are used.

Samply 0.13.1 `src/linux/perf_group.rs` and `src/linux/profiler.rs` were inspected:
live attach uses SIGSTOP/SIGCONT, and the Linux profiler loop ignores its
`_time_limit` argument. The reproducible script launches a private child and ends
with that child's lifetime. This avoids foreground-terminal job-control changes.
On this host, the existing samply wrapper retries perf events with kernel and
hypervisor sampling excluded; its preload is explicitly removed before launching
the measured application. System perf permissions are unchanged. The profile
summarizer accounts for `threadCPUDelta` and retains sample counts separately;
sampling frequency startup and off-CPU records must not be mistaken for CPU
percentages. Full process CPU comparisons use independent `getrusage` counters.

| Component | Inspected source and integration decision |
|---|---|
| PipeWire 1.6.9 | `src/pipewire/stream.{h,c}`, `core.c`, native protocol connection, DMA-BUF documentation, metadata and format helpers. Ordinary video callbacks are serialized on a thread loop; audio uses the RT callback with preallocated copies and nonblocking notification. Removal detaches wrappers without waiting. Held frame records survive removal. No explicit-sync metadata is negotiated. `maxsize == 0` is valid for DMA-BUF; allocation size comes from its FD. Caps advertise only allocated/exported native modifiers. |
| GLib/GIO 2.88.3 | `gio/gdbusconnection.c`, `gunixfdlist.c`. Subscribe before requests; dispatch on a private context. A private bus connection makes cancellation/teardown independent of GIO's shared connection cache. FD-list retrieval returns an owned duplicate. Tests deliberately emit responses before method replies. Portal v6 serials are preferred; old node IDs stay scoped to the session. |
| FFmpeg 9.0.2, libavcodec | `vaapi_encode.c`, `vaapi_encode_av1.c`, `hw_base_encode.c`, `vulkan_encode.c`, `nvenc.c`, `libaomenc.c`, `libopusenc.c`. VAAPI retains input frames until coded-buffer/surface completion. Codec ownership is drained before pool destruction. VA AV1 uses `global_quality`, not an unsupported `qp` private option. No reorder/lookahead is requested, and unexpected PTS/DTS behavior fails. libaom's first-packet extradata is obtained before committing mux headers. |
| FFmpeg hardware frames | `hwcontext_vaapi.c`, `hwcontext_vulkan.c`. Generic VA DRM mapping can retry a rejected explicit modifier through an implicit PRIME path; gpuKey deliberately uses direct PRIME_2 import without that downgrade. Vulkan upload contexts retain frame references until reused; limiting each queue family to one matches the single issuer and avoids exhausting the bounded input pool. Native representations remain behind the backend, not forced into one CPU image ABI. |
| FFmpeg libavformat | `aviobuf.c`, `mux.c`, `matroskaenc.c`, `movenc.c`. AVIO accepts an entire callback span or an error; positive short success is invalid. Copy into owned bounded slabs before returning. Nonseekable fMP4 avoids backward patches. Interleave delta is positive (100 ms), not the unbounded zero setting. Matroska precision is disclosed. Opus padding failed the fMP4 independent decoder test, so PCM is used there; Matroska retains Opus and exact discard padding. |
| FFmpeg swscale/swresample | `libswscale/utils.c`, `libswresample/swresample.c`, audio FIFO source. Explicit BT.709 matrix/range and chroma position in the shared-memory control. No sRGB→BT.709 relabeling. FIFO space is checked before writes to prohibit automatic growth. Resampling corrects gradual phase error over a second at at most 0.5%; startup/stop boundaries are separate from drift. |
| libva 2.24.1 headers | `va/va.h`, `va_drmcommon.h`, `va_vpp.h`, dispatch in `va.c`. Query AV1 entrypoints and explicit color controls; create exact native surfaces and preserve each import's plane layout. The native P010 contract ignores the bottom six bits, unlike FFmpeg's canonical CPU-format zero-padding description. GPU tests compare the meaningful high ten bits. No `vaExportSurfaceHandle` call is used as a synchronization operation. |
| Runtime Mesa 26.1.8 | `src/gallium/frontends/va/{context,postproc,surface,buffer}.c`, compositor/color conversion code. Read installed-driver source, not only newer headers. Explicit VPP color fields reach the driver; surface synchronization and context destruction are distinct. Measured P010 conversion differs from the scalar endpoint oracle by up to 3 ten-bit codes on this driver; no exact-rounding claim is made. |
| liburing 2.14 | `src/{queue,setup,register}.c`, inline SQ/CQ helpers and synchronous cancellation documentation. The writer creates/drives the single-issuer ring. No SQPOLL, IOPOLL, skipped-success CQEs or device-wide tuning. Payloads remain live through terminal CQEs; short completions resubmit only the remainder. A fatal enter error invokes synchronous cancellation. If quiescence cannot be established, payload storage is quarantined until process exit rather than reused. |
| Tracy 0.14.1 | `public/tracy/TracyC.h`, `public/client/TracyProfiler.cpp`. Disabled macros have no runtime calls. Enabled zone events use static source locations and the client's thread-local producer; instrumentation overhead and initial thread registration are not treated as free, especially in audio RT callbacks. |
| libaom / Opus | Pinned codec source plus FFmpeg adapters. libaom realtime mode, bounded threads and zero lag are an explicit software control, not GPU fallback. Opus queries actual lookahead and emits skip/discard metadata; neither priming nor final valid samples are hardcoded. |
| libdrm | Only DRM FourCC/modifier declarations are used directly; there is no privileged KMS capture or unrelated device access. |

The application links the headless FFmpeg variant; the much larger full multimedia
closure was unnecessary. Optional vendor implementations are selected through the
library's runtime discovery rather than requiring proprietary runtimes on every
installation. `libavfilter` was removed from the direct link because no filter graph
is used. FFmpeg and codec-library internal allocations remain real costs; application
pooling is not a claim that those libraries perform no allocation.

Source locations used during this implementation are recorded in the build store:

- FFmpeg: `/nix/store/sglqmrg6499fqnshlcjwww7f2d92y6ik-ffmpeg`
- PipeWire: `/nix/store/8vl3sf52601iy1y5z6wc3m6brd3782al-source`
- libva: `/nix/store/yaihslbl3lb5zlpak06mjs9f6696c7sn-source`
- liburing: `/nix/store/52z72cv36axrcvpqhqd4xi37vynxchqh-source`
- Tracy: `/nix/store/p3wq1g250v3w793q8ysrb96h0m64d5z5-source`
- installed Mesa: `/nix/store/dxfn3fqadh1xl8h419h2ckd3giij01fg-source`
- GLib archive: `/nix/store/4298knw9zvdfz2mg7xizhzdvcq4gl8sr-glib-2.88.3.tar.xz`
- Opus: `/nix/store/m6g3687b4nx3krs24hsygjmm0f57jycg-source`
- libaom: `/nix/store/mvj6agx0vz29mh779253alk0s805cji4-source`

These store paths are provenance, not runtime dependencies on unpacked source.
The paper's references C01/C02/C07–C11, G20, Q01–Q03/Q13–Q17 and IO01–IO25
provide the relevant contracts. The package's MIT metadata describes gpuKey's own
source; linked components retain their own licenses, including the pinned FFmpeg
configuration and optional vendor runtime terms.

Additional audited findings implemented after the initial integration:

- `matroskaenc.c` only writes video BlockDuration when a DefaultDuration exists.
  gpuKey supplies a one-millisecond fallback and explicit overrides, rounds
  presentation endpoints once, and tests every serialized boundary plus a long
  final static hold. It does not label the capture cap as an observed average.
- Source removal and DMA-BUF submission are serialized. The native VPP completion
  wait is a separate step outside the PipeWire lock; queued detached sources cannot
  receive new GPU reads. Owned shared-memory copies retain an independent lifetime.
- Both Mesa 26.1.8 and pinned 26.2.5 reuse input component depth for output range
  normalization in `vl_csc_get_rgbyuv_matrix`. `nix/mesa-output-range.patch` uses the
  destination depth for that operation. The optional `mesa-color` package contains
  a tested Radeon VAAPI build with the fix; original and patched 26.2.5 were compared
  on the same scalar oracle. Stock source:
  `/nix/store/hnsb15znq9rmdrbx357iqmsvl0w3hw6s-source`.
- Ordinary recordings avoid optional histogram clocks and timestamp allocations.
  The pooled VPP parameter buffer removes per-frame create/destroy calls; the
  original mechanism is preserved by `VPP_CONTROL=1` for matched comparisons.
- The portal validates object-path strings and FD-list indices before calling GIO's
  typed accessors. A malformed FD reply is exercised by the private portal test;
  accessor preconditions are not delegated to an external service.
- `hwcontext_cuda.c:cuda_transfer_data` submits asynchronous host uploads without
  retaining the caller's host frame. The NVENC fallback therefore passes software
  NV12/P010 frames plus the selected CUDA device to `nvenc.c`'s owned-input-buffer
  path: lock input buffer, synchronous image copy, checked unlock. The caller's
  pixels can then be reused without assuming asynchronous host-copy completion.
  This is an audited control; actual NVIDIA runtime execution remains unverified.
- Missing optional video rate metadata defaults to VFR 0/1; an explicitly invalid
  denominator still fails. Audio selection with no valid samples is a failed
  recording, not a successfully manufactured silent track. Once media processing
  fails, codec drain retires resources without appending later reference-dependent
  packets to the established mux prefix.
- NVENC requests `forced-idr=1`: its default maps a forced I picture to
  FORCEINTRA, which is not the same random-access request as FORCEIDR. The option
  is present in the pinned AV1 encoder and is checked for consumption at open.

## Multiple audio sources and command interface

The extension follows material §11.2–11.4 and §12.1–12.2. No new library was added.
The pinned source was inspected again at these integration points:

- PipeWire `examples/audio-capture.c` explicitly leaves rate/channel fields empty
  to negotiate native graph values. `audio/raw-utils.h` and the stream/adapter
  implementation confirm that this differs from requesting stereo. Each source
  requests interleaved F32 and `stream.dont-remix=true`; no global graph rate is
  changed. Selected names/serials are resolved on the actual audio connection,
  checked for duplicates/ambiguity, then pinned by serial before connection.
- `stream.h` defines `pw_buffer.time` as the captured buffer's graph-cycle time,
  and distinguishes the graph-rate units of `pw_time.delay` from the stream-rate
  units of `pw_time.buffered`. `stream.c:copy_position` publishes those snapshots.
  `spa/plugins/audiomixer/audiomixer.c` can copy samples without populating Header;
  treating an all-zero Header as a valid timestamp discarded all real audio in
  the first private-graph experiment. Capture now fixes its clock authority at
  the first block: populated Header, otherwise buffer-cycle time less graph and
  converter delay, with checked arithmetic. Callback arrival is never substituted.
  Physical device latency accuracy remains a hardware validation requirement.
- `stream.c:pw_stream_set_active` enters the data-loop lock before returning;
  `pw_stream_destroy` removes callbacks under that lock. The stop flag is published
  first. This establishes the final queue prefix before the owner clips/drains
  copied audio and later frees each source's storage.
- `audiomixer.c:impl_node_port_use_buffers` requires mapped data. The private test
  producer initially advertised only raw MemFd; its mixer rejected unpopulated
  pointers. Advertising MemPtr-compatible shared memory matches ordinary audio
  producers and preserves the tested callback/copy mechanism. Its synthetic graph
  drivers now publish real monotonic cycle clocks. These were fixture corrections,
  not reasons to weaken the recorder's timestamp or memory checks.
- FFmpeg `libopusenc.c:libopus_validate_layout_and_get_channel_map` and
  `libopus_encode_init`: mapping family 1 uses the surround API and the actual
  Vorbis channel permutation; the default -1 is a legacy multistream path.
  Family 0 covers mono/stereo. Unsupported layouts fail instead of being relabeled.
  libswresample converts each source's explicit input order/rate to the same
  speaker set in canonical output order at 48 kHz. It never receives a downmix.
- For the host's 16-channel AUX nodes, `libopusenc.c` and Opus
  `src/opus_multistream_encoder.c:opus_multistream_surround_encoder_init` establish
  mapping family 255 as independent channels in identity order, with no surround
  masking or speaker assignment. Source AUX indices are retained in a
  `GPUKEY_CHANNELS` Matroska tag. Equal custom input/output layouts prevent
  libswresample's implicit default-layout substitution/rematrix path.
  The PCM alternative uses the same ordered sample contract and tag.
  `mov_chan.c:ff_mov_get_channel_positions_from_layout` rejects unspecified/AUX
  positions in this MP4 writer. `matroskaenc.c` writes channel count without a
  speaker mask for native float PCM; positioned surround PCM therefore requires
  MP4, while discrete AUX PCM uses its explicit order/label tag in Matroska.
- `movenc.c:mov_write_audio_tag` emits `chnl` for multichannel PCM but omits it for
  a single channel. Only implicit mono is accepted in that case. Tests derive the
  singleton mono identity from the fixture contract and require actual identities
  for every multichannel output. Matroska uses `title`; MP4 uses `handler_name`,
  avoiding redundant track-name metadata and the reader's raw-UDTA fallback.
- The portal's [ScreenCast v6 API](https://flatpak.github.io/xdg-desktop-portal/docs/doc-org.freedesktop.portal.ScreenCast.html)
  has source-type selection but no connector-name selector. `--display` and
  `--window` request the corresponding chooser. No compositor-specific guessing,
  remembered permission, or unrelated source fallback is introduced.

### C23 integer formatting

`PRIu64` remains standard C23. The new `%w64u` spelling was tested against the
actual pinned toolchains with `-std=c23 -Wall -Wextra -Wformat=2 -Werror`.
GCC 16.2.0 accepts it and prints `UINT64_MAX` correctly; Clang 21.1.8 rejects it as
`invalid conversion specifier '6'`. The compatibility choice retains format
checking on both compilers. It is not a claim that C23 lacks width modifiers.

## Rolling replay and export ownership

This extension implements material §13.1–13.4 and the stop/retirement rules in
§14.3. It adds no library dependency. The same pinned sources were inspected at
the following mechanisms before selecting the retention representation:

- FFmpeg `encode.c:avcodec_default_get_encode_buffer` allocates padded CPU packet
  storage. `vaapi_encode.c`, `nvenc.c` and `libaomenc.c` copy native coded output
  into that storage. Replay **moves the existing AVBufferRef** rather than taking
  another reference or copying that payload again. Failure leaves the caller's
  packet intact. Sub-buffer offsets and the allocation's full size are retained.
- `vulkan_encode.c:output` can expose a mapped native bitstream while `pkt->buf`
  owns an `FFVkBuffer`, not the CPU byte range. Vulkan video therefore always takes
  the padded CPU-copy path and releases the native packet immediately. Pointer
  range tests alone are not treated as permission to retain a GPU buffer.
- `av1_parser.c` uses the coded-bitstream parser to classify frames. A fresh parser
  plus a conservative OBU-prefix check verifies each new candidate start (exact
  key equality can reuse that result, as described below): first frame
  must be a shown KEY frame (or a reduced still-picture header); hidden/inter or
  show-existing frames and unsupported layers are rejected. This is deliberately
  narrower than arbitrary reordered AV1. It follows the
  [AV1 ISOBMFF random-access requirements](https://aomediacodec.github.io/av1-isobmff/);
  a generic I-frame flag alone is insufficient.
- `libopusenc.c` reports the valid sample span in packet duration and coded tail
  padding in `AV_PKT_DATA_SKIP_SAMPLES`. `matroskaenc.c` uses OpusHead/initial
  padding and end discard separately. Replay keeps at least 80 ms of available
  Opus decoder preroll before a nonzero cut, changes the exported OpusHead pre-skip,
  and preserves/clips final padding. See
  [RFC 7845 §4.6](https://www.rfc-editor.org/rfc/rfc7845.html#section-4.6).
  The beginning of a recording uses its original available priming history.
  Its large maximum-packet allocation is internal scratch: after shrinking the
  packet, `encode.c:encode_make_refcounted` copies only the actual encoded size
  into padded CPU storage. Adoption therefore does not retain that oversized
  scratch allocation. The replay benchmark's Opus fixture restores the known
  continuous 20 ms sample clock and final padding rather than using rounded
  Matroska packet durations as sample counts.
- `av_write_frame` preserves caller ownership. The export worker first builds
  exact per-stream indices, then merges DTS with a small heap; it supplies already
  ordered packets without adding an interleaver queue. One temporary AVPacket
  reconstructs the muxer-facing fields; original chunks remain pinned until mux
  finalization and the bounded sink's accepted writes have completed.
  `mux.c:guess_pkt_duration` falls back to `codecpar.frame_size` for zero-duration
  audio. Exported Opus parameters clear that hint because valid spans vary at
  cuts; `matroskaenc.c` then writes the explicit coded-duration difference and
  discard padding. Nonempty sub-millisecond audio packets are retained, preserving
  their sample counts. An intermediate video interval that collapses to zero
  container ticks fails instead of dropping a reference-dependent packet.

On x86-64, each retained record is 32 bytes: CPU buffer reference, PTS, byte
offset/length, stream/key flag, audio duration and end padding. DTS equals PTS in
the validated encoder plan, video duration comes from the following PTS/clip end,
and codec configuration is immutable per session. Unused AVPacket fields and
encoder statistics are not retained. Blocks contain 256 records and rotate at
one second as well as on capacity, preventing sparse static input from retaining
minutes of obsolete metadata. Key bitmaps and cached boundary cursors avoid
rescanning the complete window on every append.
Only Opus sessions retain decoder preroll. PCM/video-only sessions retire blocks
ending at the selected start, while preserving any PCM packet crossing that cut.

Only the media owner changes the live handle list. Snapshot publication seals the
tail and increments block references before release-publishing the queued state. The
worker reads immutable records and never traverses mutable list links. Last
worker releases publish block IDs through an SPSC queue; only the media owner
destroys payloads and reuses IDs. An acquire/release reference decrement arbitrates
the last release; queue publication transfers exclusive destruction back to the
owner. A paused worker can pin only the sealed snapshot, not later appended data.
The single job slot uses release/acquire publication in both directions. Job
state and a monotonic stop bit share one aligned, lock-free 32-bit atomic word.
The dedicated worker uses a
[futex compare-and-sleep loop](https://man7.org/linux/man-pages/man2/FUTEX_WAIT.2const.html)
as proposed in material Q04; status transitions preserve a concurrent stop bit.
Both work publication and shutdown change the predicate before waking the worker,
so a notification before sleep cannot be lost. Only this worker can advance a
queued job to completion, excluding a complete state ABA cycle while it is
preparing to wait. EAGAIN, EINTR and spurious returns recheck the actual condition.
Completion keeps its eventfd because it shares the owner's poll loop. There is
no job mutex shared with the capture owner; a paused exporter cannot hold an
owner lock. Completion is published only after every job access and release,
and shutdown processes a last accepted job even if it was queued immediately
after the previous completion. The earlier mutex and eventfd controls are
retained for comparison. The Linux 32-bit futex ABI is a deliberate platform
constraint, not a portable C23 waiting primitive.
Shutdown joins the worker before freeing the codec template or any remaining
blocks. This is an ownership argument plus sanitizer/stress coverage, not a
whole-system lock-free progress guarantee.

The 256 MiB default charges full payload allocations, reference/allocator
allowances and block metadata. Live and pinned snapshot charges each stay within
that budget, with one spare block. Handle tables and a bounded SPSC queue are
allocated from the maximum possible block count; export additionally uses one
64-bit index per retained packet. FFmpeg, capture, GPU, output and actual allocator
working sets remain separate. The defaults are revisable; fixed metadata blocks
trade partially filled space for predictable append work.

### Local triggers and stop signals

A private Unix seqpacket socket under the owned, mode-0700 runtime directory
provides versioned, bounded requests and completion replies. Session lock files
retain their inode; stale socket deletion happens only while holding the lock.
Both sides check peer UID. Relative output paths are resolved by the client, and
file creation still uses the sink's nonoverwrite contract. Busy saves are rejected
rather than queued without a memory bound.

`TracyProfiler.cpp` initializes profiler workers before `main` in this library
configuration. The profiler-enabled replay CLI test exposed that a main-thread
signal mask cannot reliably route process signals to signalfd in that situation.
SIGINT/SIGTERM now use only a lock-free atomic descriptor load, an
[async-signal-safe write](https://man7.org/linux/man-pages/man7/signal-safety.7.html)
and errno preservation. The
[eventfd counter](https://man7.org/linux/man-pages/man2/eventfd.2.html) wakes the
existing owner loop. Its single CLOEXEC descriptor remains alive until process
exit to exclude close/reuse races with library threads. No encoding, allocation,
locking, logging or export work runs in the handler.

## VRAM allocation and retirement audit

FFmpeg 9.0.2 `hwcontext.c:hwframe_pool_prealloc` acquires a second array of frame
wrappers and returns them in order. `buffer.c:pool_release_buffer` inserts at the
head, reversing that order. `hwcontext_vaapi.c:vaapi_frames_init` has already
allocated a probe surface for `vaDeriveImage`. With our former eight fixed inputs,
that backed surface could end up unused while later inputs acquired more backing.
The bounded application pool now obtains its inputs directly from the dynamic
library pool; the encode reconstruction pool is separate. No private FFmpeg ABI
is accessed, and the application never grows the input pool during recording.

Fresh `vaapi_pool_alloc` buffers have `AV_BUFFER_FLAG_READONLY`: their **surface-ID
bytes** are not CPU-writable. Recycled pool entries lose that flag in the pinned
implementation. GPU writes to the surface therefore cannot be gated by
`av_frame_is_writable`. VAAPI now requires a sole input-buffer reference;
`vaapi_encode_wait` releases the encoder's input reference only after successful
`vaSyncBuffer` or `vaSyncSurface`. Other backends retain their existing checks.
Depth one can update its sole input after completion; a pinned external frame
reference prevents reuse in the hardware regression test. Preparation failure
is terminal, so a partially replaced image is never repeated.

PipeWire 1.6.9 `buffers.c:param_filter` enumerates compatible input/output offers,
then fixation/selection takes the first eligible buffer parameter. A preferred
range value alone did not reduce the producer's eight-buffer default in the
actual fixture. Ordered exact-count offers followed by a compatible range did.
The preference list covers the default 2..8 domain, including non-powers of two;
larger explicit capacities remain accepted. Capture token-ring capacity is
rounded independently. A producer requiring three buffers actually allocated
three in the test, and the original supported domain was preserved.

The installed Mesa 26.1.8 source is listed above. Pinned Mesa 26.2.5 source is
`/nix/store/hnsb15znq9rmdrbx357iqmsvl0w3hw6s-source`. Its auxiliary contexts are
created on demand in `si_context.c`, unlike the earlier eager initialization.
Both versions provision sparse allocator classes from slabs sized for the largest
class. `amdgpu_bo_slab_alloc` was audited through allocation, alignment, cache
reuse and fence-based reclamation. The optional `mesa-compact` variant changes
only provisioning: small classes use at least 64 KiB and a four-entry target;
large classes and the special five-entry rule retain their prior sizing. The
callback still derives entry count from the actual returned buffer size. Reuse,
fence protection, heap flags and retirement are unchanged. Smaller mappings trade
huge-page coverage for less reservation; measurements, not the allocator's name,
determine the supported performance claim.

The `mesa-media` variant also moves the graphics attribute-ring allocation from
screen initialization to graphics-queue context initialization. Every graphics
context takes the existing `tess_ring_lock` before reading this one-time published
resource. Compute queues skip its allocation and buffer-list references, so they
cannot race an unsynchronized read against first publication. Graphics readers
then use an immutable pointer without a new per-draw lock. Screen destruction
follows context retirement. The allocation has no `SI_RESOURCE_FLAG_CLEAR`, so
it does not recursively create an auxiliary context while holding this lock.
Allocation failure unlocks and reports context-creation failure; this was injected
and checked on eight concurrent constructors. The existing protected/TMZ path is
not claimed to have been comprehensively verified by these unprotected tests.

The headless graphics probe evaluates real GL rendering, not just successful
context creation. GBM, EGL and DRI must come from the same driver package: the
initial probe mixed system GBM 26.1.8 with EGL 26.2.5 and crashed inside
`driQueryOptionb` on both control and candidate. `GBM_BACKENDS_PATH` plus the
explicit EGL vendor and DRI paths repaired that test setup. The matched control
and candidate then passed. These are local, opt-in patches, not an upstream
review or a claim of validation on every Radeon GPU.

`tools/gpu-alloc-trace.c` records actual libdrm `amdgpu_bo_alloc` calls and can
inject a selected allocation failure. A release call can drop a reference
without destroying backing; it is not treated as a physical-free oracle. Memory
totals instead use Linux DRM fdinfo, deduplicated by PCI device/client ID, with
shared, resident and purgeable storage kept distinct. See
[memory targets](memory-targets.md) for the model and accounting boundary.

The final `mesa-aco` profile also removes the unused LLVM compiler runtime.
Mesa's `amd-use-llvm`, `draw-use-llvm` and `llvm` build options were inspected,
along with `si_pipe.c:ac_init_llvm_once` and `si_shader_info.c:si_nir_gather_info`.
This GPU already used ACO. Disabling LLVM removes its eager initialization and
library loading; it does not select a cheaper shader algorithm for this device.
Unused non-Radeon developer/CLC tools were also disabled to avoid requiring LLVM
through tooling. The generic Nix recipe's now-empty `cross_tools` output is
created explicitly. The driver library has no LLVM/Clang dynamic dependency.
LLVM-dependent shader workloads remain outside this opt-in profile; the
LLVM-capable controls and ordinary system-driver build remain available.

## Exact replay key-payload sharing

The material's replay contract (`chapter_io.md`, “Replay and admission control”)
requires immutable encoded ranges, verified random access, bounded retention and
snapshot ownership through export. Sharing is a local representation change:
every accepted packet still has its original bytes, PTS, flags and codec state.
It does not manufacture repeated temporal units, which `chapter_gpu.md` warns
against. Only exact equality with the newest already verified key is eligible;
changed extradata and invalid packet metadata are still rejected.

FFmpeg 9.0.2 `libavutil/buffer.c` was inspected through creation, `av_buffer_ref`,
`buffer_replace`, unref and destructor invocation. References contain independent
range metadata; their allocation increments the backing buffer's atomic count.
The replay store instead keeps one existing CPU reference per shared payload,
with a 24-byte cold descriptor. This avoids allocating another AVBufferRef for
every retained duplicate. Export creates the normal FFmpeg reference when
reconstructing a packet, and unrefs it after muxing. No raw GPU bitstream object
is retained by this optimization.

The 32-byte packet record uses its existing pointer word as a union. A video-key
tag uses otherwise unused stream-zero flag bits, preserving the audio index
domain. The owner alone changes separate live and owned counts; the worker reads
only the immutable buffer pointer through a pinned, published block. It never
dereferences the counts. Release/acquire snapshot publication and the existing
block retirement queue protect the descriptor and payload until the last export
read. Reclamation happens on the owner after all block references retire; close
joins the worker before releasing live history. Live charging removes an allocation
when its last live packet expires; owned charging waits for its last retained
packet, including snapshots. Both count the shared allocation once.
The existing block-table capacity bounds the number of retained records below
2^39, so neither 64-bit ownership count can overflow within this representation.

The newest key cannot be evicted before its successor commits, so comparison
needs no hash table, separate cache reference or extra atomic operation. An equal
new packet with a smaller backing allocation replaces an oversized representative.
Descriptor allocation is optional: lack of budget or malloc failure falls back
to ordinary reference storage. Eviction walks the existing key bitmap for live
counts rather than adding another pass over every audio/video record. All-copy
and plain-reference controls remain available. Full-history interning and packed
arenas remain unmeasured alternatives; this scoped mechanism does not establish
their inferiority or a globally minimal RAM representation.

## GPU conversion alternatives and the paired VPP kernel

The material keeps direct RGB encoding and accurate fused conversion as separate
candidates (`chapter_gpu.md` §9.4). Audited Mesa 26.2.5 also has encoder-side
processing in `frontends/va/postproc.c`, `picture.c`, and
`drivers/radeonsi/mm/radeon_vcn_enc.c`. A processing parameter buffer supplies
the RGB source and an additional YUV output. VCN can consume the RGB source
directly when `radeon_vcn_enc_efc_supported` accepts the combination; otherwise
the frontend performs ordinary VPP. The source must remain valid through encoding.
Our normal `prepare` completion releases it after conversion, so redirecting
calls alone would be unsafe in live recording.

`tests/gpu_fused_probe.c` and the diagnostic interposer use one immutable source
through encoder drain. The interposer refuses other program names; its marker
also prohibits the probe's pre-encode YUV readback mode. At 8-bit, the tested
linear and GFX12 native layouts produce the same native-conversion result.
`AMD_DEBUG=noefc` restores the ordinary VPP output. The generic DCC guard checks
`meta_offset`; that is not a valid blanket rejection of all GFX12 modifiers.
Native conversion changes rounding and chroma filtering. An eight-bit RGB source
with ten-bit output takes the shader fallback. These probes establish a mechanism
and numerical differences, not complete live-pipeline superiority. Q=0 also
exposed FFmpeg's existing AV1 VAAPI lossless transform-mode limitation; Q=1 probe
decodes are explicitly near-lossless and are not an exact conversion oracle.

The compute alternative retains the current VPP sampling. Each invocation writes
four luma samples and one interleaved chroma pair. It uses the same four chroma
texture coordinates, edge clamp, accumulation order and color dot products as
`vl_compositor_cs.c:create_rgb_yuv_shader`. The checked unit-scale case makes the
chroma projection exactly twice the luma projection. Two output images are bound
together, followed by one dispatch and one completion barrier. No destination
read is needed when blending is disabled. GPU completion and source release
remain in their original places.

The fast path requires supported packed RGB8 and NV12/P010, even full input
dimensions, no crop/scale/rotation/mirroring/interlacing/blending, equal transfer
functions and identity primaries conversion. The general path retains the other
operations. Its 64-byte constant record contains three color rows, two bounds
and two chroma offsets; all fields are consumed. Caching that record avoids two
parameter uploads per unchanged configuration. A changed record uses the
driver's discard-map protocol, preserving previous GPU reads. Validity is cleared
before mapping: failure may already have replaced the old backing. It is restored
only after a complete upload.

`vl_video_buffer_sampler_view_planes`, its constructor and its destructor were
read before reusing views. The cached view belongs to the source buffer, so no
new cache retains removed sources. Matching-context callers reuse it; other
contexts or absent getters keep the original temporary-view ownership. A
single-component resource also keeps the original path because the video-buffer
helper replicates that component into all channels, changing the normal RGB
view's swizzle. Each path releases only what it owns.

The paired shader and constants can exist before the general shader family is
initialized, so their cleanup is independent. Partial general-shader construction
also releases and clears each created state before retry. A fault-only build
injects failure after a discard and at several shader-construction positions.
Those hooks and counters are absent from the normal driver. The scalar color
probe, raw transition probe and fault-recovery probe exercise different contracts;
the graphics probe separately checks the shared driver's rendering and teardown.

## Completed AV1 output and backing reuse

The paper's ownership/completion contract and C12 DMA-BUF reference remain the
authority. The Linux documentation distinguishes [implicit fence polling](https://docs.kernel.org/driver-api/dma-buf.html#implicit-fence-poll-support)
from CPU access preparation. Polling a coded buffer's latest writer is a wake
source; ordinary `vaSyncBuffer` and VA mapping still provide completion, feedback
and coherency. No CPU pixel readback, periodic polling timer or encoder worker
is introduced.

Audited FFmpeg 9.0.2 `hw_base_encode.c`, `vaapi_encode.c/.h`, `encode.c`,
`avcodec.h` and `libavutil/refstruct.c`: the original asynchronous queue returns
EAGAIN with spare capacity even when its oldest picture has completed. The
opt-in `async_readout` path checks that oldest picture without new input. A full
queue still performs the blocking output operation, preserving send/receive
progress. Successful completion retires the input reference exactly as the
existing blocking path does. AV1 uses no B frames in this recorder; no claim is
made for other reference structures merely because the common code compiles.

Audited Mesa 26.2.5 `frontends/va/buffer.c`, `picture.c`, `picture_av1_enc.c`,
`radeonsi/mm/radeon_vcn_enc.c`, and winsys resource-export/submission paths:
coded output is allocated shareable before submission, and exported through
the existing VA buffer-handle interface. External handles disable deferred
CPU submission past the API return. The submitted resource carries the encoder's
synchronized write fence. The FFmpeg pool owns one CLOEXEC duplicate of the
exported descriptor per coded buffer. It releases the VA export lock immediately:
retaining that lock would prohibit subsequent VA mapping. Descriptors stay open
until their pool objects are freed. The constructor initializes invalid handles
and enables `AV_REFSTRUCT_POOL_FLAG_FREE_ON_INIT_ERROR`; its destructor closes
the duplicate and destroys the VA buffer on partial failure too. A retained
hidden-picture coded reference is released before the pool/context at teardown.

Public AVOptions expose capability and a borrowed readiness descriptor. Readonly
options are skipped by default initialization, so the descriptor is explicitly
initialized to -1. The recorder checks option type/flags, caches its public
storage offset and reads it only on the codec owner's thread. Unsupported driver
export falls back during initialization. A readiness notification with no
packet progress disables automatic events (or errors for explicit `events`),
preventing a persistent false notification from spinning the capture loop.

GLib 2.88.3 `gmain.c:g_main_context_query_unlocked` relies on poll records sorted
by descriptor number. Completion watches therefore keep stable descriptor
numbers and change only their event masks between owner iterations. They are
cached for the codec-pool lifetime and removed before codec destruction. Poll
errors are handled even for inactive watches because Unix poll reports HUP/ERR
with an empty events mask. No callback borrows a descriptor past close/reuse.

An input surface can be overwritten when its sole AVBuffer reference belongs to
the recorder and the codec has retired its GPU read. The last image is not an
extra owner. Reusing it avoids materializing another backing image when captures
do not overlap; retained spare handles still support overlap. Preparation
failure is terminal, preventing a partially overwritten last image from becoming
a static repeat. The frame pin/reuse probe tests an independent owner explicitly.

The recorder FFmpeg configuration also audits its consumers: four AV1 encoding
backends, Opus/PCM, AV1/Opus parsers, required AV1 bitstream filters, Matroska/MP4,
file/pipe protocols, libdav1d and audio decoding, swscale/swresample, and
VAAPI/CUDA/Vulkan hardware contexts remain. Unused codec/device/filter/network
families are excluded from the linked library. Runtime CPU dispatch remains
enabled. Full FFmpeg/ffprobe command-line tools remain separate development/check
dependencies. Configuration-selected FATE tests replace the upstream `check`
target's unconditional unrelated developer-tool builds. The public feature
registry probe and recorder integration tests verify the actual consumer set;
runtime NVIDIA/Intel operation still requires those devices for verification.

## VAAPI mapping work and synchronous transfer ownership

The paper's repeated-state/cost audit and producer/consumer lifetime rules apply
to FFmpeg's `libavutil/hwcontext_vaapi.c` as well as the recorder. Its original
pool initializer allocates a test surface and derives a CPU image for both the
input and reconstruction pools. Native GPU capture uses neither CPU view.
An allocation trace identifies the resulting temporary tiled/linear images;
an exact-call-site diagnostic on the immutable control library removes only those
probes and confirms the opportunity before changing the library.

The selected implementation creates/derives images on an actual map request.
Normal read maps retain FFmpeg's copied-image policy for CPU caching. Ordinary
writes try derivation until a capability failure or incompatible format selects
copied writes for that pool. A relaxed atomic integer is a policy hint, with no
data or resource ownership published through it. Concurrent first callers can
complete independently; none waits for another thread to finish probing.
An explicit direct-map request always tests its actual surface. This avoids
turning an earlier surface's limitation into a false direct-map rejection.
The earlier cacheless candidate is retained: it would repeat failed derivation
on every upload. The persistent-map record and its source/context ownership remain.

`hwcontext.c:ff_hwframe_map_create`, `frame.c:av_frame_copy/frame_copy_props`, and
`buffer.c` were read before specializing synchronous transfers. The original
transfer builds a temporary AVFrame, VA mapping object, HWMapDescriptor, source
frame clone and buffer references, then destroys that graph within the same call.
The transfer's caller already holds the frame and context alive. The new path
uses a stack VA image record and four plane pointers/strides, and performs the
same pixel copy and VA synchronization. No stack mapping record is stored in
an AVBufferRef or exported to a later callback. Driver-side image allocations and
the required GPU/CPU work remain costs.

The audit found late errors were logged by the old map destructor while the
transfer returned zero. A real VAAPI regression reproduces false success for
unmap, writeback and image-destruction failures. Synchronous cleanup now returns
status directly; a failed unmap suppresses copied writeback, and pixel-copy
validation failure cancels copied writeback. Persistent public maps still use
FFmpeg's void release callback and its error logging. Recorder CPU upload uses
the checked synchronous path and treats an error as terminal.

This preserves AVFrame references/properties, not a transaction that rolls back
device bytes. The generic allocator publishes a clean destination only after
successful transfer, which the failure test checks. A preallocated device buffer
can already have changed before a late failure; the pinned CUDA implementation
likewise issues earlier plane copies before checking later copies/completion.
The recorder never submits or repeats a failed preparation. No extra image backup
or unstated GPU rollback guarantee is added.

Moving fallible property copying before writable-map construction initially
exposed a size-dependent metadata issue: pan-scan data is skipped when source and
destination extents differ. The corrected constructor sets the actual displayed
source dimensions before copying properties. This also adopts the source-sized
mapped extent in the [current primary FFmpeg implementation](https://www.ffmpeg.org/doxygen/9.0/hwcontext__vaapi_8c_source.html),
instead of exposing allocation padding. Pixel tests use full allocation capacity
and smaller display extents, and poison readback storage to prevent stale bytes
from masquerading as a completed transfer. Plane order changes also carry their
corresponding strides.

Static repeat research follows the paper's C19 reference and its sections 8.3/9.7.
The authored [AV1 semantics](https://github.com/AOMediaCodec/av1-spec/blob/master/07.bitstream.semantics.md)
require a showable reference and restrict showing an existing keyframe to once;
the [syntax](https://github.com/AOMediaCodec/av1-spec/blob/master/06.bitstream.syntax.md)
makes already shown keyframes non-showable. Ordinary encoding remains the
default control under the current one-second repeat/two-second random-access
policy. Show-existing remains a candidate for other cadences/backends. The
inspected VAAPI API documents application-generated short headers and skipped
frame accounting; VCN's AV1 path currently exposes ordinary I/P picture types.
Its H.264/H.265 P-skip enum is not evidence of AV1 firmware support. No arbitrary
compressed packet repetition or undocumented firmware command is introduced.

## AV1 workspace allocation candidates

The AV1 tier-2 allocation audit follows Mesa 26.2.5 `radeon_vcn_enc.c`,
`radeon_vcn_enc_5_0.c`, `si_video.c`, `si_buffer.c` and `amdgpu_bo.c`.
The firmware-sized SDB is 947,200 bytes, default CDF 81,920 bytes and session
storage 128 KiB. Each reconstruction's FCB contains 1,024 bytes of metadata,
22,528 bytes of CDF state and 1,536 bytes of CDEF state, totaling 25,088 bytes.
The singleton allocations do not have the same occupancy/lifetime as the reusable
per-picture pool. The candidates preserve those payloads and the FCB pool.

A private resource flag selects ordinary non-suballocated BOs for AV1 tier-2
singletons. Explicit alignment retains the old allocation classes' minimum
virtual-address alignment: 1 MiB for SDB, 32 KiB for CDF and 128 KiB for session
storage. The winsys can increase alignment further. Page rounding, BO caching
and fence-based retirement remain in the existing allocator. Other codec and
legacy DPB allocation policies are unchanged.

The packed challenger shares a 256 KiB resource between CDF at offset zero and
session storage at offset 128 KiB. The resource has two explicitly acquired owned
references, preserving ordinary error/close destruction. The session packet uses
the aligned offset; the CDF packet keeps zero. No offset fields are added to hot
per-picture records. Only the eligible AV1 tier-2 constructor installs that packet
handler. The shared allocation uses more CPU-mappable space than dedicated
buffers, while removing one BO; it is evaluated as a separate tradeoff.

The source audit also found that tier-2 command construction allocated FCBs and
then dereferenced them without checking failure. The negative control reproduces
a userspace crash after a real BO allocation failure. Preparation now validates
all auxiliary resources before picture-command emission, then stores the ready
reference span in the existing context-count field. Initialization packets can
already have established a valid firmware session, so error handling keeps that
session and its backing alive until the normal close operation. The terminal
error flag is set on every failed preparation. No incomplete picture command is
submitted merely to retire the error.

Fault injection matches actual BO size/domain/flags, with observed earlier
matches skipped when necessary. Tests also require the expected encoder error,
so failing an unrelated startup allocation cannot masquerade as coverage.
CDF CPU-map injection targets the allocated handle and clears it before handle
reuse. Arm/disarm occurs between closed GPU contexts; an atomic phase flag
publishes reset counters. These hooks exist only in the separate diagnostic
interposer, not in the normal driver or recorder.

The third challenger, `mesa-workspace-whole`, retains a single 2 MiB-aligned,
2 MiB BO. SDB occupies the prefix, default CDF starts at 1 MiB, and the session
starts at 1 MiB + 128 KiB. This preserves large-page eligibility and reduces
three BO references to one backing object, at the cost of more padding and
CPU-mappable storage. The pinned winsys `amdgpu_get_optimal_alignment` explicitly
increases alignment for its PTE fragment size. Eligibility is not proof of the
actual physical page mapping, and the kernel remains responsible for placement
and migration ([AMDGPU memory domains and BOs](https://docs.kernel.org/gpu/amdgpu/driver-core.html#memory-domains)).

That layout computes the full SDB size before initializing CDF. It uses the shared
block only for tier-2 AV1 whose complete SDB fits below the 1 MiB boundary; larger
and pre-encode workspaces retain a separate SDB and the packed CDF/session pair.
`radeon_vcn_enc_get_param` has no CDF dependency. Tier-2 has zero internal DPB
slots, so the legacy DPB-resize path cannot resize this shared resource. Every
alias owns a reference; session close precedes releasing any backing. CPU CDF
initialization and the VCN 5 CDF/session packets use matching offsets. The layout
is derived from resource identity, without per-picture offset fields. Existing
SDB-relative firmware offsets remain unchanged. Tests include QVBR, which enables
pre-encode and exceeds the shared prefix at the tested 640×480 geometry.

The web copy of Mesa's current GitLab implementation was inaccessible during
this continuation; conclusions above use the inspected pinned 26.2.5 sources,
not an unverified claim about a newer upstream implementation.

The concurrent regression uses a POSIX barrier only in the test harness. The
inspected glibc [wait implementation](https://raw.githubusercontent.com/bminor/glibc/master/nptl/pthread_barrier_wait.c)
completes fixed-count rounds with acquire/release ordering; the
[destroy implementation](https://raw.githubusercontent.com/bminor/glibc/master/nptl/pthread_barrier_destroy.c)
requires no unfinished round. Four workers rendezvous after opening their
encoders and again after their first submission, ensuring initialized sessions
coexist. The owner joins every worker before destroying the barrier. No barrier
or extra synchronization is added to the recorder's frame path.

## Conversion geometry and observed AV1 configuration

The CPU audit reads FFmpeg 9.0.2 `libswscale/input.c`, `output.c`, `utils.c`,
`graph.c`, `format.c` and the x86 dispatch. `SWS_FULL_CHR_H_INT` controls RGB
output; RGB input needs `SWS_FULL_CHR_H_INP` to retain full horizontal chroma
before position-aware filtering. The old path averaged pairs first. An even-x
red impulse therefore spread across two output samples instead of remaining
centered at the declared location. Full input fixes that case and selects SIMD
input routines. Odd dimensions still use rounded-plane size ratios in swscale.
The modern frame API retained that legacy implementation in these cases: the
pinned operations backend rejects subsampled/semiplanar formats in
`fmt_analyze()`. This is not evidence against a future composed operations path.

The selected converter filters integer RGB before applying the linear matrix.
Its horizontal weights are [1,2,1]/4 and vertical weights [1,3,3,1]/8, centered
at (2x, 2y+1/2), with edge extension. This is an explicit filter choice, not a
codec mandate. Q18 coefficients preserve neutral chroma exactly; their bounded
coefficient error plus final rounding stays below 0.505 code values at both
depths. Four planar rows of uint16 horizontal sums replace repeated neighborhood
work. Individual packed lanes cannot carry: their maximum final sum is 32*255.
Source/destination/scratch are disjoint and owned by the media caller. There is
no per-frame allocation. A scratch-free equivalent handles optional cache
allocation failure. Portable, x86-64-v3 and x86-64-v4 bodies are generated at
compile time; only the definition has the implementation-specific C23 attribute.
The actual generated clones are tested independently with unchanged instruction
bytes, in addition to the normal runtime-selected entry point.

Mesa's general RGB conversion also derived UV spacing from rounded output sizes.
The paired-kernel extension uses logical source bounds and guards partial 2x2
tiles. The even kernel remains separate, preserving its hot code. For the
unit-scale NV12/P010 compute fallback, the unrounded projection and rounded draw
coverage use existing viewport/rectangle fields. `nextafterf` keeps coverage on
the enclosing integer boundary. This corrects the fallback after optional shader
or constant-map failure as well as the fast path. Other formats and transforms
retain their existing paths. Both shader handles are independently destroyed;
the existing 64-byte constant record and discard-map invalidation protocol remain.

The first output audit reads FFmpeg `av1_parser.c`, `cbs.c`, `bsf.c`,
`bsf/extract_extradata.c`, `matroskaenc.c`, `matroskadec.c`, `movenc.c` and
`amfenc_av1.c`. VCN 5 encodes padded dimensions. Changing only the in-band
dimensions preserves the MI grid but changes prediction-boundary behavior;
the tested alternative produced accumulating edge errors and was rejected.
The valid design retains the coded picture and publishes the requested
presentation rectangle using standard crop metadata. FFmpeg's AMF AV1 path uses
the same container mechanism. Matroska defines crop before display sizing in its
[element specification](https://www.matroska.org/technical/elements.html#PixelCropRight).

`extract_extradata` retains packet payload when removal is disabled and extracts
the actual sequence configuration. A one-shot parser verifies first-key geometry,
depth and color before the mux header or replay metadata becomes available to a
consumer. Its input is borrowed only after buffer-range and zero-padding checks;
the original packet remains alive until parser close. Vulkan's opaque mapped
owner does not automatically satisfy that check. The opt-in parser flag leaves
ordinary callers' copy behavior unchanged. The asynchronous encoder option
`initial_output` drains the first issued picture without requiring a full FIFO;
later pictures retain the configured capacity. gpuKey uses it for buffered and
Vulkan startup, while normal VAAPI completion remains asynchronous.

The Matroska option `write_video_duration` emits positive video BlockDuration
without requiring DefaultDuration. gpuKey can therefore retain unknown average
frame rate instead of declaring a fictitious 1000 FPS. Replay snapshots already
know their complete span and provide Segment Duration before writing begins.
All new library options default off. Actual consumer checks distinguish file
correctness from player support: mpv honors crop metadata; tested Brave 155
ignores it for Matroska, MP4 and a WebM remux. Brave does honor the known replay
duration, including static holds. No malformed bitstream workaround is shipped.

## AV1 packed-header ownership

The pinned FFmpeg 9.0.2 `vaapi_encode_av1.c`, `cbs.c`, `cbs_av1.c`,
`cbs_av1_syntax_template.c` and `refstruct.h` were inspected together with their
callers and teardown. The current upstream
[VAAPI AV1 encoder](https://raw.githubusercontent.com/FFmpeg/FFmpeg/master/libavcodec/vaapi_encode_av1.c)
and [CBS AV1 writer](https://raw.githubusercontent.com/FFmpeg/FFmpeg/master/libavcodec/cbs_av1.c)
were also checked on 23 October 2026. The source still enables offset tracing
unconditionally and constructs a refcounted unit plus an assembled fragment.
Only non-CQP modes consume those collected offsets. The CQP change uses the
same predicate as the consuming code, leaving rate-control offsets intact.

The selected private helper writes a single OBU into caller-owned storage using
the existing syntax writer. It avoids the generic write buffer and the unit and
fragment payload allocations/copies. Sequence content becomes refcounted before
serialization, so the context's sequence pointer refers to owned storage even
after the caller's raw object expires. All acquired unit references are released
on success and failure. Required AV1 reference-state updates remain. Ordinary
CBS callers keep their existing fragment/redundant-header behavior.

VAAPI emits frame and show-existing headers, never redundant-frame-header OBUs.
Its context therefore opts out of caching redundant-header bits. That flag uses
existing padding on the tested ABI; it does not enlarge the 2,672-byte CBS AV1
context. The direct helper rejects a redundant-header request in this mode.
Short-buffer and malformed-input failures are terminal for that picture; the
caller does not retry mutated syntax state. The ordinary generic writer retains
its existing resize/retry path. Capacity still follows the encoder's existing
1,024-byte packed-header contract and fixed OBU-size-field setting.

Raw sequence, frame and metadata OBUs now live only during their synchronous
writer call. Persistent state retains the coded sequence/frame buffers and the
24-byte mastering payload, four-byte light-level payload and four-byte presence
mask. Header order and metadata subset selection are unchanged. The no-metadata
return precedes temporary OBU initialization. The existing syntax writer still
keeps a rollback snapshot; eliminating that in terminal-failure callers remains
an untested alternative, not a claimed improvement.

The public VAAPI handoff is captured by a diagnostic interposer that records
packed bytes and rate-control offsets, excluding handles and structure padding.
It is never linked into production. The standalone writer oracle compiles the
audited CBS sources, compares exact serialization and offsets, invalidates raw
input immediately, checks short buffers/allocation failure/teardown, and uses
dav1d to decode transformed hidden/show-existing sequences. This checks the
mechanism even though this device does not exercise hardware AV1 B encoding.
The native probe also covers changing metadata subsets. In the fixed driver,
metadata is absent on the first decoded picture and present afterward; the
candidate preserves that observation and does not claim new HDR support.

## Command-buffer lookup ownership (evaluation in progress)

The pinned Mesa 26.2.5 `amdgpu_cs.cpp` and `amdgpu_cs.h` put one 32,768-entry
`int16_t` hint array in `amdgpu_cs`, with both command contexts pointing to it.
`amdgpu_cs_submit_ib` resolves slab backing buffers and inserts sparse backing
buffers using the same lookup/update helpers as the recording frontend. The
frontend clears the shared array after queuing asynchronous submission. Thus
the array is neither context-owned nor immutable during submission. A cleared
negative hint can incorrectly hide a buffer in the worker's still-live list.
The [upstream source mirror](https://github.com/chaotic-cx/mesa-mirror/blob/main/src/gallium/winsys/amdgpu/drm/amdgpu_cs.cpp)
was checked on 23 October 2026 and retains this arrangement; the primary GitLab
raw endpoint was inaccessible. The pinned source is the authority for these
packages. No upstream acceptance or whole-driver race-freedom is claimed.

The corrected candidates keep the original BO lists, references, usage flags,
GPU fences and queue dependencies. The optional lookup acceleration never owns
the BO. `util_queue_add_job` publishes the context through the queue mutex. The
worker completes its list/cache access before `util_queue_fence_signal`; context
reuse and destruction wait through `amdgpu_cs_sync_flush`. The futex implementation
can observe completion with a relaxed atomic load, so an acquire fence after
that wait establishes consumption of worker writes. It adds no extra wait.
The standard mutex-backed fence remains compatible with the additional acquire.

Four alternatives remain explicit. Independent fixed tables provide the simple
ownership control at up to twice the old table storage. Adaptive context-owned
hints use exact linear lookup for tiny lists, allocate/rebuild as lists grow and
retain the original 64 KiB per-context hint cap. Exact per-list open-addressed
maps store only 32-bit owned-list indices, avoiding full-key copies and linear
collision fallback; their memory instead scales with the lists. Their integer
hash uses the documented `lowbias32` permutation from
[hash-prospector](https://github.com/skeeto/hash-prospector), not a new hash claim.
The shared candidate retains the 64 KiB table with relaxed atomic accesses,
validates positive hints, and makes negative hints untrusted on the worker. It
clears the touched subset or full table only between completed and newly
published jobs; no-op flushes must not reset a live worker's table.

The adaptive and exact caches are optional. Allocation failure retains linear
or validated old-table lookup and suppresses repeated growth attempts for the
current batch. Rebuilding uses all live owned entries; clearing a resized cache
without rebuilding would introduce false absence. Failed-cache flags are cleared
only when their context is reusable. Reset/free remain proportional to actual
allocated state. Tests include duplicate IDs, wrapped 15-bit indices, paused
workers and independent context destruction. Benchmark scales are not resource
limits. Fresh native and helper comparisons select adaptive hints as the compact
bounded-cache default. Exact maps remain available for their stronger collision
behavior, alongside the fixed/shared controls; native rankings remain mixed.

A separate audit found pre-existing worker handling defects when the essential
BO-list allocation itself fails: slab backing insertion can return NULL before
an unchecked usage update, and the sparse path unlocks then continues through a
NULL result. Those paths are unchanged by this optional-cache patch and remain
an explicit follow-up. Cache-failure coverage must not be presented as proof of
all driver out-of-memory paths.

## Idle GPU capacity and dependency lifetime audit

The capacity integration uses the pinned FFmpeg 9.0.2 `libavutil/buffer.c`,
`refstruct.c`, `hwcontext_vaapi.c`, and the VAAPI encoder's input, reconstruction,
output and flush paths. `nix/ffmpeg-capacity.nix` retains the preceding header
patches. The public pool interfaces in the inspected pinned source and upstream
headers do not provide a trim operation. The local additions preserve active
leases and do not impose a permanent capacity limit.

`av_buffer_pool_trim` and `av_refstruct_pool_trim` detach excess unused entries
under the existing pool mutex. An internal pool reference spans destruction
outside that mutex. This is necessary because a free callback can uninitialize
the external owner; merely unlocking before freeing would not keep the pool
alive. The reference is released only after the detached list is destroyed.
Concurrent returns can repopulate the pool: the operation trims a snapshot.

The selected coded-output policy uses the existing RefStruct return operation.
`AV_REFSTRUCT_POOL_FLAG_SINGLE_IDLE` detaches the previous cached object while
holding its existing mutex, retains the newest returned object, and destroys the
old object outside the lock. The returning lease already pins the pool. A free
callback may destroy the newly cached object in place and uninitialize the owner;
the return routine does not dereference that object afterward. Held picture and
hidden-frame references still prevent premature return. There are no additional
per-entry fields or separate per-output trimming locks. Generic explicit trimming
remains available through `capacity-amd` for comparison.

Closing a coded-output buffer invalidates its borrowed completion FD. The VAAPI
encoder increments a read-only `async_fd_generation`; gpuKey checks its declared
AVOption type and clears cached GLib watches before waiting on a new generation.
Codec operations and GLib dispatch are serialized by the recorder owner. No
closed FD is intentionally kept as a valid cached watch, and no duplicated-FD
lease is introduced. Native repeated-backlog checks exercise reuse and closure.

Compact input records form a dense live array and materialize on demand. Trimming
requires all submitted output to have been collected; it excludes the latest
image, prepared-but-unsubmitted input, and any buffer with an external reference.
Frames remain valid through submission; callers needing longer image ownership
must retain a frame reference. Capacity growth is checked, and failure stops new
admission while draining valid accepted output. Teardown frees only constructed
frames and uses the actual allocated handle-array size. Large configured limits
are not eagerly materialized in compact mode.

The actual pool sources pass ASan/UBSan and TSan lifetime probes covering held
clones, concurrent reuse/trim, allocation failure and reentrant destruction.
Removing the temporary trim reference independently fails both pool oracles.
Native Clang checks cover both bit depths, prepared input during collection,
retained images after codec/device-owner teardown, and capacities up to one
million. These are scoped tests, not a sanitizer claim for all FFmpeg or Mesa.
The private VA interposer is diagnostic-only; it never enters production builds.

## Cross-recorder source audit

GSR is a comparison program, not a new runtime dependency. The local editable
checkout was left untouched. Clean 6.1.3 and 6.1.4 tag exports were inspected;
an upstream query then found 6.1.5 and the current commit
`ee2c6ca1e03e4432652fe9492b3a405f654b45ac` (23 October 2026). The current source
was fetched independently and built with GCC 16.2 and FFmpeg 9.0.2. The installed
6.1.3 uses FFmpeg 9.0.1; its absent derivation prevents reconstructing its complete
package provenance from that metadata. Initial installed-version runs are kept
separate from the clean-source confirmations.

The inspected GSR VAAPI path creates one input image, exports its planes into GL
textures and renders conversion into those textures. Its VAAPI options select
asynchronous depth three; the comparison also tests depth two. Its PipeWire code
duplicates DMA-BUF FDs, imports images as used, and does not request a specific
buffer count. Under the fixture's two-to-eight offer it gets eight source buffers.
gpuKey requests two. An eight-buffer source allocation therefore cannot be inferred
from the number of imported images currently counted in the consumer's DRM client.
The tests record source allocation bytes independently and also repeat with a
fixed two-buffer offer. FD duplication alone is not a proof of image-content
ownership; the comparison does not adopt another program's lifetime assumptions.

GSR's limited-range conversion uses custom matrix coefficients and chroma scaling
by 219 rather than 224. The source is identical between 6.1.4 and the inspected
current commit. A separately identified comparison build changes only those
limited-range constants to BT.709 Kr/Kb and the specified Y/Cb/Cr code ranges.
It is a control for that color difference, not an upstream fix or a claim of
identical spatial filtering. The benchmark's source is tagged sRGB; GSR's exposed
FFmpeg options set the matching transfer tag, native quantizer 32, no B pictures
and the selected depth. The actual frames are decoded independently. Constant
patch checks do not establish full-scene rate-distortion equivalence.

The real portal implementation's `xdp_request_on_signal_response` directs
responses to `request->sender`. The fixture now follows that behavior while
retaining its response-before-method-return race test. A broadcast mock exposed
GSR's differently capitalized match rule and was unsuitable as a compatibility
benchmark. The pinned portal implementation was inspected at
`/nix/store/w5w5n1ra3xlzhnrr61f92kyl8d7kdp6s-source/src/xdp-request.c`.

The [DRM fdinfo specification](https://docs.kernel.org/gpu/drm-usage-stats.html)
requires deduplication by device/client ID. Shared memory can overlap processes;
it is not automatically another process's private allocation. Engine counters
are cumulative busy nanoseconds, can temporarily regress, and are optional.
The observer records supported counters; summaries retain the maximum per client
before summing. Sampling can miss final work before a client closes and cannot
prove instantaneous peaks, energy, capture latency or game interference.

## Quantizer capability and RGB conversion audit

The [AV1 specification, section 5.9.21](https://aomediacodec.github.io/av1-spec/av1-spec.pdf)
infers `ONLY_4X4` transform mode for coded-lossless frames. The pinned FFmpeg AV1
VAAPI writer uses zero quantizer deltas and no segmentation, so its zero base
QIndex invokes that rule. `ffmpeg-quantizer-vaapi.patch` checks the reported
transform capability during initialization and selects the inferred mode for
zero-QIndex headers. The tested radeonsi driver advertises `SELECT` only in
`src/gallium/drivers/radeonsi/mm/si_mm_screen.c`. Native positive lossless support
on another device remains unverified. Vulkan's reported QIndex interval is
checked before accepting an explicit request; the recorder no longer relies on
the library silently clamping zero to one.

The pinned Mesa source's `src/amd/vulkan/radv_video.c` restricts VCN 5 to the
BT.709/limited pair because its firmware also supports BT.2020/full but not both
cross-pairs. Its conversion flags would otherwise advertise a Cartesian product.
The private full-range prototype changes only this advertised subset. The
[Vulkan RGB conversion definition](https://docs.vulkan.org/refpages/latest/refpages/source/VkVideoEncodeRgbModelConversionFlagBitsVALVE.html)
applies a numerical model conversion without applying a transfer function.
The experiment therefore preserves the source's BT.709 primaries and sRGB
transfer while signaling the selected BT.2020 nonconstant-luminance YCbCr
matrix and full range. It does not claim an HDR or wide-gamut source.

The live Niri executable's derivation resolves to source
`/nix/store/bazsf2g3j9imcrqrw18p3zn9911s7lfm-source` (26.04).
`src/screencasting/pw_utils.rs::make_video_params` offers BGRA/BGRx and the
corresponding RGB DRM formats. There is no producer-native NV12 offer in this
implementation. This constrains the current deployment, not AV1 or the design
space of a future producer/capture interface.

The standalone native probe also needs the same explicit producer readiness check
as the recorder. In Mesa's `src/gallium/frontends/va/buffer.c::vlVaUnmapBuffer`,
derived image writes call `pipe->flush(..., NULL, 0)`: the flush does not store a
new fence in the surface. `surface.c::_vlVaSyncSurface` only waits recorded
surface fences. Consequently the probe polls each exported DMA-BUF for read
readiness after upload. This fixes its missing cross-API dependency rather than
weakening decoded-output equality. The production recorder already performs
that wait through its event loop and is unchanged by this test repair.

## Advertised full-range RGB profile and consumers

`mesa-rgb-full-range.patch` is a separate process-local profile. It advertises
only BT.2020/full, the other working firmware pair identified in the material
and pinned Mesa source. The normal driver remains unchanged. The recorder never
requests a pair missing from the queried masks; this follows the
[session conversion valid-usage rules](https://docs.vulkan.org/refpages/latest/refpages/source/VkVideoEncodeSessionRgbConversionCreateInfoVALVE.html).
The selected pair occupies existing padding in the cold import state. Frame and
codec metadata use the same plan, and first-picture parsing checks the actual
matrix/range before container metadata is published. Source primaries and transfer
are retained rather than inferred from the YCbCr matrix.

Consumer inspection found two additional conversion choices. mpv 0.41's
[zimg adapter](https://raw.githubusercontent.com/mpv-player/mpv/v0.41.0/video/zimg.c)
substitutes BT.709 for sRGB on RGB output in its default fast mode. Its image
writer still tags the output with the original color properties. The measured
full-range PNG values change with `--zimg-fast=no` or `--sws-allow-zimg=no`, while
the encoded YUV values stay unchanged. This is an image-export finding, not a
measurement of mpv's ordinary GPU display route.

The [libyuv conversion table](https://chromium.googlesource.com/libyuv/libyuv/+/refs/heads/main/source/row_common.cc)
uses a reduced blue chroma coefficient for BT.709 limited range unless an
unlimited-coefficient build option is enabled. The observed headless Brave
software conversion matches this approximation: a decoded blue patch of
Y=31, Cb=239, Cr=117 renders blue near 239 instead of the independently computed
252. WebCodecs `copyTo` confirms that the decoded YUV samples themselves match
FFmpeg. Both canvas readback and a compositor screenshot show the difference.
The reported renderer is SwiftShader with software video decode/compositing;
these checks do not establish the behavior of an accelerated desktop session.
BT.2020/full's coefficients fit this conversion path more accurately in the
tested patches. No source color tags are falsified to compensate for a consumer.

The browser check uses the documented
[DevTools Target API](https://chromedevtools.github.io/devtools-protocol/tot/Target/)
and Chromium's
[NUL-delimited pipe transport](https://raw.githubusercontent.com/chromium/chromium/main/content/browser/devtools/devtools_pipe_handler.cc).
It creates an isolated profile and loopback server, uses no existing browser
session, and closes its own process and descriptors. Pixel colors are measured
in an explicit sRGB output configuration, with decoded identities checked against
the private producer. CDP logs include the actual browser version and renderer.

## Vulkan pooled-buffer ownership and idle output retention

The pinned FFmpeg `libavutil/vulkan.c::ff_vk_get_pooled_buffer` originally used
the borrowing `FFVulkanContext` as pool callback data. That context can be gone
when an externally retained packet releases its buffer. The native before/after
encoder-close control reproduces this boundary failure. The
[AVBufferPool contract](https://ffmpeg.org/doxygen/trunk/group__lavu__bufferpool.html)
keeps the pool alive until its outstanding buffers return; it does not extend
the lifetime of arbitrary borrowed callback data.

`nix/ffmpeg-vulkan-buffer-owner.patch` therefore passes an explicit owning device
reference at every pooled-buffer call site. Encoder contexts derive it from
their frames; device transfer contexts receive it from the calling frames
context. A shared pool owner pins that device and caches only UnmapMemory,
DestroyBuffer and FreeMemory. Pool destruction runs after every buffer callback,
so those callbacks retain both the device and their dispatch through their final
use. The 32-byte owner plus AVBufferRef is cold, once per pool. Generic pool
get/return/trim synchronization remains unchanged; no lock-free claim follows.

`nix/ffmpeg-pool-standby.patch` exposes a runtime integer `async_trim` policy:
zero retains unused capacity, one retains one spare, and two retains no unused
coded buffers. Trimming reaches only unused pool entries. VAAPI retains its
existing single-idle return policy and increments descriptor generation when a
coded-buffer FD closes. Vulkan completion FDs belong to execution contexts and
are unaffected by freeing coded storage. gpuKey's one media owner changes policy
after all encoded output is collected and the CPU key owns static presentation;
new input restores the one-spare policy before submission. The controller checks
descriptor generation before polling. This preserves reference images, active
buffers, codec state, latest-image fallback and the full next-output capacity.

Vulkan video-session memory is a different lifetime: its
[bindings cannot be rebound](https://docs.vulkan.org/refpages/latest/refpages/source/vkBindVideoSessionMemoryKHR.html).
The implementation does not free session workspaces to obtain the coded-buffer
saving. Reconstruction-image reclamation has a separate slot/view lifetime, as
described below.

Fatal codec API failure is also distinct from an input/capture error. The native
post-idle allocation fault shows that another drain call can reenter a partially
updated hardware reference graph and assert. `media.c` now records that failure,
finalizes the already collected prefix, frees the codec, and returns the original
error. Healthy encoders still drain after capture/input failure. The software
send/receive fault regression independently checks call order, decoded pixels,
timestamps and endpoint, and rejects the preceding implementation.

## Vulkan reconstruction reclamation

`nix/ffmpeg-dpb-idle.patch` applies only to the existing queue-owned AV1 DPB path:
one encode queue, I/P pictures without B pictures or `p_to_gpb`, and separate
reference images. `vulkan_queue_dpb_slot` already determines which pictures the
current references and `next_prev` still need. Retirement bits share its temporary
initial-layout word; the patch adds no persistent field or GPU submission.
The selected slot is omitted from the retirement mask at selection time. Ordinary
pictures with no retired slots skip the extra list scan entirely.

The [BeginVideoCoding contract](https://docs.vulkan.org/refpages/latest/refpages/source/vkCmdBeginVideoCodingKHR.html)
deactivates an active slot when its reference entry has a null picture resource.
VUID 07239 also requires every nonnegative entry to identify an active slot, so
emitting deactivation entries for already inactive slots is invalid. The patch
marks only previously active retired slots, excluding the slot that the current
reconstruction replaces. Those entries extend BeginVideoCoding's list, not the
codec's actual reference list. The existing fixed local array has room for these
three-slot schedules. Pinned Mesa's `radv_video_enc_begin_video_coding` performs
no additional firmware operation for the retirement entries; this is not a
claim about other drivers.

The [image-view destruction contract](https://docs.vulkan.org/refpages/latest/refpages/source/vkDestroyImageView.html)
requires all submitted uses to finish. Queue-owned DPB images have no per-command
frame reference, so a vacant CPU slot alone is insufficient. After a successful
output/readiness result, an empty issued-output FIFO proves completion in this
encoder path: output readout waits for the execution result. Only then can standby
destroy unused views, release their frames, and trim the unused frame-pool entries.
Live references retain their views/images. Fatal submission/readout failures skip
this reclamation path; gpuKey performs no subsequent encode/drain calls on that
failed codec, and full close waits for execution resources before destruction.
Partially constructed images have a separate initial-layout submission: the
frames backend's `vulkan_frame_free` waits on each image's timeline semaphore
before destroying it. Thus a replacement-view failure also resolves that
initial-layout work, even though no encode references the new image yet.

On the next real input, gpuKey restores the active one-spare coded-buffer policy.
Reconstruction slots allocate lazily if their images were reclaimed. Native tests
cover multiple retirement/restoration cycles, forced independent pictures, 8/10-bit
input, exact decoded-picture equality, outstanding packet ownership and failure
while creating a replacement view. The recorder's first post-standby reconstruction
allocation is separately faulted under ASan/UBSan. Memory reduction and allocation
latency remain separate objectives; retained capacity is still available.

## RADV header-template writer

The pinned Mesa `radv_video_enc.c` originally accumulates bits into bytes and then
read/modifies a command word for each byte. Its emulation-prevention mode is
never enabled by any caller in this driver; `bits_size` has no reader, and the
byte-alignment helper has no caller. Those unused fields and stages are removed.
The command stream ultimately addresses a GPU buffer through
`radv_amdgpu_cs_get_new_ib`; its placement and CPU mapping policy matter to the
cost of read/modify/write access. A cached-memory microbenchmark alone does not
represent all of those costs.

`nix/mesa-encode-bits.patch` accumulates in host encoder state and emits complete
32-bit words with zero-filled final padding. Each firmware COPY consumer needs
the current fragment's exact length. Flush returns that length and resets the
fragment, so the pending position is the bit count modulo 32. This removes the
separate position counter and the H264/HEVC consumers' cumulative subtraction.
All eight slice-template copy consumers and the AV1 copy consumer are updated.
The pending word and fragment length occupy one aligned eight-byte record; the
full encoder state falls from 72 to 48 bytes on the tested x86-64 ABI. This is
private driver state, not a public ABI or a whole-process RSS estimate.

The primitive preserves the original 0--32-bit input domain and low-bit masking.
Zero-width writes do nothing; full-width and crossing-word writes avoid shifts
by the word width. Flush counts meaningful bits, excludes physical word padding,
and starts the next fragment empty. No allocation, GPU submission, buffer-capacity
change, or new synchronization/lifetime rule is introduced.

The independently tested conventional accumulator remains available as
`rgb-word-amd`. It retains a cached position and cumulative count, using twelve
bytes of writer state and 56 bytes of encoder state. Native tests preserve exact
AV1, H264 and HEVC packet bytes and decoded pictures for both designs. The CPU
and memory/code-size alternatives are recorded in the validation results; neither
the compact record nor isolated throughput alone proves a universal winner.

`tests/encode_bits_probe.c` uses bodies extracted from the pinned original source
and the actual patch. Its bit-by-bit oracle checks padding, reuse, partial resets,
all boundary widths and each consumer-visible fragment length. The old writer's
cumulative count is converted by the same subtraction its real consumers used;
the candidate's returned length is checked directly. The executable source
checker runs under GCC, Clang and sanitizers through the flake checks.

The preceding resume audit used Vulkan entry-point timings as well as libdrm
allocation tracing. RADV's native mapping path calls the mmap ioctl directly,
so tracing `amdgpu_bo_cpu_map` alone did not measure it. Three instrumented runs
put coded allocation near 515 microseconds and mapping near 21 microseconds.
[Vulkan's mapping contract](https://docs.vulkan.org/refpages/latest/refpages/source/vkMapMemory.html)
permits mapping without checking device completion, while requiring synchronization
before host access. Deferred mapping remains possible but had a small available
overlap in this workload. All three first-resume source polls were already ready,
so those probes offered no producer wait in which to hide the larger allocation.

The pinned RADV capability code does not advertise reliable
[insufficient-buffer-range detection](https://docs.vulkan.org/refpages/latest/refpages/source/VkVideoEncodeCapabilityFlagBitsKHR.html).
Also, an unsuccessful encode leaves its reconstructed reference invalid under the
[video-coding contract](https://docs.vulkan.org/spec/latest/chapters/videocoding.html).
Smaller-output-buffer retry would therefore need a separate detection and recovery
design, including already queued dependents. The delivered writer optimization
keeps the existing coded-output capacity; it does not substitute an observed
average packet size for that reservation.

## Cached static output and raw input retirement

The recorder declares independent-only static refreshes when its configured
interval is at least the existing two-second independent-frame cadence. The
one-second default retains the previous P-picture refinement behavior. Internal
callers that leave this new option unset also retain their previous behavior.
The independent-only option accepts shorter monotonic refresh requests as keys;
it does not silently omit them when the raw image has been released.

Compact mode records whether the latest accepted input was a requested static
key. A later real input clears that state. Only after every accepted input is
collected, and with no unsubmitted preparation to disturb, does it validate and
retain the completed independent key. Cache setup is optional until commit: any
packet/props/reference allocation failure leaves the raw fallback available.

For Vulkan files, a successful CPU copy and a second packet reference replace
the pending mapped packet with identical CPU-owned data and properties before
GPU storage is trimmed. Pinned FFmpeg `packet.c` preserves timestamps, duration,
flags, side data and opaque references in `av_packet_copy_props`; a failed
`av_packet_ref` unrefs its destination. `av_packet_move_ref` requires an empty
destination, which the caller explicitly establishes. `avcodec_receive_packet`
unrefs its packet argument before receiving, including the empty-output case.
Replay instead references its already retained CPU payload. These rules let the
transition occur when the first static key completes, rather than waiting for
its next repeated presentation.

VAAPI then frees completed input frames whose buffer refcount is one, excluding
any prepared image, and trims unused frame-pool entries. The pool may reach zero
and restores a full input on the next preparation. VPP was created with no fixed
render-target list and selects the actual target at each `vaBeginPicture`.
Direct RGB drops its last-image pin and the recorder immediately retires the
corresponding capture lease. Its persistent import remains cached; shared source
storage is not counted as freed. Live codec reconstruction references remain.
Both new flags fit existing padding: media/options stay 2,280/192 bytes on x86-64.

After raw retirement, a later failure to retain a cached output packet stops with
an error and finalizes the completed prefix. Retained-raw operation can still
fall back to encoding. Native injected failures verify both outcomes. A newer
prepared image does not invalidate an existing cached key until accepted input;
the API regression checks the displayed timeline across that boundary.

## Synchronous Vulkan transfer and uploaded input/staging lifetimes

In the pinned `libavutil/hwcontext_vulkan.c`, `vulkan_transfer_frame` waited for
GPU completion on downloads (`!upload`) but returned immediately after queue
submission on uploads (`upload`). Because `FRAME_TRANSFER` uses two execution
contexts, each upload attached a clone of the caller's software `AVFrame`
(`sw_frame_deps`) and the imported host-pointer or staging `FFVkBuffer`
(`buf_deps`) to the execution context until that slot wrapped two transfers
later. That left the software buffer with `refcount > 1` on return, pinned two
host-pointer GTT imports (`21.10 MiB` at 8-bit portrait or `42.19 MiB` at
10-bit portrait) or two `fp->tmp` staging buffers simultaneously, and retained
both across static standby even after the caller freed `m->staging`.

`nix/ffmpeg-vulkan-transfer.patch` waits for `exec` completion on both transfer
directions before returning and omits the redundant execution-context software
frame and buffer dependency clones (`swf` and `bufs` remain live in the caller
and local stack frame through `ff_vk_exec_wait`). `vulkan_frame_free_cb` also
trims `fp->tmp` when an `AVVkFrame` is freed, so trimming the frames pool in
standby reclaims idle non-host-mapped GTT staging buffers while steady-state
active motion reuses its pooled staging buffer without per-frame frees.

In `src/media.c`, uploaded Vulkan now uses `fc->initial_pool_size = 0`, enables
`queue_dpb = 1` when supported by the installed `av1_vulkan` encoder, and
participates in compact input management (`compact_inputs`). Because uploaded
Vulkan preparation requires `m->pool[i] != m->last` so an unsubmitted
preparation cannot overwrite `m->last`, a naive zero-spare policy freeing every
writable frame `!= m->last` after each packet causes per-frame `VkImage`
allocation and destruction churn. `release_idle_inputs` therefore retains one
writable spare alongside `m->last` while `m->last && !m->prepared`, and trims
excess frames after an overlap burst. When compact static standby clears
`m->last`, `spare` becomes zero, releasing all idle upload images and freeing
`m->staging` (on both VAAPI and Vulkan) when `!m->prepared`. `ensure_staging`
restores `m->staging` on demand when real input resumes. On uploaded paths
(`!m->source.dmabuf`), `submit_video` also collects completed output before the
pre-send pending-packet check so encodings that finish during multi-millisecond
CPU conversion/upload can reuse a single coded output buffer.

`tests/vulkan_upload_probe.c` and `tools/extract-vulkan-upload.py` extract the
actual transfer tail, `vulkan_frame_free_cb`, `ensure_staging`,
`release_idle_inputs` and `release_static_resources` bodies and check them
under GCC and Clang with ASan/UBSan (`nix/vulkan-upload-check.nix`), while the
unpatched transfer source fails the post-transfer single-owner/synchronous-wait
property as a negative control.

## VAAPI reconstruction ownership

`nix/mesa-vaapi-surface-dpb.patch` and `nix/ffmpeg-vaapi-dpb-idle.patch` form a
paired, process-local Mesa/FFmpeg profile. The pinned VAAPI reconstruction pool
is dynamic and supplies no fixed render-target array to `vaCreateContext`.
FFmpeg retains picture metadata and surface handles longer than Mesa needs
every physical reconstruction. Giving each such handle a dedicated buffer
therefore raised active memory in the first prototype. The selected revision
keeps the original cross-surface reuse mechanism.

An active Mesa DPB entry has a nonzero surface ID, codec order hint, and buffer
borrowed from that surface. An idle entry has ID zero; its otherwise unused
order-hint word records the owning surface ID. Empty entries have no buffer.
This multiplexing preserves the 16-byte x86-64 entry and adds no allocation or
per-entry field. Reusing the cached surface itself restores its active entry;
transferring its buffer to another surface clears the old owner's buffer/DPB
flag before activating the new owner. A surface destructor removes either its
active entry or its idle borrowed entry before freeing the backing. Context
destruction leaves surface-owned buffers for their eventual surface destructors.
Buffers from a closed encoder are recreated when used with a new context;
an association with a different live encoder is rejected.

The field audit follows `picture_av1_enc.c`, `surface.c`, `context.c`,
`radeon_vcn_enc.c`, `radeon_vcn_enc_5_0.c`, and the D3D12 AV1 consumer. The
inspected Radeon order-hint consumers address codec references, whose entries
are active; they do not consume the idle owner ID as an AV1 order hint. Buffer
transfer retains the existing ordered encode queue and winsys resource
references. No new GPU synchronization or unsynchronized surface traversal is
introduced. Driver/display ownership and existing mutex serialization remain.

FFmpeg trims only unused reconstruction-pool entries when `async_trim == 2`
and receive returns EAGAIN. Its picture-reference accounting releases those
leases only after completion and reference retirement. gpuKey selects that
policy after all accepted input is collected and an independent CPU key owns
static presentation. Live references remain leased. This is tested with the
pinned Mesa profile, not a portability claim about mid-context destruction on
every VA driver; the [VA-API lifetime documentation](https://intel.github.io/libva/group__api__core.html)
requires surfaces to outlive contexts that still use them.

The source-based ownership check preserves the real signed loop indices,
byte-sized DPB counters and 32-bit ID/order fields. It tests all eight reference
slots plus the current picture, full-width valid IDs, self/cross-surface cache
reuse, reuse across an empty slot, failed construction, invalid/live-foreign
contexts and both destruction orders. Normal and ASan/UBSan builds pass under
GCC and Clang. Its old-source negative control rejects the surface-ownership
property, not the original driver's general correctness. Actual GPU completion,
pixel semantics and concurrent encoder lifetimes are checked separately.

Current upstream GitLab source URLs were inaccessible through the browsing
tool during this audit; the implementation evidence is pinned Mesa 26.2.5 and
FFmpeg 9.0.2, not an assertion that no successor implements the same approach.

## Independent static-key reuse

The [AV1 specification](https://aomediacodec.github.io/av1-spec/av1-spec.pdf)
requires a reference to be showable before `show_existing_frame` can display it.
A displayed keyframe is inferred not showable; even a hidden showable key can be
displayed through that mechanism only once. The rejected prototype violates that
rule. dav1d accepts it, but libaom rejects it with a non-showable-buffer diagnostic.
The application does not emit that prototype.

The selected path repeats a complete, independently decodable key only when no
new encoder input has intervened. The eligibility scan requires one displayed
key picture, base-layer headers, compatible single-operating-point sequence
configuration and no coded timing/still-picture restriction. It rejects metadata
that could carry additional temporal semantics. A key resets reference identities,
order hints and entropy state; replaying its exact bytes recreates the same state
that the idle encoder still holds. New accepted encoder input invalidates the
cache before another reuse. Tests compare decoded pictures after real input
resumes, not just the static interval.

The memory/lifetime integration was checked against FFmpeg's packet/ref APIs.
Replay lends a CPU-buffer reference to its newest independent packet, and the
cached record is dropped on new input or teardown. Explicit packet-copy controls
are honored. Vulkan file output copies the encoded payload and properties once;
retaining the original driver-owned output would pin a much larger allocation.
Cache allocation failure leaves the original encoding path available. Encoder
completion accounting uses actual encoded packets; reused samples have a separate
counter and cannot falsely retire an in-flight input.

Export compaction only merges consecutive byte-identical independent keys and
rewrites a worker-private index. Snapshot blocks and their payload ownership remain
immutable. A terminal independent sample prevents observed immediate-EOS behavior
for one-picture clips in Brave. Audio still merges by its original per-stream
timestamps. The pinned `libavformat/movenc.c::check_pkt` rejects durations greater
than signed `INT_MAX`; compaction retains checkpoints for that implementation
bound even though MP4's serialized duration field is unsigned. Matroska retains
its wider bound. A 6,000-second probe verifies the resulting MP4 checkpoints.

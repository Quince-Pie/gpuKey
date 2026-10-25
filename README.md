# gpuKey

A C23 Wayland recorder implementing the ownership and capability-driven design in
[`material/`](material/research/Wayland_GPU_Recorder_Design.md). It records AV1 to
fragmented MP4 or Matroska, with optional explicitly selected audio. The executable,
Nix package, overlay and build targets are named `gpukey`.

```sh
nix develop
make PROFILE=release -j
./build/gcc-release/gpukey --probe
./build/gcc-release/gpukey recording.mp4
```

`gpukey` alone opens the source chooser and saves a dated MP4 in the current
directory. With audio selected and no filename, it chooses dated Matroska to
accommodate discrete channels. A `.mkv` filename selects Matroska automatically. `--help` shows common
tasks; `--help-all` exposes hardware, quality, budgets and test controls.

Choose a monitor or window in the desktop portal dialog. Ctrl-C stops admission,
drains the codecs, finalizes the container, and waits for accepted output writes.
Existing files are never overwritten. Failed partial files are retained and the
process exits unsuccessfully. `--durable` also synchronizes the file and directory.

The default path is **PipeWire DMA-BUF → persistent VAAPI import → GPU VPP → native
AV1 → libavformat → bounded buffered writer**. No CPU framebuffer readback is used
on that path. The encoder owner waits for the specific VPP destination before
returning the source lease; it does not block the PipeWire callback or idle the
device. Imported modifiers are preserved exactly, and imports are cached by buffer
generation and plane layout, not by reusable FD numbers.

On the tested Radeon system, `nix run .#amd -- replay 30 --display --fps 60`
uses the recorder with a process-local Mesa driver that reduces unused GPU
allocations and uses ACO without the unused LLVM runtime. It includes the verified
P010 range correction and converts eligible RGB input in one compute dispatch,
including partial edge tiles. Chroma sample spacing stays correct on odd-sized
images. Existing even-sized conversion checks remain byte-identical. It also wakes the recorder
when encoded output completes, so it can collect packets and reuse their GPU
storage without waiting for another captured frame. The system driver
is unchanged, and an existing `LIBVA_DRIVERS_PATH` override is respected.
AV1 tier-2 singleton workspaces use dedicated allocations, reducing measured
portrait/60 VRAM from 105.32 to 103.43 MiB without changing codec settings.
Allocation failures are checked before picture commands are emitted.
Command-buffer lookup caches belong to their recording/submission context.
They stay unallocated for tiny lists, grow when useful and retain exact lookup
if optional allocation fails. This removes the original shared-table race and
avoids clearing a fixed 64 KiB table on every small submission. Native timing
results vary; this is not a universal throughput or latency win.
`amd-hints-reference` preserves the preceding driver for comparison.
`amd-hints-exact` uses exact index maps, which perform better under heavy hash
collisions but can consume more memory for large lists. `amd-hints-fixed` and
`amd-hints-shared` retain the independently owned fixed-table and atomic shared
controls. These profiles use the same recorder and AV1 settings.
The ordinary build remains available for the system driver and other vendors.
For a rolling recorder that should release spare GPU capacity after a burst:

```sh
nix run .#amd -- replay 30 --display --fps 60 --gpu-memory compact
```

Compact mode releases idle VAAPI input images and excess VAAPI/Vulkan coded
buffers after their owners finish. It retains the latest image until a validated
CPU key can own independent static refreshes.
It restores capacity when overlap returns. In the controlled portrait/60 backlog test this
released 11.25 MiB of VRAM and 21.15625 MiB of GTT storage after the backlog cleared.
It does not remove the storage needed during active overlap. `--gpu-memory retain`
is the default and keeps capacity for later bursts; CPU, RSS and tail-latency
comparisons are mixed. `amd-capacity-reference` retains the preceding library,
and `capacity-amd` retains the explicit-trim implementation for comparison.
`--surfaces` remains a capacity limit; VAAPI and uploaded Vulkan compact mode
create frame records only when needed. Uploaded Vulkan retains one writable
spare alongside the latest submitted image during active capture so preparing a
frame does not overwrite the last image or churn allocations on every frame.
Compact standby releases all idle upload images, unused queue-owned
reconstructions, temporary Vulkan transfer staging buffers and the CPU
conversion staging frame, restoring them on demand when motion resumes. The
system driver is not modified.

With `--static-interval 2`, an eligible cached key can also let compact mode release
all unused coded-output buffers during a static hold. The transition occurs when
the first requested static key completes. VAAPI can also release its last input
surface; direct RGB returns its last source lease. A resumed frame allocates
that capacity again. At 2560x2880 this returns 21.15625 MiB of GTT in the tested
recorder. VAAPI also returns an 11.25 MiB input image: after-motion standby
VRAM falls from 103.43 to 92.18 MiB in the controlled portrait test.
Live AV1 references and capture storage remain resident. Use `retain`
when keeping that allocation ready is more important than idle memory use.
Optional cache-setup failure keeps the raw fallback. Once compact standby has
released it, a later cached-packet allocation failure stops with a valid completed
prefix. The one-second default still retains raw input for P-picture refinement.

The `vaapi-dpb-amd` profile also releases unused AV1 reference images during
compact standby. It preserves reuse during motion and restores images when
motion returns. In the portrait test this reduces standby VRAM from 92.18 to
71.09 MiB, with the same two shared capture images. It adds allocation work on
resume; the existing `amd` profile remains available for comparison.

```sh
nix run .#vaapi-dpb-amd -- replay 30 --display --fps 60 \
  --gpu-memory compact --static-interval 2
```

For the direct-RGB Vulkan profile, compact standby also releases unused
reconstruction images after an independent key. In the portrait/60 pause/resume
test, idle VRAM falls from **91.55 to 70.41 MiB**, retaining the current reference
and both shared capture images. Moving-image capacity returns on resume.

```sh
nix run .#rgb-amd -- replay 30 --display --fps 60 --backend vulkan --device 0 \
  --gpu-memory compact --static-interval 2
```

This is a standby-memory saving, with allocation work on resume. The
[memory model](docs/memory-targets.md) and [validation record](docs/validation.md)
separate that tradeoff from active memory, CPU and latency results.

The AMD profile is verified on the RX 9070 XT; LLVM-dependent shader workloads
should use the ordinary build or the LLVM-capable driver controls.
The RGB profiles now assemble firmware header templates in a compact host
accumulator and write complete words to command memory. The source-based tests
check every fragment length as well as emitted bits. `rgb-word-amd` retains the
tested conventional accumulator, which uses an extra cached position; the
[validation record](docs/validation.md) distinguishes its CPU results from the
default's smaller state. Capture, AV1 settings and GPU image capacity are the same.
`nix run .#amd-readout-reference -- replay 30 --display --fps 60` retains the
previous FFmpeg mapping path for comparison. `amd-vpp-reference` retains the
previous VPP driver with the original full library; `amd-reference` retains the
earlier ACO driver. Saturated encode throughput is not claimed to improve.
`amd-workspace-reference` keeps the previous allocation policy with the same
recorder. `amd-workspace-packed` and `amd-workspace-whole` retain the measured
shared-workspace alternatives; their throughput ranking varied between runs.
`amd-chroma-reference` retains the driver before the odd-size conversion fix.
The default linked FFmpeg library includes the recorder's codec,
container and hardware backends; full ffmpeg/ffprobe tools remain in the shell.
`nix develop .#full` retains the original library configuration.
CPU images are created when a map or transfer needs them, so GPU-only frame
pools avoid unused mapping allocations. Synchronous transfers use borrowed plane
views and report failed writeback; the recorder stops before submitting a failed
upload. Persistent maps retain their own source/context references.
CPU input uses a fixed-geometry integer converter with a small four-row cache
and runtime SIMD selection. If that optional cache cannot be allocated, a
scratch-free implementation produces the same pixels. Default builds omit
swscale from the recorder's direct dependencies. The diagnostic
`make PROFILE=release CPU_CONVERSION_CONTROL=1` build retains swscale, including
its known odd-size sampling error, for comparisons.
AV1 packed headers use caller-owned storage and retain only the state needed by
later pictures. This removes unused CQP tracing, temporary serialization buffers
and persistent raw header objects while preserving the tested encoded bytes.
`nix run .#amd-headers-reference -- replay 30 --display --fps 60` retains the
previous header path with the same AMD driver; `header-trace` retains the simpler
tracing-only control. Native CPU improvements are modest, with unchanged VRAM
and no established throughput improvement.
These local library changes and their measured scope are documented in
[validation](docs/validation.md).

The first encoded picture supplies the actual codec configuration before the
file header is written. Hardware padding is preserved for decoding, with
Matroska crop or MP4 clean-aperture metadata retaining the requested picture.
mpv honors this metadata. Tested Brave 155 playback ignores it and shows the
padding for unaligned sizes; aligned display recordings do not need cropping.
Replay exports declare their known duration, including the final static hold.
Matroska stores explicit video durations without a fictitious frame rate.

The recorder prefers two capture buffers, negotiates more when a producer needs
them, and keeps capacity for two VAAPI inputs at the default encoding depth.
Completed inputs are reused before materializing a spare surface. Explicit
`--capture-buffers N` accepts ordinary counts, including three. `--encode-depth 1`
limits encoding to one input at a time; it can reduce maximum
throughput. `--surfaces N` retains explicit control over the input pool.

`--packet-readout auto` collects completed VAAPI output when supported and uses
completion events on the AMD profile. With a driver that cannot export completion
events, it collects at the next capture, save trigger or ordinary wake.
`available` selects that latter policy explicitly; `events` requires notifications;
`buffered` retains the original queue behavior. These controls preserve AV1 settings.
Events improve packet availability but can add wakeups; the measured CPU and
throughput tradeoffs are retained in [validation](docs/validation.md).

[Memory targets](docs/memory-targets.md) separate image payload, driver allocations,
system-memory bitstream buffers and encoded replay history. For the current
two-reference, 8-bit AV1 plan, the conditional image-payload target is 27.69 MiB
at 1920×1080 or 98.44 MiB at 2560×2880 when one completed input is reusable.
These include two shared RGB capture images, one converted input and three
reconstructions. If overlap materializes the second input, the targets become
30.65 and 108.98 MiB respectively. Stronger producer-native
YUV targets remain open; resolution and codec alone do not determine replay RAM.

## Instant replay

```sh
gpukey replay 30                 # keep the last 30 seconds; choose the display
gpukey save                     # save a dated clip while recording continues
gpukey save highlight.mkv        # choose a filename

# The same display, FPS and repeatable audio controls work in replay mode.
gpukey replay 60 --display --fps 120 --audio MIC_ID --audio DESKTOP_ID \
  --directory ~/Videos/Replays --replay-memory-mib 512
```

For an often-static screen, `--static-interval 2` permits eligible independent
AV1 keys to be reused without encoding the image again. Real source updates keep
their normal capture/FPS behavior. `--static-encoding always` forces ordinary
encoding at that same interval for comparison or to avoid a file-recording cache.
The one-second default is unchanged: intermediate P-pictures can refine a lossy
image and are not silently replaced by key copies. JSON stats distinguish actual
encoder `packets` from `reused_packets`.

Replay exports merge byte-identical independent keys into displayed holds and
retain a terminal sample for playback/seek compatibility. This can reduce the
number of video samples without removing an image change. Arbitrary dependent
packets and incompatible sequence configurations are not replayed or merged.

Press **Enter** in the recorder's terminal to save, or bind **`gpukey save`** to a
desktop keyboard shortcut. No shortcut or desktop setting is changed automatically.
`gpukey replay` defaults to 30 seconds, Matroska, no audio, and a 256 MiB live replay
budget. `--directory` defaults to the current directory; a missing final directory
component is created. `--session NAME` identifies another session; use the same
option with `gpukey save`.

Replay keeps **encoded packets in RAM**, without writing a media file until a
save. Each clip starts at the preceding verified AV1 random-access point, normally
up to one two-second keyframe interval before the requested window. During startup
it saves the available history. With audio, the endpoint is the latest completed
encoded time common to all selected tracks, which can be before the
trigger. This preserves synchronized, decodable clips without flushing or restarting
the live encoders; arbitrary frame-exact cuts are not offered. Opus exports include
decoder preroll and trim it with pre-skip; PCM is cut at sample boundaries.

One export runs alongside capture. `save` reports success after output has finished
(`gpukey replay --durable` also synchronizes it); another trigger during export reports busy.
Failed exports leave their partial files and rolling capture continues. Existing
files are never overwritten. **Ctrl-C or SIGTERM stops the session, completes an
accepted export, and discards unsaved history.** Save before stopping if you want
that history.

The live budget charges encoded allocations and chunk metadata. A pinned export
can retain up to that much again, plus one spare metadata block. Capture surfaces,
audio queues, codec/muxer workspaces, allocator overhead and index tables are
additional memory; this is not a process RSS cap. Insufficient live memory stops
explicitly instead of silently shortening the promised window. Static-repeat
intervals above two seconds are rejected in replay mode; a missing verified
keyframe for more than four seconds or an audio stream over a second behind video
also stops the session.

The CPU packet path transfers encoder-owned buffer references; Vulkan's mapped
GPU bitstreams are copied into CPU storage so replay does not retain GPU output
buffers. By default, consecutive byte-identical AV1 keyframes share immutable
payload storage, preserving each packet's original timing and flags. Savings
depend on content; changing pictures normally have distinct keyframes.
`--replay-storage reference` disables sharing, and `--replay-storage copy` also
copies every incoming packet. These controls let you compare memory and CPU costs.
Snapshot pinning,
remuxing and output happen without holding capture buffers. The implementation and
its measured scope are documented in [validation](docs/validation.md) and the
[source audit](docs/dependencies.md).

## Recording options

```sh
# Choose a particular display or window in the desktop's sharing dialog.
gpukey display.mkv --display
gpukey window.mkv --window

# Preserve the source transfer function, encode 10-bit SDR, cap capture at 120 Hz.
gpukey recording.mp4 --bits 10 --fps 120 --duration 30
gpukey recording.mkv --fps 60000/1001

# Explicit shared-memory fallback when a source cannot negotiate GPU imports.
gpukey --output recording.mp4 --allow-shm

# Discover audio inputs and output monitors without starting capture.
gpukey devices

# Add the microphone and desktop monitor IDs shown by that command.
gpukey recording.mkv --audio MIC_ID --audio DESKTOP_ID

# Exact PipeWire node names also work, including multiple system-output sinks.
gpukey recording.mp4 --audio INPUT_NAME --audio SINK_NAME --audio SECOND_SINK_NAME

# A professional interface's discrete AUX channels, kept in their original order.
gpukey interface.mkv --audio INTERFACE_NAME --audio-codec pcm

# Ordinary io_uring is an optional measured alternative, never a privilege requirement.
gpukey --output recording.mp4 --sink uring --durable --stats run.json
```

Run `gpukey --help-all` for budgets, source selection, rational frame rates, native
quantizers, and synthetic checks. Defaults are starting policies, not hardware
limits or universal quality settings. `--overload skip` discards only unsubmitted
video; `--overload stop` fails when observed content cannot be admitted. Encoded
reference-dependent packets are never arbitrarily discarded. Audio queue exhaustion
stops the session.

Each `--audio` selection creates a **separate named track**. Channels within that
device remain together, with their speaker identities preserved. Mono, stereo,
5.1, 7.1 and 16-channel AUX are covered by independent decoding tests. Audio starts off; neither a
microphone nor a desktop monitor is inferred. Inputs and sink monitors are
recognized automatically, resolved against the capture connection, and pinned by
object serial. Duplicate, ambiguous and missing selections fail explicitly.
`--audio-node` and `--audio-monitor` remain available as repeatable explicit forms.
Selecting several tracks does not mix them; ordinary players often play only one
audio track at a time, while editors can access all of them.

`--display` restricts the portal chooser to displays; select the desired monitor
there. `--window` restricts it to windows. ScreenCast v6 provides no portable
connector-name selector, so `--display DP-1` is not a supported command. Source
selection is requested each run; permissions and restoration tokens are not stored.

`--fps` is a source capture cap (1–1,000,000, including rational rates), not a CFR
conversion. Extra updates are rejected before the framebuffer copy or GPU work,
using fixed slots nearest the requested deadlines. Actual source timestamps stay
intact. Boundary slots can add one frame to a short interval's nominal count.
Static-image repeats have their own `--static-interval` policy.
If the last video update rounds to the same container tick as stop, it has no
representable display duration. After draining, that terminal packet is omitted;
the preceding frame already ends at that tick. This is counted as
`video_boundary_omitted`. Earlier reference-dependent packets are retained.

Color is SDR RGB with BT.709 primaries; sRGB or BT.709 transfer is preserved and
accurately tagged. Untagged RGB uses a disclosed sRGB default. Output is limited-range
4:2:0 at 8 or 10 bits. Ten bits do not restore discarded chroma or create HDR.

MP4 uses a 1 MHz video timescale. Its audio is 48 kHz floating-point PCM,
which has exact sample boundaries and costs **192 kB/s per channel**. Matroska uses FFmpeg's 1 ms
video timescale and Opus, with pre-skip and final discard padding. The independent
decode tests check exact audio lengths in both containers. PCM-in-MP4 player support
must be checked for the intended consumer; Matroska/Opus is the compressed-audio
alternative. Audio remains off unless selected; the restricted portal remote is
not used to discover unrelated audio sources.

Audio accepts negotiated input rates from 8 to 384 kHz and converts each track
independently to 48 kHz with bounded drift correction. Speaker order is normalized
without mixing channels. Opus validates its supported mono/surround layouts;
PCM supports additional layouts accepted by the MP4 muxer. Unsupported layouts,
unpositioned channels, mixed AUX/speaker layouts, and changes to a live source's rate/layout stop
explicitly. There is no silent stereo conversion. PipeWire may already convert
hardware audio to its graph format before it reaches the recorder.

**Discrete AUX channels use Matroska.** They retain the negotiated order and a
`GPUKEY_CHANNELS` track tag listing their identities, such as `AUX0,AUX1,...`.
Opus uses mapping family 255 with independent channels. `--audio-codec pcm`
instead stores uncompressed float audio after the declared 48 kHz conversion and
drift correction; this trades storage bandwidth for avoiding Opus encoding.
One 16-channel PCM track costs 3.072 MB/s before container overhead.
The pinned MP4 PCM muxer cannot express this discrete layout, so it rejects it
with a `.mkv` hint. For positioned surround PCM, use MP4; Matroska PCM does not
carry the necessary speaker map through this muxer. Mono/stereo PCM also work
in Matroska. Advanced consumers must retain the AUX label tag; ordinary players
may not support large discrete-channel tracks.

Each audio source uses 32 preallocated slots of at most 4096 frames: 512 KiB per
channel for capture payloads, plus the owner's one-second FIFO, conversion scratch
and codec state. Capture pages are touched before the RT callback starts using
them. These audio allocations are separate from the video `--cpu-budget-mib`
limit. Each source has its own SPSC queues, while sources share one capture
context and one encoder/mux owner. There is no added recorder worker per track.

## Supported paths and limits

| Path | Capture processing | Evidence in this workspace |
|---|---|---|
| VAAPI (AMD/Intel API) | Persistent exact-modifier DMA-BUF import and GPU VPP | Executed on RX 9070 XT / Mesa 26.1.8; Intel hardware not available |
| Vulkan AV1 | Explicit shared-memory/CPU conversion and GPU upload | Synthetic encode/decode executed on RADV |
| Optional Vulkan RGB AV1 | Persistent DMA-BUF input, hardware RGB conversion, three queue-owned reconstruction images | Native RX 9070 XT tests, including replay and audio; use the `rgb-amd` package below |
| NVENC AV1 | Explicit shared-memory/CPU conversion and NVENC-owned input copy | Builds and capability-fails gracefully here; NVIDIA hardware not available |
| Software AV1 | Explicit CPU control using libaom | Hermetic decoder, timing, and failure tests |

Use `--backend` to select an alternative. Missing AV1 support never silently selects
a different codec or software encoder. Native NVIDIA arrays,
VPL, explicit-sync early buffer return, HDR, crop/rotation processing,
source reconfiguration into new segments and multi-source composition are
not implemented. Unsupported source semantics stop explicitly. Only the implicit
DMA-BUF synchronization plan is negotiated; no release promise is fabricated.

The stock FFmpeg Vulkan AV1 backend has teardown leaks reproduced independently
of gpuKey; see the [dependency limitation](docs/validation.md#startup-negotiation-and-empty-encoder-shutdown).
The optional RGB package includes the audited Vulkan cleanup fixes. Its native
sanitizer boundary and comparison results are recorded in [validation](docs/validation.md).

The audio path expects PipeWire timestamps in the same clock domain as the selected
video source. It uses populated source timestamps, or the buffer's graph-cycle
timestamp with graph/converter delay when mixers omit that metadata. It clips the recording boundary, applies bounded gradual drift
correction, and rejects large discontinuities. Physical audio-device synchronization
and arbitrary unrelated clock domains have not been measured.

Video timing defaults to `--video-clock auto`: populated source timestamps,
otherwise monotonic receive time from the first frame.
Some compositors, including the tested Niri 26.04, leave source timestamps empty
and the graph clock stationary. The fallback is announced and recorded as
`"video_clock": "receive"` in `--stats`; source capture latency is then unknown.
Use `--video-clock source` or `--video-clock cycle` to require that clock, or
`--video-clock receive` to select receive timing from the first frame.
Auto treats header zero as unspecified; explicit source mode accepts a valid
zero-based source clock. Nonduplicate frames whose selected clock stops or resets
fail instead of producing an indefinitely frozen recording.

This is an executable recorder with hardware and lifecycle checks, **not a verified
universal state-of-the-art winner**. The design paper explicitly requires matched
hardware, quality, latency and application-interference comparisons for that claim.
See [implementation evidence](docs/validation.md), [dependency audit](docs/dependencies.md),
and the [revisable brief](IMPLEMENTATION.md).

## Verification and profiling

```sh
make check                         # native concurrency, sink, media and private portal tests
make feature-check                 # linked AV1/audio/container/backend feature set
make integration                   # isolated PipeWire server; no desktop capture
GPUKEY_TEST_GPU=1 make integration  # additionally exercise real DMA-BUF capture + hardware AV1
make hardware-check                # GPU conversion against scalar color equations
make check SANITIZE=address,undefined
nix fmt
nix flake check                    # six compiler/linker builds, identity checks, Tracy, format

make PROFILE=release TRACY=1
tracy-capture -o run.tracy -f -s 5 &
./build/gcc-release-tracy/gpukey --test-source dmabuf-static --realtime \
  --size 1920x1080 --frames 300 --output synthetic.mp4 --stats synthetic.json
```

Tracy zones cover capture handoffs, conversion/source retirement, encode/receive,
audio handoff, replay retention/snapshots/exports, portal waits, and writes.
Portal wait zones include blocked wall time; use CPU sampling/counters to distinguish
sleep from processing. Release builds without `TRACY=1` contain no Tracy client
references. Instrumented builds can perturb timing and the real-time audio thread;
benchmark uninstrumented builds separately. JSON histograms label logarithmic bucket
upper bounds instead of presenting them as exact quantiles.

The development shell also includes samply. `tools/profile-portal-samply.sh`
launches a private portal/PipeWire/GPU replay under the sampler, and
`tools/benchmark-portal.sh` compares complete uninstrumented lifecycles.
`tools/benchmark-gpu-memory.sh` measures DRM total/resident/shared memory alongside
a private producer; `tools/benchmark-gpu-matrix.sh` alternates saved references
and driver variants. `tools/memory-targets.py` calculates the documented payload
targets. `tools/check-gpu-driver.sh DRIVER_PACKAGE` checks concurrent headless
graphics alongside a media client for the optional Mesa changes.
`tools/benchmark-recorder.sh gpukey|gsr BINARY FIXTURE NEW_DIRECTORY` uses a
common private portal source for cross-recorder checks. The external fixture
embeds picture identities; `tools/analyze-recorder.py NEW_DIRECTORY` independently
counts visible repeats/gaps and measures constant BT.709 color patches. This is
not a general perceptual-quality score. `GPUKEY_FIXTURE_BUFFER_MAX=2` constrains
both consumers to the same source pool; the default offer permits eight.
`GPUKEY_RECORDER_MODE=replay GPUKEY_FIXTURE_SECONDS=40` also exercises two late
replay saves. Run these tools inside `nix develop`; all input is synthetic.
The [cross-recorder evidence](docs/validation.md#cross-recorder-comparison-23-october)
includes clean upstream versions, corrected-color controls and the remaining
comparison limits. GPU-engine counters and source allocations are kept separate
from CPU/RSS and from recorder-visible shared memory.
`tools/benchmark-gpu-drivers.sh` rotates driver order using one fixed recorder.
`tools/check-workspace-failures.sh` checks real allocation/map failures and
recovery; `tools/check-workspace-codecs.sh` compares decoded output across codecs
and pre-encode settings. `make hardware-check` also exercises four concurrent
AV1 encoders with independent decoded-pixel checks.
`GPUKEY_SAMPLER` selects an existing sampler wrapper when host perf permissions
require user-space-only sampling; the script removes any sampler preload from
the recorder. Prefer launching private tests: samply 0.13.1's PID attachment
temporarily stops the target and can disturb an interactive shell's job control.
The measured portal CPU correction and reproducible profiles are in
[validation](docs/validation.md#portal-cpu-overhead-samply-and-tracy).

The dedicated replay worker uses a futex by default, while completion uses an
eventfd in the capture owner's existing event loop. An evaluated alternative adds
one eventfd for job wakeups and keeps the same command interface and ownership
rules. Build it separately when comparing export latency/CPU on your workload:

```sh
make PROFILE=release REPLAY_EVENTFD=1 -j
./build/gcc-release-replay-eventfd/gpukey replay 30
```

Both variants have sanitizer and lifecycle checks. The measured tradeoffs,
including producer costs kept separate from replay processing, are in
[validation](docs/validation.md). This build choice does not change clip contents.

The flake retains GCC/Clang × GNU ld/mold/wild variants and optional Tracy packages:
`nix build .#clang-mold`, `nix develop .#clang-wild`, `nix build .#default.tracy`.
`make install prefix=...` installs the executable. Hardware drivers and a working
ScreenCast portal remain runtime requirements; no governor, privilege, compositor,
or system-wide settings are modified.

## Optional AMD conversion correction

The color tests found a destination-depth range error in stock Mesa 26.2.5. A small
local patch reduced maximum measured RGB8→P010 error from 3.0 to 0.675 ten-bit codes
on seven uniform-color probes, without another GPU pass. Its scope and tests are
in [validation.md](docs/validation.md).

```sh
nix build .#mesa-color --out-link result-mesa-color
LIBVA_DRIVERS_PATH="$PWD/result-mesa-color/lib/dri" \
  ./build/gcc-release/gpukey --bits 10 --output corrected.mp4
```

This is an optional Radeon VAAPI driver for this process, not a system driver
replacement. Intel and NVIDIA users should keep their own vendor drivers.

For an import-incompatible source, `--shm` requires the shared-memory path while
retaining the selected hardware encoder. `--allow-shm` merely permits that path
during negotiation. `--static-interval 60` reduces unchanged-image repeats to one
per minute; it trades seek/fragment availability for less static-scene work. The
one-second default is a policy choice, not an intrinsic capture requirement.
Replay instead requires `--static-interval` of at most two seconds to bound its
random-access history.
Timing collection is optional: ordinary recordings omit the extra per-frame
measurement clocks and per-slab timestamp storage used by `--stats`.

Examples for the upload alternatives are `--backend vulkan --shm` and
`--backend nvenc --shm`. `--backend software --shm` is the explicit CPU control.

The optional AMD direct-RGB path is available as a separate package:

```sh
nix run .#rgb-amd -- replay 30 --display --fps 60 --backend vulkan --device 0
```

This package selects its patched driver for this process. It imports compatible
8-bit RGB DMA-BUFs and lets the AV1 hardware perform BT.709 limited-range
conversion. Capture buffers remain leased through encoding and static-image
reuse. It needs the advertised RGB-conversion extension and exact modifier
support; it does not silently copy incompatible inputs. The ordinary VAAPI path
remains available for 10-bit output and as the fixed comparison control.
Use `nix develop .#rgb` with `make PROFILE=release VULKAN_RGB=1` for a local build,
or `.#rgb-clang` to compile that path with Clang. VAAPI remains the default
because the measured results retain color-precision and source-lease tradeoffs.
The RGB package now uses an ACO-only driver: the unused LLVM dependency is removed,
and measured RAM is lower than the VAAPI reference in the tested workloads. See
[the current measurements](docs/validation.md#aco-only-rgb-driver--23-october-2026).
`mesa-rgb-reference` retains the previous LLVM-enabled driver for comparison.

An additional opt-in profile selects full-range RGB conversion:

```sh
nix run .#rgb-full-amd -- replay 30 --display --fps 60 --backend vulkan --device 0
```

It advertises only the firmware-supported BT.2020-matrix/full-range pair. The
recorder queries that pair and preserves the source's BT.709 primaries and transfer
function; this is SDR, not a wide-gamut or HDR conversion. The ordinary RGB driver
continues to select BT.709/limited. Both paths validate actual AV1 headers against
the selected color plan before publishing container metadata. Native RX 9070 XT
and headless playback checks pass; this specialized profile is not a portable
default or a claim of better performance on every workload.

For accurate PNG export through mpv 0.41's image-output route, the tested
full-range clips need `--zimg-fast=no` (or `--sws-allow-zimg=no`); its default fast
conversion changes near-black values. Headless Brave's software rendering honors
the full-range profile in the tested clips. These checks do not cover every
desktop renderer. The full-range profile also increases encoded replay history
on the measured scene, so it remains an explicit choice.

AV1 quantizer requests are checked against encoder capabilities. On the tested
RX 9070 XT, `--qp 0` now fails during setup without creating a recording: VAAPI
lacks the required lossless transform mode, and Vulkan reports a minimum QIndex
of 1. It is no longer silently changed to 1 or allowed to fail on the first frame.
Software AV1 retains its own quantizer semantics; native QIndex 1 is not lossless.

The patched Matroska muxer writes explicit VFR durations without inventing a
nominal frame rate. With an unpatched FFmpeg library, the adapter retains its
one-tick nominal fallback; tools displaying that value as a frame rate should
be checked against packet timestamps. MP4 is the default precise-timing path.

If an explicitly selected audio source supplies no samples, finalization fails
instead of reporting a successful silent recording. Source corruption and gradual
clock correction are reported separately in the JSON counters.

# gpuKey

`gpuKey` is a C23 Wayland screen and window recorder built on the ownership,
zero-copy DMA-BUF, and capability-driven architecture specified in
[`material/`](material/research/Wayland_GPU_Recorder_Design.md). It encodes AV1
to fragmented MP4 or Matroska with optional multi-track audio and RAM-backed
instant replay.

## Quick start

```sh
nix develop
make PROFILE=release -j
./build/gcc-release/gpukey --probe
./build/gcc-release/gpukey recording.mp4
```

- Running `gpukey` with no arguments opens the XDG Desktop Portal chooser and
  records a dated `.mp4` in the current directory (or `.mkv` when `--audio` is
  selected).
- A positional `.mp4` or `.mkv` path selects the container automatically.
- `gpukey --help` shows everyday commands and options; `gpukey --help-all`
  lists hardware backends, quantizer, memory policy, timing, and synthetic test
  controls.
- `Ctrl-C` (`SIGINT`) or `SIGTERM` stops admission, drains codecs, finalizes
  the container, and waits for accepted writes. `--durable` also `fsync`s the
  file and parent directory. Existing files are never overwritten.

## Everyday usage

```sh
# Record a display or window chosen in the portal dialog
gpukey display.mkv --display
gpukey window.mkv --window

# 10-bit SDR AV1, 120 FPS capture cap, 30-second duration
gpukey recording.mp4 --bits 10 --fps 120 --duration 30
gpukey recording.mkv --fps 60000/1001

# Discover PipeWire audio inputs and output monitors
gpukey devices

# Record separate, unmixed audio tracks (microphone + desktop monitor)
gpukey recording.mkv --audio MIC_ID --audio DESKTOP_ID

# Preserve discrete multichannel AUX interfaces (e.g., 16-channel USB/studio interfaces)
gpukey studio.mkv --audio INTERFACE_NAME --audio-codec pcm
```

## Instant replay

```sh
# Retain the last 30 seconds of encoded AV1 in RAM
gpukey replay 30 --display --fps 60

# Save a clip while capture continues (or press Enter in an interactive terminal)
gpukey save
gpukey save highlight.mkv

# 60-second rolling buffer at 120 FPS with two audio tracks
gpukey replay 60 --display --fps 120 --audio MIC_ID --audio DESKTOP_ID \
  --directory ~/Videos/Replays --replay-memory-mib 512
```

Replay retains encoded AV1 and audio packets in flat 256-record blocks in RAM
without touching disk until `gpukey save` (or `Enter`) triggers an export.
Each exported clip starts at the preceding verified AV1 random-access keyframe
and ends at the latest common completed audio watermark, so live encoders are
never flushed or restarted mid-session. Consecutive byte-identical static
keyframes share immutable payload storage automatically.

## Hardware profiles (AMD Radeon)

The default pipeline is:

```
PipeWire DMA-BUF → persistent exact-modifier import → GPU VPP → native AV1 → libavformat → bounded writer
```

On AMD Radeon systems (verified on Radeon RX 9070 XT / VCN 5), two process-local
driver profiles are provided in the Nix flake without modifying system drivers:

### 1. VAAPI with surface-owned DPB reconstructions (`.#amd`)

```sh
nix run .#amd -- replay 30 --display --fps 60 --gpu-memory compact --static-interval 2
```

Uses a process-local RadeonSI VAAPI driver built with ACO (no LLVM dependency),
the RGB8→P010 output-range fix, single-pass paired-row compute VPP with exact
odd-dimension chroma siting, exported completion fences, dedicated AV1 tier-2
singleton workspaces, context-owned adaptive command-buffer lookup tables, and
surface-owned reusable DPB reconstructions.

### 2. Direct-RGB Vulkan Video (`.#rgb-amd` and `.#rgb-full-amd`)

```sh
nix run .#rgb-amd -- replay 30 --display --fps 60 --backend vulkan --device 0 \
  --gpu-memory compact --static-interval 2
```

Uses a process-local RADV driver and patched FFmpeg `av1_vulkan` backend that
imports 8-bit RGB DMA-BUFs once (`VK_EXT_queue_family_foreign`), converts RGB
inside the VCN hardware encoder (`VK_VALVE_video_encode_rgb_conversion`),
manages a three-slot queue-owned DPB, assembles firmware bitstream headers in a
compact 8-byte host word accumulator, and executes synchronous host transfers.
`.#rgb-full-amd` selects the hardware's BT.2020-matrix / full-range SDR profile.

### Memory policies (`--gpu-memory retain|compact`)

- `retain` (default): keeps allocated input surfaces, reconstruction slots, and
  coded bitstream buffers ready for low-latency bursts.
- `compact`: releases spare input surfaces and excess coded output buffers once
  a backlog drains. Combined with `--static-interval 2`, once a validated
  independent CPU keyframe owns static presentation, compact standby releases
  all unused coded output buffers, converted input surfaces, unused AV1
  reference reconstructions, and CPU/GTT upload staging buffers, restoring
  capacity on demand when motion resumes.

| 2560×2880 / 60 AV1 (RX 9070 XT, depth 2, 2 source buffers) | Active VRAM | Compact standby VRAM | Active GTT | Compact standby GTT |
|---|---:|---:|---:|---:|
| VAAPI VPP (`.#amd`) | 103.43 MiB | 71.09 MiB | 23.35 MiB | 2.19 MiB |
| Direct-RGB Vulkan (`.#rgb-amd`) | 91.55 MiB | 70.41 MiB | 23.24 MiB | 2.09 MiB |
| Uploaded Vulkan 8-bit (`.#rgb-amd --shm`) | 56.85 MiB | 13.18 MiB | 33.79 MiB | 2.09 MiB |
| Uploaded Vulkan 10-bit (`.#rgb-amd --shm --bits 10`) | 109.76 MiB | 23.73 MiB | 44.34 MiB | 2.09 MiB |

Native DMA-BUF VRAM totals above include **57.50 MiB** for the two shared
2560×2880 RGBX capture surfaces owned by the producer. See
[`docs/memory-targets.md`](docs/memory-targets.md) for the analytical model and
[`docs/validation.md`](docs/validation.md) for full benchmark and cross-recorder
comparisons.

## Building, testing, and verification

```sh
make check                          # unit, concurrency, media, CLI, and replay tests
make feature-check                  # verify linked AV1/audio/container/backend support
make integration                    # private PipeWire + portal integration suite
GPUKEY_TEST_GPU=1 make integration  # include native DMA-BUF + hardware AV1 integration
make hardware-check                 # GPU color oracle + 16 concurrent encoder lifetimes
make check SANITIZE=address,undefined
nix fmt
nix flake check                     # GCC 16 & Clang 21 × ld/mold/wild + source/sanitizer checks
```

## Documentation

- [`material/research/Wayland_GPU_Recorder_Design.md`](material/research/Wayland_GPU_Recorder_Design.md) —
  foundational architecture, queue/lifetime invariants, and formal models
- [`docs/memory-targets.md`](docs/memory-targets.md) — conditional VRAM, GTT,
  and replay RAM models (`python3 tools/memory-targets.py`)
- [`docs/dependencies.md`](docs/dependencies.md) — audited FFmpeg 9.0.2 and Mesa
  26.2.5 integration mechanisms and patches
- [`docs/validation.md`](docs/validation.md) — verification coverage,
  cross-recorder comparisons against `gpu-screen-recorder`, profiles, and
  material limits

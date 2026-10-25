# Validation and Benchmark Reference

Validated on Linux 7.2.10, x86-64, AMD Radeon RX 9070 XT (VCN 5, Mesa 26.2.5,
FFmpeg 9.0.2). The supplied `material/` specification and formal models are
unchanged and pass their SHA-256 manifest (`material/SHA256SUMS.json`).

---

## 1. Verification matrix

| Check | Coverage |
|---|---|
| Nix toolchain matrix (`nix flake check`) | GCC 16.2.0 and Clang 21.1.8 × GNU `ld`, `mold`, and `wild` (6 package builds, 6 ELF compiler/linker identity checks, instrumented Tracy build, and `nixfmt`/`clang-format` check). |
| Source-extracted library oracles | `vaapi-dpb` and `vaapi-dpb-clang` (`tests/vaapi_dpb_owner_probe.c`), `encode-bits` and `encode-bits-clang` (`tests/encode_bits_probe.c`), `vulkan-upload` and `vulkan-upload-clang` (`tests/vulkan_upload_probe.c`), `command-hints` (`tests/command_hint_probe.c`), `pool-lifetimes` (`tests/pool_trim_probe.c`), and `vulkan-execution` (`tests/vulkan_pool_probe.c`). |
| Sanitizers | GCC and Clang AddressSanitizer/UBSan across unit, CLI, replay, and private PipeWire/GPU integration suites; ThreadSanitizer on the SPSC ring, buffered/io_uring sinks, replay worker, and PipeWire capture callbacks. |
| Ownership and I/O (`tests/core_test.c`) | 1,000,000 SPSC FIFO token transfers crossing 32-bit wrap; full/empty invariants; `O_CREAT \| O_EXCL` refusal; short-write and injected `ENOSPC` recovery; ordered completed-prefix accounting; file and directory `fsync`. |
| Portal (`tests/portal_test.c`) | Private D-Bus ScreenCast portal mock emitting responses before returning method handles; stable serial selection, owned FD extraction, revocation, denial, cancellation, and custom `GSource` deadline waiting without internal wake churn. |
| Private PipeWire (`tests/integration.sh`) | Isolated PipeWire daemon and synthetic producer exercising shared-memory capture, two-stage tiled DMA-BUF modifier fixation, missing-PTS fallback, source removal mid-recording, and static holds without capturing the host desktop or microphone. |
| Media & AV1 (`tests/media_test.c`, `tests/av1_test.c`, `tests/repeat_test.c`) | Independent `libdav1d` decoding; BT.709 color endpoints; VFR packet boundaries; 1000 ppm audio drift resampling (240,240 output samples from 240,000 input samples over 5.005 s); independent static-key reuse; and accepted-prefix preservation on codec/input errors. |
| GPU hardware (`make hardware-check`, `tools/check-vulkan-dpb.sh`) | Native VAAPI VPP + AV1 and Direct-RGB Vulkan AV1; 8/10-bit conversion against scalar BT.709 equations; odd dimensions (`321×241`); 16 concurrent encoder lifetimes across 4 worker threads; and 250-picture 8/10-bit Vulkan validation-layer DPB retirement/restoration checks. |
| Formal models (`material/`) | Arithmetic (`check_design_arithmetic.py`), concurrency (`recorder_concurrency_model_check.py`), and output (`check_output_model.py`, 748 complete traces and 1,833 prefixes) verified against their expected results. |

---

## 2. Cross-recorder comparison against `gpu-screen-recorder`

Both recorders were evaluated against the same private portal/PipeWire DMA-BUF
producer (`tests/capture_fixture`) with embedded 32-bit picture identities and
eight unoccluded BT.709 color patches, decoded independently by
`tools/analyze-recorder.py`. Settings: AV1 8-bit, native quantizer 32, no B
frames, limited-range BT.709 with sRGB transfer, 2-second keyframe interval, two
source buffers, and the same Mesa 26.2.5 driver on RX 9070 XT.

### 8-second capture lifecycles (upstream GSR snapshot `ee2c6ca1e03e`, 3 rotating runs each)

| Workload | `gpuKey` CPU ms ± SD | GSR (depth 3) CPU ms ± SD | GSR (depth 2) CPU ms ± SD | `gpuKey` / GSR (depth 3) RSS MiB |
|---|---:|---:|---:|---:|
| 2560×2880 / 60 | **93.009 ± 18.631** | 155.639 ± 15.803 | 192.231 ± 3.105 | **37.901 ± 0.313** / 68.803 ± 0.339 |
| 1920×1080 / 240 | **179.955 ± 22.920** | 258.141 ± 15.688 | 267.469 ± 2.312 | **36.693 ± 0.241** / 67.309 ± 0.340 |

- `gpuKey` had zero interior missing identities, repeats, or backward
  transitions across all runs; GSR averaged 0.7–1.0 interior missing identities
  per run.
- Mean absolute error across the eight BT.709 Y/Cb/Cr patches is **0.184** code
  values for `gpuKey` versus **1.232** for stock GSR (`0.434` when GSR's
  limited-range matrix constants are patched).

### 40-second rolling replay (30 s window, two saves at 35 s and 38 s)

| Metric | `gpuKey` | GSR upstream (depth 3) |
|---|---:|---:|
| Complete recorder CPU | **416.047 ms** | 746.827 ms |
| Peak RSS | **44.754 MiB** | 76.160 MiB |
| Sampled VRAM peak (including 57.50 MiB shared source) | **103.434 MiB** | 113.715 MiB |
| Sampled GTT peak | **23.348 MiB** | 71.305 MiB |

---

## 3. Native DMA-BUF VAAPI and Direct-RGB Vulkan (`2560×2880 / 60`)

### Surface-owned reusable VAAPI reconstructions (`.#amd`)

In `nix/mesa-vaapi-surface-dpb.patch` and `nix/ffmpeg-vaapi-dpb-idle.patch`,
active Mesa AV1 DPB entries borrow their surface's buffer while retired entries
multiplex the unused `order_hint` field to record `owner_surface_id` (`0` bytes
of struct growth). This preserves cross-surface buffer reuse during motion while
allowing `av_buffer_pool_trim(ctx->base.recon_frames->pool, 0)` in compact
standby to free idle reconstruction surfaces.

| Workload (with `--stats`) | Retained-DPB control CPU ms ± SD | Surface-owned DPB CPU ms ± SD | Control / candidate peak RSS MiB |
|---|---:|---:|---:|
| Portrait/60 motion, 30 s | 296.444 ± 0.935 | 292.040 ± 7.688 | 38.310 / 38.495 |
| Long pause/resume (1..7 s), 10 s | 53.171 ± 3.878 | 53.889 ± 3.890 | 38.359 / 38.312 |
| Short pause/resume (1..4 s), 8 s | 56.701 ± 11.870 | 61.458 ± 10.961 | 37.786 / 38.103 |
| 1080p / 1000 FPS offered (~354.5 FPS encoded), 10 s | 212.617 ± 5.005 | 211.198 ± 3.664 | 36.591 / 37.022 |

| Workload (without `--stats`) | Control CPU ms ± SD | Candidate CPU ms ± SD | Control / candidate peak RSS MiB |
|---|---:|---:|---:|
| Portrait/60 motion, 30 s | 284.142 ± 5.618 | 289.888 ± 5.822 | 38.008 / 37.759 |
| Long pause/resume (1..7 s), 10 s | 51.312 ± 5.580 | 53.270 ± 5.505 | 38.466 / 37.853 |

- **VRAM & GTT**: Active portrait peak VRAM is **103.43359375 MiB** (`57.50 MiB`
  shared capture). During compact standby (`--gpu-memory compact
  --static-interval 2`), VRAM drops from `103.43359375 MiB` → `92.18359375 MiB`
  (raw converted input released) → **71.08984375 MiB** (two unused
  `10.546875 MiB` reconstructions released), and GTT drops from
  `23.34765625 MiB` → **2.19140625 MiB** (`21.15625 MiB` coded output buffer
  released).
- **Resume latency**: Producer-publication-to-packet delay on the first resumed
  frame after standby is `10.4570 ± 0.7773 ms` control versus
  `11.1617 ± 1.1751 ms` candidate (long pause) and `9.0401 ± 2.4472 ms` versus
  `8.6299 ± 1.0856 ms` (short pause).

### Direct-RGB Vulkan Video and RADV word bit-writer (`.#rgb-amd`)

Direct-RGB Vulkan imports PipeWire DMA-BUFs once and converts RGB inside the VCN
hardware encoder with a three-slot queue-owned DPB (`nix/ffmpeg-vulkan-rgb.patch`,
`nix/ffmpeg-dpb-idle.patch`). `nix/mesa-encode-bits.patch` replaces RADV's
byte-oriented command-buffer header writer with an 8-byte host word accumulator
(shrinking `radv_enc_state` from `72` to `48` bytes on x86-64).

| Workload | Byte-writer control CPU ms ± SD | Compact word-writer CPU ms ± SD | Control / candidate RSS MiB |
|---|---:|---:|---:|
| Portrait/60 motion, 30 s | 214.671 ± 9.280 | **199.266 ± 10.592** (-7.2%) | 26.833 / 26.471 |
| Portrait pause/resume (1..7 s), 10 s | 46.448 ± 5.276 | **37.859 ± 3.545** (-18.5%) | 26.250 / 26.617 |
| 1080p / 1000 FPS offered (~256 FPS encoded), 10 s | 157.433 ± 18.961 | **108.396 ± 2.201** (-31.1%) | 25.445 / 24.815 |

- **VRAM & GTT**: Active portrait peak VRAM is **91.5546875 MiB** (`57.50 MiB`
  shared capture) and GTT is **23.2421875 MiB**. In compact static standby, VRAM
  falls to **70.40625 MiB** (releasing two `10.57421875 MiB` reconstructions)
  and GTT falls to **2.0859375 MiB**.

---

## 4. Uploaded CPU/GPU path (`--shm`), synchronous Vulkan transfers, and standby staging reclamation

On the explicit CPU conversion and upload path (`--shm`),
`nix/ffmpeg-vulkan-transfer.patch` waits for GPU transfer completion on uploads
as well as downloads (eliminating retained execution-context `sw_frame_deps` and
`buf_deps` clones that previously pinned two simultaneous host-pointer GTT
imports across standby) and trims `fp->tmp` in `vulkan_frame_free_cb`.
`src/media.c` sets `fc->initial_pool_size = 0`, enables `queue_dpb = 1` on
uploaded Vulkan, retains one writable spare alongside `m->last` during active
capture (`4` input-frame allocations and `0` per-frame churn across a
239-picture pause/resume run, versus `129` input allocations in a naive
zero-spare challenger), releases all upload images and `m->staging` in compact
standby, and collects completed packets before the pre-send pending check.

| Workload (2560×2880 / 60, 3 rotating runs each) | Fixed CPU ms ± SD | Candidate CPU ms ± SD | Fixed → candidate peak VRAM MiB | Fixed → candidate standby VRAM MiB | Fixed → candidate peak / standby GTT MiB |
|---|---:|---:|---:|---:|---:|
| Uploaded Vulkan 8-bit pause/resume, 10 s | 2141.136 ± 25.645 | 2138.413 ± 28.077 | 166.742 → **56.852** | 166.734 → **13.184** | 90.173 / 23.188 → **33.793 / 2.086** |
| Uploaded Vulkan 10-bit pause/resume, 10 s | 2292.715 ± 71.534 | 2318.306 ± 21.153 | 318.586 → **109.762** | 318.561 → **23.730** | 121.815 / 44.281 → **44.341 / 2.086** |
| Uploaded Vulkan 8-bit motion, 8 s | 4224.340 ± 137.884 | 4275.399 ± 75.654 | 166.755 → **56.852** | — | 86.656 → **33.793** (active) |
| Uploaded VAAPI 8-bit pause/resume, 10 s | 2370.659 ± 75.740 | 2375.970 ± 98.633 | 57.133 → 57.125 | 13.227 → 13.219 | 58.566 / 5.707 → **37.410** / 5.707 |
| Uploaded VAAPI 10-bit pause/resume, 10 s | 2691.778 ± 97.676 | 2810.297 ± 4.921 | 110.020 → 110.016 | 24.082 → 24.078 | 72.629 / 9.223 → **51.473** / 9.223 |
| Native DMA-BUF RGB Vulkan pause/resume, 10 s | 45.747 ± 4.519 | 45.331 ± 4.812 | 91.555 → 91.555 | 70.406 → 70.406 | 23.242 / 2.086 → 23.242 / 2.086 |

- **Standby `smaps` anonymous RAM at `5.5 s`**: freeing `m->staging` in compact
  standby reduces anonymous RAM by **10.551 MiB** (`va8`, `71.359 → 60.809 MiB`),
  **21.102 MiB** (`va10`, `82.059 → 60.957 MiB`), **10.602 MiB** (`vk8`,
  `71.656 → 61.055 MiB`), and **21.152 MiB** (`vk10`, `82.371 → 61.219 MiB`).

---

## 5. Portal CPU overhead, replay retention, and audio

### Portal event-loop wait (`src/portal.c`)

Replacing GLib `ready-time` churn with an owner-only deadline word in a custom
`GSource` eliminated the main-thread busy loop (5 alternating 4 s runs at
2560×2880/60):

| Workload | Original CPU ms ± SD | Corrected CPU ms ± SD | % of one logical CPU (original → corrected) |
|---|---:|---:|---:|
| Static | 4040.319 ± 1.873 | **38.308 ± 0.922** (-99.1%) | 99.904% → **0.947%** |
| Changing (50 Hz updates) | 4008.451 ± 2.676 | **73.342 ± 4.314** (-98.2%) | 99.063% → **1.813%** |

### Replay packet adoption and lock-free futex handoff (`src/replay.c`)

Adopting CPU-backed `AVBufferRef` packets directly and waking the background
export worker via a futex word (`REPLAY_EVENTFD=1` retained as a build-time
alternative) avoids copying encoded payloads on every frame (7 alternating runs
per variant, 20 s of 1080p60 with 5 s retention window):

| Replay workload | All-copy control ms ± SD | Default futex ms ± SD | Extra copied bytes avoided |
|---|---:|---:|---:|
| Static video | 0.788 ± 0.073 | **0.738 ± 0.041** | 1,027,620 |
| Changing video | 1.089 ± 0.107 | **0.978 ± 0.081** | 1,929,014 |
| Static + two Opus tracks | 1.796 ± 0.144 | **1.407 ± 0.209** | 1,807,806 |
| Static + two PCM tracks | 3.338 ± 0.366 | **2.365 ± 0.173** | 16,387,620 |

### 10-bit VAAPI color correction (`nix/mesa-output-range.patch`)

Fixing `vl_csc_get_rgbyuv_matrix` in Mesa 26.2.5 to normalize output range
coefficients by the destination component depth rather than the source depth
reduced maximum RGB8→P010 conversion error across seven uniform RGB probes from
**3.0000** to **0.6747** 10-bit code values (8-bit max error unchanged at
`0.4406` codes).

---

## 6. Material limits and scope

- Measurements are established on AMD Radeon RX 9070 XT (x86-64, Linux 7.2.10)
  with pinned Mesa 26.2.5 and FFmpeg 9.0.2. Intel and NVIDIA hardware, `aarch64`
  runtime execution, physical audio-hardware latency, and live 3D game
  interference have not been executed here.
- Stock FFmpeg 9.0.2's `vulkan_encode_av1_close` omits cleanup of its coded
  bitstream context/fragment (~3.17 MiB across 4 open/close cycles in
  LeakSanitizer).
- Compact standby retains one live AV1 reference reconstruction (`10.55 MiB` at
  8-bit portrait, `21.09 MiB` at 10-bit portrait) and driver session state so
  resumed frames do not require encoder reinitialization.

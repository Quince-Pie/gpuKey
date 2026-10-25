# Wayland GPU Recorder Working Brief

Research started 2026-10-19 15:34:40 UTC. No user execution deadline supplied.

## Explicit requirements from the user

- Perfect the design before implementation; eventual application implementation in C23.
- Wayland Linux target, with explicit analysis of Linux kernel 6.12 and later.
- Research modern AMD, NVIDIA and Intel GPUs and relevant current primary literature.
- Optimize overhead, quality, throughput, latency and modern applicability together, exposing genuine tradeoffs.
- Likely codec written as AVI1; provisionally interpret as AV1, not the AVI container.
- Examine whether io_uring and advanced atomic queues are justified, including Ramalhete and LCRQ families.
- Deliver a usable, verified design artifact with evidence and material limits; do not claim unmeasured superiority.

## Verified environment and context

- Conversation workspace initially contains no task artifacts or implementation/callers.
- x86_64 container kernel reports 6.18.55. No /dev/dri or /dev/nvidia0 devices exposed.
- Cannot establish GPU interoperability, recorder throughput, capture latency or visual quality rankings on this host.
- Primary research is being gathered in parallel across capture/integration, GPU backends, I/O and concurrency.

## Proposed defaults that remain reversible

- Design a local recorder first, retaining low-latency and replay/streaming as explicit profile requirements rather than conflating their objectives.
- Portal/PipeWire as portable capture control; optional supported direct compositor capture as a measured challenger.
- GPU-local frames, persistent imports and a backend-native conversion/encoding path with explicit ownership.
- AV1 4:2:0 8/10-bit as common hardware target; chroma fidelity and archival losslessness require separate admitted alternatives.
- Bounded resources and source-generation handling; no self-imposed universal resolution/fps/GPU-family limits.
- PDF design reference as final artifact; source research and checks remain recoverable intermediate files.

## Unresolved consequential product choices

- Concrete GPUs/drivers/compositors; target resolutions and source frame rates.
- Principal consumer: local playback, editing, live transport or instant replay.
- Actual quality, latency, host-interference, memory and durability budgets.
- HDR requirement, source/window/multi-monitor behavior, recording of audio and cursor.
- Packaging/license constraints and whether codec fallback may be automatic or user-selected.

These do not block architecture research. Do not convert defaults into requirements. Finish a concrete proposal before requesting future tuning inputs.

## Validation probes planned

- Independently audit key vendor/API/kernel version claims against primary sources.
- Finite abstract lifecycle/notification models with deliberately broken controls.
- Arithmetic and temporal/damage examples that expose wrong representations or assumptions.
- PDF content extraction, citation coverage and visual layout review.
- Publish an executable implementation validation plan for hardware-dependent claims.

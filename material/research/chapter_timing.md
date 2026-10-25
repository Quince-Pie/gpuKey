# Encoding policy, temporal structure and effective configuration

## Configure observable results

Expose quality intent, bitrate constraints, latency allowance, random-access requirement and fidelity contract. Translate these into native encoder settings, then record what the runtime actually accepted. A single integer called quality or QP cannot mean the same thing across NVENC, AMF, VAAPI and VPL. Even within one API, picture type, bit depth and rate-control mode can change its meaning.

The proposed initial **local-file control** uses a supported quality-oriented variable-rate mode or constant-quantizer control, no unnecessary bitrate ceiling, source-derived VFR where consumers support it, a deliberately simple reference structure, and no optional analysis until its quality benefit is measured. The strongest quality challenger adds supported lookahead, temporal/spatial analysis, additional references and slower presets within the actual interference and latency budget. An unbounded bitrate is not a storage guarantee; admission must still enforce resource limits.

The proposed **live control** has zero future-frame lookahead, no display reordering unless explicitly budgeted, a bounded output reservoir appropriate to the transport, and a freshness policy before encode. It still permits asynchronous execution sufficient to keep the media engine busy. Rate-control buffer size divided by bitrate is a configured reservoir duration, not a proof of end-to-end latency. The application must measure actual delay through capture, encode, mux and transport.

The proposed **fidelity control** compares 10-bit 4:2:0 against an accepted higher-chroma-fidelity mode using the actual desktop reference. Bit depth, chroma resolution, color conversion and encoder quantization are measured separately. A more accurate conversion can be the largest quality improvement even when encoded bitrate and preset remain unchanged.

These are functional starting configurations. A numerical preset, GOP length, quantizer or buffer depth becomes a selected value only after the target workload and constraints establish a reason. Retaining that tuning freedom is part of the design, not an omission disguised as a universal setting.

The following API-level controls make the first comparison concrete. They are documented mechanisms and proposed controls, not measured winners. [G01, G12]

| Backend / mode | Starting control | Qualification |
|---|---|---|
| NVENC AV1 live | LL or ULL tuning; no future lookahead; frameIntervalP = 1; temporal filtering off | For a bounded channel, compare CBR with a one-frame-duration VBV candidate; this is a reservoir setting, not an end-to-end delay guarantee |
| NVENC AV1 local | HQ tuning and VBR; target quality/ceiling and finite random-access interval from consumer needs | Vendor recording examples include 2-second GOP/4-second VBV; those examples are not requirements or universal best values |
| AMF AV1 live | ULTRA_LOW_LATENCY usage, then explicit bitrate/frame-rate settings | Documented defaults include speed preference, Pre-Analysis off, lowest-latency policy and latency-constrained VBR |
| AMF AV1 10-bit control | Supported CQP or conventional VBR, Pre-Analysis off; verify effective configuration | Current QVBR/HQVBR/HQCBR restriction excludes them from this 10-bit control |
| Intel AV1 | Query a supported native configuration and its effective rate-control/reference settings | Do not infer a universal TargetUsage value or require the obsolete LP entrypoint name on modern hardware |

NVIDIA's general low-latency guidance also discusses unidirectional B frames, but its current feature description limits that feature to HEVC. Do not transplant that item into the AV1 control. Add multipass, AQ, references and lookahead only in candidates whose latency/quality contract admits their costs. [G01]

## Random access, references and transport

Choose a maximum random-access interval from seek, replay-export, recovery and live-join requirements. Shorter independent intervals can improve joining and clipping while spending bitrate and disturbing rate control. They do not inherently lower the latency of every ordinary frame. Fragment/cluster cadence is a different control and need not always force a keyframe.

AV1 has hidden frames, reference refreshes, order hints and show-existing behavior. A backend may manage these internally or require the application to manage them. The common packet contract therefore contains actual decode/presentation timing, configuration generation, a verified random-access classification, temporal-unit boundaries and retention ownership. It does not assume every input produces one immediately available packet or every packet displays one new picture. Follow the AV1 specification and the chosen container mapping. [C19]

For a local file, recovery is normally better served by complete container boundaries and occasional valid random-access points than by indiscriminately adding error resilience or very frequent intra pictures. A network mode adds loss/jitter and receiver constraints; its codec structure and transport must be designed together. There is no generic io_uring setting that replaces congestion control, packet pacing, receiver buffering or codec dependency repair. A future live backend should use a mature transport implementation and retain the same capture/encoder ownership core.

On resolution or sequence-configuration changes, drain or terminate the old configuration under the declared continuity policy, then start a new decodable sequence and appropriate segment/file. Do not assume every encoder can reset every parameter in place or that every container consumer accepts mid-track changes. A bitrate-only reset may be legal on a backend where a format change is not. Probe the exact reconfiguration and report its effective behavior.

## Cost-specific quality experiments

Compare rate-distortion curves at matched decoded semantics, then impose the recorder's resource constraints. Equal bitrate is one useful slice; equal measured quality is another. Equal QP across vendors is not a fair comparison. Include latency and application frame-time impact on the same plots or tables instead of declaring the best quality curve the overall winner.

Separate the effects of spatial AQ, temporal AQ, lookahead, multipass analysis, temporal filtering, reference count, tiles/engine splits and screen-content tools. Their gains interact, so follow individual probes with integrated confirmation of the selected combination. A new knob that enables an internal compute pass can shift the bottleneck back to shader or memory contention. A throughput gain that sacrifices text fidelity should be reported as a tradeoff rather than hidden in an aggregate natural-video metric.

# Timeline, pacing and audio

## Use source time and map clock domains explicitly

Preserve source timestamps, arrival timestamps and completion timestamps separately. PipeWire's header contains presentation timing and sequence information; its raw-video format can indicate variable rate. These are better media-time inputs than when the callback happened to run. The direct compositor protocol defines its own presentation-clock contract. Do not assume every timestamp produced by every API shares an epoch, suspend behavior or unit. [C03, C07, C08]

Create an explicit mapping from each source clock into a recording timeline. For clocks with drift, the mapping may be affine over an interval, with a measured scale and offset. Keep an uncertainty estimate and reset the mapping after discontinuities. Use source identity and generation when matching timestamps to pixels. A monotonic increasing callback counter is not a substitute for an actual capture timestamp.

For true CFR output at rational frame rate p/q, derive each target timestamp from the frame index and origin: round(n × q × 1,000,000,000 / p). Use checked integer/rational arithmetic; avoid repeated addition of a rounded period. Rational rates such as 60000/1001 are not exactly 59.94. At 240 fps, repeatedly adding a rounded nanosecond period can drift by 6.912 ms over a day, while rounding each absolute rational timestamp stays within half a nanosecond before container serialization. This is a derived and executed arithmetic check.

Define the sampling policy independently of the encoder. For each CFR deadline, choose the appropriate available source frame without capturing a future frame under an earlier timestamp. If a source update is late, either repeat the last valid display or mark/drop according to the mode; do not conceal timing gaps by relabeling arrival time. In VFR, preserve changes and durations, including cursor-only changes and the final static interval at stop. Final duration cannot be inferred from a nonexistent next frame, so stop provides an explicit timeline endpoint.

Handle duplicate sequence numbers, discontinuity flags, nonmonotonic timestamps, missing timestamps and resume explicitly. Small timestamp jitter and a new clock epoch are different conditions. A corrective policy may rebase a new segment or reject a broken source; arbitrarily clamping every timestamp forward hides problems and can change speed or A/V synchronization.

## Audio is a clocked source, not a video side effect

Capture the chosen system monitor and microphone through their authorized PipeWire session paths. Keep them as separate tracks when useful; mixing is an explicit processing step with gain, clipping and channel-layout policy. A monitor follows a particular sink, so device switching and source loss need declared behavior. A microphone selection must not silently change to another device.

A real-time audio callback should copy or transfer bounded data into preallocated storage, publish an immutable descriptor, and return. It must not encode, allocate variable-sized objects, access files, import GPU resources or wait for the video thread. PipeWire's real-time processing contract restricts blocking and non-real-time operations. A separate owner handles conversion, resampling, mixing, encoding and packetization. [C11]

Use the source's sample position and timing to relate audio to the recording clock. Derive drift from timestamp/sample-count observations over a suitable interval, reject discontinuities, and correct gradual drift with a bounded adaptive resampler when the sources are independently clocked. Do not periodically discard an arbitrary audio frame to match a noisy video callback. Preserve a configurable policy for audio gaps: explicit silence, a discontinuity or stopping that track. Track latency compensation and resampler delay in timestamps.

48 kHz is a practical proposed working rate for common video/Opus workflows, not a requirement to reconfigure the entire desktop graph. Accept negotiated source rates and convert where needed. A stereo assumption is also provisional: channel count, layout and microphone/system mixing must be explicit. Avoid silently downmixing a requested multichannel recording.

## Audio packet size, priming and finalization

Opus is a strong local/live candidate when the chosen container and consumers accept it. AAC, FLAC or PCM remain consumer/fidelity alternatives. Frame duration affects codec delay and packet overhead. Twenty milliseconds is a reasonable file-oriented control; shorter supported durations are live candidates when the latency budget justifies their overhead and quality effects. Query the codec's actual lookahead rather than hardcoding a remembered constant. Opus documents its encoder controls and lookahead query. [C20]

Codec lookahead, resampler delay, priming/pre-skip, trailing padding and final valid sample count must be represented correctly by the container. Otherwise a file can have apparently aligned first packet timestamps and still play audio late, or contain an audible tail of padded samples. Verify start and end with independently generated impulses and known sample counts.

The mux owner merges tracks by actual decode-time requirements while respecting bounded lateness and explicit stream start/end events. A temporarily silent or unavailable audio source must not cause unbounded video buffering. The output chapter explains the nonzero interleave budget and FFmpeg's misleading zero setting. Video overload should not force the audio real-time callback to wait, and a video skip should preserve elapsed time rather than compressing the audio timeline.

## Suspend, pause and stop

Application pause, compositor inactivity and system suspend are distinct. An application pause can remove elapsed time or preserve a gap according to the chosen editing semantics. System suspend may stop one source clock while another clock includes elapsed sleep. Map and signal the transition; a registered io_uring wait clock does not decide media semantics.

On normal stop, choose a common recording endpoint, stop admitting new source content, account for the last held video frame and valid audio samples, drain codec delay, finalize interleaving and mux structures, and complete the requested sink boundary. A hung device or lost source may prevent that ideal drain. Preserve and report the actually established playable/durable prefix, rather than reporting success simply because the stop button was processed.

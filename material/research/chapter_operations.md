# Admission, overload, recovery and system progress

## Credits represent actual retained resources

Use distinct credits for source leases, recorder-owned images, encoder inputs, reconstruction/reference surfaces, encoded packet bytes, mux staging and in-flight sink bytes. Count a resource until its true release event. Returning a queue slot, receiving one output packet or issuing a cancellation is not a universal release event.

Admission checks the next job's incremental demand before committing work. For a simple bounded pipeline this can be owner-local accounting rather than a global atomic semaphore. Reservations spanning owners need a documented order and rollback path. Avoid holding one scarce resource indefinitely while waiting for another in a cycle: for example, a source lease waiting for an encoder surface that cannot retire because output buffers are held by a blocked mux owner.

Reserve a small, explicitly accounted control capacity for EOS, error and retirement messages so a full data queue cannot prevent progress toward shutdown. This does not guarantee progress through a hung GPU or filesystem. A stalled owner, allocator, runtime callback or mandatory I/O operation remains a progress dependency even when every queue operation is lock-free.

Apply both byte and age limits. A large byte budget may hold little time at high bitrate and many seconds at low bitrate. A frame-count cap misses large format changes. Derive the initial surface pool from API requirements and measured retention, and derive sink credit from burst handling plus the chosen bounded-stall policy. These are workload-dependent parameters, not immutable production limits.

## Different losses have different semantics

The capture owner may reject an input it has not submitted, after consuming metadata necessary to preserve damage and timeline state. A consumer may discard an unsubmitted stale descriptor. A producer must not manipulate a consumer index to delete a frame that may already be in use. A latest-frame mailbox is an alternative only with a complete pin/claim/replacement protocol and a consumer contract allowing intermediate updates to be skipped.

Once GPU work has been accepted, unwanted output does not cancel its resource access. Once an AV1 reference-dependent packet is encoded, dropping it arbitrarily can invalidate future output. Prefer admission-side skips where permitted. If output must be discarded, restart at a verified random-access boundary and expose a discontinuity or stop with a clear partial-recording result.

A no-drop mode requires a sustainable offered workload or a defined failure response. No finite memory can guarantee uninterrupted no-drop capture and bounded latency through an arbitrarily long sink stall. This is a backlog argument, not a limitation caused by choosing a bounded queue. Increasing capacity merely extends the time before the policy must act.

## State transitions and error classes

A practical session has discovering, negotiating, ready, recording, reconfiguring, draining, completed and failed states. Backend resources also have their own creation/active/detached/retiring/destroyed states. Keep session intent separate from resource retirement so a failed session can still receive valid completion callbacks.

| Event | Required behavior |
|---|---|
| Portal denial or revocation | Stop admission; respect closure; retire accepted work and preserve the established output prefix |
| Buffer removal | Detach that borrowed buffer wrapper and retire its owned record after pending access; other compatible buffers remain valid |
| Source reconfiguration | Create a new configuration generation when shared format, color, import or synchronization assumptions change; retire old resources asynchronously |
| Source gap, corruption or unknown damage | Preserve timeline; invalidate incremental image history when necessary; apply the declared gap policy |
| Encoder busy or input-full | Make progress on the relevant outputs/completions; retain unaccepted input; retry only when its ownership state permits |
| Partial submission or failed fence export | Track any work already accepted; repair or wait for its exact retirement before allowing reuse |
| GPU device loss | Stop new submissions; use documented runtime/device-loss teardown; do not invent a signaled completion |
| ENOSPC, quota or EIO | Latch failure; stop admitting output that cannot be retained; preserve recoverable media and report accepted/completed/durable boundaries |
| Short write or out-of-order completion | Advance only completed ranges; retry valid remainder; preserve buffer lifetime and contiguous-prefix accounting |
| Source or sink hotplug | Re-resolve exact identity under authority; do not reuse stale node IDs, FDs or format assumptions |
| Stop during a replay export | The export keeps its independent chunk references; live-session shutdown cannot overwrite them |

Normal stop should be idempotent. Stop admission, notify owners, resolve in-flight source reads, drain encoder and audio delay, finish mux structures, complete accepted writes and patches, perform the requested synchronization, then publish success. Resource cleanup follows authoritative retirement, not the order in which cleanup functions happen to be called.

A recoverable container boundary, successful byte prefix and durable checkpoint are distinct. A synchronized arbitrary prefix may still omit initialization or end in a partial unit. A complete playable segment can remain only in volatile caches. A backward metadata patch has its own generation and can change which earlier bytes form a valid file. User-visible status should reflect the actual guarantee selected and reached.

## Isolation, scheduling and observability

Use process isolation when it solves a demonstrated fault-containment or privilege boundary. A tiny optional KMS helper and an untrusted-media decode test worker can have separate responsibilities. A process per pipeline stage would add IPC and lifetime complexity without an established benefit. A GPU worker process can improve recovery from a stuck userspace runtime, but termination is not proof that hardware stopped touching memory; teardown must follow driver guarantees.

Do not make global CPU governor changes, GPU clocks, real-time scheduling, forced firmware parameters or disabled power management a hidden requirement. They affect the rest of the machine. Baseline measurements should use the intended deployment policy, then label any special tuning as a separate operating condition. Thread affinity can reduce migration but also collide with game-critical cores; measure all affected threads and interrupts before selecting it.

The hot path should avoid fresh imports, shader compilation, heap allocation, unbounded log formatting, synchronous filesystem access and global locks. Some backend operations may internally allocate or block; instrument these rather than asserting that application-level pooling removed them. Bounded binary event records can capture diagnostics with deferred formatting. Use per-owner counters where possible and sampled or batched aggregation to avoid a contended global statistics cache line.

# C23 implementation contract and component boundaries

## C23 is the host language, not an artificial dependency ban

Implement the application core in C23 with explicit ownership, checked arithmetic and narrow backend boundaries. Existing C ABIs for PipeWire, Vulkan, libdrm, libva, VPL, CUDA/NVENC and libavformat are practical building blocks. An SDK may require a small compatibility wrapper or a separately compiled device program. Precompiled SPIR-V or CUDA kernels do not violate a C23 host implementation. Do not force a slower data path because a convenient kernel toolchain uses a different device-language syntax.

Pin build dependencies and record runtime versions/capabilities. A compile-time header exposing a flag does not mean the running kernel or driver supports it. Dynamically load optional vendor libraries and fail gracefully to a selected alternative when missing. Avoid making all three vendor stacks mandatory on every installation.

Use fixed-width types for serialized/protocol fields and checked conversions for dimensions, strides, plane offsets, bitrate, duration and allocation sizes. C23 checked-integer facilities are useful where the actual compiler/libc supports them; provide an audited fallback if needed. Pointer representation, atomic lock-freedom and cache-line size are platform properties, not universal C-language guarantees. Do not rely on volatile for inter-thread publication or on packed structs for live atomic state. [C21]

## Internal modules are responsibilities, not compulsory copies

| Module | Owns | Must not assume |
|---|---|---|
| Session and authority | Portal requests, source identity, lifecycle intent | That restore tokens or node numbers remain valid indefinitely |
| Capture adapter | Buffer records, immutable ingress metadata, negotiated source contract | That a returned wrapper or borrowed metadata remains usable |
| Plan selector | Capability evidence and chosen import/processing/encode route | That all backends share one native image representation |
| GPU backend | Native allocations, commands, synchronization and encoder state | That submission, input release, DPB release and output readiness coincide |
| Timeline/audio | Clock mapping, sample positions, pacing and codec delay | That arrival time is presentation time or all clocks have one epoch |
| Mux/output | Packet ordering, container state, offsets and L/W/D/C checkpoints | That accepted bytes are completed, durable or independently playable |
| Replay/export | Immutable chunks and export references | That rolling storage may be overwritten while a reader is pinned |
| Diagnostics | Effective configuration, counters and bounded trace records | That user-thread CPU alone represents overhead |

The common backend interface should transfer semantic descriptors and explicit completion tokens. A possible internal shape has a source descriptor, an immutable plan/configuration object, an admitted job identifier, and callbacks/events for input retirement and packet readiness. This is a design sketch, not a prematurely frozen public ABI. An event loop can invoke several responsibilities directly on one owner when no blocking or concurrency benefit requires a queue.

Allocation handles require tagged types with explicit ownership conventions. Differentiate borrowed DMA-BUF, owned duplicate, imported image, mapped CUDA resource, VA surface, DRM syncobj, sync_file, opaque semaphore export and codec output pointer. Each type has a specific release action and validity interval. Avoid a generic integer fd plus boolean ownership flag passed through unrelated stages.

## Build and hot-path discipline

Select a reproducible compiler/toolchain with the C23 features actually used. Keep warnings strict for ownership-sensitive code, signedness and truncation. Use sanitizers on CPU-only and supported integration paths; a clean ThreadSanitizer run does not validate external GPU memory. Keep optional CPU instruction specialization behind runtime detection and a portable fallback. Inspect generated assembly when the SPSC implementation or an arithmetic helper becomes a measured hot path, rather than treating a language-level claim as a performance result.

Preallocate bounded pools per configuration after capability discovery. Maintain a controlled growth/reconfiguration path for legitimate format changes, including 8K or unusual dimensions if the actual device supports them. A test fixture's size must not be encoded as the maximum accepted stream. On resource-allocation failure, return a specific capability or budget result; do not silently reduce resolution or fidelity unless the selected policy permits it.

Use mature mux/codec libraries initially. Where a library default is a real bottleneck or correctness constraint, compare a narrow audited patch or native adapter with a complete specialized replacement. Examples are configurable Matroska timestamp scale, native CUDA-array output from conversion, and a backend input-retirement hook. A custom AV1 muxer, memory reclamation algorithm or scheduler can be justified by a concrete missing behavior or by a mechanism that improves whole-system cost while preserving required behavior. It needs a complete validation plan and fair lifecycle evidence; it need not prove that adaptation is impossible.

Packaging must document the selected components and comply with their actual licenses and redistribution terms. No particular licensing model was specified by the user, so the architecture keeps dependencies replaceable rather than inventing a legal restriction. Release engineering should audit the final dependency set; this report does not make a legal compatibility determination.

# Validation that can select the winner

## Establish complete controls and meaningful challengers

The first complete recorder is a control with correct pixels, timing, audio and finalization. Its role is to expose costs across the real lifecycle. Retain it unchanged as the cumulative regression reference. A faster conversion kernel alone cannot replace it as the baseline, because the new path may retain buffers, use another GPU engine or alter image quality.

| Decision | Simple complete control | Strong challenger and decisive observation |
|---|---|---|
| Capture | Portal/PipeWire, negotiated DMA-BUF | Supported direct compositor/KMS path; same pixels, authority, cursor, color and offered updates |
| NVIDIA input | Eligible conventional texture/device-pointer path | Native multi-planar array plus real source interop; inspect hidden retile, source lease and game impact |
| AMD input | Correct fused conversion into native YUV | Direct RGB and VPE; inspect precision, accepted range/siting, actual engine and source retention |
| Intel processing | Native VA/VPL input and correct conversion | VEBox/SFC or alternate API; preserve HuC/rate-control path and measure complete synchronization cost |
| Conversion work | Full-frame recompute | Revision-aware damage and no-stage direct encode; decode against the same reference after skips/reconfiguration |
| Encoded ownership | Retain driver buffer | Copy to owned slab; compare encoder stalls, CPU bytes and sink lifetime |
| Output | Dedicated buffered writer | Integrated ordinary io_uring, then favorable direct/polling cases; same bytes, durability and resources |
| Queue structure | Direct owner dispatch or bounded mutex/SPSC | Only a demonstrated MPMC need justifies advanced families, including their allocation/reclamation costs |
| Timestamp container | Existing mature muxer with measured quantization | Configurable fMP4 or audited Matroska scale; precision, recovery, CPU and actual players |

A challenger is not fairly tested if its enabling mechanism is missing. Native arrays need native writes; direct RGB needs actual eligible color/input; io_uring should be allowed to remove a handoff where that is its benefit; a bounded queue must be compared at the same retained-resource budget. Conversely, a hypothetical unimplemented redesign is not a measured winner.

## Workloads and offered load

Use independently captured or generated references covering static terminal text, colored subpixel patterns, cursor-only movement, scrolling code, browser/UI animation, mixed video and desktop, high-motion games, gradients and HDR highlights. Include small windows, portrait/rotated outputs, fractional scaling, odd dimensions/alignment boundaries, multiple monitors with mixed refresh rates, and cross-GPU configurations. Choose sizes and rates from supported devices; 1080p, 4K, 8K, 60 and 240 Hz are coverage examples, not a scope boundary.

Measure idle/static periods as well as saturation. A poller that wins during sustained encoded output can waste a core between sparse desktop updates. A damage path can win on a terminal and lose on full-screen motion due to bookkeeping and conservative overdraw. A principled dispatcher should use observable conditions such as required transforms, source format and measured damage coverage, with hysteresis when needed to avoid expensive oscillation. Include dispatch overhead and maintain a correct fallback.

An externally defined offered frame schedule or reproducible game trace is necessary to expose dropped work. Latency among surviving frames alone rewards a recorder that silently drops difficult inputs. Report offered frames, admitted frames, encoded/displayed frames, skipped time, discontinuities and output bytes alongside latency. If an application-specific source supplies only content changes, measure change delivery and display-duration correctness separately from an invented timer rate.

## Quality oracles

Separate capture correctness, conversion error and codec loss. Compare captured pixels with an independent intended source where possible; compare GPU conversion against a precise CPU reference with specified transfer, matrix, range, chroma filter and rounding; then decode the encoded stream with an independent AV1 decoder and compare against the conversion reference. Test both luma and chroma. A high score against an already incorrect conversion does not establish correct recording.

Use bitstream inspection for profile, bit depth, range/primaries/transfer, temporal-unit structure, random access, reference/repeat behavior and HDR metadata. Decode long sequences spanning resets, drops and hidden/repeated frames. Use independent decoders where practical and validate real intended players/editors, including seeks, VFR, fine timestamps and incomplete-file recovery. A codec library accepting a packet is not the same as every consumer displaying it correctly.

PSNR/SSIM, SSIMULACRA2 and VMAF can reveal different failures; none alone establishes desktop fidelity. Pin the exact metric version/model and its domain. Netflix introduced v1 models in 2026, so 'VMAF' without a model identifier is not a stable experiment. Use an applicable no-enhancement-gain mode when evaluating codec distortion and report limitations on text, chroma and HDR. Inspect moving text, saturated one-pixel detail, gradients and temporal filtering subjectively at the intended display scale. [C22, C23]

Report a rate-distortion-resource surface rather than one magic score: output bitrate or file size, objective and task-specific quality, source-to-packet delay, frame drops, CPU/energy and game interference. If preferences do not choose between nondominated points, preserve them as modes instead of inventing weights.

## Instrument the full system

Count recorder threads, compositor/PipeWire work, kernel workers, polling threads, interrupts and filesystem writeback. Track GPU graphics/compute, video, copy and memory activity; source/encoder pool occupancy; VRAM and host/page-cache/pinned memory; power; and the recorded application's frame times. Report absolute cost plus change from the fixed reference. Percentage utilization across different engines or CPUs is not directly comparable work.

Use GPU timestamp queries where applicable and calibrated timestamps to map device and host time, including reported maximum deviation. Vulkan provides a calibration API; do not subtract timestamps from unrelated clocks as though they shared an epoch. Instrumentation itself can add barriers, query traffic and wakeups, so use a low-overhead measurement configuration and quantify perturbation with and without it. [C24]

Report distributions of complete per-frame paths: p50, p95, p99, p99.9 where sample size supports it, and maxima/stall events with run context. Do not add independently measured stage p99 values and call the sum end-to-end p99. Long tails matter to both game stutter and recorder freshness. For actual display-to-display latency, use an appropriate external optical or camera measurement; software timestamps alone do not establish photon timing.

## Fair experiments and fresh confirmation

Separate exploratory tuning from confirmation. Randomize or alternate complete candidates under matched thermal state, clocks/power policy, content, driver, CPU budget and background workload. Repeat runs and report variation, confidence intervals where justified, and sample counts. Include challenger-favorable regimes and boundaries, not only the workload that favored the first prototype.

Once a configuration is selected, confirm on fresh traces and at least the configurations whose mechanisms differ. Keep the original control and cumulative nonregression limits fixed while applying multiple optimizations. An improvement relative to the immediately preceding version can conceal a large regression from the original. Rerun checks affected by a change rather than broadening tests without a concrete risk.

Production acceptance requires actual hardware from the three vendors and representative compositor implementations. The specific device/driver matrix should be selected from deployment needs, including an older AV1-capable family and the newer media architecture where its path differs. Record kernel, firmware, compositor, PipeWire, portal, Mesa/proprietary driver, SDK, FFmpeg and metric versions. A result on one Ada or VCN configuration is not a universal family guarantee.

## Failure and concurrency validation

Exercise paused participants at every ownership boundary; delayed GPU completion; output arrival without input release; input release without a matching packet; format change with old jobs in flight; buffer removal between a worker request and wrapper return; failed release-signal export after accepted GPU work; and device loss. Confirm no new generation can be validated by an old completion.

Inject queue-full, pool exhaustion, allocation/registration denial, cursor-only updates, missing damage history, timestamp discontinuity, audio underrun/overrun, clock drift, suspended sources, portal revocation, sink stalls, short writes across every vector boundary, out-of-order CQEs, cancellation races, final-patch failures, ENOSPC/EIO and crash truncation at structural boundaries. Verify the declared response and retained media, not merely absence of a process crash.

Use GenMC for native publication/reuse/notification, supplemented by ownership assertions, stress tests and supported sanitizers. These finite Python models do not prove C23 weak-memory or GPU correctness, nor whole-recorder wait-freedom. [C25]

# Checks performed for this design

## Executed, reproducible checks

Three small Python programs are included with the companion bundle. They do not implement the recorder. Their purpose is to challenge consequential design invariants with independent small oracles and deliberately broken alternatives.

The concurrency checker explored finite abstract schedules for event notification and pooled GPU-resource lifetime. Correct variants had no violation in the modeled state space: unconditional notification visited 37 states, armed-RMW notification 93, and retirement after GPU completion 286. Broken variants produced concrete witnesses for a lost wakeup, reuse while the GPU still reads, and destruction before GPU retirement. The queue chapter records transitions and scope. These results support those abstract rules only.

The damage/arithmetic checker used a deterministic full-image oracle against incremental conversion into three rotating destinations. Across 20,000 source updates, including 5,021 skipped submissions, it checked 14,979 submitted results. Revision-aware accumulated damage matched full recomputation. A deliberately broken latest-damage-only method produced a wrong image. The model is a scalar 4 by 4 input with a 2 by 2 box-filter output; it does not validate a real shader, chroma filter, GPU fence or driver modifier.

The same program verified tight-layout image sizes, the stated 4K240 traffic arithmetic, encoded-rate conversions, lookahead bounds and Matroska relative-time spans. It recorded the source-derived range constants and checked P010 bit placement; it did not implement an RGB-to-YCbCr endpoint oracle. It checked calculated day-end timestamps for 60, 240, 60000/1001 and 240000/1001 fps, rather than enumerating every timestamp. Absolute rational rounding at those probes stayed within half a nanosecond, consistent with the general nearest-integer rounding bound; repeated rounded-period accumulation drifted. It also demonstrated that distinct chroma arrangements can map to the same 4:2:0 average.

The output checker independently compared an interval-based completed-prefix frontier with a completed-byte-set oracle. It enumerated every positive completion partition/interleaving for two small write layouts, producing 748 complete traces and 1,833 checked prefixes. The frontier matched throughout; a broken highest-completed-end rule failed at 834 prefixes. Independent composition/multinomial counts matched the enumeration. A separate five-trace cancellation model retained payloads until target completion; recycling on a cancel CQE violated the conservative contract in two traces, including one modeled access after recycle following an EALREADY result. These are finite abstract checks of nonoverlapping writes and limited cancellation orders, not kernel execution. They do not cover zero/negative writes, fsync, container recovery, identifier reuse, lost CQEs or memory ordering.

## What remains unestablished

No GPU capture, encoder, native interop, shader, storage throughput or player-compatibility benchmark was run. No production C23 queue or recorder was built. The finite models do not prove weak-memory correctness, unbounded execution, release-fence interoperability, runtime callbacks or whole-system progress. The proposed FFmpeg Matroska change was not implemented.

The report's primary-source evidence establishes eligibility and constraints. Its architecture and functional defaults are proposed. Its arithmetic is derived and checked. Its performance rankings remain open until the complete candidate comparisons run on hardware. No optional experiment was abandoned because a prototype was already 'good enough'; this environment lacks the device access needed for those experiments.

# Decision ledger and implementation order

## Consequential decisions and reopening conditions

| Selected design direction | Reason established here | Reopen when |
|---|---|---|
| Common semantic contract, multiple native plans | Format, interop and completion semantics differ materially | A new backend can share a representation without excluding a stronger route |
| Portal/PipeWire control path | Portable authorized source selection and buffer negotiation | A direct supported path wins the same capture semantics with acceptable deployment |
| Persistent imports and generation-safe retirement | Per-frame import is avoidable; wrappers and allocations have distinct lifetimes | Driver restrictions require a different lifetime or import policy |
| Direct RGB and native arrays remain first-class | They can eliminate complete hidden/visible image stages | Exact color, modifier, synchronization or retention makes them ineligible or inferior |
| No global per-frame GPU wait | Precise dependencies preserve concurrency and isolate waits | An API cannot express the dependency; use the narrowest correct fallback |
| Bounded SPSC/owner-local normal path | Actual topology does not establish a hot MPMC requirement | Multi-source scheduling produces a measured shared queue bottleneck after simpler decomposition |
| Mature muxer with timing audit | Container correctness is substantial; library defaults can still be changed | A missing behavior or measured whole-system improvement favors a complete specialized muxer over an adapter/patch |
| Buffered writer and ordinary io_uring control pair | Compressed rates and lifetime costs differ from raw GPU traffic | Storage-favorable workloads justify direct, polling or later-kernel alternatives |
| Separate quality/freshness/fidelity modes | Objectives have real conflicts and the user supplied no scalar weights | Deployment budgets choose a narrower feasible set |
| Full lifecycle validation before SOTA claim | API names and isolated charts cannot establish system superiority | Complete, fresh, matched hardware evidence establishes a scoped nondominated result |

## Implementation sequence with usable checkpoints

**Checkpoint 1: observable complete control.** Implement authority and source selection, immutable generations, persistent imports, a correct conversion, one native encoder path, timing/audio, mature muxing and buffered output. Include effective-configuration diagnostics, clean stop and failure preservation. Independently decode its output and compare pixels/timestamps before treating it as a performance control.

**Checkpoint 2: lifetime and cross-vendor coverage.** Add AMD, NVIDIA and Intel native backends with input/DPB/output retirement contracts, exact source synchronization and reconfiguration. Preserve original control output semantics. Validate each backend's bit depth, color, format, rate control and unsupported-path behavior on real devices.

**Checkpoint 3: representation challengers.** Implement eligible AMD direct RGB, NVIDIA native arrays and strong Intel VPP/VPL/VA alternatives. Complete the import/export and failure paths. Compare source retention, conversion accuracy, device memory traffic and application interference. Add revision-aware partial conversion and native repeated-display candidates only with independent reconstruction/decode oracles.

**Checkpoint 4: output and temporal polish.** Compare integrated ordinary io_uring with the buffered control, audit asynchronous AVIO ownership, test fine timestamp containers and implement the chosen small muxer patch if required. Add replay/export with pinned immutable chunks and explicit random-access boundaries. Verify finalization and recovery under injected failures.

**Checkpoint 5: evidence-selected release candidate.** Tune affordable encoder analysis and concurrency, confirm on fresh workloads, verify cumulative nonregression against the original control, and publish exact supported configurations and material limits. Ship modes for genuine Pareto tradeoffs. Keep a reliable fallback for a driver update or source modifier that invalidates a specialized path.

The next implementation decision should be made with the first actual target machines and intended consumers. That information can select preset values and budgets; it does not require redesigning the core contracts established here. The strongest justified deliverable at this stage is this concrete architecture, its preserved challengers, explicit invariants and reproducible design checks, ready to turn into a measured C23 implementation.

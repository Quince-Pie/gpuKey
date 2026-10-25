# Design decision and evidence status

**Select a capability-driven recorder with a small ownership core, native GPU encoding paths, and measured alternatives for capture, color conversion and output.** The portable control path is the ScreenCast portal plus PipeWire. The performance path is selected from the actual source allocation, GPU, driver, color contract and consumer requirements. Keep frames on a compatible GPU whenever that is the winning complete path. Do not require an intermediate image merely to make backends look alike.

The proposal is a serious candidate for a state-of-the-art implementation, not a claim that an unimplemented recorder already wins a benchmark. Primary specifications and source mechanisms were inspected on 19 October 2026. This environment exposes no GPU device. We can establish API constraints, derive bounds, compare mechanisms and check abstract invariants; we cannot establish capture throughput, hardware image quality, driver interoperation or game interference here. Those remain explicit acceptance tests.

The likely codec is interpreted as **AV1**. AVI is a different container and is not assumed. AV1 hardware encoding in 4:2:0 at 8 or 10 bits is the common target across the relevant vendors. It cannot simultaneously promise preservation of arbitrary RGB desktop detail: chroma subsampling is a separate, irreversible loss. A fidelity profile must be allowed to select a different supported representation or codec when that requirement is consequential.

## The architecture to implement

The application constructs an executable plan during session negotiation. Its candidates are: directly encode a compatible captured RGB or YUV image; convert once into the encoder's preferred allocation; use a dedicated video processing engine; or make an intentional ownership-isolating GPU copy when retaining the capture buffer costs more. GPU API names do not determine the winner. Exact importability, synchronization, conversion accuracy, source retention, engine contention and output latency do.

The selected path exposes semantic events instead of one universal texture type: source available, source no longer read, encoder input consumed, reference image no longer retained, packet available, sink bytes completed, and durability checkpoint reached. Some events coincide on one backend and differ on another. Collapsing them creates either premature reuse or unnecessary retention.

The initial implementation should contain a complete, simple control: authorized capture, persistent GPU imports, one correct conversion when needed, native hardware AV1, properly timed audio, mature muxing, and a dedicated buffered writer. This is a reference for stronger challengers, not a stopping criterion. Preserve direct RGB input, native encoder arrays, fixed-function conversion and ordinary io_uring as complete alternatives with their enabling mechanisms intact.

| Decision | Proposed direction | What could change it |
|---|---|---|
| Capture portability | Portal/PipeWire by default | A supported direct compositor path improves the same capture semantics and whole-system cost |
| Pixel representation | Backend-native allocation and format | A different allocation reduces conversion or releases scarce source buffers earlier |
| GPU scheduling | GPU dependencies; no per-frame device-wide wait | A backend lacks a usable interop bridge, requiring an isolated CPU wait |
| Video codec | Hardware AV1, capability-gated 8/10-bit 4:2:0 | Consumer compatibility, fidelity requirements or measured encode quality favor another path |
| Queues | Owner-local state and bounded SPSC at real thread boundaries | A demonstrated many-producer/many-consumer workload survives simpler decomposition |
| Output | Mature muxer; buffered write control versus integrated ordinary io_uring | Measured sink pressure justifies direct I/O, polling or later-kernel alternatives |
| Quality policy | Explicit color contract and consumer-specific constraints | A negotiated source cannot supply the requested color or fidelity |
| Overload | Bounded admission with a declared continuity policy | A lossless-capture mode requires a sustainable source rate or explicit stop |

## How to read the evidence

**Verified** means a retrieved primary contract or inspected implementation says the stated thing; it does not mean every installed driver implements it correctly. **Derived** means arithmetic or an invariant follows from explicitly stated assumptions. **Checked here** identifies actual executable finite checks. **Proposed** identifies a design choice awaiting implementation. **Unresolved** marks an experiment or product decision that can change the choice.

This distinction applies throughout. A vendor feature matrix is an eligibility filter. A successfully created encoder is stronger evidence. Decoding and inspecting output while measuring the whole pipeline is stronger still. A paper's progress theorem belongs to its complete algorithm under its assumptions, not to a queue vaguely inspired by it.

# Requirements, consumers and operating modes

## Constraint origins

The user's requirements are Wayland Linux; eventual C23 implementation; modern AMD, NVIDIA and Intel; Linux 6.12 and later; AV1 as the likely codec; deep investigation of overhead, quality, throughput and latency; and a rigorous design before implementation. The request does not fix resolution, frame rate, GPU model, compositor, container, target player, memory budget, storage device or live transport.

Local recording is a provisional first product mode. Live output, replay, HDR, multiple sources and editing are kept visible because they change architecture and consumer contracts. Their detailed policies must not silently become mandatory features. Nor should a benchmark such as 4K240 become a production limit. Resource bounds are negotiated from actual capabilities, budgets and failure policy.

No scalar weighting of all objectives is invented. Instead, measure a feasible set under each mode's constraints, then compare nondominated choices. Quality improvements that consume latency budget may be appropriate for a file and unacceptable for a live feed. A lower CPU percentage that steals GPU bandwidth from the recorded game can lose the intended objective.

## Operating profiles

| Profile | Primary constraint to make explicit | Candidate policy |
|---|---|---|
| Interactive/live | Maximum frame age and transport availability | No future-frame lookahead unless budgeted; bounded reorder; discard stale unsubmitted work |
| Balanced local file | Visual quality and low application interference | VFR when consumers accept it; quality-oriented rate control; only measured, affordable analysis |
| Desktop fidelity | Text, fine chroma and correct color | Compare 4:2:0 loss against a supported 4:4:4/RGB alternative; preserve appropriate bit depth and tags |
| High-throughput capture | Sustainable offered frame rate with a stated drop bound | Tune engine use, memory layout, concurrency and encoder settings under that bound |
| Replay | Retention duration, export behavior and bounded memory | Store immutable encoded chunks, retain configuration and valid random-access dependencies |
| Durable recording | Defined loss window under process or power failure | Container boundaries plus completed write and synchronization checkpoints |

The interface must show meaningful effective behavior: source, output format, HDR/SDR state, target or cap, active latency/quality mode, continuity policy and output health. Internal queue names, semaphore APIs or kernel flags belong in diagnostics, not the recording workflow.

## Define what is being recorded

A window recording, a composed monitor image and a physical display's post-LUT electrical output are different results. Hardware overlays, cursor planes, scaling, HDR tone mapping and display transforms make that distinction observable. The capture backend must advertise its semantics. Comparing a raw primary-plane KMS read with a compositor-rendered window capture is not a fair speed comparison.

For multiple monitors, decide between independent streams and an explicitly composed canvas. Mixed refresh rates do not imply that every source must be duplicated at the fastest rate. Independent tracks can preserve native timing where consumers support them. A combined canvas needs a declared sampling rule: each output timestamp selects an available frame from every source, records staleness, and composites on an appropriate GPU. Cross-device copies can then dominate. Window movement between GPUs and display hotplug require renegotiation, not hidden interpretation of stale dimensions.

The source cursor may be embedded, supplied as metadata, or omitted. Embedded plus local compositing produces two cursors. Metadata cursor changes can be the only visible update during an otherwise static screen. Audio sources require their own authority and identity; the portal's restricted PipeWire remote is not a license to inspect the user's general audio graph. [C01]

# First principles: remove avoidable work before tuning it

## The repeated units of state

The full lifecycle contains several different units. A captured image identifies allocation, generation, interpretation, timestamp and synchronization. A conversion job identifies only the transform and destination actually needed. An encoder input can remain referenced after submission. An encoded temporal unit can contain a different relationship to displayed pictures than one-input/one-output. A muxed byte span has offset, owned storage and completion state. A durable segment adds a recovery boundary. The design should materialize each only when its consumer needs it.

A per-frame object graph with duplicated format descriptions, heap allocations and reference-counted wrappers is convenient but not required by the media. Immutable generation records can hold device, dimensions, format, modifier and color metadata once. Per-frame descriptors carry stable identifiers, timing and changing metadata. Pools can use compact indices where ownership is single-threaded or well-defined; atomic reference counts belong only on objects actually shared independently, such as a replay chunk simultaneously being written and exported.

Similarly, a stage boundary is not a command to create another full image. Crop, rotation, scaling, color conversion and cursor composition can be one GPU dispatch when all are required. Direct encoder conversion may remove that dispatch entirely. Conversely, an explicit conversion into owned memory can improve the system by releasing the compositor allocation much earlier.

## Bandwidth and storage are different scales

For tightly packed 3840 by 2160 images, ignoring pitch, alignment, compression and metadata overhead, the following quantities are derived:

| Representation | Bytes per frame | Purpose of the comparison |
|---|---:|---|
| RGBA/BGRA 8-bit | 33,177,600 | Common RGB capture footprint |
| NV12 | 12,441,600 | 8-bit 4:2:0 encoder input |
| P010 | 24,883,200 | 10-bit samples stored in 16-bit words |
| RGBA half-float | 66,355,200 | Higher-precision linear/HDR processing candidate |

At 240 frames per second, reading RGBA8 once and writing P010 once accounts for 13.934592 GB/s of image traffic. An extra RGBA8 read-and-write copy adds 15.925248 GB/s. These are traffic estimates, not measured DRAM utilization: tiling, caches, compression, unified memory and driver internals change physical transactions.

By comparison, 100 Mbit/s of encoded output is 12.5 MB/s; even 1 Gbit/s is 125 MB/s. This explains why eliminating raw-image passes deserves early attention and why a compressed CPU copy can be beneficial. It does not prove storage irrelevant: bursty keyframes, multiple streams, slow devices, page-cache pressure, durability and finalization can still determine tail latency.

## The cost of ownership

Direct source encoding removes a conversion/copy but can lengthen the source lease. The ordering is backend-dependent: a native encoder can consume promptly, while a conversion can queue behind busy GPU work. If source allocations are scarce and the encoder retains an input through analysis or delayed consumption, the compositor can stall before the encoder itself reaches capacity. The correct comparison includes actual last-source-read time, source-pool occupancy, compositor buffering, raw bytes moved, output quality and game frame times. A locally cheaper producer can leave a more expensive system.

Maintain independent counts for source leases, recorder-owned conversion surfaces, encoder references, output buffers and pending sink bytes. Do not approximate every one by a single queue length. A frame count also fails for compressed output, where packet sizes vary widely.

Little's law, occupancy equals arrival rate times average residence time in a stable system, is useful for estimating pool pressure. It does not size the pool against long tails or prove bounded latency. A practical pool must satisfy the API's minimum surfaces and a measured residence distribution under the chosen admission policy. If the required memory is unacceptable, change concurrency, quality settings, cadence or ownership; do not merely deepen a queue.

## Causal latency

Measure source timestamp to capture availability, submission, input consumption, encoded packet, mux availability, completed write and requested durability separately. A recorded file has no network receiver; live output adds transport, receiver buffering, decode and display. Do not market storage completion as photon-to-photon latency.

Future-frame analysis imposes a causal lower bound. Sixteen future frames at 60 fps require about 266.7 ms of future content; at 240 fps they require about 66.7 ms. Reordering and output buffering can add more. Enlarging asynchronous depth increases outstanding work; it is not automatically an increase in either minimum latency or useful throughput. High throughput and low latency can coexist below saturation, but an unbounded admission stream under overload cannot keep both.

# Capture authority and source architecture

## Portable control: ScreenCast portal and PipeWire

Use the ScreenCast portal for portable user-authorized source selection, then connect to its restricted PipeWire remote. The normal sequence creates a session, selects sources, starts it and opens the remote. Treat request cancellation, denial and asynchronous session closure as ordinary state transitions. A restore token is a revocable, single-use restoration mechanism that can be replaced by a new returned token; it is not permanent capture permission. [C01]

Version-gate fields instead of equating kernel version with desktop API version. Portal version 6 adds a stable `pipewire-serial` property for targeting through `PW_KEY_TARGET_OBJECT`; a raw node ID can be reused. Use the stable property when supplied and the documented older behavior otherwise. The portal's logical position and size are not a substitute for negotiated pixel dimensions. Cursor modes and available source types must be read from capabilities. [C01]

PipeWire supplies format negotiation, buffers and per-frame metadata. Prefer DMA-BUF when the exact import path is usable, with a clearly identified shared-memory fallback. A negotiated DMA-BUF handle does not establish that the consumer can decode its tiling, wait on its producer or obtain the requested color interpretation. Device placement, format/modifier intersection and synchronization are first-class plan inputs. [C02]

Avoid busy polling for screen updates. Content-driven delivery can remove wakeups and repeated work on static desktops. A CFR consumer still needs a representation for elapsed display time, but that does not require recapturing unchanged pixels on every timer tick.

## Direct compositor capture as a serious challenger

The `ext-image-copy-capture-v1` protocol can let a client supply capture buffers under compositor-provided constraints. It is a meaningful alternative when present and appropriate to the source semantics. Its version-1 session allows at most one outstanding frame object; after completion or failure that object must be destroyed before the next. The compositor may wait for a change, so a request is not a promise of a periodic timer callback. [C03]

The buffer-damage contract is particularly important. The client reports regions needing refresh since that particular destination buffer was last captured, not merely the latest source damage. Initial or unknown contents need full refresh. The frame's ready event establishes capture availability under this protocol; ordinary `wl_buffer.release` is not its reuse signal. The client must still wait for its own GPU readers before reusing the destination. Do not assume a synchronization protocol defined for `wl_surface` automatically applies to a capture object. [C03]

A direct path may remove compositor/PipeWire copying or buffering, but that is an empirical question. A portal backend may already use equivalent mechanisms internally. Compare the same monitor/window semantics, cursor, color, cadence and authorization model. Keep this backend optional rather than pretending every Wayland compositor implements a new protocol.

## KMS and privileged capture

KMS is an optional, separately authorized path when its actual semantics and deployment model are acceptable. `GETFB2` access to framebuffer handles is restricted to DRM master or the relevant administrative capability; access to a render node does not confer permission to read the display. Fresh returned handles need correct lifetime accounting, including shared planes. A helper should have the minimum responsibility needed to provide authorized capture resources. [C04]

Capturing one primary plane may omit overlay video, hardware cursor, transformations or color processing. Reading scanout without a coherent snapshot protocol can also produce a result that differs from a compositor-rendered capture. Evaluate exact final pixels, update synchronization, multi-plane composition, HDR, rotation, protected content, multi-GPU and permissions before ranking it. The existing GPU Screen Recorder project is a useful practical control across the three vendors, but its own comparative marketing is not independent evidence that a new design wins or loses. [C05]

## Fallbacks should preserve declared behavior

If DMA-BUF import is unavailable, an explicit shared-memory upload can keep the recorder functional with an understood cost. If AV1 is unavailable, report that fact and use a previously selected fallback policy. Do not silently replace HDR with SDR, a selected window with the whole monitor, or a requested fidelity mode with a lower-fidelity codec. These are semantic changes, not implementation details.

# Negotiation, frame records and resource lifetime

## Immutable generations, mutable frame values

A source generation describes the source identity, GPU/device relationship, dimensions, plane interpretation, format, modifier, crop/transform basis, color contract and negotiated metadata/synchronization features. A buffer record represents one backing allocation within that generation. A frame record captures changing values: frame/sequence ID, PTS, flags, damage, crop, cursor state and acquire/release points.

Copy per-frame values before returning a PipeWire buffer. The buffer wrapper and its metadata are borrowed storage that can be reused. A persistent GPU import or duplicated FD retains a resource reference, but does not freeze its pixels. Content immutability is supplied by the producer/consumer synchronization contract, not by the fact that a Unix FD remains open.

Use persistent imports keyed by a stable buffer record and generation. An integer FD is not a stable resource identity: descriptor numbers are reused. Multi-plane offsets, pitches and aliases are part of the import. Validate size arithmetic with overflow checks, but follow the DMA-BUF contract: `maxsize` or chunk size can be zero for a GPU buffer and must not by itself be treated as an empty frame. Additional data entries can be synchronization objects, so the total data count is not automatically the number of pixel planes. [C02]

`DRM_FORMAT_MOD_INVALID` denotes an implicit modifier convention, not guaranteed linear memory. A modifier that an API can import for sampling may be ineligible for encoder use, storage writes or external re-export. Query the exact image format, tiling/modifier, usage, external handle types and device pairing. Device-ID negotiation introduced in PipeWire's 1.6 development cycle is useful when actually supported; it does not retroactively exist on every 1.4 deployment. [C02, C06]

## Negotiate once; validate each frame cheaply

At generation setup, intersect producer formats/modifiers with import and processing capabilities, select source/output color semantics, allocate pools, compile or select kernels, and prepare synchronization bridges. Initialize each persistent import and view when its buffer is added; buffers may arrive or disappear independently within a configuration. Per frame, perform only bounded validation of changing fields and state: correct generation, known buffer, sequence/flags, valid timestamp and damage bounds, cursor updates, and an available destination/encode credit.

Metadata with a crop, transform, discontinuity or corruption flag is part of the frame contract, not an optional visualization hint. A zero framerate denominator must be rejected where invalid; a negotiated framerate of 0/1 can denote variable-rate video and needs an explicit maximum or admission policy rather than a division by zero. PipeWire's raw-video description also carries range, matrix, transfer, primaries and chroma location. [C07, C08]

If reconfiguration invalidates imports or color assumptions, create a new generation. Old in-flight jobs retain their old immutable records until retirement. Do not mutate a global width/height/color object that old GPU submissions still interpret.

## Early return with an actual release promise

With a usable explicit-sync producer contract, returning the PipeWire wrapper can precede completion of GPU reads. Establish this sequence: copy all mutable metadata; retain independent imports and synchronization references; successfully arrange acquire wait, all source reads and release signaling; publish the release promise; then return the wrapper. The release point must cover the actual last source reader. A conversion-only path can release after its conversion reads; direct encode needs the backend's input-consumed condition. [C02, C09]

The release path may be a GPU signal or a valid host retirement path. For example, AMF's input-release observer is a documented reuse notification, not itself a GPU fence or PipeWire timeline signal. Its owner must issue the negotiated signal after that event and keep the callback/signal state alive. Early wrapper return does not require pretending every encoder exposes a pre-submittable all-GPU chain. [G26]

Current PipeWire metadata includes a negotiated RELEASE feature and an `UNSCHEDULED_RELEASE` flag set by the producer. When that feature is actually negotiated, a consumer clears the flag when it promises a release signal. Leave it set for an unused frame with no access and no promise. Metadata presence or installed version alone is not proof that the feature was accepted. Under the legacy explicit-sync contract, fulfill its release obligation even for dropped frames. A failed submission must not publish a release promise that will never complete. [C09]

Do not access the returned wrapper, chunks, metadata pointers or borrowed FD numbers afterward. Complete metadata publication before handoff. This is stronger than copying an illustrative single-context example that modifies metadata after queueing. The current PipeWire synchronization example uses eventfds as stand-ins for synchronization objects; that does not make blocking eventfd reads a correct DRM timeline implementation. [C10]

Once any source-reading work has been accepted, a subsequent failure to arrange or export its release signal is an outstanding-retirement obligation, not an unused-frame drop. Retain the lease and owned references, then repair the release chain or await the exact accepted work outside the real-time callback. Never expose an unscheduled-release flag as permission to overwrite while accepted readers remain possible. Device loss follows the backend's quiescence/destruction contract.

If the negotiated bridge cannot safely express an early return, retain access ownership until reads finish and return only if the wrapper is still valid. Serialize that validity check and the final queue operation with buffer removal on the owner of the PipeWire wrapper lifecycle. Workers request a return through that owner using stable record/generation tokens. Neither path may block a real-time callback on GPU completion.

## Removal and teardown

PipeWire's stream wrapper is valid only under its stream lifecycle; `remove_buffer` invalidates it. Holding a dequeued pointer is not a general guarantee that stream destruction cannot remove it. Serialize record removal with submission ownership. Mark the record detached, prohibit new use, disconnect the wrapper, and defer destruction of independently owned imports until in-flight references retire. Late completions address stable records and generations, never removed wrappers. [C11]

A removal callback should not wait synchronously for arbitrary GPU retirement. Such a wait can block the loop needed to process other lifecycle events. A single owner for record state, plus immutable ingress/completion tokens, makes the race easier to reason about. The distinction between logical removal and physical retirement is essential during monitor unplug, format change, portal revocation and device reset.

# GPU visibility, dependencies and plan construction

## Four different obligations

Correct GPU sharing requires allocation compatibility, execution ordering, memory visibility and lifetime. A successful import proves only a subset. A host atomic publication makes a descriptor visible to another CPU; it does not make a GPU render complete. A semaphore can order execution without automatically repairing an incorrect image layout or ownership transition. A valid FD can preserve an allocation while its contents are already reused.

For Vulkan, apply the stage/access scopes, image layouts and queue-family ownership transitions required by the actual external/foreign ownership contract. Use timeline semaphores or API-specific dependency objects where supported, and external binary sync files where they are the bridge. A DRM syncobj timeline FD, a sync_file FD and a Vulkan opaque semaphore FD are different objects. Import/export ownership rules differ across APIs; encode them in typed wrappers and one cleanup path per handle type. [C12, C13]

For an implicit-sync DMA-BUF producer, Linux provides export/import sync-file ioctls to bridge fences. Export the appropriate current reservation fences, submit work waiting on them, then import the new completion fence with its read/write meaning. This sequence is not atomic against unrelated users: the buffer lease or other coordination must prevent an interleaving access from escaping the snapshot. CPU mapping synchronization ioctls concern CPU cache access and do not replace the GPU scheduling protocol. [C12]

## Make the selected plan inspectable

The plan records source device and import route, native input allocation, every actual conversion/copy, source-release condition, encoder reference retention, packet availability, synchronization bridges and expected resource costs. Diagnostics should expose why a faster-looking candidate was ineligible: unsupported modifier export, color transform needed, incorrect chroma location, missing fence bridge, driver capability rejection or failed probe.

This is not an automatic graph compiler searching arbitrary paths at runtime. It is a small set of deliberately implemented plans with verified preconditions. Cache successful probes by device UUID/identity, driver and relevant library version, source generation attributes and selected mode. Invalidate when any assumption changes. Never silently choose an unvalidated path because its API advertised a broad extension.

At startup, a small functional probe can create the requested encoder/configuration, submit a known image, decode and inspect its output, and test the synchronization bridge when practical. Persisting the probe result saves repeated startup cost, but a cached result cannot guarantee a different modifier or imported allocation works.

## Avoid global synchronization and unnecessary engine transfers

Prefer dependency chains to `deviceWaitIdle`, stream-wide synchronization after every frame, or CPU polling loops. Where an encoder API has a blocking output operation, give it an owner that can block without preventing capture, command submission or output completion servicing. The host may submit asynchronously even when the vendor exposes only synchronous output retrieval.

A separate GPU queue can overlap useful work, but may also add ownership transfers and synchronization. A transfer engine copy can free shader resources while consuming the same memory bandwidth. Dedicated video hardware may share memory, power and firmware resources with rendering. Evaluate the actual bottleneck and game interference; the word 'dedicated' is not a zero-overhead guarantee.

Cross-GPU processing deserves a separate plan. The GPU best at encoding may be the wrong one if every frame must cross a PCIe link or be retiled. An iGPU can sometimes isolate encoding from a busy discrete GPU; on a different platform, shared-memory contention or import limitations can reverse the result. Compare same-GPU native encoding against real cross-device paths including synchronization, transfer, conversion and display behavior. No device marketing hierarchy resolves that comparison.

# Color, HDR and desktop fidelity

## Specify the output pixels before selecting encoder settings

The color contract includes source primaries, transfer function, matrix/range, bit depth, alpha interpretation and chroma location; output equivalents; any tone mapping; and what metadata consumers receive. Layout such as P010 does not identify the color space. A ten-bit buffer is not proof of HDR, and an HDR monitor does not prove that the compositor supplied HDR screencast pixels.

KWin's documented capture behavior includes rendering screencasts/screenshots into an sRGB representation and tone mapping HDR content. Other implementations can expose HDR-related PipeWire metadata. Inspect the actual negotiated source and implementation behavior. If it has already been tone-mapped to SDR, relabeling it PQ cannot reconstruct the original HDR signal. [C14, C15]

A defensible SDR candidate is an explicitly specified RGB-to-YCbCr transform with output primaries, transfer and limited/full range accurately signaled. sRGB and BT.709 transfer functions are not identical. If the output is tagged with the BT.709 transfer, perform the intended transform rather than copying sRGB code values and merely changing metadata. Alternatively, preserve the accurate source transfer when the codec/container/consumer chain supports it. The best interoperability choice is a tested consumer decision, not an excuse to mislabel pixels.

For HDR, preserve the source's actual PQ/HLG and primaries when supported, or apply a declared tone map into a target SDR/HDR space. Emit mastering and content-light metadata only when known or legitimately derived under a specified method. A plausible-looking fixed mastering luminance is fabricated metadata. The fact that an existing project uses such a fallback is not a reason to copy it. [C15]

## Chroma and quantization are separate quality losses

In 4:2:0, one chroma sample describes a neighborhood of luma samples. Different colored-pixel arrangements can have the same downsampled chroma. No bitrate or AV1 preset can recover which original arrangement existed. Fine colored text, subpixel antialiasing and thin saturated edges therefore deserve a fidelity comparison against a supported 4:4:4/RGB path. Ten-bit 4:2:0 improves quantization precision; it does not restore missing chroma resolution.

RGB8 converted to ten-bit YUV can preserve more precision from the matrix/range operation than quantizing its intermediate YUV to eight bits. It does not add source detail. Conversely, hardware that accepts eight-bit input and emits ten-bit output can save memory but may simply expand already quantized samples. Keep these paths distinct in quality experiments.

For BT.709 nonlinear components, the luma coefficients are 0.2126, 0.7152 and 0.0722; chroma follows the specified differences and scaling. Limited ten-bit luma uses nominal code values 64 through 940, with chroma centered at 512 and nominal range 64 through 960. P010 stores the ten significant bits in the high bits of sixteen-bit words. These give useful endpoint and neutral-gray tests, not a complete implementation specification. [C16, C17]

## Conversion precision, scaling and cursor composition

A fused kernel can combine crop, rotation, scaling, color conversion, range mapping, chroma filtering and cursor compositing. First define the order and spaces in which those operations occur. Physically appropriate alpha blending and many resampling operations require linear-light reasoning; preserving desktop appearance and avoiding ringing still needs consumer-oriented evaluation. Do not assume one filter or one workgroup shape is optimal for both tiny text and high-motion games.

Use sufficient intermediate precision for HDR and matrix operations, then quantize once at the final representation. Compare half-float and full-float arithmetic where it can change error, bandwidth or occupancy. Handle premultiplied alpha correctly. Metadata cursor positions, hotspot, crop and rotation must share a defined coordinate transform, and cached bitmap updates must distinguish 'no new bitmap' from 'hidden cursor.' Test old and new cursor bounds for damage. [C18]

Chroma filtering must match the signaled sample location. Simply retaining one RGB pixel from each 2 by 2 block creates aliasing. A box average is a useful complete control; stronger reconstruction/downsampling filters should be compared on text, saturated diagonals and motion. Filter support extends the damage halo. Dithering can reduce quantization banding, but temporal dither changes static pixels and may increase encoding work. Stable spatial dithering is a candidate when it preserves appearance and static-scene semantics.

Do not force every source through linear RGBA16F. If the encoder can perform the exact needed conversion directly from a compatible image with acceptable precision, that representation adds avoidable work. If a fixed-function RGB conversion cannot perform a transfer conversion, tone map, cursor blend or required scale, it is not a complete replacement for those operations.

# Conversion, damage and unchanged frames

## Four complete alternatives

Compare direct input with hardware conversion, a dedicated VPP/VPE path, a fused shader writing native encoder storage, and a straightforward correct conversion control. Each candidate must produce the same intended output interpretation and lifecycle. A vendor path that silently falls back to compute is still a path, but its actual engine use must be measured. Do not credit it with fixed-function isolation merely because a preference flag was set.

For shader work, group luma/chroma production so the same source neighborhood is reused when beneficial. Coalesced loads/stores, register pressure, occupancy and the native image layout matter. Candidate 8 by 8 or 16 by 16 workgroups are tuning points, not requirements. Measure kernel time, device memory traffic, source lease time and game effects. Fusing a stage that prevents hardware compression or forces an export can lose despite fewer dispatches.

A native array or tiled encoder target can eliminate a hidden driver conversion. A mandatory linear NV12/P010 buffer in the common API would exclude that improvement before testing it. Keep external sampling capability and encoder-write capability separate: the allocation ideal for capture can be a poor encoder input, and vice versa.

## Damage is history about a particular image

Authoritative source damage can avoid full-frame conversion when little changes. It is useful only with a complete destination image. Distinguish the immutable configuration generation from the advancing content revision. Each destination carries both its configuration generation and its last valid content revision. Rotating through an encoder input pool means each destination can contain a different content revision. The update region for destination j is the union of source changes since that destination's last valid revision, transformed through all required crop/scale/filter operations.

A scheme that applies only the latest frame's damage to whichever pool slot is free is incorrect. A pixel changed two frames ago can be missing from an older slot even if that pixel is not in the latest damage. Consume authoritative damage, cursor changes and discontinuity information before discarding an observed input. If an unobserved gap exists and the producer does not guarantee cumulative damage across it, force full refresh. Initial contents, missing damage, color changes, transform changes and untracked source discontinuities invalidate the whole destination.

Maintain bounded history using revision-stamped tiles or a damage log whose overflow forces full refresh. The representation is a design choice: per-tile last-change revision can answer which regions changed since a slot's valid revision without storing every intermediate rectangle. A rectangle history can be cheaper for a few large updates. Compare both under cursor-only, scrolling and full-motion workloads, including merging cost and overdraw. A full-frame GPU hash is a scan and synchronization cost; it is not a free substitute for reliable compositor metadata.

Snapshot each conversion job's intended content revision. Advance destination validity only after successful completion, or represent a pending revision whose consumers depend on that job and whose failure invalidates the destination. A late completion from an old configuration must never validate a newly reused slot.

For each changed region, include the support halo of scaling/chroma filters and the old and new cursor rectangles. Converting a partial source without having valid unchanged destination pixels is not a valid optimization. Once submitted to the encoder, a destination cannot be patched until its input and any relevant reference retention have ended.

## Unchanged presentation need not mean repeated capture

A static screen may require no new image capture or conversion. A VFR file can represent elapsed display time through timestamps and durations if the consumer chain handles it correctly. CFR output needs a valid representation for repeated display. The AV1 format supports show-existing behavior, and some vendor APIs expose relevant controls, but support and reference-state obligations differ. Repeating an arbitrary compressed packet is not generally valid.

Retain this as a backend-native challenger: no-op capture/conversion plus valid repeated presentation, compared with ordinary encoder submission of an unchanged image. Verify the emitted temporal units, reference IDs, timestamps, rate-control response, source retention and decoder output. The existence of a force-show-existing enum does not prove zero input reads or zero encoder work. If a path cannot express a safe repeat, ordinary encoding remains the correctness control.

Damage can also guide quality allocation, but ROI semantics differ across vendors. A change map is not automatically a perceptual importance map: a static terminal may be more important than a moving background. Keep activity, text/fidelity importance and encoder-specific ROI translation separate, and verify that requested hints were actually accepted.

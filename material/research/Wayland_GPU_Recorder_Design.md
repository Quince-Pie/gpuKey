# Wayland GPU Recorder Design Reference

Research snapshot: 19 October 2026. Design proposal; hardware validation pending.

# 1. Design decision and evidence status

**Select a capability-driven recorder with a small ownership core, native GPU encoding paths, and measured alternatives for capture, color conversion and output.** The portable control path is the ScreenCast portal plus PipeWire. The performance path is selected from the actual source allocation, GPU, driver, color contract and consumer requirements. Keep frames on a compatible GPU whenever that is the winning complete path. Do not require an intermediate image merely to make backends look alike.

The proposal is a serious candidate for a state-of-the-art implementation, not a claim that an unimplemented recorder already wins a benchmark. Primary specifications and source mechanisms were inspected on 19 October 2026. This environment exposes no GPU device. We can establish API constraints, derive bounds, compare mechanisms and check abstract invariants; we cannot establish capture throughput, hardware image quality, driver interoperation or game interference here. Those remain explicit acceptance tests.

The likely codec is interpreted as **AV1**. AVI is a different container and is not assumed. AV1 hardware encoding in 4:2:0 at 8 or 10 bits is the common target across the relevant vendors. It cannot simultaneously promise preservation of arbitrary RGB desktop detail: chroma subsampling is a separate, irreversible loss. A fidelity profile must be allowed to select a different supported representation or codec when that requirement is consequential.

## 1.1 The architecture to implement

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

## 1.2 How to read the evidence

**Verified** means a retrieved primary contract or inspected implementation says the stated thing; it does not mean every installed driver implements it correctly. **Derived** means arithmetic or an invariant follows from explicitly stated assumptions. **Checked here** identifies actual executable finite checks. **Proposed** identifies a design choice awaiting implementation. **Unresolved** marks an experiment or product decision that can change the choice.

This distinction applies throughout. A vendor feature matrix is an eligibility filter. A successfully created encoder is stronger evidence. Decoding and inspecting output while measuring the whole pipeline is stronger still. A paper's progress theorem belongs to its complete algorithm under its assumptions, not to a queue vaguely inspired by it.

# 2. Requirements, consumers and operating modes

## 2.1 Constraint origins

The user's requirements are Wayland Linux; eventual C23 implementation; modern AMD, NVIDIA and Intel; Linux 6.12 and later; AV1 as the likely codec; deep investigation of overhead, quality, throughput and latency; and a rigorous design before implementation. The request does not fix resolution, frame rate, GPU model, compositor, container, target player, memory budget, storage device or live transport.

Local recording is a provisional first product mode. Live output, replay, HDR, multiple sources and editing are kept visible because they change architecture and consumer contracts. Their detailed policies must not silently become mandatory features. Nor should a benchmark such as 4K240 become a production limit. Resource bounds are negotiated from actual capabilities, budgets and failure policy.

No scalar weighting of all objectives is invented. Instead, measure a feasible set under each mode's constraints, then compare nondominated choices. Quality improvements that consume latency budget may be appropriate for a file and unacceptable for a live feed. A lower CPU percentage that steals GPU bandwidth from the recorded game can lose the intended objective.

## 2.2 Operating profiles

| Profile | Primary constraint to make explicit | Candidate policy |
|---|---|---|
| Interactive/live | Maximum frame age and transport availability | No future-frame lookahead unless budgeted; bounded reorder; discard stale unsubmitted work |
| Balanced local file | Visual quality and low application interference | VFR when consumers accept it; quality-oriented rate control; only measured, affordable analysis |
| Desktop fidelity | Text, fine chroma and correct color | Compare 4:2:0 loss against a supported 4:4:4/RGB alternative; preserve appropriate bit depth and tags |
| High-throughput capture | Sustainable offered frame rate with a stated drop bound | Tune engine use, memory layout, concurrency and encoder settings under that bound |
| Replay | Retention duration, export behavior and bounded memory | Store immutable encoded chunks, retain configuration and valid random-access dependencies |
| Durable recording | Defined loss window under process or power failure | Container boundaries plus completed write and synchronization checkpoints |

The interface must show meaningful effective behavior: source, output format, HDR/SDR state, target or cap, active latency/quality mode, continuity policy and output health. Internal queue names, semaphore APIs or kernel flags belong in diagnostics, not the recording workflow.

## 2.3 Define what is being recorded

A window recording, a composed monitor image and a physical display's post-LUT electrical output are different results. Hardware overlays, cursor planes, scaling, HDR tone mapping and display transforms make that distinction observable. The capture backend must advertise its semantics. Comparing a raw primary-plane KMS read with a compositor-rendered window capture is not a fair speed comparison.

For multiple monitors, decide between independent streams and an explicitly composed canvas. Mixed refresh rates do not imply that every source must be duplicated at the fastest rate. Independent tracks can preserve native timing where consumers support them. A combined canvas needs a declared sampling rule: each output timestamp selects an available frame from every source, records staleness, and composites on an appropriate GPU. Cross-device copies can then dominate. Window movement between GPUs and display hotplug require renegotiation, not hidden interpretation of stale dimensions.

The source cursor may be embedded, supplied as metadata, or omitted. Embedded plus local compositing produces two cursors. Metadata cursor changes can be the only visible update during an otherwise static screen. Audio sources require their own authority and identity; the portal's restricted PipeWire remote is not a license to inspect the user's general audio graph. [C01]

# 3. First principles: remove avoidable work before tuning it

## 3.1 The repeated units of state

The full lifecycle contains several different units. A captured image identifies allocation, generation, interpretation, timestamp and synchronization. A conversion job identifies only the transform and destination actually needed. An encoder input can remain referenced after submission. An encoded temporal unit can contain a different relationship to displayed pictures than one-input/one-output. A muxed byte span has offset, owned storage and completion state. A durable segment adds a recovery boundary. The design should materialize each only when its consumer needs it.

A per-frame object graph with duplicated format descriptions, heap allocations and reference-counted wrappers is convenient but not required by the media. Immutable generation records can hold device, dimensions, format, modifier and color metadata once. Per-frame descriptors carry stable identifiers, timing and changing metadata. Pools can use compact indices where ownership is single-threaded or well-defined; atomic reference counts belong only on objects actually shared independently, such as a replay chunk simultaneously being written and exported.

Similarly, a stage boundary is not a command to create another full image. Crop, rotation, scaling, color conversion and cursor composition can be one GPU dispatch when all are required. Direct encoder conversion may remove that dispatch entirely. Conversely, an explicit conversion into owned memory can improve the system by releasing the compositor allocation much earlier.

## 3.2 Bandwidth and storage are different scales

For tightly packed 3840 by 2160 images, ignoring pitch, alignment, compression and metadata overhead, the following quantities are derived:

| Representation | Bytes per frame | Purpose of the comparison |
|---|---:|---|
| RGBA/BGRA 8-bit | 33,177,600 | Common RGB capture footprint |
| NV12 | 12,441,600 | 8-bit 4:2:0 encoder input |
| P010 | 24,883,200 | 10-bit samples stored in 16-bit words |
| RGBA half-float | 66,355,200 | Higher-precision linear/HDR processing candidate |

At 240 frames per second, reading RGBA8 once and writing P010 once accounts for 13.934592 GB/s of image traffic. An extra RGBA8 read-and-write copy adds 15.925248 GB/s. These are traffic estimates, not measured DRAM utilization: tiling, caches, compression, unified memory and driver internals change physical transactions.

By comparison, 100 Mbit/s of encoded output is 12.5 MB/s; even 1 Gbit/s is 125 MB/s. This explains why eliminating raw-image passes deserves early attention and why a compressed CPU copy can be beneficial. It does not prove storage irrelevant: bursty keyframes, multiple streams, slow devices, page-cache pressure, durability and finalization can still determine tail latency.

## 3.3 The cost of ownership

Direct source encoding removes a conversion/copy but can lengthen the source lease. The ordering is backend-dependent: a native encoder can consume promptly, while a conversion can queue behind busy GPU work. If source allocations are scarce and the encoder retains an input through analysis or delayed consumption, the compositor can stall before the encoder itself reaches capacity. The correct comparison includes actual last-source-read time, source-pool occupancy, compositor buffering, raw bytes moved, output quality and game frame times. A locally cheaper producer can leave a more expensive system.

Maintain independent counts for source leases, recorder-owned conversion surfaces, encoder references, output buffers and pending sink bytes. Do not approximate every one by a single queue length. A frame count also fails for compressed output, where packet sizes vary widely.

Little's law, occupancy equals arrival rate times average residence time in a stable system, is useful for estimating pool pressure. It does not size the pool against long tails or prove bounded latency. A practical pool must satisfy the API's minimum surfaces and a measured residence distribution under the chosen admission policy. If the required memory is unacceptable, change concurrency, quality settings, cadence or ownership; do not merely deepen a queue.

## 3.4 Causal latency

Measure source timestamp to capture availability, submission, input consumption, encoded packet, mux availability, completed write and requested durability separately. A recorded file has no network receiver; live output adds transport, receiver buffering, decode and display. Do not market storage completion as photon-to-photon latency.

Future-frame analysis imposes a causal lower bound. Sixteen future frames at 60 fps require about 266.7 ms of future content; at 240 fps they require about 66.7 ms. Reordering and output buffering can add more. Enlarging asynchronous depth increases outstanding work; it is not automatically an increase in either minimum latency or useful throughput. High throughput and low latency can coexist below saturation, but an unbounded admission stream under overload cannot keep both.

# 4. Capture authority and source architecture

## 4.1 Portable control: ScreenCast portal and PipeWire

Use the ScreenCast portal for portable user-authorized source selection, then connect to its restricted PipeWire remote. The normal sequence creates a session, selects sources, starts it and opens the remote. Treat request cancellation, denial and asynchronous session closure as ordinary state transitions. A restore token is a revocable, single-use restoration mechanism that can be replaced by a new returned token; it is not permanent capture permission. [C01]

Version-gate fields instead of equating kernel version with desktop API version. Portal version 6 adds a stable `pipewire-serial` property for targeting through `PW_KEY_TARGET_OBJECT`; a raw node ID can be reused. Use the stable property when supplied and the documented older behavior otherwise. The portal's logical position and size are not a substitute for negotiated pixel dimensions. Cursor modes and available source types must be read from capabilities. [C01]

PipeWire supplies format negotiation, buffers and per-frame metadata. Prefer DMA-BUF when the exact import path is usable, with a clearly identified shared-memory fallback. A negotiated DMA-BUF handle does not establish that the consumer can decode its tiling, wait on its producer or obtain the requested color interpretation. Device placement, format/modifier intersection and synchronization are first-class plan inputs. [C02]

Avoid busy polling for screen updates. Content-driven delivery can remove wakeups and repeated work on static desktops. A CFR consumer still needs a representation for elapsed display time, but that does not require recapturing unchanged pixels on every timer tick.

## 4.2 Direct compositor capture as a serious challenger

The `ext-image-copy-capture-v1` protocol can let a client supply capture buffers under compositor-provided constraints. It is a meaningful alternative when present and appropriate to the source semantics. Its version-1 session allows at most one outstanding frame object; after completion or failure that object must be destroyed before the next. The compositor may wait for a change, so a request is not a promise of a periodic timer callback. [C03]

The buffer-damage contract is particularly important. The client reports regions needing refresh since that particular destination buffer was last captured, not merely the latest source damage. Initial or unknown contents need full refresh. The frame's ready event establishes capture availability under this protocol; ordinary `wl_buffer.release` is not its reuse signal. The client must still wait for its own GPU readers before reusing the destination. Do not assume a synchronization protocol defined for `wl_surface` automatically applies to a capture object. [C03]

A direct path may remove compositor/PipeWire copying or buffering, but that is an empirical question. A portal backend may already use equivalent mechanisms internally. Compare the same monitor/window semantics, cursor, color, cadence and authorization model. Keep this backend optional rather than pretending every Wayland compositor implements a new protocol.

## 4.3 KMS and privileged capture

KMS is an optional, separately authorized path when its actual semantics and deployment model are acceptable. `GETFB2` access to framebuffer handles is restricted to DRM master or the relevant administrative capability; access to a render node does not confer permission to read the display. Fresh returned handles need correct lifetime accounting, including shared planes. A helper should have the minimum responsibility needed to provide authorized capture resources. [C04]

Capturing one primary plane may omit overlay video, hardware cursor, transformations or color processing. Reading scanout without a coherent snapshot protocol can also produce a result that differs from a compositor-rendered capture. Evaluate exact final pixels, update synchronization, multi-plane composition, HDR, rotation, protected content, multi-GPU and permissions before ranking it. The existing GPU Screen Recorder project is a useful practical control across the three vendors, but its own comparative marketing is not independent evidence that a new design wins or loses. [C05]

## 4.4 Fallbacks should preserve declared behavior

If DMA-BUF import is unavailable, an explicit shared-memory upload can keep the recorder functional with an understood cost. If AV1 is unavailable, report that fact and use a previously selected fallback policy. Do not silently replace HDR with SDR, a selected window with the whole monitor, or a requested fidelity mode with a lower-fidelity codec. These are semantic changes, not implementation details.

# 5. Negotiation, frame records and resource lifetime

## 5.1 Immutable generations, mutable frame values

A source generation describes the source identity, GPU/device relationship, dimensions, plane interpretation, format, modifier, crop/transform basis, color contract and negotiated metadata/synchronization features. A buffer record represents one backing allocation within that generation. A frame record captures changing values: frame/sequence ID, PTS, flags, damage, crop, cursor state and acquire/release points.

Copy per-frame values before returning a PipeWire buffer. The buffer wrapper and its metadata are borrowed storage that can be reused. A persistent GPU import or duplicated FD retains a resource reference, but does not freeze its pixels. Content immutability is supplied by the producer/consumer synchronization contract, not by the fact that a Unix FD remains open.

Use persistent imports keyed by a stable buffer record and generation. An integer FD is not a stable resource identity: descriptor numbers are reused. Multi-plane offsets, pitches and aliases are part of the import. Validate size arithmetic with overflow checks, but follow the DMA-BUF contract: `maxsize` or chunk size can be zero for a GPU buffer and must not by itself be treated as an empty frame. Additional data entries can be synchronization objects, so the total data count is not automatically the number of pixel planes. [C02]

`DRM_FORMAT_MOD_INVALID` denotes an implicit modifier convention, not guaranteed linear memory. A modifier that an API can import for sampling may be ineligible for encoder use, storage writes or external re-export. Query the exact image format, tiling/modifier, usage, external handle types and device pairing. Device-ID negotiation introduced in PipeWire's 1.6 development cycle is useful when actually supported; it does not retroactively exist on every 1.4 deployment. [C02, C06]

## 5.2 Negotiate once; validate each frame cheaply

At generation setup, intersect producer formats/modifiers with import and processing capabilities, select source/output color semantics, allocate pools, compile or select kernels, and prepare synchronization bridges. Initialize each persistent import and view when its buffer is added; buffers may arrive or disappear independently within a configuration. Per frame, perform only bounded validation of changing fields and state: correct generation, known buffer, sequence/flags, valid timestamp and damage bounds, cursor updates, and an available destination/encode credit.

Metadata with a crop, transform, discontinuity or corruption flag is part of the frame contract, not an optional visualization hint. A zero framerate denominator must be rejected where invalid; a negotiated framerate of 0/1 can denote variable-rate video and needs an explicit maximum or admission policy rather than a division by zero. PipeWire's raw-video description also carries range, matrix, transfer, primaries and chroma location. [C07, C08]

If reconfiguration invalidates imports or color assumptions, create a new generation. Old in-flight jobs retain their old immutable records until retirement. Do not mutate a global width/height/color object that old GPU submissions still interpret.

## 5.3 Early return with an actual release promise

With a usable explicit-sync producer contract, returning the PipeWire wrapper can precede completion of GPU reads. Establish this sequence: copy all mutable metadata; retain independent imports and synchronization references; successfully arrange acquire wait, all source reads and release signaling; publish the release promise; then return the wrapper. The release point must cover the actual last source reader. A conversion-only path can release after its conversion reads; direct encode needs the backend's input-consumed condition. [C02, C09]

The release path may be a GPU signal or a valid host retirement path. For example, AMF's input-release observer is a documented reuse notification, not itself a GPU fence or PipeWire timeline signal. Its owner must issue the negotiated signal after that event and keep the callback/signal state alive. Early wrapper return does not require pretending every encoder exposes a pre-submittable all-GPU chain. [G26]

Current PipeWire metadata includes a negotiated RELEASE feature and an `UNSCHEDULED_RELEASE` flag set by the producer. When that feature is actually negotiated, a consumer clears the flag when it promises a release signal. Leave it set for an unused frame with no access and no promise. Metadata presence or installed version alone is not proof that the feature was accepted. Under the legacy explicit-sync contract, fulfill its release obligation even for dropped frames. A failed submission must not publish a release promise that will never complete. [C09]

Do not access the returned wrapper, chunks, metadata pointers or borrowed FD numbers afterward. Complete metadata publication before handoff. This is stronger than copying an illustrative single-context example that modifies metadata after queueing. The current PipeWire synchronization example uses eventfds as stand-ins for synchronization objects; that does not make blocking eventfd reads a correct DRM timeline implementation. [C10]

Once any source-reading work has been accepted, a subsequent failure to arrange or export its release signal is an outstanding-retirement obligation, not an unused-frame drop. Retain the lease and owned references, then repair the release chain or await the exact accepted work outside the real-time callback. Never expose an unscheduled-release flag as permission to overwrite while accepted readers remain possible. Device loss follows the backend's quiescence/destruction contract.

If the negotiated bridge cannot safely express an early return, retain access ownership until reads finish and return only if the wrapper is still valid. Serialize that validity check and the final queue operation with buffer removal on the owner of the PipeWire wrapper lifecycle. Workers request a return through that owner using stable record/generation tokens. Neither path may block a real-time callback on GPU completion.

## 5.4 Removal and teardown

PipeWire's stream wrapper is valid only under its stream lifecycle; `remove_buffer` invalidates it. Holding a dequeued pointer is not a general guarantee that stream destruction cannot remove it. Serialize record removal with submission ownership. Mark the record detached, prohibit new use, disconnect the wrapper, and defer destruction of independently owned imports until in-flight references retire. Late completions address stable records and generations, never removed wrappers. [C11]

A removal callback should not wait synchronously for arbitrary GPU retirement. Such a wait can block the loop needed to process other lifecycle events. A single owner for record state, plus immutable ingress/completion tokens, makes the race easier to reason about. The distinction between logical removal and physical retirement is essential during monitor unplug, format change, portal revocation and device reset.

# 6. GPU visibility, dependencies and plan construction

## 6.1 Four different obligations

Correct GPU sharing requires allocation compatibility, execution ordering, memory visibility and lifetime. A successful import proves only a subset. A host atomic publication makes a descriptor visible to another CPU; it does not make a GPU render complete. A semaphore can order execution without automatically repairing an incorrect image layout or ownership transition. A valid FD can preserve an allocation while its contents are already reused.

For Vulkan, apply the stage/access scopes, image layouts and queue-family ownership transitions required by the actual external/foreign ownership contract. Use timeline semaphores or API-specific dependency objects where supported, and external binary sync files where they are the bridge. A DRM syncobj timeline FD, a sync_file FD and a Vulkan opaque semaphore FD are different objects. Import/export ownership rules differ across APIs; encode them in typed wrappers and one cleanup path per handle type. [C12, C13]

For an implicit-sync DMA-BUF producer, Linux provides export/import sync-file ioctls to bridge fences. Export the appropriate current reservation fences, submit work waiting on them, then import the new completion fence with its read/write meaning. This sequence is not atomic against unrelated users: the buffer lease or other coordination must prevent an interleaving access from escaping the snapshot. CPU mapping synchronization ioctls concern CPU cache access and do not replace the GPU scheduling protocol. [C12]

## 6.2 Make the selected plan inspectable

The plan records source device and import route, native input allocation, every actual conversion/copy, source-release condition, encoder reference retention, packet availability, synchronization bridges and expected resource costs. Diagnostics should expose why a faster-looking candidate was ineligible: unsupported modifier export, color transform needed, incorrect chroma location, missing fence bridge, driver capability rejection or failed probe.

This is not an automatic graph compiler searching arbitrary paths at runtime. It is a small set of deliberately implemented plans with verified preconditions. Cache successful probes by device UUID/identity, driver and relevant library version, source generation attributes and selected mode. Invalidate when any assumption changes. Never silently choose an unvalidated path because its API advertised a broad extension.

At startup, a small functional probe can create the requested encoder/configuration, submit a known image, decode and inspect its output, and test the synchronization bridge when practical. Persisting the probe result saves repeated startup cost, but a cached result cannot guarantee a different modifier or imported allocation works.

## 6.3 Avoid global synchronization and unnecessary engine transfers

Prefer dependency chains to `deviceWaitIdle`, stream-wide synchronization after every frame, or CPU polling loops. Where an encoder API has a blocking output operation, give it an owner that can block without preventing capture, command submission or output completion servicing. The host may submit asynchronously even when the vendor exposes only synchronous output retrieval.

A separate GPU queue can overlap useful work, but may also add ownership transfers and synchronization. A transfer engine copy can free shader resources while consuming the same memory bandwidth. Dedicated video hardware may share memory, power and firmware resources with rendering. Evaluate the actual bottleneck and game interference; the word 'dedicated' is not a zero-overhead guarantee.

Cross-GPU processing deserves a separate plan. The GPU best at encoding may be the wrong one if every frame must cross a PCIe link or be retiled. An iGPU can sometimes isolate encoding from a busy discrete GPU; on a different platform, shared-memory contention or import limitations can reverse the result. Compare same-GPU native encoding against real cross-device paths including synchronization, transfer, conversion and display behavior. No device marketing hierarchy resolves that comparison.

# 7. Color, HDR and desktop fidelity

## 7.1 Specify the output pixels before selecting encoder settings

The color contract includes source primaries, transfer function, matrix/range, bit depth, alpha interpretation and chroma location; output equivalents; any tone mapping; and what metadata consumers receive. Layout such as P010 does not identify the color space. A ten-bit buffer is not proof of HDR, and an HDR monitor does not prove that the compositor supplied HDR screencast pixels.

KWin's documented capture behavior includes rendering screencasts/screenshots into an sRGB representation and tone mapping HDR content. Other implementations can expose HDR-related PipeWire metadata. Inspect the actual negotiated source and implementation behavior. If it has already been tone-mapped to SDR, relabeling it PQ cannot reconstruct the original HDR signal. [C14, C15]

A defensible SDR candidate is an explicitly specified RGB-to-YCbCr transform with output primaries, transfer and limited/full range accurately signaled. sRGB and BT.709 transfer functions are not identical. If the output is tagged with the BT.709 transfer, perform the intended transform rather than copying sRGB code values and merely changing metadata. Alternatively, preserve the accurate source transfer when the codec/container/consumer chain supports it. The best interoperability choice is a tested consumer decision, not an excuse to mislabel pixels.

For HDR, preserve the source's actual PQ/HLG and primaries when supported, or apply a declared tone map into a target SDR/HDR space. Emit mastering and content-light metadata only when known or legitimately derived under a specified method. A plausible-looking fixed mastering luminance is fabricated metadata. The fact that an existing project uses such a fallback is not a reason to copy it. [C15]

## 7.2 Chroma and quantization are separate quality losses

In 4:2:0, one chroma sample describes a neighborhood of luma samples. Different colored-pixel arrangements can have the same downsampled chroma. No bitrate or AV1 preset can recover which original arrangement existed. Fine colored text, subpixel antialiasing and thin saturated edges therefore deserve a fidelity comparison against a supported 4:4:4/RGB path. Ten-bit 4:2:0 improves quantization precision; it does not restore missing chroma resolution.

RGB8 converted to ten-bit YUV can preserve more precision from the matrix/range operation than quantizing its intermediate YUV to eight bits. It does not add source detail. Conversely, hardware that accepts eight-bit input and emits ten-bit output can save memory but may simply expand already quantized samples. Keep these paths distinct in quality experiments.

For BT.709 nonlinear components, the luma coefficients are 0.2126, 0.7152 and 0.0722; chroma follows the specified differences and scaling. Limited ten-bit luma uses nominal code values 64 through 940, with chroma centered at 512 and nominal range 64 through 960. P010 stores the ten significant bits in the high bits of sixteen-bit words. These give useful endpoint and neutral-gray tests, not a complete implementation specification. [C16, C17]

## 7.3 Conversion precision, scaling and cursor composition

A fused kernel can combine crop, rotation, scaling, color conversion, range mapping, chroma filtering and cursor compositing. First define the order and spaces in which those operations occur. Physically appropriate alpha blending and many resampling operations require linear-light reasoning; preserving desktop appearance and avoiding ringing still needs consumer-oriented evaluation. Do not assume one filter or one workgroup shape is optimal for both tiny text and high-motion games.

Use sufficient intermediate precision for HDR and matrix operations, then quantize once at the final representation. Compare half-float and full-float arithmetic where it can change error, bandwidth or occupancy. Handle premultiplied alpha correctly. Metadata cursor positions, hotspot, crop and rotation must share a defined coordinate transform, and cached bitmap updates must distinguish 'no new bitmap' from 'hidden cursor.' Test old and new cursor bounds for damage. [C18]

Chroma filtering must match the signaled sample location. Simply retaining one RGB pixel from each 2 by 2 block creates aliasing. A box average is a useful complete control; stronger reconstruction/downsampling filters should be compared on text, saturated diagonals and motion. Filter support extends the damage halo. Dithering can reduce quantization banding, but temporal dither changes static pixels and may increase encoding work. Stable spatial dithering is a candidate when it preserves appearance and static-scene semantics.

Do not force every source through linear RGBA16F. If the encoder can perform the exact needed conversion directly from a compatible image with acceptable precision, that representation adds avoidable work. If a fixed-function RGB conversion cannot perform a transfer conversion, tone map, cursor blend or required scale, it is not a complete replacement for those operations.

# 8. Conversion, damage and unchanged frames

## 8.1 Four complete alternatives

Compare direct input with hardware conversion, a dedicated VPP/VPE path, a fused shader writing native encoder storage, and a straightforward correct conversion control. Each candidate must produce the same intended output interpretation and lifecycle. A vendor path that silently falls back to compute is still a path, but its actual engine use must be measured. Do not credit it with fixed-function isolation merely because a preference flag was set.

For shader work, group luma/chroma production so the same source neighborhood is reused when beneficial. Coalesced loads/stores, register pressure, occupancy and the native image layout matter. Candidate 8 by 8 or 16 by 16 workgroups are tuning points, not requirements. Measure kernel time, device memory traffic, source lease time and game effects. Fusing a stage that prevents hardware compression or forces an export can lose despite fewer dispatches.

A native array or tiled encoder target can eliminate a hidden driver conversion. A mandatory linear NV12/P010 buffer in the common API would exclude that improvement before testing it. Keep external sampling capability and encoder-write capability separate: the allocation ideal for capture can be a poor encoder input, and vice versa.

## 8.2 Damage is history about a particular image

Authoritative source damage can avoid full-frame conversion when little changes. It is useful only with a complete destination image. Distinguish the immutable configuration generation from the advancing content revision. Each destination carries both its configuration generation and its last valid content revision. Rotating through an encoder input pool means each destination can contain a different content revision. The update region for destination j is the union of source changes since that destination's last valid revision, transformed through all required crop/scale/filter operations.

A scheme that applies only the latest frame's damage to whichever pool slot is free is incorrect. A pixel changed two frames ago can be missing from an older slot even if that pixel is not in the latest damage. Consume authoritative damage, cursor changes and discontinuity information before discarding an observed input. If an unobserved gap exists and the producer does not guarantee cumulative damage across it, force full refresh. Initial contents, missing damage, color changes, transform changes and untracked source discontinuities invalidate the whole destination.

Maintain bounded history using revision-stamped tiles or a damage log whose overflow forces full refresh. The representation is a design choice: per-tile last-change revision can answer which regions changed since a slot's valid revision without storing every intermediate rectangle. A rectangle history can be cheaper for a few large updates. Compare both under cursor-only, scrolling and full-motion workloads, including merging cost and overdraw. A full-frame GPU hash is a scan and synchronization cost; it is not a free substitute for reliable compositor metadata.

Snapshot each conversion job's intended content revision. Advance destination validity only after successful completion, or represent a pending revision whose consumers depend on that job and whose failure invalidates the destination. A late completion from an old configuration must never validate a newly reused slot.

For each changed region, include the support halo of scaling/chroma filters and the old and new cursor rectangles. Converting a partial source without having valid unchanged destination pixels is not a valid optimization. Once submitted to the encoder, a destination cannot be patched until its input and any relevant reference retention have ended.

## 8.3 Unchanged presentation need not mean repeated capture

A static screen may require no new image capture or conversion. A VFR file can represent elapsed display time through timestamps and durations if the consumer chain handles it correctly. CFR output needs a valid representation for repeated display. The AV1 format supports show-existing behavior, and some vendor APIs expose relevant controls, but support and reference-state obligations differ. Repeating an arbitrary compressed packet is not generally valid.

Retain this as a backend-native challenger: no-op capture/conversion plus valid repeated presentation, compared with ordinary encoder submission of an unchanged image. Verify the emitted temporal units, reference IDs, timestamps, rate-control response, source retention and decoder output. The existence of a force-show-existing enum does not prove zero input reads or zero encoder work. If a path cannot express a safe repeat, ordinary encoding remains the correctness control.

Damage can also guide quality allocation, but ROI semantics differ across vendors. A change map is not automatically a perceptual importance map: a static terminal may be more important than a moving background. Keep activity, text/fidelity importance and encoder-specific ROI translation separate, and verify that requested hints were actually accepted.


# 9. GPU backend architecture

The encoder backend should select a complete GPU execution plan: how it reads the captured allocation, performs any required image operations, supplies the encoder, and releases every resource. Selecting an encoder name after constructing a universal linear NV12 frame would decide the most consequential representation question too early. Current hardware and drivers offer materially different routes: AMD can encode compatible RGB images through Vulkan Video; NVIDIA can encode native block-linear CUDA arrays; Intel can use dedicated video processing and encoding through its media driver. Their advantages survive only when the complete path satisfies the required image, synchronization and lifetime contracts. [G01, G05, G10, G17]

This chapter distinguishes **verified** API or source behavior, **proposed** design choices, and **unresolved** device-dependent outcomes. The research snapshot is **19 October 2026**. No GPU was available for hardware tests, so there is no measured cross-vendor quality ranking, throughput winner, or universally optimal preset.

## 9.1 Preserve the backend's useful representations

The common interface should describe an image and its readiness without requiring that image to exist in a particular API. Its meaningful fields include the physical device identity, native allocation handles, plane layout and modifiers, dimensions and crop, color description, and producer completion dependency. An **encode plan** then states which input representations it accepts, whether it may alias the source, which image operations remain necessary, its output format, and the events that release its source and destination leases. These are proposed internal contracts, not a frozen public C ABI.

Three separate completion points matter. First, a conversion or copy can finish reading the compositor allocation. Second, the encoder finishes using its input allocation. Third, the compressed-output consumer finishes using an output allocation. Combining these into one generic “frame complete” event either delays reuse or releases something prematurely. A direct RGB plan can remove conversion work yet retain a scarce producer buffer longer; an owned converted surface can release that producer earlier. Neither route wins from its diagram alone.

Allocation negotiation must precede the final plan. An encoder-compatible tiled surface may be directly writable by a shader, available only through a vendor processing API, or importable with restricted usage. Successful DMA-BUF import establishes shared access, not local memory placement, compatible video usage, or an ordering dependency. A same-GPU plan is a sensible provisional default; a second GPU remains a challenger when reduced game interference can outweigh transfers and synchronization.

Capability discovery therefore has two levels: enumerate supported operations and then validate the exact combination requested. The combination includes codec, coded bit depth, input representation, size/alignment, color conversion, rate control, reference structure, and external synchronization. Record effective parameters after initialization when the API supports readback. A silently corrected setting is a different candidate, not evidence that the requested candidate succeeded. [G01, G12, G17, G19]

## 9.2 Hardware and driver eligibility

The following is an eligibility matrix, not a performance table. “8K” below follows the respective vendor documentation; it is not a promised recording rate or a recorder-wide dimension limit.

| Family | Documented AV1 encode input/output envelope | Consequential qualification |
|---|---|---|
| NVIDIA Ada, RTX 40 generation | AV1 Main, 4:2:0, 8/10-bit; documented maximum 8192 × 8192 | Desktop encoder counts vary: 4060/4060 Ti/4070/4070 Super have one; listed 4070 Ti and higher desktop models have two. Query exact SKU and API. |
| NVIDIA Blackwell, RTX 50 generation | AV1 Main, 4:2:0, 8/10-bit; documented maximum 8192 × 8192 | Desktop 5060/5060 Ti/5070 have one NVENC; 5070 Ti/5080 have two; 5090 has three. New H.264/HEVC 4:2:2 support does not imply AV1 4:2:2. |
| AMD VCN 4, including RDNA 3 Radeon RX 7000 and supported APUs | AV1 4:2:0, 8/10-bit, up to 8K | VCN 3 provides AV1 decoding, not AV1 encoding. Marketing names alone do not identify the media block. |
| AMD VCN 5, including RDNA 4 Radeon RX 9000 | AV1 4:2:0, 8/10-bit, up to 8K | AMD lists RX 907x/Navi 48 and RX 906x/Navi 44 with one VCN instance each. Direct RGB conversion has additional driver/color restrictions. |
| Intel DG2/Arc A; MTLx, including Meteor Lake and Arrow Lake S/H; LNL/Lunar Lake; BMG/Arc B; PTL/Panther Lake | Current media-driver matrix lists AV1 NV12 8-bit and P010 10-bit encoding, up to 8K | Use the installed media-driver/runtime capabilities. Panther Lake/Core Ultra Series 3 is included; do not stop discovery at Xe2. |

These entries come from vendor support matrices and Intel driver documentation. Laptop, professional, integrated and desktop variants must not inherit another SKU's engine count. Intel's current release also lists Nova Lake and Crescent Island support; that establishes driver work, not the availability or behavior of an unspecified production system. [G02, G03, G09, G17, G18]

Multiple media engines do not establish single-stream scaling. NVIDIA explicitly distinguishes aggregate multi-session capacity from split-frame encoding. AMD lists two VCN instances on some RDNA 3 devices while qualifying AV1 to one, with Strix Halo an exception supporting AV1 on both. Intel Arc B has twin multi-format engines, but the actual single-stream scheduling path still requires measurement. [G02, G09, G24]

The implementation should retain a software or alternative-codec fallback as a separately identified plan. Unsupported AV1 hardware is not permission to label software encoding “GPU AV1,” and a family benchmark does not justify rejecting a larger valid stream supported by the actual queried device.

## 9.3 NVIDIA: eliminate the hidden retile when the full path permits it

### Native CUDA arrays are a serious challenger

The current Video Codec SDK documentation is version 13.1. Its native block-linear CUDA-array path can avoid the pitch-linear-to-block-linear preprocessing normally performed when NVENC receives a `CUdeviceptr`. That preprocessing uses CUDA execution resources. Producing the encoder's final compatible array directly therefore attacks a whole materialization stage, rather than merely making the same conversion kernel faster. [G01, G04]

Current FFmpeg source makes the representation concrete. It allocates multi-planar NV12/P016 CUDA arrays with `CUDA_ARRAY3D_SURFACE_LDST` and `CUDA_ARRAY3D_VIDEO_ENCODE_DECODE`, and provides a CUDA-array input path to NVENC. CUDA's `cuArrayGetPlane` exposes plane arrays; for NV12, these describe a full-size single-channel Y plane and a half-width, half-height two-channel UV plane. A fused kernel can therefore target plane surface objects belonging to the eventual encoder allocation. This is a viable construction plan, not a claim that the entire Wayland import path has been tested. [G06, G07]

There is a material integration gap: the SDK guide/header describe registration pitch using allocation width multiplied by channel count, while current FFmpeg computes width multiplied by component byte depth for CUDA-array input. Multi-planar array descriptors make those expressions different. The implementation must resolve the correct pitch and layout against the target runtime with known Y/UV patterns and independent decode. It must not turn an ambiguous documentation formula into a supposedly verified C23 implementation. [G01, G07, G08]

The CUDA-array resource enum predates SDK 13.1. The relevant advance is the newly documented supported native multi-planar path and its surrounding implementation, not invention of the enum itself. Runtime API-version discovery remains necessary; the newest SDK's requirements should gate its features rather than become an unnecessary minimum for every NVIDIA backend. [G01, G04, G08]

### DMA-BUF interoperability is the difficult half

CUDA 13.4 documents direct `DMABUF_FD` external-memory import only on Tegra starting with Thor. It is not the generic desktop Ada/Blackwell capture route. Even that supported Tegra import cannot be mapped as a CUDA mipmapped array. Passing a compositor DMA-BUF descriptor while merely labeling it `OPAQUE_FD` is not a valid substitute. [G05]

The strongest plausible alternatives are:

1. Import the source through Vulkan with its actual modifier, then re-export the allocation as a genuine opaque-memory handle **if** the exact external-memory capability query permits export from the imported handle type. Matching device identity, allocation description and CUDA mapping requirements remain necessary. This could retain aliasing without an intermediate copy; it is an unresolved target-driver probe.
2. Import through EGL as an eligible ordinary OpenGL texture and register it for CUDA access. CUDA image registration supports specified OpenGL texture targets, including `GL_TEXTURE_2D`, but not `GL_TEXTURE_EXTERNAL_OES`. An external-only import cannot silently use the ordinary-texture route.
3. Read the imported source using a supported graphics path and write or copy into an owned, externally shareable allocation. This adds work but can make synchronization and early source release tractable.

Vulkan's `exportFromImportedHandleTypes` and compatible-handle properties govern the first route. A documented Vulkan/CUDA example is evidence for the relevant interoperability mechanism, not proof that arbitrary compositor modifiers or multi-planar encode images work. [G05, G23, G25]

The simple controls should include direct eligible OpenGL texture input and conventional CUDA device-pointer input. Native arrays should replace those controls only after conversion, registration, synchronization and source retention have all been included in the comparison. Encoding RGB itself can use CUDA preprocessing, so accepting an RGB texture is not equivalent to dedicated-engine-only conversion. [G01]

### Submission and resource completion

Create the context/session and reusable allocations once per compatible configuration; register persistent resources once, then map and submit each use. Arrange producer work and NVENC processing through the appropriate CUDA stream using `NvEncSetIOCudaStreams`. A stream dependency replaces a CPU wait only where the documented path actually consumes that ordering. It does not replace output completion or authorize concurrent writes to an encoder input. [G01, G08]

Linux NVENC requires synchronous output processing: `NvEncLockBitstream` blocks with `doNotWait=0`. Put that wait in a dedicated output worker while a submission worker supplies work. `NEED_MORE_INPUT` can mean that an input has been buffered for reorder/lookahead; keep the associated resources and continue supplying input. Do not lock an output prematurely or interpret that result as a rejected frame to submit twice. Preserve the API's output ordering and timestamp association. [G01, G08]

The output pointer remains valid until bitstream unlock. If downstream processing retains it, retain that output lease; alternatively copy compressed bytes into an owned packet and promptly unlock. Reuse an input only after its documented encode use has completed and its mapping has been released. Normal shutdown sends EOS, drains outputs, unlocks buffers, unmaps and unregisters resources, then destroys allocations and session state. These are bounded-pool operations, not a globally lock-free pipeline. [G01, G08]

### Quality and multi-engine scheduling

Evaluate speed/quality presets together with tuning, rate control, lookahead, reference structure, adaptive quantization and temporal processing. UHQ is not a free quality switch: it can enable analysis and temporal filtering, increasing work, retained memory and delay. Temporal filtering also deserves a screen-fidelity check for thin moving text. One fixed preset cannot minimize latency while maximizing compression quality. [G01, G04]

NVIDIA's split-frame mode can use multiple NVENCs for one frame. Its automatic AV1 activation includes a frame-height threshold of 2048 pixels and specific preset/tuning combinations; explicit modes also exist. Compare automatic, forced and disabled modes on eligible devices. More tiles/engines can improve throughput while changing compression efficiency and tail latency. GPU-resident bitstream output is incompatible with split-frame encoding, so the apparently cheaper output representation can block a stronger encoder plan. [G01, G02]

AV1 can accept 8-bit input while producing 10-bit output using hardware expansion. Consequently, a 10-bit SDR recording mode need not force a P010 intermediate on this backend. Compare that narrower input route against explicit P010 when their numerical color/chroma requirements match. Expansion does not reconstruct information absent from the source. [G01]

## 9.4 AMD: compare direct RGB, native processing and ordinary surfaces

### RADV can remove the application conversion stage

RADV AV1 encode appears in Mesa 25.2; Mesa 26.0 adds `VK_VALVE_video_encode_rgb_conversion`. The inspected current source programs the source GPU address, pitch and swizzle and emits VCN input/output-format commands for RGB-to-YUV encoding. The application need not dispatch its own conversion shader for an eligible image. This is source-level evidence of the decisive mechanism; it does not measure firmware traffic or latency. [G13, G14, G16]

The current format enumeration exposes BGRA8/RGBA8 for an 8-bit coded profile and packed RGB10A2/BGR10A2 for a 10-bit profile. It includes modifier tiling for applicable sources, but the exact captured modifier still needs a video-usage compatibility query. Do not infer arbitrary compressed-image compatibility from a listed RGB format. Nor does this enumeration establish NVIDIA-style 8-bit RGB input to a 10-bit output profile. [G16]

VCN 5 has a particularly important restriction. The current firmware works for linked combinations of BT.709 with limited range and BT.2020 with full range. The Vulkan API reports conversion models and ranges as independent masks, so exposing both would incorrectly advertise their cross-product. Mesa therefore exposes only BT.709/limited for this hardware; the restriction appears in the Mesa 26.2.4 fix. A recorder must honor the advertised pair rather than attempt an undocumented BT.2020/full workaround. [G15, G16]

The extension performs model, range and chroma conversion; it does not apply transfer-function conversion, and its range-compression precision is not guaranteed. Current RADV exposes horizontally cosited chroma, with limited vertical choices. Direct encode is eligible only when those numerical and siting semantics are acceptable and the captured RGB image already represents the desired final image. An outstanding scale, transform, cursor composition or color operation reopens the processing plan. [G10, G16]

This means the direct route belongs beside a high-quality fused conversion route. The former can save a full-frame YUV materialization. The latter can provide controlled arithmetic and filtering, satisfy more color configurations, and return the compositor's allocation earlier. Selecting direct RGB solely because it contains fewer visible commands would leave the real decision unresolved.

### AMF remains a modern Linux option

Current AMF history includes version 1.5.3; stable RADV support for Linux conversion, scaling and encoding dates to 1.4.34. A design that rejects AMF because it necessarily requires the proprietary AMD Vulkan implementation would be stale. Installation/runtime compatibility still needs checking on the actual system. [G11]

The converter's `VPE_PREFERRED` option is a credible alternative to shader processing where the hardware and operation support it. It falls back to compute when necessary, and VPE requires shareable inputs; an attached Vulkan image needs suitable external-memory sharing. Query converter availability and verify the actual engine path. A successful conversion call is insufficient evidence that VPE performed the operation. Complex combined operations may still require compute or another plan. [G12]

AMF Vulkan interop carries more than an image handle: allocation, format, current layout, usage/access and synchronization metadata belong to the contract. The public structures support semaphore/fence information and timeline metadata; the context's timeline-semaphore option defaults off. Configure and negotiate this deliberately rather than assuming ordinary Vulkan timeline support automatically selects it inside AMF. [G12]

AMF's current AV1 reference documents a nominal frame-rate range of 1-120 fps. A higher-rate recording requires verified runtime acceptance and correct effective timing/rate control, or another eligible backend such as RADV Vulkan Video. Query property metadata, validate/set/init and read back the effective value, then inspect output behavior. This is an AMF API qualification, not a recorder-wide frame-rate limit or a measured bound on VCN throughput. The inspected RADV path forwards nonzero rate numerators/denominators without that clamp, but remains a hardware probe. Processing a lower-rate transcode faster than 120 frames per wall-clock second would not resolve this presentation-rate restriction. Do not declare a false lower encoder frame rate as a workaround. [G12, G16, G26]

### AMF's actual input release signal

Use `AMFSurfaceObserver::OnSurfaceDataRelease` to track when a submitted surface becomes free of the component's exclusive use. The general AMF API explicitly warns against assuming a one-to-one relationship between input and output samples. A matching output packet is therefore not the general input-reuse oracle. The observer and its bookkeeping must remain alive until the component can no longer call it, and callback work should publish completion without performing expensive pipeline work. [G26]

`SubmitInput` may report that more input is required before output can appear. `AMF_INPUT_FULL` requires progress on output before retrying submission. `QueryOutput` can report `AMF_REPEAT`, or success with no output yet; neither grants resource reuse. A configured query timeout blocks until readiness or timeout and can reduce polling in an output worker. The documented default input depth of 16 is a library setting, not a justified recording latency target. [G12, G26]

On normal termination, stop new input, call `Drain`, retrieve output until EOF, and resolve outstanding input release notifications before dismantling the surface pool. `Flush` is a different operation and must not substitute for a loss-preserving drain. Hold returned output objects until their consumers finish. Submitted GPU dependencies, wrapper objects, underlying allocations and callbacks each need their own valid lifetime. [G26]

### AMD quality controls must be interpreted literally

AMF exposes AV1 screen-content/palette controls, integer-motion selection, QVBR and other rate controls, optional pre-analysis, and latency/power choices. Current documentation restricts QVBR, HQVBR and HQCBR to 8-bit configurations and requires Pre-Analysis; they are not a valid assumed 10-bit/HDR configuration. Pre-analysis itself reads frames; its static-scene detection needs lookahead and is not as cheap as avoiding a submission using trustworthy upstream equality information. Its documented gaming temporal-AQ mode has a DX11-memory restriction, so a Windows HQ recommendation must not become an assumed Linux Vulkan configuration. [G12, G27]

The current AV1 ROI input is a GRAY32 importance map, with values 0-10 per 64 × 64 block. It is not NVIDIA's signed delta-Q map. Current AMF QIndex fields start at 1, so a generic “QP 0 means lossless” control would be misleading. SmartAccess Video is a supported-system job-distribution feature; it does not prove that one AV1 frame uses every VCN instance. [G09, G12]

## 9.5 Intel: preserve the dedicated media path behind the API

Intel's current media-driver release is 26.3.5. The relevant practical backends are iHD VAAPI and Intel VPL over the installed media runtime. Their shared dedicated media machinery makes them credible controls for each other: VPL can provide higher-level scheduling and codec controls, while direct VAAPI can reduce wrapper responsibilities or offer a simpler integration with existing VA surfaces. Neither API name establishes lower overhead. [G17, G18]

The modern entrypoint name matters. Starting with MTL, Intel consolidates `VAEntrypointEncSliceLP` and `VAEntrypointEncSlice` into **`VAEntrypointEncSlice`**, while using VDEnc/HuC hardware acceleration. Requiring the old LP name would reject or misclassify the desired modern path. HuC firmware is required for low-power bitrate-control modes such as CBR/VBR; codec enumeration alone does not prove those modes work. Probe the requested combination on the installed stack and report a fallback rather than silently changing rate-control semantics. [G17]

Video processing through VEBox/SFC is another meaningful challenger to shader conversion. Its advantage is reduced use of general graphics execution resources, subject to supported formats, filters and operation combinations. Driver build variants differ in some processing capabilities. Discover those differences through queries; do not encode a guessed distribution/package policy into the recorder. [G17]

### VAAPI resource contract

Create or import surfaces with their real DRM plane/modifier description and query the required encode/VPP configuration. `vaExportSurfaceHandle` does not synchronize an exported surface. An external reader needs completion of prior VA writes; external writes must complete before VA uses the surface again. A valid memory handle alone provides neither guarantee. The public API does not establish a universal per-frame input sync-fd handoff for every driver path, so a promised entirely GPU-side Vulkan-to-VA dependency requires actual implementation evidence. A per-resource completion wait is a correct fallback; a device-wide idle is unnecessarily broad. [G20]

`vaEndPicture` queues work. Use supported completion queries or waits; current libva includes bitstream-specific `vaSyncBuffer`, while `vaSyncSurface` covers the surface's work more broadly. Keep the coded buffer mapped until its packet consumer finishes, or copy into an owned packet before unmapping. Input, reconstruction/reference surfaces, and coded buffers have different reuse conditions. Destroy resources only after their pending uses have been resolved. [G20]

### VPL resource contract

Run `Query`, inspect corrections/warnings, then use effective initialized parameters and `QueryIOSurf` information to size the initial pool. The suggested surface count is a starting requirement for that configuration, not an application-wide numerical limit. `EncodeFrameAsync` can accept/retain input without immediately producing an output sync point. Keep the input and its per-frame extension data alive until the runtime releases them; do not count returned packets as proof that corresponding inputs are reusable. Use documented surface ownership and completion mechanisms rather than unsynchronized ad hoc polling of SDK state. [G19]

Handle device-busy responses by arranging progress and retrying appropriately. Drain by supplying no new surface until the API reports no more output. Retain each bitstream allocation through its sync operation and downstream use. VPL surface-sharing APIs distinguish shared import/export from permitted copies; record which happened. Experimental Vulkan-image sharing must remain a separately gated candidate rather than replace the stable VA interoperability plan without evidence. [G19, G22]

VPL's AV1 screen-content extension exposes palette and intra-block-copy controls, with per-extension and per-option query requirements. AV1 segmentation supplies up to eight segments with per-segment controls and a queried map block size. Invalid maps may be discarded. A static segmentation map remains live until replacement/reset or close; a per-frame map remains live while its input is retained. These rules preclude a temporary stack map passed into asynchronous encoding. [G21]

## 9.6 Vulkan Video: a first-class contender with explicit codec responsibilities

Vulkan Video is especially attractive when imported image access, processing and video submission can share one device and synchronization model. That can avoid inter-API mapping and dependencies, while exposing native image layouts and explicit bitstream buffers. The AMD RGB-conversion path makes this more than an API-unification argument. [G10, G14]

Actual implementation support limits the present selection. RADV has released AV1 encoding. NVIDIA advertises AV1 encoding in its Vulkan driver history, with subsequent fixes affecting Blackwell output and video-image interoperability. Query and test the actual production driver. In inspected Mesa release tag 26.2.5, ANV does not advertise AV1 encode; current main permits it only on DG2 with video-encode debug gating, while later Intel hardware is excluded from that encode path. This rules out a promised universal modern-Intel Vulkan AV1 backend today; it does not prove Vulkan inherently inferior. [G13, G16, G28]

The application must implement the codec-management work that native APIs often hide: sequence/session parameters, supported reference structures, DPB slot assignment, reference refresh, coded versus display order, frame headers and output packaging. Query AV1 capability flags, reference-name masks, tile limits, rate-control support and quality levels. A generic “B-frame count” field cannot by itself define a legal AV1 schedule across these implementations. [G10]

The source image must reach the required video layout with the necessary memory dependency and queue-family ownership. The destination bitstream range must satisfy alignment and capacity requirements. Reconstruction resources must remain available for later references. Queue completion can permit source-input reuse while a reconstruction stays live in the DPB; bitstream visibility to the host is a further memory-coherency condition. Recycle command/query/bitstream slots only after the relevant submission completes. Never reuse a query result from a previous slot generation as completion of new work. [G10, G23]

An encoder-only microbenchmark can conceal the costs of software reference planning, header generation, query handling and output staging. Compare the complete Vulkan implementation with native encoders at equivalent image semantics and output objectives. Conversely, do not reject Vulkan because a preliminary common interface assumed opaque native-encoder scheduling: revise that interface to admit a different plan.

## 9.7 AV1 quality, repeated frames and screen-specific tools

### Format quality precedes preset quality

These hardware AV1 routes chiefly target 4:2:0 8/10-bit output. Chroma subsampling can erase colored text or one-pixel color detail before codec quantization becomes the dominant loss. RGB input does not imply 4:4:4 output, and choosing a slower encoder preset cannot restore chroma discarded by the input/output contract. Preserve a separately identified higher-chroma-fidelity option, such as supported HEVC 4:4:4, when the consumer can use it. A software AV1 4:4:4 mode has a different throughput/overhead envelope. [G02, G09, G18]

Ten-bit SDR is a candidate for improved gradient and quantization behavior, not evidence of HDR or recovered source precision. The required color interpretation comes from the capture contract. The backend must carry and correctly signal that interpretation; adding HDR metadata cannot convert an already tone-mapped SDR capture back into HDR.

For local recording, compare quality-controlled variable-rate modes against constant-Q controls before adopting strict CBR. A bitrate ceiling can prevent attainment of a quality target. For latency-sensitive delivery, constrain actual future-frame dependency, buffering and output delay. Pricing lookahead, adaptive analysis, temporal filtering, extra references and multipass processing separately exposes which quality gains are affordable. Vendor quality numbers and preset names are not interchangeable scales. [G01, G12, G19]

### Four distinct uses of unchanged regions

**VFR omission** avoids submitting an unchanged displayed image when the consumer permits its duration to extend. **Show-existing** emits a legal codec instruction to display an already reconstructed reference. **Partial conversion** updates only the pixels needed in a working surface. **ROI** changes coding decisions or quality allocation. These are different operations, with different guarantees; none should be implemented by pretending another has occurred.

AMF explicitly exposes `FORCE_FRAME_TYPE_SHOW_EXISTING`, subject to higher-priority key/intra/switch decisions. NVENC's AV1 overlay mechanism requires application picture-type control and an existing matching display-order reference; its show-existing flag is not a general automatic-PTD repeat switch. Current inspected RADV code writes `show_existing_frame=0`. Each route therefore has different repeat capabilities, and repeated submission still needs measurement to determine whether it avoids raw-image reads or only reduces output. [G08, G12, G16]

Do not replay arbitrary encoded packet bytes to fill cadence. Reference identities, frame IDs, order hints, entropy state, hidden frames and sequence transitions can make such replay invalid. Native repeat generation is a useful candidate, but VFR omission may avoid more work with much less codec-state machinery when downstream semantics allow it.

Partial conversion in a rotating surface pool requires accumulated damage since **that slot** was last current. Applying only the newest damage to an older slot leaves stale pixels. Include filter footprints and cursor old/new coverage, invalidate appropriately on configuration changes, and order writes after prior encoder reads. A persistent single destination avoids per-slot reconstruction but creates a read/write dependency with the encoder. Compare that dependency against extra snapshots or complete fused conversion; do not call the persistent surface free.

ROI does not establish that the encoder skips full-frame analysis. NVIDIA's signed per-superblock delta map, AMD's importance map, Intel's segmentation and Vulkan's quantization maps have different meanings. Current RADV VCN 5 even inserts a map-layout resolve before encoding. Query accepted modes and inspect output behavior. Generic dirty-rectangle APIs documented for other codecs are not evidence of AV1 work skipping. [G08, G12, G16, G21]

### Screen-content tools require implementation evidence

The AV1 standard's palette and intra-block-copy tools do not establish hardware use. AMF exposes palette and integer-motion controls; Intel VPL exposes palette and intra-block-copy options subject to queries. Inspected RADV source forces `allow_intrabc=0` while forwarding other screen-content choices. No named public palette/intrabc switch was identified in the current NVENC header; the implementation may still make internal decisions. Inspect actual decoded quality and bitstream tool use before crediting a setting with a benefit. Integer-only motion can help some screen patterns while degrading mixed games/video with fractional motion. [G08, G12, G16, G21]

## 9.8 Selection and verification before implementation commitment

The proposed selection process first rejects only incompatible plans, then compares eligible complete plans. Keep a simple hardware-backed control for every vendor. Prioritize the challengers with a concrete avoided cost: NVIDIA final CUDA-array output; AMD direct RGB and VPE; Intel dedicated VPP; complete Vulkan encode where supported. Include the producer's retained-buffer time and the consumer's output lifetime in every comparison.

The first device probes should resolve representation correctness: asymmetric plane patterns, gradients, extremes, odd crops/alignment, actual modifiers, source readiness, and reuse after delayed completion. Resolve the CUDA-array pitch discrepancy explicitly. For RGB hardware conversion, evaluate color/range/siting behavior against an independently specified conversion, with independent decode rather than encoder-reported reconstruction as the sole oracle. Test reconfiguration, required random-access points, repeated frames and drain behavior.

Performance confirmation then measures complete paths at matched quality/bitrate/latency constraints, under both static UI and full-motion workloads and with concurrent graphics load. Report capture-source release, processing and encode delay, sustainable throughput, memory use and affected game frame-time tails. Do not extrapolate vendor standalone Windows encoder tables into Linux recording performance.

The host application can be C23 while using C ABIs for CUDA, NVENC, Vulkan, VAAPI and VPL, and AMF's C-facing interfaces. GPU programs may be compiled separately to SPIR-V or CUDA device code. Choosing a host language does not require inventing a C23 GPU compiler or discarding a useful backend. The deliverable design preserves that freedom; the unresolved hardware probes above remain acceptance gates, not claimed successes.

# 10. Encoding policy, temporal structure and effective configuration

## 10.1 Configure observable results

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

## 10.2 Random access, references and transport

Choose a maximum random-access interval from seek, replay-export, recovery and live-join requirements. Shorter independent intervals can improve joining and clipping while spending bitrate and disturbing rate control. They do not inherently lower the latency of every ordinary frame. Fragment/cluster cadence is a different control and need not always force a keyframe.

AV1 has hidden frames, reference refreshes, order hints and show-existing behavior. A backend may manage these internally or require the application to manage them. The common packet contract therefore contains actual decode/presentation timing, configuration generation, a verified random-access classification, temporal-unit boundaries and retention ownership. It does not assume every input produces one immediately available packet or every packet displays one new picture. Follow the AV1 specification and the chosen container mapping. [C19]

For a local file, recovery is normally better served by complete container boundaries and occasional valid random-access points than by indiscriminately adding error resilience or very frequent intra pictures. A network mode adds loss/jitter and receiver constraints; its codec structure and transport must be designed together. There is no generic io_uring setting that replaces congestion control, packet pacing, receiver buffering or codec dependency repair. A future live backend should use a mature transport implementation and retain the same capture/encoder ownership core.

On resolution or sequence-configuration changes, drain or terminate the old configuration under the declared continuity policy, then start a new decodable sequence and appropriate segment/file. Do not assume every encoder can reset every parameter in place or that every container consumer accepts mid-track changes. A bitrate-only reset may be legal on a backend where a format change is not. Probe the exact reconfiguration and report its effective behavior.

## 10.3 Cost-specific quality experiments

Compare rate-distortion curves at matched decoded semantics, then impose the recorder's resource constraints. Equal bitrate is one useful slice; equal measured quality is another. Equal QP across vendors is not a fair comparison. Include latency and application frame-time impact on the same plots or tables instead of declaring the best quality curve the overall winner.

Separate the effects of spatial AQ, temporal AQ, lookahead, multipass analysis, temporal filtering, reference count, tiles/engine splits and screen-content tools. Their gains interact, so follow individual probes with integrated confirmation of the selected combination. A new knob that enables an internal compute pass can shift the bottleneck back to shader or memory contention. A throughput gain that sacrifices text fidelity should be reported as a tradeoff rather than hidden in an aggregate natural-video metric.

# 11. Timeline, pacing and audio

## 11.1 Use source time and map clock domains explicitly

Preserve source timestamps, arrival timestamps and completion timestamps separately. PipeWire's header contains presentation timing and sequence information; its raw-video format can indicate variable rate. These are better media-time inputs than when the callback happened to run. The direct compositor protocol defines its own presentation-clock contract. Do not assume every timestamp produced by every API shares an epoch, suspend behavior or unit. [C03, C07, C08]

Create an explicit mapping from each source clock into a recording timeline. For clocks with drift, the mapping may be affine over an interval, with a measured scale and offset. Keep an uncertainty estimate and reset the mapping after discontinuities. Use source identity and generation when matching timestamps to pixels. A monotonic increasing callback counter is not a substitute for an actual capture timestamp.

For true CFR output at rational frame rate p/q, derive each target timestamp from the frame index and origin: round(n × q × 1,000,000,000 / p). Use checked integer/rational arithmetic; avoid repeated addition of a rounded period. Rational rates such as 60000/1001 are not exactly 59.94. At 240 fps, repeatedly adding a rounded nanosecond period can drift by 6.912 ms over a day, while rounding each absolute rational timestamp stays within half a nanosecond before container serialization. This is a derived and executed arithmetic check.

Define the sampling policy independently of the encoder. For each CFR deadline, choose the appropriate available source frame without capturing a future frame under an earlier timestamp. If a source update is late, either repeat the last valid display or mark/drop according to the mode; do not conceal timing gaps by relabeling arrival time. In VFR, preserve changes and durations, including cursor-only changes and the final static interval at stop. Final duration cannot be inferred from a nonexistent next frame, so stop provides an explicit timeline endpoint.

Handle duplicate sequence numbers, discontinuity flags, nonmonotonic timestamps, missing timestamps and resume explicitly. Small timestamp jitter and a new clock epoch are different conditions. A corrective policy may rebase a new segment or reject a broken source; arbitrarily clamping every timestamp forward hides problems and can change speed or A/V synchronization.

## 11.2 Audio is a clocked source, not a video side effect

Capture the chosen system monitor and microphone through their authorized PipeWire session paths. Keep them as separate tracks when useful; mixing is an explicit processing step with gain, clipping and channel-layout policy. A monitor follows a particular sink, so device switching and source loss need declared behavior. A microphone selection must not silently change to another device.

A real-time audio callback should copy or transfer bounded data into preallocated storage, publish an immutable descriptor, and return. It must not encode, allocate variable-sized objects, access files, import GPU resources or wait for the video thread. PipeWire's real-time processing contract restricts blocking and non-real-time operations. A separate owner handles conversion, resampling, mixing, encoding and packetization. [C11]

Use the source's sample position and timing to relate audio to the recording clock. Derive drift from timestamp/sample-count observations over a suitable interval, reject discontinuities, and correct gradual drift with a bounded adaptive resampler when the sources are independently clocked. Do not periodically discard an arbitrary audio frame to match a noisy video callback. Preserve a configurable policy for audio gaps: explicit silence, a discontinuity or stopping that track. Track latency compensation and resampler delay in timestamps.

48 kHz is a practical proposed working rate for common video/Opus workflows, not a requirement to reconfigure the entire desktop graph. Accept negotiated source rates and convert where needed. A stereo assumption is also provisional: channel count, layout and microphone/system mixing must be explicit. Avoid silently downmixing a requested multichannel recording.

## 11.3 Audio packet size, priming and finalization

Opus is a strong local/live candidate when the chosen container and consumers accept it. AAC, FLAC or PCM remain consumer/fidelity alternatives. Frame duration affects codec delay and packet overhead. Twenty milliseconds is a reasonable file-oriented control; shorter supported durations are live candidates when the latency budget justifies their overhead and quality effects. Query the codec's actual lookahead rather than hardcoding a remembered constant. Opus documents its encoder controls and lookahead query. [C20]

Codec lookahead, resampler delay, priming/pre-skip, trailing padding and final valid sample count must be represented correctly by the container. Otherwise a file can have apparently aligned first packet timestamps and still play audio late, or contain an audible tail of padded samples. Verify start and end with independently generated impulses and known sample counts.

The mux owner merges tracks by actual decode-time requirements while respecting bounded lateness and explicit stream start/end events. A temporarily silent or unavailable audio source must not cause unbounded video buffering. The output chapter explains the nonzero interleave budget and FFmpeg's misleading zero setting. Video overload should not force the audio real-time callback to wait, and a video skip should preserve elapsed time rather than compressing the audio timeline.

## 11.4 Suspend, pause and stop

Application pause, compositor inactivity and system suspend are distinct. An application pause can remove elapsed time or preserve a gap according to the chosen editing semantics. System suspend may stop one source clock while another clock includes elapsed sleep. Map and signal the transition; a registered io_uring wait clock does not decide media semantics.

On normal stop, choose a common recording endpoint, stop admitting new source content, account for the last held video frame and valid audio samples, drain codec delay, finalize interleaving and mux structures, and complete the requested sink boundary. A hung device or lost source may prevent that ideal drain. Preserve and report the actually established playable/durable prefix, rather than reporting success simply because the stop button was processed.


# 12. Ownership, threads and queues

## 12.1 Derive concurrency from the resource graph

The recorder needs overlapping capture, GPU work, encoding and output. It does not necessarily need a separate CPU thread for every stage. GPU submissions already execute asynchronously; adding a thread and queue between two submissions can introduce a wakeup, cache transfer and extra buffering without creating useful parallelism.

The preferred starting architecture is a serialized owner for session state, GPU submissions and completions, surface pools and retirement. Operations dispatched on that owner use ordinary state and direct calls. Introduce a thread boundary where an actual API requires it: a PipeWire realtime callback, a blocking encoder operation, or an output backend that cannot integrate with the owner’s completion loop. Use a bounded SPSC connection for an edge with exactly one producer and consumer. Keep infrequent control commands on a conventional bounded mutex queue.

This is a provisional decomposition, not a restriction on stronger implementations. Compare a complete owner-based control with a version that isolates a demonstrably blocking backend. A separate submission worker can win if it prevents a driver call from delaying capture; it can lose through scheduling and cache overhead. Multiple encoder sessions may warrant separate owners. A central MPMC worker pool becomes relevant only when multiple workers really can execute interchangeable jobs without violating codec, device-context or resource ownership requirements.

Audio and video naturally provide separate ordered packet streams. One mux owner can merge their timestamps. A shared MPMC queue does not supply timestamp ordering, and sorting its output can merely move work downstream. Establish the required order at the consumer before choosing a global queue.

## 12.2 Represent ownership instead of duplicating frame state

The recurring transferable object is a descriptor owning access to a resource, not a copied image. Keep stable format, plane layout, color interpretation and imported GPU objects in a buffer/session record. Per-frame descriptors contain the changing identity, timestamps, metadata snapshot and synchronization points. Imports and file-descriptor duplication belong to negotiated buffer lifetime wherever the API permits; they should not recur just because another frame callback arrived.

A single owner can maintain completion counts and free lists without atomic reference counts on every frame. This removes synchronization around bookkeeping, while preserving the underlying resource references. A queue-cell lifetime, source-image lifetime, encoder input lifetime, reconstruction/reference lifetime and encoded-packet lifetime remain distinct.

An explicitly lossy live mode admits another representation: one pending immutable frame token transferred by atomic exchange. The producer exchanges in a new token and retires only the old pending token returned; the consumer exchanges with null and owns only its returned token. Consumer-held GPU work remains separate. This can remove a backlog of obsolete, unsubmitted frames, but it intentionally changes FIFO semantics and requires spare surfaces for replacement. It is a candidate for freshness, not a replacement for archival capture. A loaded pointer must never be dereferenced before ownership is acquired; a version check cannot retrospectively repair a C data race or GPU use-after-free.

## 12.3 SPSC publication, consumption and reuse

For a genuine SPSC edge, shared head/tail read-modify-write operations are unnecessary. Each side writes its own index, caches the last observed peer index, and refreshes that cache when it appears empty or full. The following is explanatory C23 pseudocode for a preallocated descriptor ring; initialization, alignment and error handling are omitted. [Q01, Q02]

```c
// Producer-only locals: w, cached_r.
if (w - cached_r == N) {
    cached_r = atomic_load_explicit(&released_r, memory_order_acquire);
    if (w - cached_r == N)
        return WOULD_BLOCK;             // Caller retains the input token.
}
slots[w & (N - 1)] = complete_descriptor;
w++;
atomic_store_explicit(&published_w, w, memory_order_release);

// Consumer-only locals: r, cached_w.
if (r == cached_w) {
    cached_w = atomic_load_explicit(&published_w, memory_order_acquire);
    if (r == cached_w)
        return WOULD_BLOCK;
}
result = slots[r & (N - 1)];             // Ownership transfers with this copy.
r++;
atomic_store_explicit(&released_r, r, memory_order_release);
```

The producer’s release/acquire edge orders complete descriptor initialization before consumer reads. The consumer’s release/acquire edge orders the completed descriptor copy before producer overwrite of that cell. Securing the resource token is part of initialization. A reservation is private until publication; publishing an index before the descriptor is complete is incorrect.

A producer paused before publication exposes no partial descriptor. A consumer paused before releasing its index retains ring capacity, preventing overwrite. Once the consumer copies the descriptor, the ring cell can be reused even while the represented surface remains in flight. This separation avoids retaining queue capacity merely because a GPU operation has not finished.

For this representation, `N` is a power of two; counters are unsigned, consistently initialized, and their live modular distance remains at most `N`, with capacity below half the counter range. A paused SPSC peer limits how far the other can advance before it stops for lack of data or space. That supports a wrap argument for this bounded topology. Verify it with reduced-width counters. Do not transplant it into MPMC designs with skipped tickets or packed epochs. These are representation constraints, not production resolution or frame-rate limits.

Use `_Atomic` and explicit ordering, not `volatile` or ordinary fields copied from kernel examples. Check the chosen types with the C atomic lock-free facilities and inspect generated code. The algorithm performs a bounded number of abstract accesses; native atomic implementations, preemption and scheduling remain separate progress assumptions. Isolate producer- and consumer-written indices on correctly aligned cache lines. Additional padding and batching are measurable choices, not universal prescriptions. [Q01]

## 12.4 Sleeping is part of the handoff

The simple notification control uses `eventfd(EFD_NONBLOCK | EFD_CLOEXEC)`, with a write after each successfully published batch. The consumer drains eventfd, drains its queue, and waits through its normal polling loop. The ring’s acquire/release operations publish memory; eventfd supplies readiness. Handle interruption and errors explicitly. A saturated nonblocking eventfd counter already represents pending readiness. [Q03]

The scheduling argument is straightforward: work published before the consumer drains is consumed; publication that escapes that drain leaves a notification pending or has a producer still responsible for issuing it. An unnecessary wake is harmless. An unsafe optimization is to inspect whether the queue is empty before enqueueing and notify only when that observation was true. The producer can observe an older item, pause while the consumer drains and sleeps, then publish without waking it.

If measured notification cost justifies coalescing, a single-consumer armed protocol is a credible challenger:

1. After draining work, the consumer performs an `acq_rel` exchange setting `armed = true`, then rechecks the queue with acquire semantics.
2. If work exists, it exchanges `armed = false` and resumes draining; otherwise it polls eventfd.
3. After publication, the producer exchanges `armed = false` with `acq_rel`. A returned true value requires an eventfd write.

All changes to the arm state, including cancellation, use the specified exchange protocol. A producer exchange ordered before arming can synchronize with that arm and its recheck; one ordered afterward sees the wake request. Replacing this handshake with relaxed guesses removes that reasoning. Its abstract schedule behavior was checked below; production memory ordering still needs dedicated C weak-memory validation.

Neither notification scheme makes the entire handoff wait-free. A producer paused after publication but before its required wake can delay a sleeping consumer; its combined operation has not completed. Kernel notification and thread scheduling add their own blocking behavior. Likewise, a driver wait or allocator call does not inherit a queue’s progress guarantee.

A dedicated worker may instead use a futex condition/sequence protocol. `FUTEX_WAIT` atomically checks the expected word and enters sleep; the caller still loops around its real condition and handles interruption and spurious returns. Its aligned 32-bit ABI is Linux-specific. Compare complete waking and idle behavior against eventfd’s ability to share the existing poll loop. [Q04]

## 12.5 Strong MPMC alternatives, if the topology requires them

A fair comparison includes descriptor access, pools, notification, allocation, reclamation and shutdown. None of the following algorithms is selected merely because its name appears in high-throughput literature.

The control for a real MPSC/MPMC boundary is a preallocated bounded mutex queue with the same full/empty policy and wakeup behavior. Its lock can block behind a paused holder, but short uncontended critical sections may beat a more elaborate algorithm at recorder traffic rates. Compare both CPU cost and consequential capture-delay tails. A blocking enqueue and an immediate `WOULD_BLOCK` operation have different semantics; match them before timing.

| Candidate | Decisive mechanism | Consequence for this recorder |
|---|---|---|
| LCRQ | FAA reservations and cyclic cell reuse; closes problematic rings and links successors | Closing/linking and safe node reclamation are essential. An extracted single CRQ does not retain the complete progress argument. Original wide-CAS and lifetime costs belong in the comparison. [Q05] |
| FAAArrayQueue | Noncyclic array cells, FAA assignment and a prefilled newly linked node | Portable cell operations simplify the structure, but cells are not recycled within the array; allocation and reclamation recur. [Q06] |
| SCQ | Payload indirection, allocated/free index queues, epoch/safe state and a threshold preventing endless invalidation | Its doubled ring capacity, `3n-1` threshold, at-most-`n` participating threads and original nonwrapping-counter assumption support the proof. Bounded queue storage does not eliminate payload lifetime. [Q08] |
| wCQ | SCQ fast path plus bounded-retry transition to cooperative helping | Per-thread requests, phase and completion state are substantive. The main design uses wide CAS; complete allocation/registration/wakeup paths need separate guarantees. [Q09] |
| LPRQ | Changed epoch transitions; token reservation, epoch promotion and publication replace original CAS2 use | A serious portable cyclic-ring challenger. The paper's standalone PRQ is obstruction-free; closure/linking restores the complete design's progress. Simply splitting two-word CAS operations is incorrect. Reclamation still matters. [Q10] |
| CRTurn | Published requests, turn-based helping and associated reclamation | Thread-registration lifetime and allocator behavior must be included. The primary C++ enqueue allocates a node, so its abstract progress does not make that allocator bounded-step. [Q11] |

Ramalhete’s follow-up provides contrary workload evidence: many dedicated consumers can invalidate cells before a producer commits in LCRQ/FAAArrayQueue, forcing retries. Include producer/consumer imbalance, idle/wake cycles, near-empty traffic, bursts and paused participants. Enqueue/dequeue pairs on a dedicated, pinned benchmark machine cannot establish superiority for this recorder. [Q07]

NBLFQ, published at IPDPS 2025, specifically addresses low-contention bounded MPMC queues. Its primary indexed metadata is relevant, but full text was inaccessible during this review. Its decisive mechanism was not inspected and it remains unranked. Obtain that evidence if a genuine MPMC decision becomes consequential; missing access is not evidence of inferiority. [Q12]

FAA reservation alone is not a progress proof. A paused producer can reserve the next FIFO position while another publishes later work. A consumer that must await the first producer can stall despite available later items. The named designs matter because they address such cases with specific mechanisms. Keeping only their normal path does not preserve their guarantees.

## 12.6 Resource retirement, drop and shutdown

GPU access ends at the backend’s actual completion condition, not at CPU descriptor dequeue. Conversion input, direct encoder input, reconstruction/DPB state and encoded packet storage may retire at different times. CPU atomics publish synchronization handles; they neither signal GPU fences nor perform external-memory visibility or ownership transitions.

The producer may reject its own unpublished input. It must not advance an SPSC consumer index to discard older data that the consumer may already be reading. The consumer can discard unsubmitted inputs under an explicit freshness policy. Submitted GPU work retains resources until completion even when its output is unwanted. Arbitrarily discarding encoded AV1 packets can invalidate later reference-dependent frames.

Stop admission, wake owners, settle reservations, drain or cancel unsubmitted inputs according to policy, flush delayed encoder output, finish asynchronous sink accesses and finalize the container. Retire resources exactly once from authoritative completion paths. Do not hold a lock while joining a thread that needs it to finish.

No finite-memory design guarantees continuous no-drop capture and bounded latency across an arbitrary sink stall. Backlog grows while the sink provides no service. Budget exhaustion requires a defined admission, drop or stop response. Raw surfaces, encoder references, encoded bytes and write-inflight storage need separate budgets. Average occupancy estimates are not worst-case latency proofs.

## 12.7 PipeWire lifetime cross-check

The early-return, release-promise and buffer-removal contract is integrated in Chapter 5. The independent review agrees with the official producer/consumer contract; its source pointers are retained here for audit. [Q13, Q14, Q15, Q16, Q17]

## 12.8 Executed design probes and remaining proof obligations

The companion Python checker exhaustively enumerated finite, sequentially consistent atomic abstract schedules. Deliberately broken variants validate its failure oracles. It completed successfully. [Q18]

| Probe | Result | Discovered states | Expanded transitions |
|---|---|---:|---:|
| Unconditional eventfd wake | No lost wake in model | 37 | 47 |
| Armed-RMW plus recheck | No lost wake in model | 93 | 140 |
| Retire after GPU completion | No generation reuse/use-after-free in model | 286 | 616 |
| Broken pre-enqueue empty check | Counterexample | 27 | 35 |
| Broken recycle-on-submit | Counterexample | 59 | 82 |
| Broken CPU-only shutdown drain | Counterexample | 70 | 106 |

The notification model has one existing item and one new producer item. The lifetime model has two surfaces, one queued descriptor, at most three frames, asynchronous completions and arbitrary stopping. Broken-case counts stop at the first counterexample. These results do not prove C23 weak-memory behavior, real eventfd/driver contracts, unbounded execution or performance. The early PipeWire return path and latest-frame mailbox are not modeled by this checker. Native implementation requires independent memory-order checks, lifecycle fault injection and whole-machine confirmation.

# 13. Packets, containers, replay and output

## 13.1 Encoded bytes are the storage boundary

The output architecture should preserve encoded media, timing, codec configuration and decoding dependencies while releasing capture and encoder resources promptly. It should not carry framebuffer allocations into an ordinary file-writing abstraction. io_uring can manage supported file operations, readiness and timers; it does not replace PipeWire buffer negotiation, GPU command submission, encoder APIs or DMA-BUF synchronization. A DMA-BUF fd is not a readable encoded bytestream merely because both objects have file descriptors.

The scale difference matters. A 3840 × 2160 × four-byte surface at 120 frames/s represents 3.981 GB/s, whereas an illustrative 200 Mbit/s encoded stream represents 25 MB/s before audio and container overhead. These are verified arithmetic examples, not bitrate or resolution limits. Consequently, retaining an expensive encoder output object to avoid copying relatively few compressed bytes may increase total overhead. Compare retaining the driver buffer until I/O completion against one copy into owned slabs followed by immediate driver release. Measure pool occupancy, encoder stalls, CPU work and latency across both lifetimes.

Use a mature muxer as the production control. Represent accepted data as immutable owned ranges with lengths, offsets and explicit release conditions. Aggregate small headers without requiring one allocation or syscall per element; compare scatter/gather traversal against packing into a contiguous slab. Keep packet ownership separate from write-request ownership so aggregation can change independently of encoding.

A specialized streaming muxer remains a credible stronger candidate if mux/AVIO copies, generic buffering or header bookkeeping become consequential. It can preserve native temporal-unit ownership and emit compact headers plus payload ranges directly to the sink. Evaluate that mechanism against an adapter or small library patch, including initialization, ordering, precise timestamps, duration/priming, indexes, random access, recovery, errors and finalization. Required behavior need not be impossible in the library for a replacement to win; a complete measured improvement is sufficient. No such replacement is implemented or ranked here.

The evidence status throughout this chapter is explicit: API semantics and cited implementation mechanisms have been inspected; the numerical examples have been checked; recorder performance and player interoperability have not been measured. Defaults are proposed candidates, not established universally optimal settings.

## 13.2 Mux ownership must survive asynchronous output

libavformat offers different ownership and ordering contracts. `av_interleaved_write_frame` can retain packets while establishing decode-timestamp order; `av_write_frame` leaves ordering to its caller. Follow the selected reference-ownership and padding requirements rather than assuming all packet buffers remain valid until a later flush. [IO01]

Custom AVIO introduces a consequential trap. Current FFmpeg `writeout` calls `write_packet`, records a negative result as an error, and otherwise accounts for the entire requested length. A positive short return is not a safe partial-acceptance protocol. An asynchronous adapter must either finish using the supplied bytes before returning or fully accept them into owned storage and return the full length. Enqueuing the temporary pointer and returning success does not extend its lifetime. [IO02]

The adapter therefore needs a persistent asynchronous-error latch, byte admission credits and a final drain. Subsequent callbacks must surface previous failures, and successful enqueue cannot be reported as successful recording finalization. Seek callbacks change the logical cursor; overlapping backward patches still need ordered physical completion. Compare the adapter's copying and bookkeeping costs with ordinary synchronous AVIO on a dedicated output thread. A nominally asynchronous sink that hides those costs is not a fair challenger.

## 13.3 Exact timing and container selection

Preserve internal timestamps in nanoseconds or an exact rational representation with explicit clock conversion and rounding. Serialize once at the container boundary. Distinguish capture time, presentation time, decode time, arrival time and completion time; final held-frame duration must remain representable. AV1 temporal units, invisible frames and reordering prevent a universal one-packet/one-displayed-frame assumption.

The Matroska AV1 mapping defines a Block as a Temporal Unit and uses an AV1CodecConfigurationRecord in CodecPrivate. Its configuration and random-access rules must be applied to actual encoder output. Treat substantive sequence-configuration changes as segment/configuration transitions; do not read the mapping's sequence-header wording as forbidding repeated identical headers at random-access points. The ISO BMFF AV1 mapping likewise specifies temporal-unit samples and synchronization requirements. A generic I-frame label alone does not prove independent decodability. [IO05, IO06]

### Matroska precision is a library decision, not a format limit

RFC 9559 expresses TimestampScale in nanoseconds. At unit TrackTimestampScale, Block timestamps are signed 16-bit offsets in Segment ticks. Current FFmpeg Matroska source writes a 1,000,000 ns scale and sets stream timing to `1/1000`; the container itself permits finer precision. At 240 Hz, millisecond serialization produces mixed 4/5 ms spacing instead of retaining the ideal 4⅙ ms cadence. [IO03, IO04]

For a cluster timestamp placed at its first packet, the maximum positive relative offset is:

| TimestampScale | Positive Block span | Design consequence |
|---:|---:|---|
| 1 ms | 32.767 s | Lower cluster turnover; millisecond precision |
| 100 µs | 3.2767 s | Fine timing with multi-second headroom |
| 10 µs | 327.67 ms | More frequent clusters |
| 1 µs | 32.767 ms | Very frequent cluster and index work |

These are representation costs, not recording-duration limits. Reordering can require negative offsets or different cluster bases. Select precision from consumer requirements, then measure cluster overhead and interoperability.

A small upstreamable libavformat timestamp-scale option is the strongest first challenger to a custom Matroska muxer. It must audit stream time bases, cluster limits, duration, cues, codec delay and seek preroll-not merely replace the EBML header value. Preserve existing defaults and validate fine-scale audio/video timing, seeks, VFR, reordered timestamps and long static holds. This patch is proposed, not implemented.

### fMP4 is the immediately available fine-timing candidate

Where consumers accept fragmented MP4, libavformat exposes a configurable video track timescale and fragment controls. Completed fragments can preserve usable output after interruption where ordinary unfinished end-moov MP4 may fail. Fragment cadence and random-access cadence are separate: independently playable fragments need suitable starting points, but bounded availability does not inherently require a quality-expensive keyframe at every fragment. [IO07]

Choose a timescale from acceptable serialization error, audio relationships, field ranges and consumer support. One million ticks/s is a candidate, not a requirement. Blindly transferring nanoseconds into every container field creates constraints: a 32-bit sample-duration field spans only about 4.295 seconds at one billion ticks/s. Long held frames need an explicit representable policy.

Use fMP4 as the readily available precise-timing candidate and retain Matroska as an equal architectural contender, including the audited scale option when needed. Container selection remains consumer-driven. Matroska can stream with unknown Segment size; a partial file without final Cues may be recoverable without being instantly seekable or durable. Avoid a finalization policy that shifts an hours-long file merely to move Cues to its front. [IO07, IO08]

Interleaving deserves its own latency budget. Bound waiting for delayed or absent audio using decode-time ordering, stream lifecycle events and an explicit lateness policy. In FFmpeg, `max_interleave_delta=0` removes the maximum-difference escape; it can wait indefinitely for a packet from every stream. Packet flushing and smaller positive interleave budgets spend overhead to improve availability. Neither is an automatic best setting. [IO07]

## 13.4 Replay and admission control

Retain encoded replay chunks rather than raw surfaces. Required memory is retention time times encoded rate, plus burst allowance, metadata and the preceding random-access interval needed for decoding. Average bitrate does not bound instantaneous bytes. Bound retained bytes and time, not just packet count.

RAM chunks or rolling disk segments carry immutable generations, configuration and decoding dependencies. An export snapshot pins those generations; neither its readers nor outstanding writes may observe reused storage. A completed queue publication is not permission to overwrite a chunk still owned elsewhere.

Reflink or `copy_file_range` can reduce bulk copying on supported filesystems but cannot manufacture valid initialization, timestamps or indexes. Export usually remuxes from a preceding independently decodable AV1 point. Frame-exact arbitrary starts require supported preroll/edit semantics or boundary re-encoding. Price that precision separately instead of silently including extra visible frames.

For sustained storage overload, distinguish transient buffering from insufficient long-term service rate. Arbitrary compressed-packet drops can damage reference chains. Prefer an explicitly signaled capture-side skip before encoding where the selected recording mode permits it. Otherwise stop cleanly or restart after a declared discontinuity at a verified fresh random-access point. Do not invent a universal drop policy for a mode requiring every frame.

## 13.5 Compare complete sink architectures

| Candidate | Mechanism that can win | Costs that must remain in the comparison |
|---|---|---|
| Dedicated buffered `writev`/`pwritev` writer | Simple ownership and byte-granular mux output; absorbs bursts outside capture | Thread handoff, page-cache copy, dirty throttling, writeback tails |
| Ordinary buffered io_uring owner | Overlap without blocking; potentially removes writer handoff | SQ/CQ work, longer payload lifetime, worker fallback, AVIO adapter |
| Direct io_uring with aligned slabs | Avoids page-cache pollution/copy; amortizes repeated pinning | Alignment, aggregation, tail protocol, metadata patches, pinned-memory budget |
| SQPOLL/IOPOLL variants | Reduces submission transitions or completion interrupts in favorable workloads | Polling CPU/energy, restricted operations, device setup, workload interference |

The provisional default is buffered output, with both the simple writer and integrated ordinary io_uring retained until evidence selects between them. High-quality or multistream recording can justify direct I/O; ordinary low-rate desktop recording must remain in the comparison. Count io-wq, polling threads, filesystem writeback, interrupts, energy and game frame-time impact. Moving work out of the recorder's userspace CPU counter is not eliminating it.

## 13.6 io_uring: Linux 6.12 stage decisions

A credible ordinary-ring starting point is `SINGLE_ISSUER | DEFER_TASKRUN | NO_SQARRAY`, optionally `TASKRUN_FLAG` for peek-style servicing, using compatible liburing. The owner must create or enable the ring and regularly drive completion work. A blocking encoder API must not occupy that owner indefinitely. This configuration is a proposal supported by mechanisms, not a benchmark result. [IO09, IO10]

| Stage / knob | Baseline facts and compatibility | Proposed policy |
|---|---|---|
| Issuer: `SINGLE_ISSUER` | Since 6.0; creator, or enabler of `R_DISABLED`, is the issuer | Enforce one actual SQ owner; never treat the hint as a concurrent-producer queue |
| Completion scheduling: `DEFER_TASKRUN` | Since 6.1; requires single issuer and completion-driving `GETEVENTS` entry | Strong ordinary-ring candidate; guarantee service during idle and shutdown |
| `COOP_TASKRUN`, `TASKRUN_FLAG` | Since 5.19; cooperative task work and pending-work indication | COOP is an alternative scheduling choice; TASKRUN_FLAG supports safe liburing peeking |
| SQ layout: `NO_SQARRAY` | Since 6.6; removes indirection | Use for ordinary ordered SQE production; maintain library compatibility |
| Batch submission: `SUBMIT_ALL` | Since 5.18; continues after submission-time errors | Optional accounting simplification; still handle partial consumption and every CQ result |
| Ring memory: `NO_MMAP`, `REGISTERED_FD_ONLY` | Since 6.5; the latter requires the former | Optional setup optimization, not required to register the ring fd |
| Queue sizing / `CQSIZE` | SQ bounds batch size, not all in-flight work; default CQ is twice SQ | Explicit outstanding credits; capacity for original, cancellation and control completions |
| Files / ring fd registration | Avoids repeated lookup/refcount work; descriptor turnover remains | Register stable resources if useful; include rolling-file replacement costs |
| Buffer registration | Repeated direct-I/O pin/map work can be amortized; locked-memory limits apply | Optional owned-slab registration; buffered writes still copy into page cache |
| Issue policy: `IOSQE_ASYNC` | Forces worker execution instead of the normal first attempt | Off initially; test only when an operation consistently needs worker execution |
| io-wq caps / affinity | Caps available by 5.15, affinity by 5.14; zero cap means query/no change | Tune from actual service/progress needs; avoid interference with game-critical CPUs |
| `ATTACH_WQ` | Shares workers, and can share SQ polling resources | Only when saved resources outweigh coupling between rings |
| Wait count and MIN_TIMEOUT | Minimum completion batching wait available in 6.12 | Batch available work first; any intentional delay must fit the latency budget |
| Wait clock / absolute deadlines | Configurable monotonic/boottime clock available in 6.12 | Choose suspension semantics explicitly; keep media-clock mapping separate |
| `CQE_SKIP_SUCCESS` | Removes successful CQEs; incompatible with IO_DRAIN across the ring | Off initially; one coalesced writev often provides simpler group accounting |
| `IO_LINK`, `IO_DRAIN`, `IO_HARDLINK` | Chain order, ring-wide barrier, and failure-persistent chain differ | Use exact dependencies; avoid global drains behind permanent requests |
| SQE128 / CQE32 | Larger entries serve operations needing them | Ordinary file writes need no enlarged entries |

Sources for the table are the syscall API and inspected pre-6.13 kernel implementation, plus registration, sizing, submission, wait and worker interfaces. [IO09-IO19, IO21] The source commit is the parent of the 6.13 io_uring merge, not a claim to have inspected the exact v6.12 release tag. Feature-probe optional paths and pin the eventual library/kernel versions.

Queue dimensions follow the event model: reserve CQ space for all completions that can arrive before the next drain, and separately bound bytes held by outstanding operations. `NODROP` is not permission to overflow routinely; allocation failure can still lose completions and invalidate ordinary reclamation assumptions. [IO11]

Flush on bytes, oldest-packet deadline, checkpoint or stop-whichever comes first. Consuming an available batch amortizes work; waiting to fill one spends latency. No fixed queue depth, slab size or microsecond wait is established as optimal. Registered-buffer cloning exists by 6.12, but adding rings merely to use it creates unnecessary ownership complexity. [IO15, IO16]

### Polling is a separate architecture

SQPOLL removes entry calls while its kernel thread remains awake. The inspected kernel rejects SQPOLL with COOP_TASKRUN, TASKRUN_FLAG or DEFER_TASKRUN. Critically, `sq_thread_idle=0` becomes one second, not immediate sleeping; ordinary frame intervals can therefore leave the poller continuously active. Follow liburing's wakeup protocol. [IO10, IO20]

IOPOLL requires appropriate direct-I/O, filesystem and device polling support for ordinary file read/write variants. Many control operations cannot share the ring; another path for synchronization must be included in the cost. SQ_AFF places the submission poller, not all device or GPU completion work. Modern unprivileged support does not override sysctl/seccomp policy: setup rejection should select the ordinary writer, not make privilege a recording dependency. [IO09, IO36]

| Later option | Established introduction | Applicability |
|---|---:|---|
| Hybrid IOPOLL, registered wait regions, ring resize | 6.13 | Separate optional experiments; hybrid retains polling prerequisites |
| `RWF_DONTCACHE` | 6.14 | One-pass buffered-output challenger; best-effort cache pruning/writeback, not durability |
| NO_IOWAIT | 6.15 | Measure scheduling, energy and wakeup-latency effects |
| FDP `write_stream` | 6.16 block merge | Device/filesystem propagation required; not a generic regular-file speedup |
| Mixed-size CQEs | 6.18 | Relevant only when some actual operations need larger CQEs |
| SQ rewind | 7.0 in current documentation | Later SQ layout experiment, not a 6.12 prerequisite |

These boundaries come from upstream merges and APIs. [IO09, IO31-IO35] Newer is not automatically better for sparse compressed writes.

## 13.7 Completion, failure and durability invariants

Assign non-overlapping explicit offsets to ordinary append output. Track four distinct states:

- **L:** bytes accepted with owned backing storage and logical positions.
- **W:** the contiguous successfully written prefix, without holes.
- **D:** a successful durability checkpoint covering its required data and metadata generation.
- **C:** a complete recoverable container boundary with required initialization and dependencies.

Later successful CQEs cannot advance W over earlier missing bytes. Backward metadata patches require generation-aware checkpoint accounting. “Accepted,” “available to a reader” and “durable” are different latency metrics.

On a positive short write, mark only its prefix completed and resubmit the remainder with advanced file and buffer offsets; consume vector prefixes correctly. A zero result for nonzero data requires a bounded nonprogress policy. Negative errors need classification, not blind whole-request retry. Synchronous writes can fail late, and a failed direct-I/O request may already have modified its range. Treat uncertain data as a recovery problem. [IO23]

Submission return counts are not successful operation counts. Preserve structural arguments through their documented consumption boundary and payloads through terminal completion; SQPOLL complicates using submit's return as an exact consumed count. Cancellation has its own result: wait for the original terminal CQE before recycling its buffer or identity. Cancellation is not rollback. Use generations where IDs can be reused. [IO22, IO24]

Independent io_uring writes and fsync have no implicit ordering. Wait for the exact completed write set or encode a valid dependency. IO_LINK orders its chain, not unrelated operations; IO_DRAIN is ring-wide and can wait forever behind a permanent request. HARDLINK is inappropriate when a later commit assumes earlier execution succeeded. Queue simplicity does not establish whole-program lock freedom: paused owners, drivers, allocation and filesystems retain their own progress limits. [IO21]

Differentiate process crash, kernel crash and power loss. Expose the required durability policy and include it in every performance comparison. Finalization stops new production, drains encoder output, finishes muxing, completes accepted writes and patches, checks synchronization, and then releases resources. For durable publication, sync the file before rename and sync its directory when the name must survive a crash. Preserve failed partial recordings and report the established recoverable boundary. Device power-loss protection does not flush userspace or filesystem caches. [IO25]

## 13.8 Direct I/O and cache pressure

Direct I/O is credible for sustained output or page-cache interference, provided the complete mux lifecycle remains valid. Query STATX_DIOALIGN where supported; compliant address, length and file offsets are filesystem/device-specific. Variable packet sizes, headers, tails and backward patches require aligned aggregation. Arbitrary padding cannot be inserted into container bytes. An aligned physical tail plus later truncation needs an explicit ordered protocol. Avoid overlapping direct and buffered operations; O_DIRECT is not durability. [IO26, IO27]

Rolling `fallocate(KEEP_SIZE)` reservation can move allocation work off the critical path and reveal capacity failure earlier without publishing an oversized file. It cannot guarantee bandwidth. NOREUSE has useful replacement-policy semantics since 6.3; SEQUENTIAL mostly changes read-ahead. DONTNEED does not guarantee eviction of dirty data, and sync_file_range is writeback scheduling rather than a durability primitive. [IO28-IO30]

RWF_ATOMIC, available from 6.11 under specific direct-I/O, alignment and device/filesystem constraints, protects qualifying writes against tearing. It is not a transaction over packets and metadata, nor automatic durability. RWF_DONTCACHE is a stronger later buffered challenger for one-pass output; it can lose when replay immediately reads those bytes. Do not silently change global dirty-page settings, CPU governors or NVMe poll-queue configuration to manufacture a win. [IO32]

## 13.9 Experiments that can overturn the defaults

The primary study on io_uring in high-performance DBMSs supplies useful mechanisms-integrated asynchronous redesign, polling costs and batching tradeoffs-but its storage-heavy results do not establish recorder performance. [IO37] The decisive recorder experiments are:

1. Retained driver output versus owned compressed slabs, including encoder-resource occupancy and end-to-end tails.
2. Integrated buffered writer versus ordinary io_uring with identical mux bytes, durability, CPU resources and bounded backlog.
3. Direct I/O and DONTCACHE under high output and competing memory pressure; polling under favorable high request rates and ordinary sparse recording, counting all system work.
4. Fine-scale Matroska versus fMP4 across required players, audio gaps, VFR, reordering, long holds, seeks, configuration changes and interrupted finalization.
5. Injected short writes, delayed/out-of-order completions, ENOSPC/EDQUOT/EIO, cancellation races, paused owners, allocation denial and crashes at structural boundaries.

After tuning, confirm on fresh captures and conditions against a fixed control. Report latency distributions, backlog, game frame-time effects, dropped/skipped content, throughput and energy with run-to-run variation. A synthetic disk result cannot substitute for this producer-to-consumer comparison.

# 14. Admission, overload, recovery and system progress

## 14.1 Credits represent actual retained resources

Use distinct credits for source leases, recorder-owned images, encoder inputs, reconstruction/reference surfaces, encoded packet bytes, mux staging and in-flight sink bytes. Count a resource until its true release event. Returning a queue slot, receiving one output packet or issuing a cancellation is not a universal release event.

Admission checks the next job's incremental demand before committing work. For a simple bounded pipeline this can be owner-local accounting rather than a global atomic semaphore. Reservations spanning owners need a documented order and rollback path. Avoid holding one scarce resource indefinitely while waiting for another in a cycle: for example, a source lease waiting for an encoder surface that cannot retire because output buffers are held by a blocked mux owner.

Reserve a small, explicitly accounted control capacity for EOS, error and retirement messages so a full data queue cannot prevent progress toward shutdown. This does not guarantee progress through a hung GPU or filesystem. A stalled owner, allocator, runtime callback or mandatory I/O operation remains a progress dependency even when every queue operation is lock-free.

Apply both byte and age limits. A large byte budget may hold little time at high bitrate and many seconds at low bitrate. A frame-count cap misses large format changes. Derive the initial surface pool from API requirements and measured retention, and derive sink credit from burst handling plus the chosen bounded-stall policy. These are workload-dependent parameters, not immutable production limits.

## 14.2 Different losses have different semantics

The capture owner may reject an input it has not submitted, after consuming metadata necessary to preserve damage and timeline state. A consumer may discard an unsubmitted stale descriptor. A producer must not manipulate a consumer index to delete a frame that may already be in use. A latest-frame mailbox is an alternative only with a complete pin/claim/replacement protocol and a consumer contract allowing intermediate updates to be skipped.

Once GPU work has been accepted, unwanted output does not cancel its resource access. Once an AV1 reference-dependent packet is encoded, dropping it arbitrarily can invalidate future output. Prefer admission-side skips where permitted. If output must be discarded, restart at a verified random-access boundary and expose a discontinuity or stop with a clear partial-recording result.

A no-drop mode requires a sustainable offered workload or a defined failure response. No finite memory can guarantee uninterrupted no-drop capture and bounded latency through an arbitrarily long sink stall. This is a backlog argument, not a limitation caused by choosing a bounded queue. Increasing capacity merely extends the time before the policy must act.

## 14.3 State transitions and error classes

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

## 14.4 Isolation, scheduling and observability

Use process isolation when it solves a demonstrated fault-containment or privilege boundary. A tiny optional KMS helper and an untrusted-media decode test worker can have separate responsibilities. A process per pipeline stage would add IPC and lifetime complexity without an established benefit. A GPU worker process can improve recovery from a stuck userspace runtime, but termination is not proof that hardware stopped touching memory; teardown must follow driver guarantees.

Do not make global CPU governor changes, GPU clocks, real-time scheduling, forced firmware parameters or disabled power management a hidden requirement. They affect the rest of the machine. Baseline measurements should use the intended deployment policy, then label any special tuning as a separate operating condition. Thread affinity can reduce migration but also collide with game-critical cores; measure all affected threads and interrupts before selecting it.

The hot path should avoid fresh imports, shader compilation, heap allocation, unbounded log formatting, synchronous filesystem access and global locks. Some backend operations may internally allocate or block; instrument these rather than asserting that application-level pooling removed them. Bounded binary event records can capture diagnostics with deferred formatting. Use per-owner counters where possible and sampled or batched aggregation to avoid a contended global statistics cache line.

# 15. C23 implementation contract and component boundaries

## 15.1 C23 is the host language, not an artificial dependency ban

Implement the application core in C23 with explicit ownership, checked arithmetic and narrow backend boundaries. Existing C ABIs for PipeWire, Vulkan, libdrm, libva, VPL, CUDA/NVENC and libavformat are practical building blocks. An SDK may require a small compatibility wrapper or a separately compiled device program. Precompiled SPIR-V or CUDA kernels do not violate a C23 host implementation. Do not force a slower data path because a convenient kernel toolchain uses a different device-language syntax.

Pin build dependencies and record runtime versions/capabilities. A compile-time header exposing a flag does not mean the running kernel or driver supports it. Dynamically load optional vendor libraries and fail gracefully to a selected alternative when missing. Avoid making all three vendor stacks mandatory on every installation.

Use fixed-width types for serialized/protocol fields and checked conversions for dimensions, strides, plane offsets, bitrate, duration and allocation sizes. C23 checked-integer facilities are useful where the actual compiler/libc supports them; provide an audited fallback if needed. Pointer representation, atomic lock-freedom and cache-line size are platform properties, not universal C-language guarantees. Do not rely on volatile for inter-thread publication or on packed structs for live atomic state. [C21]

## 15.2 Internal modules are responsibilities, not compulsory copies

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

## 15.3 Build and hot-path discipline

Select a reproducible compiler/toolchain with the C23 features actually used. Keep warnings strict for ownership-sensitive code, signedness and truncation. Use sanitizers on CPU-only and supported integration paths; a clean ThreadSanitizer run does not validate external GPU memory. Keep optional CPU instruction specialization behind runtime detection and a portable fallback. Inspect generated assembly when the SPSC implementation or an arithmetic helper becomes a measured hot path, rather than treating a language-level claim as a performance result.

Preallocate bounded pools per configuration after capability discovery. Maintain a controlled growth/reconfiguration path for legitimate format changes, including 8K or unusual dimensions if the actual device supports them. A test fixture's size must not be encoded as the maximum accepted stream. On resource-allocation failure, return a specific capability or budget result; do not silently reduce resolution or fidelity unless the selected policy permits it.

Use mature mux/codec libraries initially. Where a library default is a real bottleneck or correctness constraint, compare a narrow audited patch or native adapter with a complete specialized replacement. Examples are configurable Matroska timestamp scale, native CUDA-array output from conversion, and a backend input-retirement hook. A custom AV1 muxer, memory reclamation algorithm or scheduler can be justified by a concrete missing behavior or by a mechanism that improves whole-system cost while preserving required behavior. It needs a complete validation plan and fair lifecycle evidence; it need not prove that adaptation is impossible.

Packaging must document the selected components and comply with their actual licenses and redistribution terms. No particular licensing model was specified by the user, so the architecture keeps dependencies replaceable rather than inventing a legal restriction. Release engineering should audit the final dependency set; this report does not make a legal compatibility determination.

# 16. Validation that can select the winner

## 16.1 Establish complete controls and meaningful challengers

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

## 16.2 Workloads and offered load

Use independently captured or generated references covering static terminal text, colored subpixel patterns, cursor-only movement, scrolling code, browser/UI animation, mixed video and desktop, high-motion games, gradients and HDR highlights. Include small windows, portrait/rotated outputs, fractional scaling, odd dimensions/alignment boundaries, multiple monitors with mixed refresh rates, and cross-GPU configurations. Choose sizes and rates from supported devices; 1080p, 4K, 8K, 60 and 240 Hz are coverage examples, not a scope boundary.

Measure idle/static periods as well as saturation. A poller that wins during sustained encoded output can waste a core between sparse desktop updates. A damage path can win on a terminal and lose on full-screen motion due to bookkeeping and conservative overdraw. A principled dispatcher should use observable conditions such as required transforms, source format and measured damage coverage, with hysteresis when needed to avoid expensive oscillation. Include dispatch overhead and maintain a correct fallback.

An externally defined offered frame schedule or reproducible game trace is necessary to expose dropped work. Latency among surviving frames alone rewards a recorder that silently drops difficult inputs. Report offered frames, admitted frames, encoded/displayed frames, skipped time, discontinuities and output bytes alongside latency. If an application-specific source supplies only content changes, measure change delivery and display-duration correctness separately from an invented timer rate.

## 16.3 Quality oracles

Separate capture correctness, conversion error and codec loss. Compare captured pixels with an independent intended source where possible; compare GPU conversion against a precise CPU reference with specified transfer, matrix, range, chroma filter and rounding; then decode the encoded stream with an independent AV1 decoder and compare against the conversion reference. Test both luma and chroma. A high score against an already incorrect conversion does not establish correct recording.

Use bitstream inspection for profile, bit depth, range/primaries/transfer, temporal-unit structure, random access, reference/repeat behavior and HDR metadata. Decode long sequences spanning resets, drops and hidden/repeated frames. Use independent decoders where practical and validate real intended players/editors, including seeks, VFR, fine timestamps and incomplete-file recovery. A codec library accepting a packet is not the same as every consumer displaying it correctly.

PSNR/SSIM, SSIMULACRA2 and VMAF can reveal different failures; none alone establishes desktop fidelity. Pin the exact metric version/model and its domain. Netflix introduced v1 models in 2026, so 'VMAF' without a model identifier is not a stable experiment. Use an applicable no-enhancement-gain mode when evaluating codec distortion and report limitations on text, chroma and HDR. Inspect moving text, saturated one-pixel detail, gradients and temporal filtering subjectively at the intended display scale. [C22, C23]

Report a rate-distortion-resource surface rather than one magic score: output bitrate or file size, objective and task-specific quality, source-to-packet delay, frame drops, CPU/energy and game interference. If preferences do not choose between nondominated points, preserve them as modes instead of inventing weights.

## 16.4 Instrument the full system

Count recorder threads, compositor/PipeWire work, kernel workers, polling threads, interrupts and filesystem writeback. Track GPU graphics/compute, video, copy and memory activity; source/encoder pool occupancy; VRAM and host/page-cache/pinned memory; power; and the recorded application's frame times. Report absolute cost plus change from the fixed reference. Percentage utilization across different engines or CPUs is not directly comparable work.

Use GPU timestamp queries where applicable and calibrated timestamps to map device and host time, including reported maximum deviation. Vulkan provides a calibration API; do not subtract timestamps from unrelated clocks as though they shared an epoch. Instrumentation itself can add barriers, query traffic and wakeups, so use a low-overhead measurement configuration and quantify perturbation with and without it. [C24]

Report distributions of complete per-frame paths: p50, p95, p99, p99.9 where sample size supports it, and maxima/stall events with run context. Do not add independently measured stage p99 values and call the sum end-to-end p99. Long tails matter to both game stutter and recorder freshness. For actual display-to-display latency, use an appropriate external optical or camera measurement; software timestamps alone do not establish photon timing.

## 16.5 Fair experiments and fresh confirmation

Separate exploratory tuning from confirmation. Randomize or alternate complete candidates under matched thermal state, clocks/power policy, content, driver, CPU budget and background workload. Repeat runs and report variation, confidence intervals where justified, and sample counts. Include challenger-favorable regimes and boundaries, not only the workload that favored the first prototype.

Once a configuration is selected, confirm on fresh traces and at least the configurations whose mechanisms differ. Keep the original control and cumulative nonregression limits fixed while applying multiple optimizations. An improvement relative to the immediately preceding version can conceal a large regression from the original. Rerun checks affected by a change rather than broadening tests without a concrete risk.

Production acceptance requires actual hardware from the three vendors and representative compositor implementations. The specific device/driver matrix should be selected from deployment needs, including an older AV1-capable family and the newer media architecture where its path differs. Record kernel, firmware, compositor, PipeWire, portal, Mesa/proprietary driver, SDK, FFmpeg and metric versions. A result on one Ada or VCN configuration is not a universal family guarantee.

## 16.6 Failure and concurrency validation

Exercise paused participants at every ownership boundary; delayed GPU completion; output arrival without input release; input release without a matching packet; format change with old jobs in flight; buffer removal between a worker request and wrapper return; failed release-signal export after accepted GPU work; and device loss. Confirm no new generation can be validated by an old completion.

Inject queue-full, pool exhaustion, allocation/registration denial, cursor-only updates, missing damage history, timestamp discontinuity, audio underrun/overrun, clock drift, suspended sources, portal revocation, sink stalls, short writes across every vector boundary, out-of-order CQEs, cancellation races, final-patch failures, ENOSPC/EIO and crash truncation at structural boundaries. Verify the declared response and retained media, not merely absence of a process crash.

Use GenMC for native publication/reuse/notification, supplemented by ownership assertions, stress tests and supported sanitizers. These finite Python models do not prove C23 weak-memory or GPU correctness, nor whole-recorder wait-freedom. [C25]

# 17. Checks performed for this design

## 17.1 Executed, reproducible checks

Three small Python programs are included with the companion bundle. They do not implement the recorder. Their purpose is to challenge consequential design invariants with independent small oracles and deliberately broken alternatives.

The concurrency checker explored finite abstract schedules for event notification and pooled GPU-resource lifetime. Correct variants had no violation in the modeled state space: unconditional notification visited 37 states, armed-RMW notification 93, and retirement after GPU completion 286. Broken variants produced concrete witnesses for a lost wakeup, reuse while the GPU still reads, and destruction before GPU retirement. The queue chapter records transitions and scope. These results support those abstract rules only.

The damage/arithmetic checker used a deterministic full-image oracle against incremental conversion into three rotating destinations. Across 20,000 source updates, including 5,021 skipped submissions, it checked 14,979 submitted results. Revision-aware accumulated damage matched full recomputation. A deliberately broken latest-damage-only method produced a wrong image. The model is a scalar 4 by 4 input with a 2 by 2 box-filter output; it does not validate a real shader, chroma filter, GPU fence or driver modifier.

The same program verified tight-layout image sizes, the stated 4K240 traffic arithmetic, encoded-rate conversions, lookahead bounds and Matroska relative-time spans. It recorded the source-derived range constants and checked P010 bit placement; it did not implement an RGB-to-YCbCr endpoint oracle. It checked calculated day-end timestamps for 60, 240, 60000/1001 and 240000/1001 fps, rather than enumerating every timestamp. Absolute rational rounding at those probes stayed within half a nanosecond, consistent with the general nearest-integer rounding bound; repeated rounded-period accumulation drifted. It also demonstrated that distinct chroma arrangements can map to the same 4:2:0 average.

The output checker independently compared an interval-based completed-prefix frontier with a completed-byte-set oracle. It enumerated every positive completion partition/interleaving for two small write layouts, producing 748 complete traces and 1,833 checked prefixes. The frontier matched throughout; a broken highest-completed-end rule failed at 834 prefixes. Independent composition/multinomial counts matched the enumeration. A separate five-trace cancellation model retained payloads until target completion; recycling on a cancel CQE violated the conservative contract in two traces, including one modeled access after recycle following an EALREADY result. These are finite abstract checks of nonoverlapping writes and limited cancellation orders, not kernel execution. They do not cover zero/negative writes, fsync, container recovery, identifier reuse, lost CQEs or memory ordering.

## 17.2 What remains unestablished

No GPU capture, encoder, native interop, shader, storage throughput or player-compatibility benchmark was run. No production C23 queue or recorder was built. The finite models do not prove weak-memory correctness, unbounded execution, release-fence interoperability, runtime callbacks or whole-system progress. The proposed FFmpeg Matroska change was not implemented.

The report's primary-source evidence establishes eligibility and constraints. Its architecture and functional defaults are proposed. Its arithmetic is derived and checked. Its performance rankings remain open until the complete candidate comparisons run on hardware. No optional experiment was abandoned because a prototype was already 'good enough'; this environment lacks the device access needed for those experiments.

# 18. Decision ledger and implementation order

## 18.1 Consequential decisions and reopening conditions

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

## 18.2 Implementation sequence with usable checkpoints

**Checkpoint 1: observable complete control.** Implement authority and source selection, immutable generations, persistent imports, a correct conversion, one native encoder path, timing/audio, mature muxing and buffered output. Include effective-configuration diagnostics, clean stop and failure preservation. Independently decode its output and compare pixels/timestamps before treating it as a performance control.

**Checkpoint 2: lifetime and cross-vendor coverage.** Add AMD, NVIDIA and Intel native backends with input/DPB/output retirement contracts, exact source synchronization and reconfiguration. Preserve original control output semantics. Validate each backend's bit depth, color, format, rate control and unsupported-path behavior on real devices.

**Checkpoint 3: representation challengers.** Implement eligible AMD direct RGB, NVIDIA native arrays and strong Intel VPP/VPL/VA alternatives. Complete the import/export and failure paths. Compare source retention, conversion accuracy, device memory traffic and application interference. Add revision-aware partial conversion and native repeated-display candidates only with independent reconstruction/decode oracles.

**Checkpoint 4: output and temporal polish.** Compare integrated ordinary io_uring with the buffered control, audit asynchronous AVIO ownership, test fine timestamp containers and implement the chosen small muxer patch if required. Add replay/export with pinned immutable chunks and explicit random-access boundaries. Verify finalization and recovery under injected failures.

**Checkpoint 5: evidence-selected release candidate.** Tune affordable encoder analysis and concurrency, confirm on fresh workloads, verify cumulative nonregression against the original control, and publish exact supported configurations and material limits. Ship modes for genuine Pareto tradeoffs. Keep a reliable fallback for a driver update or source modifier that invalidates a specialized path.

The next implementation decision should be made with the first actual target machines and intended consumers. That information can select preset values and budgets; it does not require redesigning the core contracts established here. The strongest justified deliverable at this stage is this concrete architecture, its preserved challengers, explicit invariants and reproducible design checks, ready to turn into a measured C23 implementation.

# Primary sources and verification artifacts

- [C01] freedesktop.org, ScreenCast portal interface, current version 6. https://flatpak.github.io/xdg-desktop-portal/docs/doc-org.freedesktop.portal.ScreenCast.html

- [C02] PipeWire, 1.4 DMA-BUF sharing and explicit synchronization. https://docs.pipewire.org/1.4/page_dma_buf.html

- [C03] ext-image-copy-capture-v1 protocol XML rendered by Wayland Explorer; upstream freedesktop protocol links are provided there. https://wayland.app/protocols/ext-image-copy-capture-v1

- [C04] Linux kernel 6.12 DRM UAPI, including GETFB2 permissions and handle lifetime. https://www.kernel.org/doc/html/v6.12/gpu/drm-uapi.html

- [C05] GPU Screen Recorder, primary project documentation and implementation entry point. https://git.dec05eba.com/gpu-screen-recorder/about/

- [C06] PipeWire NEWS, 1.6 release and device-ID/metadata capability development history; current development DMA-BUF contract. https://github.com/PipeWire/pipewire/blob/master/NEWS ; https://docs.pipewire.org/devel/page_dma_buf.html

- [C07] PipeWire, raw video format structure. https://docs.pipewire.org/structspa__video__info__raw.html

- [C08] PipeWire, frame metadata header and buffer metadata types. https://docs.pipewire.org/structspa__meta__header.html ; https://docs.pipewire.org/group__spa__buffer.html

- [C09] PipeWire 1.6.9 metadata definitions and synchronization timeline. https://docs.pipewire.org/meta_8h_source.html ; https://docs.pipewire.org/structspa__meta__sync__timeline.html

- [C10] PipeWire synchronization examples; illustrative eventfd stand-ins must not be confused with DRM timeline APIs. https://docs.pipewire.org/video-src-sync_8c-example.html ; https://docs.pipewire.org/video-play-sync_8c-example.html

- [C11] PipeWire stream lifecycle, event threads and processing contract. https://docs.pipewire.org/page_streams.html ; https://docs.pipewire.org/structpw__stream__events.html ; https://docs.pipewire.org/group__pw__stream.html

- [C12] Linux kernel, DMA-BUF sharing, sync-file export/import and CPU-access synchronization. https://docs.kernel.org/driver-api/dma-buf.html

- [C13] Khronos, Vulkan synchronization and external-resource ownership. https://docs.vulkan.org/spec/latest/chapters/synchronization.html

- [C14] KDE KWin, color-management and capture behavior. https://github.com/KDE/kwin/blob/master/doc/color-management.md

- [C15] Sunshine primary PipeWire implementation, version 2026.914.233613, including HDR classification and metadata fallback. https://docs.lizardbyte.dev/projects/sunshine/v2026.914.233613/pipewire_8cpp_source.html

- [C16] ITU-R, Recommendation BT.709-6, normative color components and coding. https://www.itu.int/dms_pubrec/itu-r/rec/bt/R-REC-BT.709-6-201506-I%21%21PDF-E.pdf

- [C17] Microsoft, 10-bit and 16-bit YUV video formats, including P010 layout. https://learn.microsoft.com/en-us/windows/win32/medfound/10-bit-and-16-bit-yuv-video-formats

- [C18] PipeWire cursor metadata structure. https://docs.pipewire.org/structspa__meta__cursor.html

- [C19] AOMedia, AV1 bitstream and decoding specification. https://aomediacodec.github.io/av1-spec/ ; https://aomediacodec.github.io/av1-spec/av1-spec.pdf

- [C20] Xiph.Org, Opus encoder controls, API 1.6. https://opus-codec.org/docs/opus_api-1.6/group__opus__encoderctls.html

- [C21] WG14, C23 working draft N3096. https://www.open-std.org/jtc1/sc22/wg14/www/docs/n3096.pdf

- [C22] Netflix, VMAF v1 and v0 model documentation and metric scope. https://github.com/Netflix/vmaf/blob/master/resource/doc/models_v1.md ; https://github.com/Netflix/vmaf/blob/master/resource/doc/models_v0.md ; https://github.com/Netflix/vmaf/blob/master/resource/doc/faq.md

- [C23] Cloudinary, SSIMULACRA2 primary implementation and methodology. https://github.com/cloudinary/ssimulacra2/blob/main/README.md

- [C24] Khronos, calibrated Vulkan timestamps and maximum-deviation contract. https://docs.vulkan.org/refpages/latest/refpages/source/vkGetCalibratedTimestampsKHR.html

- [C25] MPI-SWS, GenMC model checker; proposed future native weak-memory validation, not executed here. https://github.com/MPI-SWS/genmc

- **[G01]** NVIDIA, *Video Codec SDK 13.1: NVENC Video Encoder API Programming Guide*. https://docs.nvidia.com/video-technologies/video-codec-sdk/13.1/nvenc-video-encoder-api-prog-guide/index.html

- **[G02]** NVIDIA, *Video Codec SDK 13.1: NVENC Application Note*. https://docs.nvidia.com/video-technologies/video-codec-sdk/13.1/nvenc-application-note/index.html

- **[G03]** NVIDIA, *Video Encode and Decode GPU Support Matrix*. https://developer.nvidia.com/video-encode-decode-support-matrix

- **[G04]** NVIDIA, *Video Codec SDK 13.1 Read Me*. https://docs.nvidia.com/video-technologies/video-codec-sdk/13.1/read-me/index.html

- **[G05]** NVIDIA, *CUDA Driver API 13.4: External Resource Interoperability*. https://docs.nvidia.com/cuda/cuda-driver-api/cuda_driver_api/group__CUDA__EXTRES__INTEROP.html

- **[G06]** NVIDIA, *CUDA Driver API 13.4: Memory Management*, including `cuArrayGetPlane`. https://docs.nvidia.com/cuda/cuda-driver-api/cuda_driver_api/group__CUDA__MEM.html

- **[G07]** FFmpeg, current CUDA-array construction and NVENC registration source. https://ffmpeg.org/doxygen/trunk/nvdec_8c_source.html and https://ffmpeg.org/doxygen/trunk/nvenc_8c_source.html

- **[G08]** NVIDIA-authored NVENC 13.1 public header, distributed by FFmpeg. https://github.com/FFmpeg/nv-codec-headers/blob/master/include/ffnvcodec/nvEncodeAPI.h

- **[G09]** AMD, *AMF GPU and APU Hardware Features and Support*. https://github.com/GPUOpen-LibrariesAndSDKs/AMF/wiki/GPU-and-APU-HW-Features-and-Support

- **[G10]** Khronos, current Vulkan specification, *Video Coding*, including AV1 and RGB conversion. https://docs.vulkan.org/spec/latest/chapters/videocoding.html

- **[G11]** AMD, AMF release history and Linux runtime instructions. https://github.com/GPUOpen-LibrariesAndSDKs/AMF/blob/master/amf/doc/README.md and https://github.com/GPUOpen-LibrariesAndSDKs/AMF/wiki/Driver-Linux

- **[G12]** AMD, *AMF AV1 Encoder API*, *Video Converter API* and Vulkan interoperability header. https://github.com/GPUOpen-LibrariesAndSDKs/AMF/blob/master/amf/doc/AMF_Video_Encode_AV1_API.md ; https://github.com/GPUOpen-LibrariesAndSDKs/AMF/blob/master/amf/doc/AMF_Video_Converter_API.md ; https://github.com/GPUOpen-LibrariesAndSDKs/AMF/blob/master/amf/public/include/core/VulkanAMF.h

- **[G13]** Mesa, *25.2.0 Release Notes*. https://docs.mesa3d.org/relnotes/25.2.0.html

- **[G14]** Mesa, *26.0.0 Release Notes*. https://docs.mesa3d.org/relnotes/26.0.0.html

- **[G15]** Mesa, *26.2.4 Release Notes*. https://docs.mesa3d.org/relnotes/26.2.4.html

- **[G16]** Mesa source, commit `56175d9536156502510ae41b2a10f6d0da360b0f` (19 October 2026), RADV video/encode and ANV device capabilities; ANV release tag 26.2.5 also inspected. https://gitlab.freedesktop.org/mesa/mesa/-/blob/56175d9536156502510ae41b2a10f6d0da360b0f/src/amd/vulkan/radv_video.c ; https://gitlab.freedesktop.org/mesa/mesa/-/blob/56175d9536156502510ae41b2a10f6d0da360b0f/src/amd/vulkan/radv_video_enc.c ; https://gitlab.freedesktop.org/mesa/mesa/-/blob/56175d9536156502510ae41b2a10f6d0da360b0f/src/intel/vulkan/anv_physical_device.c ; https://gitlab.freedesktop.org/mesa/mesa/-/blob/mesa-26.2.5/src/intel/vulkan/anv_physical_device.c . Primary authored files were read through the accessible https://github.com/chaotic-cx/mesa-mirror mirror because direct GitLab extraction was blocked.

- **[G17]** Intel, media-driver README: entrypoints, acceleration, firmware and build capabilities. https://github.com/intel/media-driver/blob/master/README.md

- **[G18]** Intel, media-driver feature matrix and 2026 Q3 release. https://github.com/intel/media-driver/blob/master/docs/media_features.md and https://github.com/intel/media-driver/releases/tag/intel-media-26.3.5

- **[G19]** Intel VPL, *VideoENCODE API*. https://intel.github.io/libvpl/latest/API_ref/VPL_func_vid_encode.html

- **[G20]** Intel libva, current core API reference. https://intel.github.io/libva/group__api__core.html

- **[G21]** Intel VPL, *Encode Structures* and change log. https://intel.github.io/libvpl/latest/API_ref/VPL_structs_encode.html and https://intel.github.io/libvpl/latest/VPL_change_log.html

- **[G22]** Intel VPL, shared-surface guide and experimental API status. https://intel.github.io/libvpl/latest/programming_guide/VPL_prg_surface_sharing.html and https://intel.github.io/libvpl/latest/Experimental.html

- **[G23]** Khronos, Vulkan specification, *Additional Capabilities*, including external-memory handle compatibility. https://docs.vulkan.org/spec/latest/chapters/capabilities.html

- **[G24]** Intel, *Arc B-Series Graphics Launch*: twin multi-format engines. https://www.intel.com/content/www/us/en/newsroom/news/intel-launches-arc-b-series-graphics-cards.html

- **[G25]** NVIDIA, *CUDA Driver API: OpenGL Interoperability* and graphics interoperability guide. https://docs.nvidia.com/cuda/cuda-driver-api/cuda_driver_api/group__CUDA__GL.html and https://docs.nvidia.com/cuda/cuda-programming-guide/04-special-topics/graphics-interop.html

- **[G26]** AMD, *AMF API Reference*, surface observers and component input/output/drain contracts. https://github.com/GPUOpen-LibrariesAndSDKs/AMF/blob/master/amf/doc/AMF_API_Reference.md

- **[G27]** AMD, *AMF Video PreAnalysis API*. https://github.com/GPUOpen-LibrariesAndSDKs/AMF/blob/master/amf/doc/AMF_Video_PreAnalysis_API.md

- **[G28]** NVIDIA, *Vulkan Driver Support*, including AV1 and 2026 interoperability fixes. https://developer.nvidia.com/vulkan-driver

- **[Q01]** WG14, C23 working draft N3096: https://www.open-std.org/jtc1/sc22/wg14/www/docs/n3096.pdf

- **[Q02]** Linux kernel, Circular Buffers: https://docs.kernel.org/core-api/circular-buffers.html

- **[Q03]** Linux man-pages, eventfd(2): https://man7.org/linux/man-pages/man2/eventfd.2.html

- **[Q04]** Linux man-pages, FUTEX_WAIT: https://man7.org/linux/man-pages/man2/FUTEX_WAIT.2const.html

- **[Q05]** Morrison and Afek, LCRQ, PPoPP 2013: https://www.cs.tau.ac.il/~mad/publications/ppopp2013-x86queues.pdf

- **[Q06]** Ramalhete/Correia, FAAArrayQueue implementation: https://github.com/pramalhe/ConcurrencyFreaks/blob/master/CPP/queues/array/FAAArrayQueue.hpp

- **[Q07]** Ramalhete, producer/consumer counterexample workload: https://concurrencyfreaks.blogspot.com/2016/12/lcrq-and-faaarrayqueue-are-not-good-for.html

- **[Q08]** Nikolaev, SCQ, DISC 2019: https://rusnikola.github.io/files/ringpaper-disc.pdf

- **[Q09]** Nikolaev and Ravindran, wCQ, SPAA 2022: https://arxiv.org/pdf/2201.02179

- **[Q10]** Romanov and Koval, LPRQ, PPoPP 2023: https://nikitakoval.org/publications/ppopp23-lprq.pdf

- **[Q11]** Ramalhete/Correia, CRTurn implementation: https://github.com/pramalhe/ConcurrencyFreaks/blob/master/CPP/papers/crturnqueue/include/CRTurnQueue.hpp

- **[Q12]** Denis and Goedefroit, NBLFQ, IPDPS 2025; metadata inspected, full text unavailable: https://inria.hal.science/hal-04851700

- **[Q13]** PipeWire 1.4, DMA-BUF Sharing: https://docs.pipewire.org/1.4/page_dma_buf.html

- **[Q14]** PipeWire 1.6 metadata definitions: https://docs.pipewire.org/meta_8h.html

- **[Q15]** PipeWire sync source example: https://docs.pipewire.org/video-src-sync_8c-example.html

- **[Q16]** PipeWire sync consumer example: https://docs.pipewire.org/video-play-sync_8c-example.html

- **[Q17]** PipeWire stream-event contract: https://docs.pipewire.org/structpw__stream__events.html

- **[Q18]** Executed companion artifacts: `recorder_concurrency_model_check.py` and `recorder_concurrency_model_results.json`.

- [IO01] FFmpeg, libavformat API: https://ffmpeg.org/doxygen/trunk/avformat_8h_source.html

- [IO02] FFmpeg, AVIO implementation: https://ffmpeg.org/doxygen/trunk/aviobuf_8c_source.html

- [IO03] IETF, RFC 9559, Matroska: https://datatracker.ietf.org/doc/html/rfc9559

- [IO04] FFmpeg, Matroska muxer: https://ffmpeg.org/doxygen/trunk/matroskaenc_8c_source.html

- [IO05] Matroska codec mappings: https://www.matroska.org/technical/codec_specs.html

- [IO06] AOMedia, AV1 ISO BMFF mapping: https://aomediacodec.github.io/av1-isobmff/

- [IO07] FFmpeg format options: https://ffmpeg.org/ffmpeg-formats.html

- [IO08] Matroska streaming: https://www.matroska.org/technical/streaming.html

- [IO09] io_uring_setup(2): https://man7.org/linux/man-pages/man2/io_uring_setup.2.html

- [IO10] Linux, io_uring implementation before the 6.13 merge: https://git.zx2c4.com/linux-rng/tree/io_uring/io_uring.c?id=77a0cfafa9af9c0d5b43534eb90d530c189edca1

- [IO11] liburing queue initialization: https://man7.org/linux/man-pages/man3/io_uring_queue_init_mem.3.html

- [IO12] liburing buffer registration: https://man7.org/linux/man-pages/man3/io_uring_register_buffers.3.html

- [IO13] liburing file registration: https://man7.org/linux/man-pages/man3/io_uring_register_files.3.html

- [IO14] liburing ring-fd registration: https://man7.org/linux/man-pages/man3/io_uring_register_ring_fd.3.html

- [IO15] io_uring_register(2): https://man7.org/linux/man-pages/man2/io_uring_register.2.html

- [IO16] liburing minimum completion wait: https://man7.org/linux/man-pages/man3/io_uring_submit_and_wait_min_timeout.3.html

- [IO17] liburing wait clock: https://man7.org/linux/man-pages/man3/io_uring_register_clock.3.html

- [IO18] liburing SQE flags: https://man7.org/linux/man-pages/man3/io_uring_sqe_set_flags.3.html

- [IO19] liburing worker limits: https://man7.org/linux/man-pages/man3/io_uring_register_iowq_max_workers.3.html

- [IO20] Linux, SQPOLL implementation: https://git.zx2c4.com/linux-rng/tree/io_uring/sqpoll.c?id=77a0cfafa9af9c0d5b43534eb90d530c189edca1

- [IO21] io_uring_enter(2): https://man7.org/linux/man-pages/man2/io_uring_enter.2.html

- [IO22] liburing submission: https://man7.org/linux/man-pages/man3/io_uring_submit.3.html

- [IO23] write(2): https://man7.org/linux/man-pages/man2/write.2.html

- [IO24] liburing cancellation: https://man7.org/linux/man-pages/man3/io_uring_prep_cancel.3.html

- [IO25] fsync(2): https://man7.org/linux/man-pages/man2/fsync.2.html

- [IO26] open(2), direct-I/O semantics: https://man7.org/linux/man-pages/man2/open.2.html

- [IO27] statx(2): https://man7.org/linux/man-pages/man2/statx.2.html

- [IO28] fallocate(2): https://man7.org/linux/man-pages/man2/fallocate.2.html

- [IO29] posix_fadvise(2): https://man7.org/linux/man-pages/man2/posix_fadvise.2.html

- [IO30] sync_file_range(2): https://man7.org/linux/man-pages/man2/sync_file_range.2.html

- [IO31] Linux 6.13 io_uring merge: https://git.zx2c4.com/linux-rng/commit/?id=8350142a4b4cedebfa76cd4cc6e5a7ba6a330629

- [IO32] pwritev2(2): https://man7.org/linux/man-pages/man2/pwritev2.2.html

- [IO33] liburing NO_IOWAIT: https://man7.org/linux/man-pages/man3/io_uring_set_iowait.3.html

- [IO34] FDP write-stream mechanism: https://lists.infradead.org/pipermail/linux-nvme/2025-May/056063.html

- [IO35] Linux 6.16 block merge: https://git.zx2c4.com/wireguard-linux/commit/io_uring/io_uring.c?id=6f59de9bc0d576eb5a5edfea470527902315e924

- [IO36] Linux kernel io_uring sysctls: https://docs.kernel.org/admin-guide/sysctl/kernel.html

- [IO37] Primary research, io_uring for High-Performance DBMSs: When and How to Use It: https://arxiv.org/html/2512.04859v1

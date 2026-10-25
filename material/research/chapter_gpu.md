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

The current AV1 ROI input is a GRAY32 importance map, with values 0–10 per 64 × 64 block. It is not NVIDIA's signed delta-Q map. Current AMF QIndex fields start at 1, so a generic “QP 0 means lossless” control would be misleading. SmartAccess Video is a supported-system job-distribution feature; it does not prove that one AV1 frame uses every VCN instance. [G09, G12]

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

## GPU chapter sources

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

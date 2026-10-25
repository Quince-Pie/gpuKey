# 17. Packets, containers, replay and output

## Encoded bytes are the storage boundary

The output architecture should preserve encoded media, timing, codec configuration and decoding dependencies while releasing capture and encoder resources promptly. It should not carry framebuffer allocations into an ordinary file-writing abstraction. io_uring can manage supported file operations, readiness and timers; it does not replace PipeWire buffer negotiation, GPU command submission, encoder APIs or DMA-BUF synchronization. A DMA-BUF fd is not a readable encoded bytestream merely because both objects have file descriptors.

The scale difference matters. A 3840 × 2160 × four-byte surface at 120 frames/s represents 3.981 GB/s, whereas an illustrative 200 Mbit/s encoded stream represents 25 MB/s before audio and container overhead. These are verified arithmetic examples, not bitrate or resolution limits. Consequently, retaining an expensive encoder output object to avoid copying relatively few compressed bytes may increase total overhead. Compare retaining the driver buffer until I/O completion against one copy into owned slabs followed by immediate driver release. Measure pool occupancy, encoder stalls, CPU work and latency across both lifetimes.

Use a mature muxer as the production control. Represent accepted data as immutable owned ranges with lengths, offsets and explicit release conditions. Aggregate small headers without requiring one allocation or syscall per element; compare scatter/gather traversal against packing into a contiguous slab. Keep packet ownership separate from write-request ownership so aggregation can change independently of encoding.

A specialized streaming muxer remains a credible stronger candidate if mux/AVIO copies, generic buffering or header bookkeeping become consequential. It can preserve native temporal-unit ownership and emit compact headers plus payload ranges directly to the sink. Evaluate that mechanism against an adapter or small library patch, including initialization, ordering, precise timestamps, duration/priming, indexes, random access, recovery, errors and finalization. Required behavior need not be impossible in the library for a replacement to win; a complete measured improvement is sufficient. No such replacement is implemented or ranked here.

The evidence status throughout this chapter is explicit: API semantics and cited implementation mechanisms have been inspected; the numerical examples have been checked; recorder performance and player interoperability have not been measured. Defaults are proposed candidates, not established universally optimal settings.

## Mux ownership must survive asynchronous output

libavformat offers different ownership and ordering contracts. `av_interleaved_write_frame` can retain packets while establishing decode-timestamp order; `av_write_frame` leaves ordering to its caller. Follow the selected reference-ownership and padding requirements rather than assuming all packet buffers remain valid until a later flush. [IO01]

Custom AVIO introduces a consequential trap. Current FFmpeg `writeout` calls `write_packet`, records a negative result as an error, and otherwise accounts for the entire requested length. A positive short return is not a safe partial-acceptance protocol. An asynchronous adapter must either finish using the supplied bytes before returning or fully accept them into owned storage and return the full length. Enqueuing the temporary pointer and returning success does not extend its lifetime. [IO02]

The adapter therefore needs a persistent asynchronous-error latch, byte admission credits and a final drain. Subsequent callbacks must surface previous failures, and successful enqueue cannot be reported as successful recording finalization. Seek callbacks change the logical cursor; overlapping backward patches still need ordered physical completion. Compare the adapter's copying and bookkeeping costs with ordinary synchronous AVIO on a dedicated output thread. A nominally asynchronous sink that hides those costs is not a fair challenger.

## Exact timing and container selection

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

A small upstreamable libavformat timestamp-scale option is the strongest first challenger to a custom Matroska muxer. It must audit stream time bases, cluster limits, duration, cues, codec delay and seek preroll—not merely replace the EBML header value. Preserve existing defaults and validate fine-scale audio/video timing, seeks, VFR, reordered timestamps and long static holds. This patch is proposed, not implemented.

### fMP4 is the immediately available fine-timing candidate

Where consumers accept fragmented MP4, libavformat exposes a configurable video track timescale and fragment controls. Completed fragments can preserve usable output after interruption where ordinary unfinished end-moov MP4 may fail. Fragment cadence and random-access cadence are separate: independently playable fragments need suitable starting points, but bounded availability does not inherently require a quality-expensive keyframe at every fragment. [IO07]

Choose a timescale from acceptable serialization error, audio relationships, field ranges and consumer support. One million ticks/s is a candidate, not a requirement. Blindly transferring nanoseconds into every container field creates constraints: a 32-bit sample-duration field spans only about 4.295 seconds at one billion ticks/s. Long held frames need an explicit representable policy.

Use fMP4 as the readily available precise-timing candidate and retain Matroska as an equal architectural contender, including the audited scale option when needed. Container selection remains consumer-driven. Matroska can stream with unknown Segment size; a partial file without final Cues may be recoverable without being instantly seekable or durable. Avoid a finalization policy that shifts an hours-long file merely to move Cues to its front. [IO07, IO08]

Interleaving deserves its own latency budget. Bound waiting for delayed or absent audio using decode-time ordering, stream lifecycle events and an explicit lateness policy. In FFmpeg, `max_interleave_delta=0` removes the maximum-difference escape; it can wait indefinitely for a packet from every stream. Packet flushing and smaller positive interleave budgets spend overhead to improve availability. Neither is an automatic best setting. [IO07]

## Replay and admission control

Retain encoded replay chunks rather than raw surfaces. Required memory is retention time times encoded rate, plus burst allowance, metadata and the preceding random-access interval needed for decoding. Average bitrate does not bound instantaneous bytes. Bound retained bytes and time, not just packet count.

RAM chunks or rolling disk segments carry immutable generations, configuration and decoding dependencies. An export snapshot pins those generations; neither its readers nor outstanding writes may observe reused storage. A completed queue publication is not permission to overwrite a chunk still owned elsewhere.

Reflink or `copy_file_range` can reduce bulk copying on supported filesystems but cannot manufacture valid initialization, timestamps or indexes. Export usually remuxes from a preceding independently decodable AV1 point. Frame-exact arbitrary starts require supported preroll/edit semantics or boundary re-encoding. Price that precision separately instead of silently including extra visible frames.

For sustained storage overload, distinguish transient buffering from insufficient long-term service rate. Arbitrary compressed-packet drops can damage reference chains. Prefer an explicitly signaled capture-side skip before encoding where the selected recording mode permits it. Otherwise stop cleanly or restart after a declared discontinuity at a verified fresh random-access point. Do not invent a universal drop policy for a mode requiring every frame.

## Compare complete sink architectures

| Candidate | Mechanism that can win | Costs that must remain in the comparison |
|---|---|---|
| Dedicated buffered `writev`/`pwritev` writer | Simple ownership and byte-granular mux output; absorbs bursts outside capture | Thread handoff, page-cache copy, dirty throttling, writeback tails |
| Ordinary buffered io_uring owner | Overlap without blocking; potentially removes writer handoff | SQ/CQ work, longer payload lifetime, worker fallback, AVIO adapter |
| Direct io_uring with aligned slabs | Avoids page-cache pollution/copy; amortizes repeated pinning | Alignment, aggregation, tail protocol, metadata patches, pinned-memory budget |
| SQPOLL/IOPOLL variants | Reduces submission transitions or completion interrupts in favorable workloads | Polling CPU/energy, restricted operations, device setup, workload interference |

The provisional default is buffered output, with both the simple writer and integrated ordinary io_uring retained until evidence selects between them. High-quality or multistream recording can justify direct I/O; ordinary low-rate desktop recording must remain in the comparison. Count io-wq, polling threads, filesystem writeback, interrupts, energy and game frame-time impact. Moving work out of the recorder's userspace CPU counter is not eliminating it.

## io_uring: Linux 6.12 stage decisions

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

Sources for the table are the syscall API and inspected pre-6.13 kernel implementation, plus registration, sizing, submission, wait and worker interfaces. [IO09–IO19, IO21] The source commit is the parent of the 6.13 io_uring merge, not a claim to have inspected the exact v6.12 release tag. Feature-probe optional paths and pin the eventual library/kernel versions.

Queue dimensions follow the event model: reserve CQ space for all completions that can arrive before the next drain, and separately bound bytes held by outstanding operations. `NODROP` is not permission to overflow routinely; allocation failure can still lose completions and invalidate ordinary reclamation assumptions. [IO11]

Flush on bytes, oldest-packet deadline, checkpoint or stop—whichever comes first. Consuming an available batch amortizes work; waiting to fill one spends latency. No fixed queue depth, slab size or microsecond wait is established as optimal. Registered-buffer cloning exists by 6.12, but adding rings merely to use it creates unnecessary ownership complexity. [IO15, IO16]

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

These boundaries come from upstream merges and APIs. [IO09, IO31–IO35] Newer is not automatically better for sparse compressed writes.

## Completion, failure and durability invariants

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

## Direct I/O and cache pressure

Direct I/O is credible for sustained output or page-cache interference, provided the complete mux lifecycle remains valid. Query STATX_DIOALIGN where supported; compliant address, length and file offsets are filesystem/device-specific. Variable packet sizes, headers, tails and backward patches require aligned aggregation. Arbitrary padding cannot be inserted into container bytes. An aligned physical tail plus later truncation needs an explicit ordered protocol. Avoid overlapping direct and buffered operations; O_DIRECT is not durability. [IO26, IO27]

Rolling `fallocate(KEEP_SIZE)` reservation can move allocation work off the critical path and reveal capacity failure earlier without publishing an oversized file. It cannot guarantee bandwidth. NOREUSE has useful replacement-policy semantics since 6.3; SEQUENTIAL mostly changes read-ahead. DONTNEED does not guarantee eviction of dirty data, and sync_file_range is writeback scheduling rather than a durability primitive. [IO28–IO30]

RWF_ATOMIC, available from 6.11 under specific direct-I/O, alignment and device/filesystem constraints, protects qualifying writes against tearing. It is not a transaction over packets and metadata, nor automatic durability. RWF_DONTCACHE is a stronger later buffered challenger for one-pass output; it can lose when replay immediately reads those bytes. Do not silently change global dirty-page settings, CPU governors or NVMe poll-queue configuration to manufacture a win. [IO32]

## Experiments that can overturn the defaults

The primary study on io_uring in high-performance DBMSs supplies useful mechanisms—integrated asynchronous redesign, polling costs and batching tradeoffs—but its storage-heavy results do not establish recorder performance. [IO37] The decisive recorder experiments are:

1. Retained driver output versus owned compressed slabs, including encoder-resource occupancy and end-to-end tails.
2. Integrated buffered writer versus ordinary io_uring with identical mux bytes, durability, CPU resources and bounded backlog.
3. Direct I/O and DONTCACHE under high output and competing memory pressure; polling under favorable high request rates and ordinary sparse recording, counting all system work.
4. Fine-scale Matroska versus fMP4 across required players, audio gaps, VFR, reordering, long holds, seeks, configuration changes and interrupted finalization.
5. Injected short writes, delayed/out-of-order completions, ENOSPC/EDQUOT/EIO, cancellation races, paused owners, allocation denial and crashes at structural boundaries.

After tuning, confirm on fresh captures and conditions against a fixed control. Report latency distributions, backlog, game frame-time effects, dropped/skipped content, throughput and energy with run-to-run variation. A synthetic disk result cannot substitute for this producer-to-consumer comparison.

## References

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

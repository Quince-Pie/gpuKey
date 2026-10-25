# 15. Ownership, threads and queues

## 15.1 Derive concurrency from the resource graph

The recorder needs overlapping capture, GPU work, encoding and output. It does not necessarily need a separate CPU thread for every stage. GPU submissions already execute asynchronously; adding a thread and queue between two submissions can introduce a wakeup, cache transfer and extra buffering without creating useful parallelism.

The preferred starting architecture is a serialized owner for session state, GPU submissions and completions, surface pools and retirement. Operations dispatched on that owner use ordinary state and direct calls. Introduce a thread boundary where an actual API requires it: a PipeWire realtime callback, a blocking encoder operation, or an output backend that cannot integrate with the owner’s completion loop. Use a bounded SPSC connection for an edge with exactly one producer and consumer. Keep infrequent control commands on a conventional bounded mutex queue.

This is a provisional decomposition, not a restriction on stronger implementations. Compare a complete owner-based control with a version that isolates a demonstrably blocking backend. A separate submission worker can win if it prevents a driver call from delaying capture; it can lose through scheduling and cache overhead. Multiple encoder sessions may warrant separate owners. A central MPMC worker pool becomes relevant only when multiple workers really can execute interchangeable jobs without violating codec, device-context or resource ownership requirements.

Audio and video naturally provide separate ordered packet streams. One mux owner can merge their timestamps. A shared MPMC queue does not supply timestamp ordering, and sorting its output can merely move work downstream. Establish the required order at the consumer before choosing a global queue.

## 15.2 Represent ownership instead of duplicating frame state

The recurring transferable object is a descriptor owning access to a resource, not a copied image. Keep stable format, plane layout, color interpretation and imported GPU objects in a buffer/session record. Per-frame descriptors contain the changing identity, timestamps, metadata snapshot and synchronization points. Imports and file-descriptor duplication belong to negotiated buffer lifetime wherever the API permits; they should not recur just because another frame callback arrived.

A single owner can maintain completion counts and free lists without atomic reference counts on every frame. This removes synchronization around bookkeeping, while preserving the underlying resource references. A queue-cell lifetime, source-image lifetime, encoder input lifetime, reconstruction/reference lifetime and encoded-packet lifetime remain distinct.

An explicitly lossy live mode admits another representation: one pending immutable frame token transferred by atomic exchange. The producer exchanges in a new token and retires only the old pending token returned; the consumer exchanges with null and owns only its returned token. Consumer-held GPU work remains separate. This can remove a backlog of obsolete, unsubmitted frames, but it intentionally changes FIFO semantics and requires spare surfaces for replacement. It is a candidate for freshness, not a replacement for archival capture. A loaded pointer must never be dereferenced before ownership is acquired; a version check cannot retrospectively repair a C data race or GPU use-after-free.

## 15.3 SPSC publication, consumption and reuse

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

## 15.4 Sleeping is part of the handoff

The simple notification control uses `eventfd(EFD_NONBLOCK | EFD_CLOEXEC)`, with a write after each successfully published batch. The consumer drains eventfd, drains its queue, and waits through its normal polling loop. The ring’s acquire/release operations publish memory; eventfd supplies readiness. Handle interruption and errors explicitly. A saturated nonblocking eventfd counter already represents pending readiness. [Q03]

The scheduling argument is straightforward: work published before the consumer drains is consumed; publication that escapes that drain leaves a notification pending or has a producer still responsible for issuing it. An unnecessary wake is harmless. An unsafe optimization is to inspect whether the queue is empty before enqueueing and notify only when that observation was true. The producer can observe an older item, pause while the consumer drains and sleeps, then publish without waking it.

If measured notification cost justifies coalescing, a single-consumer armed protocol is a credible challenger:

1. After draining work, the consumer performs an `acq_rel` exchange setting `armed = true`, then rechecks the queue with acquire semantics.
2. If work exists, it exchanges `armed = false` and resumes draining; otherwise it polls eventfd.
3. After publication, the producer exchanges `armed = false` with `acq_rel`. A returned true value requires an eventfd write.

All changes to the arm state, including cancellation, use the specified exchange protocol. A producer exchange ordered before arming can synchronize with that arm and its recheck; one ordered afterward sees the wake request. Replacing this handshake with relaxed guesses removes that reasoning. Its abstract schedule behavior was checked below; production memory ordering still needs dedicated C weak-memory validation.

Neither notification scheme makes the entire handoff wait-free. A producer paused after publication but before its required wake can delay a sleeping consumer; its combined operation has not completed. Kernel notification and thread scheduling add their own blocking behavior. Likewise, a driver wait or allocator call does not inherit a queue’s progress guarantee.

A dedicated worker may instead use a futex condition/sequence protocol. `FUTEX_WAIT` atomically checks the expected word and enters sleep; the caller still loops around its real condition and handles interruption and spurious returns. Its aligned 32-bit ABI is Linux-specific. Compare complete waking and idle behavior against eventfd’s ability to share the existing poll loop. [Q04]

## 15.5 Strong MPMC alternatives, if the topology requires them

A fair comparison includes descriptor access, pools, notification, allocation, reclamation and shutdown. None of the following algorithms is selected merely because its name appears in high-throughput literature.

The control for a real MPSC/MPMC boundary is a preallocated bounded mutex queue with the same full/empty policy and wakeup behavior. Its lock can block behind a paused holder, but short uncontended critical sections may beat a more elaborate algorithm at recorder traffic rates. Compare both CPU cost and consequential capture-delay tails. A blocking enqueue and an immediate `WOULD_BLOCK` operation have different semantics; match them before timing.

| Candidate | Decisive mechanism | Consequence for this recorder |
|---|---|---|
| LCRQ | FAA reservations and cyclic cell reuse; closes problematic rings and links successors | Closing/linking and safe node reclamation are essential. An extracted single CRQ does not retain the complete progress argument. Original wide-CAS and lifetime costs belong in the comparison. [Q05] |
| FAAArrayQueue | Noncyclic array cells, FAA assignment and a prefilled newly linked node | Portable cell operations simplify the structure, but cells are not recycled within the array; allocation and reclamation recur. [Q06] |
| SCQ | Payload indirection, allocated/free index queues, epoch/safe state and a threshold preventing endless invalidation | Its doubled ring capacity, `3n−1` threshold, at-most-`n` participating threads and original nonwrapping-counter assumption support the proof. Bounded queue storage does not eliminate payload lifetime. [Q08] |
| wCQ | SCQ fast path plus bounded-retry transition to cooperative helping | Per-thread requests, phase and completion state are substantive. The main design uses wide CAS; complete allocation/registration/wakeup paths need separate guarantees. [Q09] |
| LPRQ | Changed epoch transitions; token reservation, epoch promotion and publication replace original CAS2 use | A serious portable cyclic-ring challenger. The paper's standalone PRQ is obstruction-free; closure/linking restores the complete design's progress. Simply splitting two-word CAS operations is incorrect. Reclamation still matters. [Q10] |
| CRTurn | Published requests, turn-based helping and associated reclamation | Thread-registration lifetime and allocator behavior must be included. The primary C++ enqueue allocates a node, so its abstract progress does not make that allocator bounded-step. [Q11] |

Ramalhete’s follow-up provides contrary workload evidence: many dedicated consumers can invalidate cells before a producer commits in LCRQ/FAAArrayQueue, forcing retries. Include producer/consumer imbalance, idle/wake cycles, near-empty traffic, bursts and paused participants. Enqueue/dequeue pairs on a dedicated, pinned benchmark machine cannot establish superiority for this recorder. [Q07]

NBLFQ, published at IPDPS 2025, specifically addresses low-contention bounded MPMC queues. Its primary indexed metadata is relevant, but full text was inaccessible during this review. Its decisive mechanism was not inspected and it remains unranked. Obtain that evidence if a genuine MPMC decision becomes consequential; missing access is not evidence of inferiority. [Q12]

FAA reservation alone is not a progress proof. A paused producer can reserve the next FIFO position while another publishes later work. A consumer that must await the first producer can stall despite available later items. The named designs matter because they address such cases with specific mechanisms. Keeping only their normal path does not preserve their guarantees.

## 15.6 Resource retirement, drop and shutdown

GPU access ends at the backend’s actual completion condition, not at CPU descriptor dequeue. Conversion input, direct encoder input, reconstruction/DPB state and encoded packet storage may retire at different times. CPU atomics publish synchronization handles; they neither signal GPU fences nor perform external-memory visibility or ownership transitions.

The producer may reject its own unpublished input. It must not advance an SPSC consumer index to discard older data that the consumer may already be reading. The consumer can discard unsubmitted inputs under an explicit freshness policy. Submitted GPU work retains resources until completion even when its output is unwanted. Arbitrarily discarding encoded AV1 packets can invalidate later reference-dependent frames.

Stop admission, wake owners, settle reservations, drain or cancel unsubmitted inputs according to policy, flush delayed encoder output, finish asynchronous sink accesses and finalize the container. Retire resources exactly once from authoritative completion paths. Do not hold a lock while joining a thread that needs it to finish.

No finite-memory design guarantees continuous no-drop capture and bounded latency across an arbitrary sink stall. Backlog grows while the sink provides no service. Budget exhaustion requires a defined admission, drop or stop response. Raw surfaces, encoder references, encoded bytes and write-inflight storage need separate budgets. Average occupancy estimates are not worst-case latency proofs.

## 15.7 PipeWire: early wrapper return with explicit synchronization

Returning `pw_buffer` and releasing GPU access to its backing allocation are separate events. The official PipeWire 1.4 DMA-BUF contract prohibits producer reuse before the release point signals and allows acquire waits and release signals in hardware. This permits an aggressive path when the actual negotiated producer/backend contract supports it. [Q13]

Copy per-frame metadata, secure independently owned imports and synchronization references, and successfully arrange acquire wait → all source reads → release signal. Publish the release promise before returning the wrapper. With negotiated `SPA_META_FEATURE_SYNC_TIMELINE_RELEASE`, the producer sets `SPA_META_SYNC_TIMELINE_UNSCHEDULED_RELEASE`; the consumer clears it when promising the signal. Verify actual negotiated features, not just library version or metadata presence. A failed submission must not leave an impossible promise. [Q14]

For an unused frame with that feature and no access/promise already made, leave the producer-set flag intact. Legacy explicit sync instead requires fulfilling its release obligation. Dropping output after submission does not erase the source-read obligation. A direct RGB encoder path must release after actual input consumption; a conversion-only path can release after its reads finish.

Once returned, do not access the wrapper, metadata, chunks or borrowed FD numbers. Persistent imports or duplicated descriptors preserve allocation references, not old pixel contents; synchronization protects the contents. On `remove_buffer`, detach the wrapper, prevent new submissions for that generation and defer destruction of owned GPU objects until in-flight references retire. Late completion handling must not queue a removed wrapper. Even the conservative “retain until read-complete” path needs removal handling during teardown.

PipeWire documents ordinary stream events as main-loop callbacks unless specified otherwise. Removal/reconfiguration should mark retirement and return, not wait or spin for GPU completion. Serialize record lifecycle with submission ownership. The illustrative sync examples use eventfds in place of real DRM synchronization objects; their blocking operations and local queue/metadata ordering are not a production asynchronous-GPU template. [Q15, Q16, Q17]

## 15.8 Executed design probes and remaining proof obligations

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

## Sources

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

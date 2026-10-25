#!/usr/bin/env python3
"""Finite sequential models of output accounting, not a kernel/recorder test.

The independent write oracle is a set of successfully completed byte positions.
The candidate tracks interval endpoints instead. Every positive short-completion
partition and interleaving is explored for the two stated layouts. A remainder
is immediately eligible for abstract resubmission; failures and real scheduling
are deliberately outside this model.

Cancellation uses the documented EALREADY case: a cancellation CQE can arrive
while the target can still access its payload. Successful/ENOENT cases use only
the conservative target-terminal-before-cancel observation order. No assertion
about all kernel completion-order possibilities is made.
"""

from __future__ import annotations

import argparse
import itertools
import json
import math
from dataclasses import dataclass, field
from pathlib import Path


@dataclass
class IntervalFrontier:
    watermark: int = 0
    pending: dict[int, int] = field(default_factory=dict)

    def completed(self, start: int, length: int) -> "IntervalFrontier":
        candidate = IntervalFrontier(self.watermark, dict(self.pending))
        assert start not in candidate.pending
        candidate.pending[start] = start + length
        while candidate.watermark in candidate.pending:
            candidate.watermark = candidate.pending.pop(candidate.watermark)
        return candidate


def byte_oracle(completed_bytes: frozenset[int]) -> int:
    first_missing = 0
    while first_missing in completed_bytes:
        first_missing += 1
    return first_missing


def check_write_layout(lengths: tuple[int, ...]) -> dict:
    starts = tuple(sum(lengths[:i]) for i in range(len(lengths)))
    total_length = sum(lengths)
    counts = {
        "prefix_checks_including_root": 0,
        "complete_traces": 0,
        "completion_steps": 0,
        "positive_short_completion_steps": 0,
        "completion_steps_above_a_hole": 0,
        "broken_highest_end_mismatch_prefixes": 0,
    }
    distinct_progress = set()
    shortest_bad = None

    def visit(progress, candidate, known_bytes, bad_watermark, trace):
        nonlocal shortest_bad
        counts["prefix_checks_including_root"] += 1
        distinct_progress.add(progress)
        expected = byte_oracle(known_bytes)
        assert candidate.watermark == expected, (trace, candidate, known_bytes)
        if bad_watermark != expected:
            counts["broken_highest_end_mismatch_prefixes"] += 1
            witness = {
                "events": trace,
                "completed_byte_positions": sorted(known_bytes),
                "oracle_contiguous_W": expected,
                "broken_W": bad_watermark,
            }
            if shortest_bad is None or len(trace) < len(shortest_bad["events"]):
                shortest_bad = witness

        if progress == lengths:
            counts["complete_traces"] += 1
            assert known_bytes == frozenset(range(total_length))
            assert candidate.watermark == total_length
            assert not candidate.pending
            return

        for index, length in enumerate(lengths):
            remaining = length - progress[index]
            for result in range(1, remaining + 1):
                start = starts[index] + progress[index]
                added = frozenset(range(start, start + result))
                assert not added.intersection(known_bytes)
                next_progress = list(progress)
                next_progress[index] += result
                event = {
                    "write": index,
                    "offset": start,
                    "requested_remainder": remaining,
                    "cqe_res": result,
                }
                counts["completion_steps"] += 1
                counts["positive_short_completion_steps"] += result < remaining
                counts["completion_steps_above_a_hole"] += start > expected
                visit(
                    tuple(next_progress),
                    candidate.completed(start, result),
                    known_bytes | added,
                    max(bad_watermark, start + result),
                    trace + [event],
                )

    visit((0,) * len(lengths), IntervalFrontier(), frozenset(), 0, [])
    assert shortest_bad is not None
    # Independent coverage count: compositions of each request's positive
    # completion lengths times multinomial interleavings preserving its order.
    expected_trace_count = 0
    for chunk_counts in itertools.product(*(range(1, n + 1) for n in lengths)):
        partitions = math.prod(math.comb(n - 1, k - 1) for n, k in zip(lengths, chunk_counts))
        interleavings = math.factorial(sum(chunk_counts)) // math.prod(math.factorial(k) for k in chunk_counts)
        expected_trace_count += partitions * interleavings
    assert counts["complete_traces"] == expected_trace_count
    return {
        "write_ranges_half_open": [[s, s + n] for s, n in zip(starts, lengths)],
        **counts,
        "distinct_progress_vectors": len(distinct_progress),
        "independent_composition_multinomial_trace_count": expected_trace_count,
        "candidate_matches_independent_byte_oracle": True,
        "shortest_broken_highest_end_counterexample": shortest_bad,
        "counting_note": "Prefix and edge counts include their different trace histories; progress vectors are deduplicated separately.",
    }


def check_cancel_lifetime() -> dict:
    cases = []
    # EALREADY explicitly permits a still-active target. Its last payload access
    # precedes its terminal CQE; cancel-CQE timing is otherwise unconstrained here.
    for order in itertools.permutations(("cancel_cqe", "target_access", "target_terminal_cqe")):
        if order.index("target_access") < order.index("target_terminal_cqe"):
            cases.append(("EALREADY", order))
    # Conservative success/completed-before-cancel cases avoid depending on an
    # unestablished success-CQE ordering claim.
    for result in ("success", "ENOENT_already_completed"):
        cases.append((result, ("target_access", "target_terminal_cqe", "cancel_cqe")))

    correct_checks = 0
    broken_unsafe_traces = 0
    broken_payload_access_after_recycle_traces = 0
    witness = None
    for result, order in cases:
        target_terminal_observed = False
        correct_recycled = False
        broken_recycled = False
        unsafe = False
        payload_hazard = False
        history = []
        for event in order:
            if event == "target_access":
                assert not correct_recycled
                payload_hazard |= broken_recycled
            if event == "target_terminal_cqe":
                target_terminal_observed = True
                correct_recycled = True
                broken_recycled = True
            elif event == "cancel_cqe":
                broken_recycled = True  # deliberately defective policy
            # Independent conservative payload-lifetime contract: retain the
            # payload until target terminal observation. Failed cancel alone
            # does not establish the end of target use. Identifier reuse with
            # outstanding cancellation requests is outside this model.
            required_live = not target_terminal_observed
            assert not (correct_recycled and required_live)
            unsafe |= broken_recycled and required_live
            correct_checks += 1
            history.append({
                "event": event,
                "target_terminal_observed": target_terminal_observed,
                "correct_recycled": correct_recycled,
                "broken_recycled": broken_recycled,
            })
        assert correct_recycled
        broken_unsafe_traces += unsafe
        broken_payload_access_after_recycle_traces += payload_hazard
        if payload_hazard and witness is None:
            witness = {"cancel_result": result, "events": history}

    assert broken_unsafe_traces > 0 and witness is not None
    return {
        "traces": len(cases),
        "EALREADY_traces": 3,
        "conservative_success_or_ENOENT_traces": 2,
        "event_prefix_checks": correct_checks,
        "correct_policy_satisfies_lifetime_contract": True,
        "broken_recycle_on_cancel_contract_violating_traces": broken_unsafe_traces,
        "broken_payload_access_after_recycle_traces": broken_payload_access_after_recycle_traces,
        "counterexample": witness,
        "primary_contract": "https://man7.org/linux/man-pages/man3/io_uring_prep_cancel.3.html",
    }


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, default=Path(__file__).with_name("output_model_results.json"))
    args = parser.parse_args()
    layouts = [check_write_layout((2, 2)), check_write_layout((2, 3, 2))]
    cancellation = check_cancel_lifetime()
    result = {
        "status": "PASS",
        "scope": "Finite abstract accounting and lifetime models, not an implemented recorder or Linux execution.",
        "write_models": layouts,
        "cancellation_model": cancellation,
        "totals": {
            "write_complete_traces": sum(m["complete_traces"] for m in layouts),
            "write_prefix_checks": sum(m["prefix_checks_including_root"] for m in layouts),
            "write_completion_steps": sum(m["completion_steps"] for m in layouts),
            "cancel_traces": cancellation["traces"],
            "cancel_prefix_checks": cancellation["event_prefix_checks"],
        },
        "limits": [
            "Only two specified contiguous nonoverlapping layouts of four and seven bytes.",
            "All positive completion partitions/interleavings for those layouts; no zero/negative write results, overlap, loss or duplicate CQEs.",
            "Partial remainder is immediately eligible for abstract resubmission; no SQ capacity or kernel scheduling modeled.",
            "Cancellation is separate from byte accounting; the decisive memory-lifetime witness uses documented EALREADY.",
            "No identifier reuse or interaction with cancellation requests still pending after target completion is modeled.",
            "No successful-cancellation CQE reordering guarantee is assumed.",
            "No fsync, filesystem, device durability, mux syntax, weak-memory, C23, GPU, throughput or latency validation.",
        ],
    }
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(result, indent=2) + "\n")
    print(json.dumps({"output": str(args.output), "status": result["status"], **result["totals"]}))


if __name__ == "__main__":
    main()

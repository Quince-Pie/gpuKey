#!/usr/bin/env python3
"""Finite, sequentially consistent abstract schedule checks for recorder design.

This is a design probe, not a C23 weak-memory proof, GPU model, or implementation
test. All transitions below are atomic abstract steps. Exhaustiveness applies
only to these finite state spaces. Broken variants validate the failure oracles.
"""

from __future__ import annotations

from collections import deque
from dataclasses import dataclass, replace
import json


def search(initial, transitions, violation):
    todo = deque([initial])
    previous = {initial: None}
    labels = {}
    edges = 0
    while todo:
        state = todo.popleft()
        reason = violation(state)
        if reason:
            witness = []
            cursor = state
            while previous[cursor] is not None:
                witness.append(labels[cursor])
                cursor = previous[cursor]
            witness.reverse()
            return {
                "result": "COUNTEREXAMPLE",
                "visited_states": len(previous),
                "expanded_transitions": edges,
                "reason": reason,
                "witness": witness,
            }
        for label, nxt in transitions(state):
            edges += 1
            if nxt not in previous:
                previous[nxt] = state
                labels[nxt] = label
                todo.append(nxt)
    return {
        "result": "PASS_FINITE_MODEL",
        "visited_states": len(previous),
        "expanded_transitions": edges,
    }


@dataclass(frozen=True)
class WakeState:
    # One old queued item and one pending producer item make the empty race real.
    q: int = 1
    event: int = 1
    armed: bool = False
    producer: str = "snapshot"
    consumer: str = "read_event"
    should_notify: bool = False


def wake_transitions(variant):
    def step(s):
        p = s.producer
        if p == "snapshot":
            yield "producer observes whether queue was empty", replace(
                s, producer="publish", should_notify=(s.q == 0)
            )
        elif p == "publish":
            next_p = "exchange_arm" if variant == "armed_rmw" else "notify"
            yield "producer publishes new descriptor", replace(
                s, q=s.q + 1, producer=next_p
            )
        elif p == "exchange_arm":
            yield "producer exchanges armed=false", replace(
                s, armed=False, should_notify=s.armed, producer="notify"
            )
        elif p == "notify":
            signal = variant == "unconditional" or s.should_notify
            yield ("producer writes eventfd" if signal else "producer skips wake"), replace(
                s, event=s.event + int(signal), producer="done"
            )

        c = s.consumer
        if c == "read_event":
            yield "consumer drains eventfd", replace(s, event=0, consumer="drain")
        elif c == "drain":
            next_c = "arm" if variant == "armed_rmw" else "wait"
            yield "consumer drains all published descriptors", replace(
                s, q=0, consumer=next_c
            )
        elif c == "arm":
            yield "consumer exchanges armed=true", replace(s, armed=True, consumer="recheck")
        elif c == "recheck":
            if s.q:
                yield "consumer sees work after arming", replace(s, consumer="cancel_arm")
            else:
                yield "consumer sees empty after arming", replace(s, consumer="wait")
        elif c == "cancel_arm":
            yield "consumer exchanges armed=false", replace(s, armed=False, consumer="drain")
        elif c == "wait":
            if s.event:
                yield "poll sees pending eventfd readiness", replace(s, consumer="read_event")
            else:
                yield "poll blocks with no eventfd readiness", replace(s, consumer="blocked")
        elif c == "blocked" and s.event:
            yield "pending eventfd wakes consumer", replace(s, consumer="read_event")

    return step


def wake_violation(s):
    if s.producer == "done" and s.consumer == "blocked" and s.q > 0 and s.event == 0:
        return "A completed producer left queued work with its sole consumer asleep and no wake pending."
    return None


Token = tuple[int, int, int]  # slot index, generation, input frame sequence


@dataclass(frozen=True)
class LifetimeState:
    generations: tuple[int, ...] = (0, 0)
    free: tuple[bool, ...] = (True, True)
    alive: tuple[bool, ...] = (True, True)
    ring: tuple[Token, ...] = ()
    producer: Token | None = None
    consumer: Token | None = None
    gpu: tuple[Token, ...] = ()
    next_frame: int = 0
    stopping: bool = False
    destroyed: bool = False


def set_index(values, index, value):
    return values[:index] + (value,) + values[index + 1 :]


def lifetime_transitions(variant, max_frames=3, ring_capacity=1):
    def step(s):
        if s.destroyed:
            return
        if not s.stopping:
            yield "stop capture admission", replace(s, stopping=True)
        if not s.stopping and s.producer is None and s.next_frame < max_frames:
            for slot, available in enumerate(s.free):
                if available:
                    gen = s.generations[slot] + 1
                    token = (slot, gen, s.next_frame)
                    yield f"producer reserves/writes slot {slot} generation {gen}", replace(
                        s,
                        generations=set_index(s.generations, slot, gen),
                        free=set_index(s.free, slot, False),
                        producer=token,
                        next_frame=s.next_frame + 1,
                    )
        if s.producer is not None:
            slot = s.producer[0]
            if len(s.ring) < ring_capacity and not s.stopping:
                yield "producer publishes its complete descriptor", replace(
                    s, ring=s.ring + (s.producer,), producer=None
                )
            # This owner may cancel its own unpublished input. It never pops
            # or advances the consumer-owned index to drop older inputs.
            if len(s.ring) == ring_capacity or s.stopping:
                yield "producer drops its own unpublished input", replace(
                    s, producer=None, free=set_index(s.free, slot, True)
                )
        if s.ring and s.consumer is None:
            yield "consumer copies/removes descriptor, retaining surface", replace(
                s, ring=s.ring[1:], consumer=s.ring[0]
            )
            if s.stopping:
                token = s.ring[0]
                yield "consumer cancels a queued, unsubmitted input", replace(
                    s, ring=s.ring[1:], free=set_index(s.free, token[0], True)
                )
        if s.consumer is not None:
            token = s.consumer
            nxt_free = s.free
            if variant == "recycle_on_submit":
                nxt_free = set_index(nxt_free, token[0], True)
            yield "consumer submits asynchronous GPU read", replace(
                s, consumer=None, gpu=s.gpu + (token,), free=nxt_free
            )
            if s.stopping:
                yield "consumer cancels its unsubmitted input", replace(
                    s, consumer=None, free=set_index(s.free, token[0], True)
                )
        # No fairness is assumed: a GPU participant may be paused while every
        # other enabled transition executes. Completion order is arbitrary.
        for index, token in enumerate(s.gpu):
            yield f"GPU read completes for slot {token[0]} generation {token[1]}", replace(
                s,
                gpu=s.gpu[:index] + s.gpu[index + 1 :],
                free=set_index(s.free, token[0], True),
            )
        cpu_drained = s.producer is None and s.consumer is None and not s.ring
        gpu_drained = not s.gpu or variant == "destroy_before_gpu_done"
        if s.stopping and cpu_drained and gpu_drained:
            yield "destroy resource pool", replace(
                s, alive=tuple(False for _ in s.alive), destroyed=True
            )

    return step


def lifetime_violation(s):
    # Independent consumer contract: an asynchronous read must continue to see
    # exactly the immutable allocation generation submitted to it, until done.
    outstanding = s.gpu
    for slot, gen, frame in outstanding:
        if not s.alive[slot]:
            return f"GPU frame {frame} still reads slot {slot}, but its backing resource was destroyed."
        if s.generations[slot] != gen:
            return f"GPU frame {frame} expected slot {slot} generation {gen}, but producer reused generation {s.generations[slot]}."
    return None


def main():
    results = {
        "scope": {
            "memory_model": "sequentially consistent atomic abstract transitions",
            "wake_workload": "one pre-existing queued descriptor, one new producer descriptor, one consumer",
            "lifetime_workload": "two pooled surfaces, one queued descriptor, at most three inputs, asynchronous GPU reads, arbitrary stop",
            "not_covered": ["C23 weak memory", "real eventfd ABI", "GPU execution", "real driver lifetime contracts", "unbounded executions", "performance"],
        },
        "notification": {},
        "surface_lifetime": {},
    }
    for variant in ("naive_was_empty", "unconditional", "armed_rmw"):
        results["notification"][variant] = search(
            WakeState(), wake_transitions(variant), wake_violation
        )
    for variant in ("retire_after_gpu_done", "recycle_on_submit", "destroy_before_gpu_done"):
        results["surface_lifetime"][variant] = search(
            LifetimeState(), lifetime_transitions(variant), lifetime_violation
        )
    expected = {
        ("notification", "naive_was_empty"): "COUNTEREXAMPLE",
        ("notification", "unconditional"): "PASS_FINITE_MODEL",
        ("notification", "armed_rmw"): "PASS_FINITE_MODEL",
        ("surface_lifetime", "retire_after_gpu_done"): "PASS_FINITE_MODEL",
        ("surface_lifetime", "recycle_on_submit"): "COUNTEREXAMPLE",
        ("surface_lifetime", "destroy_before_gpu_done"): "COUNTEREXAMPLE",
    }
    for (section, variant), result in expected.items():
        assert results[section][variant]["result"] == result, (section, variant, results)
    print(json.dumps(results, indent=2))


if __name__ == "__main__":
    main()

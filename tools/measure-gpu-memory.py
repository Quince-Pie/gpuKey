#!/usr/bin/env python3
"""Launch a private command and sample Linux DRM fdinfo, without ptrace or stops.

Use inside nix develop. Duplicate file descriptors are deduplicated by device
and DRM client ID. Shared bytes are reported separately, not subtracted from
total physical demand across producer/consumer processes. Sampling cannot
establish an exact allocation high-water mark between observations.
Unavailable process/descriptor reads are counted, including permission changes;
they do not become fabricated zero-usage DRM clients.
CPU and peak RSS are collected after the sole child exits, including its final
reporting/cleanup. They exclude this observer's own CPU and memory.
"""

import argparse
import json
import math
import resource
import re
import signal
import subprocess
import time
from pathlib import Path


def snapshot(pid: int, unavailable: dict[str, dict[str, int]]) -> dict[str, dict[str, int]]:
    clients: dict[str, dict[str, int]] = {}
    def missed(error: OSError) -> None:
        counts = unavailable.setdefault(str(pid), {})
        name = type(error).__name__
        counts[name] = counts.get(name, 0) + 1

    try:
        descriptors = list(Path(f"/proc/{pid}/fdinfo").iterdir())
    except (FileNotFoundError, ProcessLookupError, PermissionError) as error:
        missed(error)
        return clients
    for descriptor in descriptors:
        try:
            fields = dict(
                line.split(":", 1)
                for line in descriptor.read_text(encoding="ascii").splitlines()
                if line.startswith("drm-")
            )
        except (FileNotFoundError, ProcessLookupError, PermissionError) as error:
            missed(error)
            continue
        if "drm-client-id" not in fields:
            continue
        key = f"{fields.get('drm-pdev', '').strip()}:{fields['drm-client-id'].strip()}"
        if key in clients:
            continue
        values: dict[str, int] = {}
        for name, value in fields.items():
            if name.startswith('drm-engine-'):
                number, *unit = value.split()
                if unit == ['ns']:
                    values[name.removeprefix('drm-')] = int(number)
                continue
            if not name.startswith(("drm-total-", "drm-shared-", "drm-resident-", "drm-purgeable-")):
                continue
            number, *unit = value.split()
            scale = {"": 1, "KiB": 1024, "MiB": 1024**2}[unit[0] if unit else ""]
            values[name.removeprefix("drm-")] = int(number) * scale
        clients[key] = values
    return clients


def smaps(pid: int) -> list[dict]:
    """Preserve mappings separately; a shared library can span several VMAs."""
    mappings = []
    fields = {"Size", "Rss", "Pss", "Private_Clean", "Private_Dirty",
              "Shared_Clean", "Shared_Dirty", "Anonymous", "Swap"}
    for line in Path(f"/proc/{pid}/smaps").read_text().splitlines():
        if re.match(r"^[0-9a-f]+-[0-9a-f]+ ", line):
            address, permissions, offset, device, inode, *name = line.split(maxsplit=5)
            mappings.append({"address": address, "permissions": permissions,
                             "offset": offset, "device": device, "inode": inode,
                             "name": name[0] if name else "[anonymous]"})
        elif mappings and ':' in line:
            name, value = line.split(':', 1)
            if name in fields:
                number, unit = value.split()
                if unit != 'kB':
                    raise ValueError(f'unexpected smaps unit: {unit}')
                mappings[-1][name] = int(number) * 1024
    if not mappings:
        raise ValueError('empty smaps response')
    return mappings


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--interval", type=float, default=0.02)
    parser.add_argument("--smaps-after", type=float,
                        help="take one diagnostic mapping/RSS/PSS snapshot after this many seconds")
    parser.add_argument("--stop-after", type=float,
                        help="send SIGINT to this child after this many seconds from launch")
    parser.add_argument("--stop-timeout", type=float, default=10,
                        help="seconds allowed for requested shutdown before killing this child")
    parser.add_argument("--observe", type=int, action="append", default=[],
                        help="also sample this producer PID; keep its counters separate")
    parser.add_argument("command", nargs=argparse.REMAINDER)
    args = parser.parse_args()
    command = args.command[1:] if args.command[:1] == ["--"] else args.command
    if not command or not 0.001 <= args.interval <= 1:
        parser.error("a command and an interval from 0.001 to 1 second are required")
    if (args.stop_after is not None and (not math.isfinite(args.stop_after) or args.stop_after <= 0)
            or not math.isfinite(args.stop_timeout) or args.stop_timeout <= 0):
        parser.error("stop times must be positive finite seconds")
    if args.smaps_after is not None and (not math.isfinite(args.smaps_after) or args.smaps_after < 0):
        parser.error("smaps time must be finite and nonnegative")
    # Refuse to clobber evidence; open before launching work.
    with args.output.open("x", encoding="utf-8") as output:
        started = time.monotonic_ns()
        samples = []
        stop_sent_ns = None
        stop_killed = False
        unavailable: dict[str, dict[str, int]] = {}
        mappings = None
        before = resource.getrusage(resource.RUSAGE_CHILDREN)
        with subprocess.Popen(command, stdin=subprocess.DEVNULL) as child:
            def forward(signum: int, _frame: object) -> None:
                if child.poll() is None:
                    child.send_signal(signum)

            previous = {s: signal.signal(s, forward) for s in (signal.SIGINT, signal.SIGTERM)}
            try:
                while child.poll() is None:
                    now = time.monotonic_ns()
                    if args.smaps_after is not None and mappings is None and now - started >= args.smaps_after * 1e9:
                        try:
                            mappings = {"elapsed_ns": now - started, "mappings": smaps(child.pid)}
                        except (OSError, ValueError) as error:
                            mappings = {"elapsed_ns": now - started, "error": type(error).__name__,
                                        "message": str(error)}
                    if (args.stop_after is not None and stop_sent_ns is None
                            and now - started >= args.stop_after * 1e9):
                        child.send_signal(signal.SIGINT)
                        stop_sent_ns = now
                    if (stop_sent_ns is not None and not stop_killed
                            and now - stop_sent_ns >= args.stop_timeout * 1e9):
                        child.kill()
                        stop_killed = True
                    clients = snapshot(child.pid, unavailable)
                    if clients:
                        samples.append({"elapsed_ns": time.monotonic_ns() - started,
                                        "clients": clients,
                                        "observers": {str(pid): snapshot(pid, unavailable)
                                                      for pid in args.observe}})
                    time.sleep(args.interval)
            finally:
                if child.poll() is None:
                    child.terminate()
                child.wait()
                for sig, handler in previous.items():
                    signal.signal(sig, handler)
            after = resource.getrusage(resource.RUSAGE_CHILDREN)
            json.dump({"command": command, "pid": child.pid, "returncode": child.returncode,
                       "started_monotonic_ns": started,
                       "elapsed_ns": time.monotonic_ns() - started,
                       "stop_after_seconds": args.stop_after,
                       "stop_sent_ns": stop_sent_ns,
                       "stop_killed": stop_killed,
                       "unavailable_fdinfo": unavailable,
                       "smaps": mappings,
                       "child_cpu_user_us": round((after.ru_utime - before.ru_utime) * 1e6),
                       "child_cpu_system_us": round((after.ru_stime - before.ru_stime) * 1e6),
                       "child_max_rss_kib": after.ru_maxrss,
                       "child_voluntary_context_switches": after.ru_nvcsw - before.ru_nvcsw,
                       "child_involuntary_context_switches": after.ru_nivcsw - before.ru_nivcsw,
                       "child_minor_faults": after.ru_minflt - before.ru_minflt,
                       "child_major_faults": after.ru_majflt - before.ru_majflt,
                       "interval_seconds": args.interval, "samples": samples}, output)
            output.write("\n")
        return child.returncode if child.returncode >= 0 else 128 - child.returncode


if __name__ == "__main__":
    raise SystemExit(main())

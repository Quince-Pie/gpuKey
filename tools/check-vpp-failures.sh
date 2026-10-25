#!/usr/bin/env bash
# Fault-only driver; compare recovery against check-vpp-driver's control images.
# PROBE FAULT_DRIVER REFERENCE_TRANSITION_DIRECTORY NEW_OUTPUT_DIRECTORY
set -euo pipefail
probe=$(realpath "$1")
driver=$(realpath "$2")
reference=$(realpath "$3")
output=$4
mkdir "$output"
output=$(realpath "$output")
export LIBVA_DRIVERS_PATH="$driver/lib/dri:/run/opengl-driver/lib/dri"
sha256sum "$probe" "$driver/lib/dri/radeonsi_drv_video.so" >"$output/binaries.sha256"
for scenario in map shader1 shader3 shader7 shader13; do
    mkdir "$output/$scenario"
    unset GPUKEY_VPP_FAIL_MAP GPUKEY_VPP_FAIL_SHADER
    expected=0
    args=()
    if [[ $scenario = map ]]; then
        export GPUKEY_VPP_FAIL_MAP=2
    else
        export GPUKEY_VPP_FAIL_SHADER=${scenario#shader}
        if [[ $scenario != shader1 ]]; then
            args=(--expect-failure)
            expected=1
        fi
    fi
    "$probe" "$output/$scenario" "${args[@]}" >"$output/$scenario.log" 2>&1
    [[ $(grep -c VPP_TEST_FAILURE "$output/$scenario.log") = 1 ]]
    python3 - "$reference" "$output/$scenario" "$expected" <<'PY'
from pathlib import Path
import sys

reference, candidate = map(Path, sys.argv[1:3])
names = {p.name for p in reference.glob("*.yuv")}
actual = {p.name for p in candidate.glob("*.yuv")}
expected_missing = {"0-8-06.yuv"} if sys.argv[3] == "1" else set()
assert len(names) == 576
assert names - actual == expected_missing
assert not actual - names
for name in sorted(actual):
    assert (candidate / name).read_bytes() == (reference / name).read_bytes(), name
print(f"{candidate.name}: {len(actual)} raw outputs match; {len(expected_missing)} expected operation failure")
PY
done
sha256sum -c "$output/binaries.sha256" >"$output/binaries-verified.log"

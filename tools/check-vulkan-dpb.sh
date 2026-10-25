#!/usr/bin/env bash
# In nix develop .#rgb: PROBE NEW_DIRECTORY. Select the pinned ICD/layer first.
# Forced independent pictures retire and regrow reconstructions several times.
set -euo pipefail
probe=$(realpath "$1")
output=$2
mkdir "$output"
output=$(realpath "$output")
export GPUKEY_RGB_VALIDATE=1 GPUKEY_RGB_FORCED_KEYS=1 GPUKEY_RGB_FRAMES=250
export GPUKEY_RGB_EVENTS=1 GPUKEY_RGB_QUEUE_DPB=1 GPUKEY_RGB_DPB_AUDIT=1
unset GPUKEY_RGB_FAIL_RECON_RESTORE
for bits in 8 10; do
    for trim in 0 2; do
        name=$bits-$trim
        env GPUKEY_RGB_TRIM="$trim" "$probe" "$output/$name.mkv" "$bits" > "$output/$name.log" 2>&1
        if grep -Eq 'VUID-|Validation Error|SYNC-HAZARD' "$output/$name.log"; then
            printf 'Validation failed: %s\n' "$name" >&2
            exit 1
        fi
        ffmpeg -v error -xerror -err_detect explode -i "$output/$name.mkv" \
            -fps_mode passthrough -f framemd5 "$output/$name.framemd5"
        [[ $(grep -vc '^#' "$output/$name.framemd5") = 250 ]]
    done
    cmp "$output/$bits-0.framemd5" "$output/$bits-2.framemd5"
    status=0
    timeout 15 env GPUKEY_RGB_TRIM=2 GPUKEY_RGB_FAIL_RECON_RESTORE=1 \
        "$probe" "$output/$bits-failure.mkv" "$bits" > "$output/$bits-failure.log" 2>&1 || status=$?
    [[ $status = 1 ]]
    grep -q 'Injected reconstruction-view failure after reclamation' "$output/$bits-failure.log"
    if grep -Eq 'VUID-|Validation Error|SYNC-HAZARD' "$output/$bits-failure.log"; then
        printf 'Failure cleanup validation failed: %s\n' "$bits" >&2
        exit 1
    fi
done
python3 - "$output" <<'PY'
from pathlib import Path
import re
import sys

root = Path(sys.argv[1])
for bits in (8, 10):
    for trim in (0, 2):
        text = (root / f"{bits}-{trim}.log").read_text()
        match = re.search(r"DPB_AUDIT created=(\d+) retired=(\d+) live=(\d+)", text)
        assert match, (bits, trim)
        created, retired, live = map(int, match.groups())
        assert created - retired == live and 1 <= live <= 3, (bits, trim, match[0])
        if trim:
            assert created > 3 and retired >= 6, match[0]
        else:
            assert created == 3 and retired == 0, match[0]
        print(bits, trim, match[0])
PY
sha256sum "$probe" > "$output/probe.sha256"
printf 'Reconstruction retirement, repeated restoration, decode equality and failed restoration passed.\n'

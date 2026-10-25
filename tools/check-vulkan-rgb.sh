#!/usr/bin/env bash
# In nix develop .#rgb. PROBE NEW_DIRECTORY; caller selects ICD/validation layer.
# GPU correctness and failure checks, never performance measurements.
set -euo pipefail
probe=$(realpath "$1")
output=$2
mkdir "$output"
output=$(realpath "$output")
export GPUKEY_RGB_VALIDATE=1 GPUKEY_RGB_FORCED_KEYS=1 GPUKEY_RGB_FRAMES=250
export GPUKEY_RGB_EVENTS=1
case ${GPUKEY_RGB_PROFILE:-limited} in
    limited) unset GPUKEY_RGB_FULL_RANGE GPUKEY_RGB_MATRIX_2020 GPUKEY_RGB_BT2020 ;;
    full)
        export GPUKEY_RGB_FULL_RANGE=1 GPUKEY_RGB_MATRIX_2020=1
        unset GPUKEY_RGB_BT2020
        ;;
    *) printf 'Unknown RGB test profile\n' >&2; exit 2 ;;
esac
positive() {
    local name=$1 bits=$2
    shift 2
    env "$@" "$probe" "$output/$name.mkv" "$bits" >"$output/$name.log" 2>&1
    if grep -Eq 'VUID-|Validation Error|SYNC-HAZARD' "$output/$name.log"; then
        printf 'Validation failed: %s\n' "$name" >&2
        return 1
    fi
    ffmpeg -v error -xerror -err_detect explode -i "$output/$name.mkv" \
        -fps_mode passthrough -f framemd5 "$output/$name.framemd5"
    [[ $(grep -vc '^#' "$output/$name.framemd5") = 250 ]]
}
positive control 8 GPUKEY_RGB_DMABUF=1
positive queue 8 GPUKEY_RGB_DMABUF=1 GPUKEY_RGB_QUEUE_DPB=1
cmp "$output/control.framemd5" "$output/queue.framemd5"
positive ten-bit 10 GPUKEY_RGB_QUEUE_DPB=1
positive portrait 8 GPUKEY_RGB_SIZE=2560x2880 GPUKEY_RGB_DMABUF=1 GPUKEY_RGB_QUEUE_DPB=1
for fault in SUBMIT EXPORT VIEW; do
    status=0
    timeout 8 env GPUKEY_RGB_DMABUF=1 GPUKEY_RGB_QUEUE_DPB=1 "GPUKEY_RGB_FAIL_$fault=1" \
        "$probe" "$output/fail-$fault.mkv" 8 >"$output/fail-$fault.log" 2>&1 || status=$?
    [[ $status = 1 ]]
    grep -q 'Injected failure before seventh' "$output/fail-$fault.log"
    if grep -Eq 'VUID-|Validation Error|SYNC-HAZARD' "$output/fail-$fault.log"; then
        printf 'Failure cleanup validation failed: %s\n' "$fault" >&2
        exit 1
    fi
done
status=0
env GPUKEY_RGB_BAD_PROFILE=1 "$probe" "$output/bad-profile.mkv" 8 \
    >"$output/bad-profile.log" 2>&1 || status=$?
[[ $status = 1 ]]
grep -q '04854' "$output/bad-profile.log"
for feature in FULL_RANGE MATRIX_2020; do
    status=0
    if [[ ${GPUKEY_RGB_PROFILE:-limited} == full ]]; then
        env -u "GPUKEY_RGB_$feature" "$probe" "$output/unsupported-$feature.mkv" 8 \
            >"$output/unsupported-$feature.log" 2>&1 || status=$?
    else
        env "GPUKEY_RGB_$feature=1" "$probe" "$output/unsupported-$feature.mkv" 8 \
            >"$output/unsupported-$feature.log" 2>&1 || status=$?
    fi
    [[ $status = 1 && ! -e "$output/unsupported-$feature.mkv" ]]
done
sha256sum "$probe" >"$output/probe.sha256"
printf 'RGB imports, forced keys, reconstruction reuse, decode and failure teardown passed.\n'

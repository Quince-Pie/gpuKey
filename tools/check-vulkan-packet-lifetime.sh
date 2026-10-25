#!/usr/bin/env bash
# In nix develop .#rgb: PROBE NEW_DIRECTORY [LIBRARY_DIRECTORY].
# Set GPUKEY_PACKET_EXPECT_FAILURE=1 only for the preserved unsafe library.
set -euo pipefail
ulimit -c 0
probe=$(realpath "$1")
output=$2
mkdir "$output"
output=$(realpath "$output")
probe_env=(env)
if [[ $# = 3 ]]; then probe_env+=("LD_LIBRARY_PATH=$3${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"); fi
export GPUKEY_RGB_FRAMES=5 GPUKEY_RGB_QUEUE_DPB=1 GPUKEY_RGB_EVENTS=1
unset GPUKEY_RGB_STATIC GPUKEY_RGB_FULL_RANGE GPUKEY_RGB_MATRIX_2020 GPUKEY_RGB_BT2020
export ASAN_OPTIONS=detect_leaks=0:max_free_fill_size=1048576:free_fill_byte=253
for kind in imported8 uploaded10 staged10; do
    bits=8
    unset GPUKEY_RGB_STAGING
    export GPUKEY_RGB_DMABUF=1
    if [[ $kind != imported8 ]]; then bits=10; unset GPUKEY_RGB_DMABUF; fi
    if [[ $kind = staged10 ]]; then export GPUKEY_RGB_STAGING=1; fi
    for mode in before encoder device; do
        name=$kind-$mode
        status=0
        GPUKEY_RGB_PACKET_HOLD=$mode "${probe_env[@]}" "$probe" "$output/$name.mkv" "$bits" \
            >"$output/$name.log" 2>&1 || status=$?
        printf '%s %s\n' "$name" "$status" >>"$output/status.txt"
        if [[ ${GPUKEY_PACKET_EXPECT_FAILURE:-0} = 1 && $mode != before ]]; then
            [[ $status != 0 ]]
            grep -Eq 'AddressSanitizer|Device destroyed before retained packet release' "$output/$name.log"
        else
            [[ $status = 0 ]]
            if grep -Eq 'VUID-|Validation Error|SYNC-HAZARD|ERROR: AddressSanitizer|runtime error:' "$output/$name.log"; then
                exit 1
            fi
        fi
        ffmpeg -v error -xerror -err_detect explode -i "$output/$name.mkv" \
            -fps_mode passthrough -f framemd5 "$output/$name.framemd5"
        [[ $(grep -vc '^#' "$output/$name.framemd5") = 5 ]]
    done
    cmp "$output/$kind-before.framemd5" "$output/$kind-encoder.framemd5"
    cmp "$output/$kind-before.framemd5" "$output/$kind-device.framemd5"
done
cmp "$output/uploaded10-before.framemd5" "$output/staged10-before.framemd5"
sha256sum "$probe" "${BASH_SOURCE[0]}" >"$output/inputs.sha256"
printf 'Vulkan packet ownership checks completed; expected failure control=%s.\n' \
    "${GPUKEY_PACKET_EXPECT_FAILURE:-0}"

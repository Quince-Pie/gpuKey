#!/usr/bin/env bash
# In nix develop: NEW_DIRECTORY DRIVER NAME FFMPEG_LIBDIR [NAME FFMPEG_LIBDIR ...]
# Compare the exact VAAPI handoff and encoded bytes, with independent decoding.
set -euo pipefail
output=$1 driver=$(realpath "$2")
shift 2
(( $# > 0 && $# % 2 == 0 ))
mkdir "$output"
output=$(realpath "$output")
probe="build/${TOOLCHAIN:-gcc}-release/tests/av1_headers_probe"
make -s PROFILE=release CFLAGS=-Werror "$probe"
${CC:-cc} -std=c23 -O2 -g -Wall -Wextra -Wpedantic -Wconversion -Werror \
    -shared -fPIC $(pkg-config --cflags libva) tools/vaapi-header-trace.c \
    -ldl -pthread -o "$output/trace.so"
export LIBVA_DRIVERS_PATH="$driver/lib/dri:/run/opengl-driver/lib/dri"
reference=
while (( $# )); do
    name=$1 library=$(realpath "$2")
    shift 2
    [[ $name =~ ^[a-z][a-z0-9]*$ ]]
    directory="$output/$name"
    mkdir "$directory"
    GPUKEY_HEADER_TRACE="$directory/headers.txt" LD_PRELOAD="$output/trace.so" \
        LD_LIBRARY_PATH="$library" "$probe" "$directory" \
        >"$directory/decode.log" 2>"$directory/errors.log"
    # Two headers for the first key, then one CLL-only and one MDCV-only key.
    [[ $(grep -c '^parameter 4 ' "$directory/headers.txt") = 16 ]]
    [[ $(grep -c '^offsets ' "$directory/headers.txt") = 48 ]]
    if [[ -n $reference ]]; then
        for filename in headers.txt decode.log cqp-8.packets cqp-10.packets qvbr-8.packets qvbr-10.packets; do
            cmp "$output/$reference/$filename" "$directory/$filename"
        done
    fi
    sha256sum "$library"/libavcodec.so* >>"$output/libraries.sha256"
    [[ -n $reference ]] || reference=$name
done
sha256sum "$probe" "$output/trace.so" "$driver/lib/dri/radeonsi_drv_video.so" >"$output/binaries.sha256"
echo 'AV1 CQP/QVBR, both depths: exact packed headers, offsets, metadata subsets, encoded bytes and decoded properties match.'

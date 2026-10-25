#!/usr/bin/env bash
# In nix develop: NEW_DIRECTORY PINNED_FFMPEG_SOURCE PATCHED_SOURCE PACKET_DIRECTORY
# PATCHED_SOURCE supplies the four modified cbs_av1 files. Public packet inputs
# come from av1_headers_probe; no GPU is used by this writer/decoder check.
set -euo pipefail
output=$1 base=$(realpath "$2") patched=$(realpath "$3") packets=$(realpath "$4")
mkdir "$output"
output=$(realpath "$output")
mkdir "$output/config"
(
    cd "$output/config"
    bash "$base/configure" --disable-everything --disable-autodetect --disable-x86asm \
        --disable-programs --disable-doc --disable-network
) >"$output/configure.log" 2>&1
defines=(-DCBS_AV1=1 -DCBS_APV=0 -DCBS_H264=0 -DCBS_H265=0 -DCBS_H266=0
         -DCBS_LCEVC=0 -DCBS_JPEG=0 -DCBS_MPEG2=0 -DCBS_VP8=0 -DCBS_VP9=0)
extra=()
if [[ ${GPUKEY_HEADER_SANITIZE:-0} = 1 ]]; then
    extra=(-fsanitize=address,undefined -fno-omit-frame-pointer)
fi
${CC:-cc} -std=c23 -O2 -g "${extra[@]}" -I"$output/config" -I"$patched" \
    -I"$base" -I"$base/libavcodec" $(pkg-config --cflags libavcodec libavutil) \
    "${defines[@]}" tests/av1_header_writer_probe.c "$base/libavcodec/cbs.c" \
    "$patched/libavcodec/cbs_av1.c" $(pkg-config --libs libavcodec libavutil) \
    -o "$output/probe" >"$output/build.log" 2>&1
for input in "$packets"/*.packets; do
    "$output/probe" "$input" >"$output/${input##*/}.log" 2>&1
done
sha256sum "$output/probe" tests/av1_header_writer_probe.c "$base/libavcodec/cbs.c" \
    "$patched"/libavcodec/cbs_av1* "$packets"/*.packets >"$output/inputs.sha256"
echo 'Direct AV1 writer: exact headers/offsets, buffer bounds, failed-write teardown, borrowed-input lifetime and hidden/show-existing decode pass.'

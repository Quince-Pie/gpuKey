#!/usr/bin/env bash
# In nix develop: NEW_OUTPUT_DIRECTORY NAME DRIVER [NAME DRIVER ...]
# Compare independent decode hashes across codecs, reference order and pre-encode.
set -euo pipefail
output=$1
shift
(( $# > 0 && $# % 2 == 0 ))
mkdir "$output"
output=$(realpath "$output")
reference=
while (( $# )); do
    name=$1 driver=$(realpath "$2")
    shift 2
    [[ $name =~ ^[a-z][a-z0-9]*$ ]]
    export LIBVA_DRIVERS_PATH="$driver/lib/dri:/run/opengl-driver/lib/dri"
    sha256sum "$driver/lib/dri/radeonsi_drv_video.so" >>"$output/binaries.sha256"
    for mode in h264 hevc av1 av1qvbr; do
        codec=${mode%qvbr}
        args=(-bf 2 -qp 24)
        # This VCN platform exposes HEVC without future-reference encoding.
        # FFmpeg otherwise silently substitutes I/P even with -bf 2.
        if [[ $codec = hevc ]]; then args=(-bf 0 -qp 24); fi
        if [[ $codec = av1 ]]; then args=(-bf 0 -qp 32); fi
        if [[ $mode = av1qvbr ]]; then args=(-bf 0 -rc_mode QVBR -b:v 2M -global_quality 24); fi
        stem="$output/$name-$mode"
        ffmpeg -v warning -vaapi_device /dev/dri/renderD128 -f lavfi \
            -i testsrc2=size=640x480:rate=60 -vf format=nv12,hwupload \
            -frames:v 120 -c:v "${codec}_vaapi" "${args[@]}" "$stem.mkv" >"$stem.log" 2>&1
        ffmpeg -v error -i "$stem.mkv" -fps_mode passthrough -f framemd5 "$stem.md5"
        ffmpeg -v error -i "$stem.mkv" -map 0:v -c copy -f hash -hash sha256 "$stem.encoded"
        [[ $(grep -c '^0,' "$stem.md5") = 120 ]]
        if [[ $codec != av1 ]]; then
            ffprobe -v error -select_streams v:0 -show_entries frame=pict_type \
                -of csv=p=0 "$stem.mkv" >"$stem.frames"
            if [[ $codec = h264 ]]; then
                grep -q '^B' "$stem.frames"
            else
                grep -q '^P' "$stem.frames"
                if grep -q '^B' "$stem.frames"; then
                    printf 'Unexpected B picture: %s\n' "$stem" >&2
                    exit 1
                fi
            fi
        fi
        [[ -z $reference ]] || cmp "$output/$reference-$mode.md5" "$stem.md5"
        [[ -z $reference ]] || cmp "$output/$reference-$mode.encoded" "$stem.encoded"
    done
    [[ -n $reference ]] || reference=$name
done
sha256sum -c "$output/binaries.sha256" >"$output/binaries-verified.log"
echo 'H.264 I/P/B, HEVC I/P and AV1 CQP/QVBR: 120 decoded frames and encoded payload hashes per case match.'

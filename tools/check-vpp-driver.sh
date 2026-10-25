#!/usr/bin/env bash
# Run inside nix develop: CONTROL_DRIVER CANDIDATE_DRIVER NEW_OUTPUT_DIRECTORY.
set -euo pipefail
control=$(realpath "$1")
candidate=$(realpath "$2")
output=$3
mkdir "$output"
output=$(realpath "$output")
build="build/${TOOLCHAIN:-gcc}-release"
make -s PROFILE=release CFLAGS=-Werror \
    "$build/tests/gpu_probe" "$build/tests/gpu_vpp_probe" "$build/tests/gpu_fused_probe"
sha256sum "$control/lib/dri/radeonsi_drv_video.so" "$candidate/lib/dri/radeonsi_drv_video.so" \
    "$build/tests/gpu_probe" "$build/tests/gpu_vpp_probe" "$build/tests/gpu_fused_probe" \
    >"$output/binaries.sha256"
for variant in control candidate; do
    driver=$control
    [[ $variant = candidate ]] && driver=$candidate
    export LIBVA_DRIVERS_PATH="$driver/lib/dri:/run/opengl-driver/lib/dri"
    mkdir "$output/$variant" "$output/$variant/transitions"
    "$build/tests/gpu_probe" >"$output/$variant/color.log" 2>&1
    "$build/tests/gpu_vpp_probe" "$output/$variant/transitions" >"$output/$variant/transitions.log" 2>&1
    [[ $(wc -l <"$output/$variant/transitions.log") = 576 ]]
    for bits in 8 10; do
        for layout in linear native; do
            stem="$output/$variant/$bits-$layout"
            "$build/tests/gpu_fused_probe" "$stem.mkv" "$layout" "$bits" --raw >"$stem.log" 2>&1
            ffmpeg -v error -i "$stem.mkv" -frames:v 1 -f framemd5 "$stem.decoded"
        done
    done
done
diff -rq "$output/control/transitions" "$output/candidate/transitions"
for bits in 8 10; do
    for layout in linear native; do
        cmp "$output/control/$bits-$layout.mkv.yuv" "$output/candidate/$bits-$layout.mkv.yuv"
        cmp "$output/control/$bits-$layout.decoded" "$output/candidate/$bits-$layout.decoded"
    done
done
sha256sum -c "$output/binaries.sha256" >"$output/binaries-verified.log"
echo 'VPP: scalar endpoints, pinned input, 576 raw transitions, both depths/layouts and decoded patterns pass.'

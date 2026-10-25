#!/usr/bin/env bash
# In nix develop: NEW_DIRECTORY NAME DRIVER [NAME DRIVER ...]
set -euo pipefail
output=$1
shift
(( $# > 0 && $# % 2 == 0 ))
mkdir "$output"
output=$(realpath "$output")
build="build/${TOOLCHAIN:-gcc}-release"
make -s PROFILE=release CFLAGS=-Werror "$build/tests/gpu_probe" \
    "$build/tests/gpu_chroma_probe" "$build/tests/gpu_contexts_probe"
${CC:-cc} -D_GNU_SOURCE -std=c23 -O2 -g -Wall -Wextra -Wpedantic -Wconversion -Werror \
    $(pkg-config --cflags egl glesv2 gbm libavutil) tests/gpu_graphics_probe.c \
    $(pkg-config --libs egl glesv2 gbm libavutil) -pthread -ldl -o "$output/graphics"
while (( $# )); do
    name=$1 driver=$(realpath "$2")
    shift 2
    [[ $name =~ ^[a-z][a-z0-9]*$ ]]
    directory="$output/$name"
    mkdir "$directory" "$directory/contexts"
    export LIBVA_DRIVERS_PATH="$driver/lib/dri:/run/opengl-driver/lib/dri"
    export LIBGL_DRIVERS_PATH="$driver/lib/dri" GBM_BACKENDS_PATH="$driver/lib/gbm"
    export __EGL_VENDOR_LIBRARY_FILENAMES="$driver/share/glvnd/egl_vendor.d/50_mesa.json"
    export GPUKEY_EXPECT_DRIVER="$driver"
    "$build/tests/gpu_probe" >"$directory/colors.log" 2>&1
    "$build/tests/gpu_chroma_probe" >"$directory/chroma.log" 2>&1
    "$build/tests/gpu_contexts_probe" "$directory/contexts" >"$directory/contexts.log" 2>&1
    "$output/graphics" >"$directory/graphics.log" 2>&1
    sha256sum "$driver/lib/dri/radeonsi_drv_video.so" >>"$output/drivers.sha256"
    echo "$name: color, chroma, concurrent encoders and graphics/teardown pass"
done
sha256sum "$output/graphics" "$build/tests/gpu_probe" "$build/tests/gpu_chroma_probe" \
    "$build/tests/gpu_contexts_probe" >"$output/binaries.sha256"

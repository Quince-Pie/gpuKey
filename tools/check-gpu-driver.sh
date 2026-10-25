#!/usr/bin/env bash
# Compile/run the optional driver's headless graphics regression probe in nix develop.
set -euo pipefail
driver=$(realpath "$1")
device=${2:-/dev/dri/renderD128}
scratch=$(mktemp -d)
trap 'rm -rf "$scratch"' EXIT
export LIBVA_DRIVERS_PATH="$driver/lib/dri"
export LIBGL_DRIVERS_PATH="$driver/lib/dri"
export GBM_BACKENDS_PATH="$driver/lib/gbm"
export __EGL_VENDOR_LIBRARY_FILENAMES="$driver/share/glvnd/egl_vendor.d/50_mesa.json"
${CC:-cc} -D_GNU_SOURCE -std=c23 -O2 -g -Wall -Wextra -Wpedantic -Wconversion -Werror \
    $(pkg-config --cflags libavutil egl glesv2 gbm) tests/gpu_graphics_probe.c \
    -o "$scratch/probe" $(pkg-config --libs libavutil egl glesv2 gbm) -pthread
if [[ -n ${GPUKEY_FAIL_BO_BYTES:-} ]]; then
    ${CC:-cc} -std=c23 -Wall -Wextra -Werror -fPIC -shared \
        $(pkg-config --cflags libdrm_amdgpu) tools/gpu-alloc-trace.c \
        -ldl -o "$scratch/alloc-trace.so"
    export LD_PRELOAD="$scratch/alloc-trace.so${LD_PRELOAD:+:$LD_PRELOAD}"
    export GPUKEY_EXPECT_CONTEXT_OOM=1
fi
"$scratch/probe" "$device"

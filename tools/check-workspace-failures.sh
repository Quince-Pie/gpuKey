#!/usr/bin/env bash
# In nix develop: DRIVER_DIRECTORY direct|packed|whole NEW_OUTPUT_DIRECTORY
set -euo pipefail
driver=$(realpath "$1") variant=$2 output=$3
[[ $variant = direct || $variant = packed || $variant = whole ]]
mkdir "$output"
output=$(realpath "$output")
build="build/${TOOLCHAIN:-gcc}-release"
make -s PROFILE=release CFLAGS=-Werror "$build/tests/gpu_workspace_failure_probe" "$build/tests/gpu_probe"
${CC:-cc} -std=c23 -O2 -g -Wall -Wextra -Wpedantic -Wconversion -Werror -fPIC -shared \
    $(pkg-config --cflags libdrm_amdgpu) tools/encoder-alloc-fault.c -ldl -o "$output/fault.so"
export LIBVA_DRIVERS_PATH="$driver/lib/dri:/run/opengl-driver/lib/dri"
run() {
    local name=$1 bytes=$2 flags=$3 skip=$4 map=$5 message=$6
    env LD_PRELOAD="$output/fault.so" GPUKEY_FAIL_BYTES="$bytes" GPUKEY_FAIL_HEAP=4 \
        GPUKEY_FAIL_FLAGS="$flags" GPUKEY_FAIL_SKIP="$skip" GPUKEY_FAIL_MAP="$map" \
        "$build/tests/gpu_workspace_failure_probe" >"$output/$name.log" 2>&1
    grep -q 'INJECTED_.*FAILURE' "$output/$name.log"
    grep -q "$message" "$output/$name.log"
    grep -q 'eight failed and healthy lifetimes, balanced descriptors' "$output/$name.log"
}
cdf=81920 cdf_skip=0 fcb_skip=1
if [[ $variant = whole ]]; then
    fcb_skip=0
    run workspace 2097152 4 0 0 'create DPB buffer'
    run cdf-map 2097152 4 0 1 'create cdf buffer'
else
    if [[ $variant = packed ]]; then cdf=262144 cdf_skip=1 fcb_skip=0; fi
    run cdf "$cdf" 4 "$cdf_skip" 0 'create cdf buffer'
    run cdf-map "$cdf" 4 "$cdf_skip" 1 'create cdf buffer'
    run sdb 950272 6 0 0 'create DPB buffer'
    [[ $variant = packed ]] || run session 131072 6 0 0 'create session buffer'
fi
run fcb 131072 6 "$fcb_skip" 0 'create fcb buffer'
"$build/tests/gpu_probe" >"$output/healthy-color.log" 2>&1
sha256sum "$build/tests/gpu_workspace_failure_probe" "$build/tests/gpu_probe" \
    "$driver/lib/dri/radeonsi_drv_video.so" "$output/fault.so" >"$output/binaries.sha256"
echo "Workspace $variant allocation/map failures, recovery cycles and 8/10-bit color pass."

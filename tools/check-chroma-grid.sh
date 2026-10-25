#!/usr/bin/env bash
# In nix develop: SELECTED_DRIVER FAULT_DRIVER NEW_DIRECTORY
set -euo pipefail
driver=$(realpath "$1") fault=$(realpath "$2") output=$3
mkdir "$output"
output=$(realpath "$output")
probe="build/${TOOLCHAIN:-gcc}-release/tests/gpu_chroma_probe"
make -s PROFILE=release CFLAGS=-Werror "$probe"
LIBVA_DRIVERS_PATH="$driver/lib/dri:/run/opengl-driver/lib/dri" \
    "$probe" >"$output/normal.log" 2>&1
for spec in SHADER:1 SHADER:2 MAP:1 MAP:2; do
    kind=${spec%:*} count=${spec#*:}
    LIBVA_DRIVERS_PATH="$fault/lib/dri:/run/opengl-driver/lib/dri" \
        env "GPUKEY_VPP_FAIL_$kind=$count" "$probe" >"$output/$kind-$count.log" 2>&1
    [[ $(grep -c VPP_TEST_FAILURE "$output/$kind-$count.log") = 1 ]]
    grep -q 'GPU chroma: independent' "$output/$kind-$count.log"
done
sha256sum "$probe" "$driver/lib/dri/radeonsi_drv_video.so" \
    "$fault/lib/dri/radeonsi_drv_video.so" >"$output/binaries.sha256"
echo 'Chroma geometry, shader/map failure, retry and teardown checks pass.'

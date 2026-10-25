#!/usr/bin/env bash
# In the recorder dev shell: EVENT_DRIVER NEW_OUTPUT_DIR.
set -euo pipefail
driver=$(realpath "$1") output=$2
mkdir "$output"
output=$(realpath "$output")
build="build/${TOOLCHAIN:-gcc}-release"
make -s PROFILE=release CFLAGS=-Werror "$build/gpukey" \
    "$build/tests/capture_fixture" "$build/tests/packet_ready_probe" \
    "$build/tests/completion_lifecycle_probe" feature-check
export LIBVA_DRIVERS_PATH="$driver/lib/dri:/run/opengl-driver/lib/dri"
"$build/tests/completion_lifecycle_probe" >"$output/lifecycle.log" 2>&1
"$build/tests/packet_ready_probe" "$output/ready.mkv" 2 events >"$output/ready.json" 2>"$output/ready.log"
jq -e '.early_save_status == 0 and .saved == 1' "$output/ready.json" >/dev/null
"$build/tests/packet_ready_probe" "$output/buffered.mkv" 2 buffered \
    >"$output/buffered.json" 2>"$output/buffered.log"
jq -e '.early_save_status == -11 and .saved == 0' "$output/buffered.json" >/dev/null
ffmpeg -v error -i "$output/ready.mkv" -f null -
for readout in available events; do
    GPUKEY_FIXTURE_EARLY_SAVES=1 GPUKEY_FIXTURE_RATE=60 GPUKEY_FIXTURE_SIZE=1920x1080 \
        GPUKEY_PACKET_READOUT="$readout" GPUKEY_MEMORY_LIBVA_DRIVERS_PATH="$LIBVA_DRIVERS_PATH" \
        bash tools/benchmark-gpu-memory.sh "$build/gpukey" "$build/tests/capture_fixture" \
        "$output/early-$readout" --gpu-portal-replay-static
done
sha256sum "$build/gpukey" "$build/tests/completion_lifecycle_probe" \
    "$build/tests/packet_ready_probe" "$driver/lib/dri/radeonsi_drv_video.so" >"$output/binaries.sha256"
echo 'Completed-output notification, early saves, fallback, exhaustion and descriptor reuse pass.'

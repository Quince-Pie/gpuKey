#!/usr/bin/env bash
# Requires a Vulkan AV1 encoder. Synthetic pixels/audio only; no desktop capture.
set -euo pipefail
binary=$1
output=$2
mkdir "$output"
runtime=$(mktemp -d)
recorder=
cleanup() {
    if [[ -n "$recorder" ]]; then
        kill -TERM "$recorder" 2>/dev/null || true
        wait "$recorder" 2>/dev/null || true
    fi
    rm -rf "$runtime"
}
trap cleanup EXIT
export XDG_RUNTIME_DIR="$runtime"
sha256sum "$binary" >"$output/binary.sha256"
for sink in buffered uring; do
    "$binary" replay 1 --backend vulkan --test-source pattern --size 320x240 \
        --test-audio --test-audio --realtime --duration 4.2 --sink "$sink" --durable \
        --directory "$output" --stats "$output/$sink.json" >"$output/$sink.log" 2>&1 &
    recorder=$!
    for ((attempt=0; attempt<500; attempt++)); do
        grep -q "Replay 'default' ready" "$output/$sink.log" && break
        kill -0 "$recorder"
        sleep 0.02
    done
    grep -q "Replay 'default' ready" "$output/$sink.log"
    sleep 3.2
    "$binary" save "$output/$sink.mkv"
    wait "$recorder"
    recorder=
    jq -e '.success and .replay_saved == 1 and .replay_failed == 0 and
        .replay_copied_bytes > 0 and .replay_saved_bytes > 0' "$output/$sink.json"
    ffmpeg -v error -i "$output/$sink.mkv" -map 0 -f null -
done
echo 'Vulkan replay: mapped output copied, both sinks drained durably, independent video/audio decode passed'

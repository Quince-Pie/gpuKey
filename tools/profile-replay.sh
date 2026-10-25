#!/usr/bin/env bash
# Run in nix develop with an instrumented recorder and the native GPU fixture.
set -euo pipefail
binary=$1
fixture=$2
output=$3
mode=${4:-"--gpu-replay"}
mkdir "$output"
runtime=$(mktemp -d)
server= capture=
cleanup() {
    for process in "$capture" "$server"; do
        if [[ -n "$process" ]]; then
            kill "$process" 2>/dev/null || true
            wait "$process" 2>/dev/null || true
        fi
    done
    rm -rf "$runtime"
}
trap cleanup EXIT
export PIPEWIRE_RUNTIME_DIR="$runtime" PIPEWIRE_REMOTE=gpukey-test XDG_RUNTIME_DIR="$runtime"
pipewire -c "$PWD/tests/pipewire.conf" >"$output/server.log" 2>&1 &
server=$!
for ((attempt=0; attempt<100; attempt++)); do
    [[ -S "$runtime/gpukey-test" ]] && break
    sleep 0.02
done
[[ -S "$runtime/gpukey-test" ]]
tracy-capture -o "$output/replay.tracy" -f -s "${GPUKEY_TRACY_SECONDS:-8}" >"$output/capture.log" 2>&1 &
capture=$!
GPUKEY_FIXTURE_STATS="$output/recording.json" "$fixture" "$binary" "$output/clip.mkv" "$mode" \
    >"$output/recording.log" 2>&1
wait "$capture"
capture=
tracy-csvexport "$output/replay.tracy" >"$output/zones.csv" 2>"$output/export.log"
grep -q 'replay.export' "$output/zones.csv"
grep -q 'replay.retain_video' "$output/zones.csv"
ffmpeg -v error -i "$output/clip.mkv" -map 0 -f null -
ffmpeg -v error -i "$output/clip.mkv.second.mkv" -map 0 -f null -
sha256sum "$binary" "$fixture" >"$output/binaries.sha256"
echo "Tracy replay capture and independent clip decoding passed: $output"

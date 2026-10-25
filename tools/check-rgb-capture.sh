#!/usr/bin/env bash
# In nix develop. RECORDER_WRAPPER FIXTURE NEW_DIRECTORY.
# The wrapper selects the candidate backend/driver and passes save commands through.
set -euo pipefail
binary=$(realpath "$1") fixture=$(realpath "$2") output=$3
mkdir "$output"
output=$(realpath "$output")
runtime=$(mktemp -d)
server=
cleanup() {
    if [[ -n $server ]]; then kill "$server" 2>/dev/null || true; wait "$server" 2>/dev/null || true; fi
    rm -rf "$runtime"
}
trap cleanup EXIT
export PIPEWIRE_RUNTIME_DIR="$runtime" PIPEWIRE_REMOTE=gpukey-test XDG_RUNTIME_DIR="$runtime"
export GPUKEY_FIXTURE_SIZE=320x240
unset GPUKEY_FIXTURE_SECONDS GPUKEY_FIXTURE_RATE GPUKEY_FIXTURE_PATTERN
unset GPUKEY_FIXTURE_BUFFER_MIN GPUKEY_FIXTURE_BUFFER_MAX
unset GPUKEY_FIXTURE_LATE_SAVES GPUKEY_FIXTURE_EARLY_SAVES
pipewire -c "$PWD/tests/pipewire.conf" >"$output/server.log" 2>&1 &
server=$!
for ((i=0;i<100;i++)); do [[ -S "$runtime/gpukey-test" ]] && break; sleep 0.02; done
[[ -S "$runtime/gpukey-test" ]]
for mode in gpu gpu-aux gpu-replay gpu-reconfigure gpu-portal-replay-static gpu-static-replay gpu-replay-niri; do
    export GPUKEY_FIXTURE_SIZE=320x240
    # Keep the original portrait/missing-PTS fixture's timing semantics.
    [[ $mode = gpu-replay-niri ]] && unset GPUKEY_FIXTURE_SIZE
    extension=mkv
    [[ $mode = gpu || $mode = gpu-reconfigure ]] && extension=mp4
    clip="$output/$mode.$extension"
    GPUKEY_FIXTURE_STATS="$output/$mode.json" \
        "$fixture" "$binary" "$clip" "--$mode" >"$output/$mode.log" 2>&1
    if grep -Eq 'VUID-|Validation Error|SYNC-HAZARD|ERROR: AddressSanitizer|runtime error:' "$output/$mode.log"; then
        printf 'Capture validation failed: %s\n' "$mode" >&2
        exit 1
    fi
    ffmpeg -v error -xerror -err_detect explode -i "$clip" -map 0 -f null -
    if [[ $mode = gpu-aux ]]; then
        python3 tools/check-audio-startup.py "$clip" >"$output/audio-startup.json"
    fi
    if [[ -e $clip.second.mkv ]]; then
        ffmpeg -v error -xerror -err_detect explode -i "$clip.second.mkv" -map 0 -f null -
    fi
done
sha256sum "$binary" "$fixture" >"$output/binaries.sha256"
printf 'Native capture, multichannel audio, replay, reconfiguration and static repeats passed.\n'

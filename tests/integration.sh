#!/usr/bin/env bash
set -euo pipefail
binary=$1
fixture=$2
scratch=$(mktemp -d)
server=
cleanup() {
    if [[ -n "$server" ]]; then kill "$server" 2>/dev/null || true; wait "$server" 2>/dev/null || true; fi
    rm -rf "$scratch"
}
trap cleanup EXIT
export PIPEWIRE_RUNTIME_DIR="$scratch"
export XDG_RUNTIME_DIR="$scratch"
export PIPEWIRE_REMOTE=gpukey-test
pipewire -c "$(pwd)/tests/pipewire.conf" >"$scratch/server.log" 2>&1 &
server=$!
for ((attempt=0; attempt<100; attempt++)); do
    [[ -S "$scratch/gpukey-test" ]] && break
    sleep 0.02
done
if [[ ! -S "$scratch/gpukey-test" ]]; then cat "$scratch/server.log"; exit 1; fi
"$fixture" "$binary" "$scratch/result.mp4"
ffmpeg -v error -i "$scratch/result.mp4" -f null -
"$fixture" "$binary" "$scratch/portal.mp4" --portal
ffmpeg -v error -i "$scratch/portal.mp4" -f null -
"$fixture" "$binary" "$scratch/portal-static.mkv" --portal-static
ffmpeg -v error -i "$scratch/portal-static.mkv" -f null -
GPUKEY_FIXTURE_STATS="$scratch/missing-pts.json" \
    "$fixture" "$binary" "$scratch/missing-pts.mp4" --missing-pts
grep -q '"video_clock": "receive"' "$scratch/missing-pts.json"
ffmpeg -v error -i "$scratch/missing-pts.mp4" -f null -
GPUKEY_FIXTURE_STATS="$scratch/cycle-pts.json" \
    "$fixture" "$binary" "$scratch/cycle-pts.mp4" --cycle-pts
grep -q '"video_clock": "cycle"' "$scratch/cycle-pts.json"
ffmpeg -v error -i "$scratch/cycle-pts.mp4" -f null -
"$fixture" "$binary" "$scratch/zero-epoch.mp4" --zero-epoch
ffmpeg -v error -i "$scratch/zero-epoch.mp4" -f null -
"$fixture" "$binary" "$scratch/same-sequence.mp4" --same-sequence
ffmpeg -v error -i "$scratch/same-sequence.mp4" -f null -
"$fixture" "$binary" "$scratch/lost-pts.mp4" --lost-pts >"$scratch/lost-pts.log" 2>&1 ||
    { cat "$scratch/lost-pts.log"; exit 1; }
grep -q 'video clock stopped or moved backwards' "$scratch/lost-pts.log"
ffmpeg -v error -i "$scratch/lost-pts.mp4" -f null -
"$fixture" "$binary" "$scratch/lost-cycle.mp4" --lost-cycle >"$scratch/lost-cycle.log" 2>&1 ||
    { cat "$scratch/lost-cycle.log"; exit 1; }
grep -q 'video clock stopped or moved backwards' "$scratch/lost-cycle.log"
ffmpeg -v error -i "$scratch/lost-cycle.mp4" -f null -
"$fixture" "$binary" "$scratch/revoked.mp4" --revoke
ffmpeg -v error -i "$scratch/revoked.mp4" -f null -
"$fixture" "$binary" "$scratch/reconfigured.mp4" --reconfigure >"$scratch/reconfigured.log" 2>&1 ||
    { cat "$scratch/reconfigured.log"; exit 1; }
grep -q 'capture: source configuration changed' "$scratch/reconfigured.log"
ffmpeg -v error -i "$scratch/reconfigured.mp4" -f null -
"$fixture" "$binary" "$scratch/static.mkv" --static
ffmpeg -v error -i "$scratch/static.mkv" -f null -
"$fixture" "$binary" "$scratch/capped.mp4" --fps
ffmpeg -v error -i "$scratch/capped.mp4" -f null -
"$fixture" "$binary" "$scratch/audio.mp4" --audio
ffmpeg -v error -i "$scratch/audio.mp4" -map 0 -f null -
[[ $(ffmpeg -v error -i "$scratch/audio.mp4" -map 0:a:0 -f f32le - | wc -c) = 192000 ]]
[[ $(ffmpeg -v error -i "$scratch/audio.mp4" -map 0:a:1 -f f32le - | wc -c) = 1152000 ]]
"$fixture" "$binary" "$scratch/duration-boundary.mp4" --duration-boundary
ffmpeg -v error -i "$scratch/duration-boundary.mp4" -map 0 -f null -
"$fixture" "$binary" "$scratch/audio-revoked.mp4" --audio-revoke
ffmpeg -v error -i "$scratch/audio-revoked.mp4" -map 0 -f null -
"$fixture" "$binary" "$scratch/missing.mp4" --audio-missing >"$scratch/missing.log" 2>&1 ||
    { cat "$scratch/missing.log"; exit 1; }
grep -q 'capture: selected audio is unavailable' "$scratch/missing.log"
"$fixture" "$binary" "$scratch/duplicate.mp4" --audio-duplicate >"$scratch/duplicate.log" 2>&1 ||
    { cat "$scratch/duplicate.log"; exit 1; }
grep -q 'capture: the same audio device was selected twice' "$scratch/duplicate.log"
"$fixture" "$binary" "$scratch/kind.mp4" --audio-kind >"$scratch/kind.log" 2>&1 ||
    { cat "$scratch/kind.log"; exit 1; }
grep -q 'capture: audio source/monitor kind does not match' "$scratch/kind.log"
"$fixture" "$binary" "$scratch/discovery.mp4" --devices >"$scratch/devices.txt"
grep -q 'gpukey-fixture-mic' "$scratch/devices.txt"
grep -q 'gpukey-fixture-sink' "$scratch/devices.txt"
"$fixture" "$binary" "$scratch/aux.mkv" --aux
ffmpeg -v error -i "$scratch/aux.mkv" -map 0 -f null -
"$fixture" "$binary" "$scratch/aux-pcm.mkv" --aux-pcm
ffmpeg -v error -i "$scratch/aux-pcm.mkv" -map 0 -f null -
"$fixture" "$binary" "$scratch/replay.mkv" --replay
ffmpeg -v error -i "$scratch/replay.mkv" -map 0 -f null -
ffmpeg -v error -i "$scratch/replay.mkv.second.mkv" -map 0 -f null -
"$fixture" "$binary" "$scratch/portal-replay.mkv" --portal-replay
ffmpeg -v error -i "$scratch/portal-replay.mkv" -map 0 -f null -
ffmpeg -v error -i "$scratch/portal-replay.mkv.second.mkv" -map 0 -f null -
"$fixture" "$binary" "$scratch/static-replay.mkv" --static-replay
ffmpeg -v error -i "$scratch/static-replay.mkv" -map 0 -f null -
ffmpeg -v error -i "$scratch/static-replay.mkv.second.mkv" -map 0 -f null -
"$fixture" "$binary" "$scratch/replay-revoke.mkv" --replay-revoke
ffmpeg -v error -i "$scratch/replay-revoke.mkv" -map 0 -f null -
"$binary" devices
if [[ $(printenv GPUKEY_TEST_GPU) = 1 ]]; then
    # Shared-memory capture with native encoding exercises the real CPU upload
    # consumer, its audio timeline and replay export on hardware-sized images.
    cat >"$scratch/cpu-vaapi" <<'SH'
#!/usr/bin/env bash
if [[ ${1:-} = save ]]; then exec "$GPUKEY_TEST_BINARY" "$@"; fi
exec "$GPUKEY_TEST_BINARY" "$@" --backend vaapi --shm
SH
    chmod 700 "$scratch/cpu-vaapi"
    GPUKEY_TEST_BINARY="$binary" GPUKEY_FIXTURE_SIZE=320x240 \
        "$fixture" "$scratch/cpu-vaapi" "$scratch/cpu-vaapi.mkv" --portal-replay
    ffmpeg -v error -i "$scratch/cpu-vaapi.mkv" -map 0 -f null -
    ffmpeg -v error -i "$scratch/cpu-vaapi.mkv.second.mkv" -map 0 -f null -
    "$fixture" "$binary" "$scratch/gpu.mp4" --gpu
    ffmpeg -v error -i "$scratch/gpu.mp4" -f null -
    "$fixture" "$binary" "$scratch/gpu-fixate.mp4" --gpu-fixate
    ffmpeg -v error -i "$scratch/gpu-fixate.mp4" -f null -
    "$fixture" "$binary" "$scratch/gpu-reconfigured.mp4" --gpu-reconfigure >"$scratch/gpu-reconfigured.log" 2>&1 ||
        { cat "$scratch/gpu-reconfigured.log"; exit 1; }
    grep -q 'capture: source configuration changed' "$scratch/gpu-reconfigured.log"
    ffmpeg -v error -i "$scratch/gpu-reconfigured.mp4" -f null -
    "$fixture" "$binary" "$scratch/gpu-aux.mkv" --gpu-aux
    ffmpeg -v error -i "$scratch/gpu-aux.mkv" -map 0 -f null -
    "$fixture" "$binary" "$scratch/gpu-replay.mkv" --gpu-replay
    ffmpeg -v error -i "$scratch/gpu-replay.mkv" -map 0 -f null -
    ffmpeg -v error -i "$scratch/gpu-replay.mkv.second.mkv" -map 0 -f null -
    "$fixture" "$binary" "$scratch/gpu-replay-fixate.mkv" --gpu-replay-fixate
    ffmpeg -v error -i "$scratch/gpu-replay-fixate.mkv" -map 0 -f null -
    ffmpeg -v error -i "$scratch/gpu-replay-fixate.mkv.second.mkv" -map 0 -f null -
    "$fixture" "$binary" "$scratch/gpu-replay-portrait.mkv" --gpu-replay-portrait
    ffmpeg -v error -i "$scratch/gpu-replay-portrait.mkv" -map 0 -f null -
    ffmpeg -v error -i "$scratch/gpu-replay-portrait.mkv.second.mkv" -map 0 -f null -
    "$fixture" "$binary" "$scratch/gpu-replay-niri.mkv" --gpu-replay-niri
    ffmpeg -v error -i "$scratch/gpu-replay-niri.mkv" -map 0 -f null -
    ffmpeg -v error -i "$scratch/gpu-replay-niri.mkv.second.mkv" -map 0 -f null -
    "$fixture" "$binary" "$scratch/gpu-portal-replay.mkv" --gpu-portal-replay
    ffmpeg -v error -i "$scratch/gpu-portal-replay.mkv" -map 0 -f null -
    ffmpeg -v error -i "$scratch/gpu-portal-replay.mkv.second.mkv" -map 0 -f null -
    "$fixture" "$binary" "$scratch/gpu-portal-replay-static.mkv" --gpu-portal-replay-static
    ffmpeg -v error -i "$scratch/gpu-portal-replay-static.mkv" -map 0 -f null -
    ffmpeg -v error -i "$scratch/gpu-portal-replay-static.mkv.second.mkv" -map 0 -f null -
    # The source can require a non-power-of-two pool. Preferences must preserve
    # its supported domain while the token ring uses an independent capacity.
    GPUKEY_FIXTURE_BUFFER_MIN=3 GPUKEY_FIXTURE_RATE=60 GPUKEY_FIXTURE_SIZE=320x240 \
        GPUKEY_FIXTURE_REPORT="$scratch/three-buffers.json" \
        "$fixture" "$binary" "$scratch/three-buffers.mkv" --gpu-portal-replay
    grep -q '"peak_source_buffers": 3' "$scratch/three-buffers.json"
    cat >"$scratch/serial" <<'SH'
#!/usr/bin/env bash
if [[ ${1:-} = save ]]; then exec "$GPUKEY_TEST_BINARY" "$@"; fi
exec "$GPUKEY_TEST_BINARY" "$@" --encode-depth 1 --capture-buffers 3
SH
    chmod 700 "$scratch/serial"
    GPUKEY_TEST_BINARY="$binary" GPUKEY_FIXTURE_RATE=60 GPUKEY_FIXTURE_SIZE=320x240 \
        GPUKEY_FIXTURE_PATTERN=scene GPUKEY_FIXTURE_STATS="$scratch/serial.json" \
        GPUKEY_FIXTURE_REPORT="$scratch/serial-producer.json" \
        "$fixture" "$scratch/serial" "$scratch/serial.mkv" --gpu-portal-replay
    grep -q '"encoder_inputs": 1, "encode_depth": 1' "$scratch/serial.json"
    grep -q '"peak_source_buffers": 3' "$scratch/serial-producer.json"
    ffmpeg -v error -i "$scratch/serial.mkv.second.mkv" -map 0 -f null -
fi
echo "integration: private PipeWire source -> recorder -> independent decoder passed"

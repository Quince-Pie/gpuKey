#!/usr/bin/env bash
# One full private portal/PipeWire/AV1/replay lifecycle with sampled DRM memory.
# Run in nix develop; pass BINARY FIXTURE NEW_OUTPUT_DIR [FIXTURE_MODE].
# GPUKEY_FIXTURE_SIZE/RATE/SECONDS select video-only benchmark inputs.
# GPUKEY_SURFACES/ENCODE_DEPTH/CAPTURE_BUFFERS override only the tested policy.
set -euo pipefail
binary=$(realpath "$1")
fixture=$(realpath "$2")
output=$3
mode=${4:---gpu-portal-replay}
mkdir "$output"
output=$(realpath "$output")
runtime=$(mktemp -d)
server=
cleanup() {
    if [[ -n "$server" ]]; then kill "$server" 2>/dev/null || true; wait "$server" 2>/dev/null || true; fi
    rm -rf "$runtime"
}
trap cleanup EXIT
export PIPEWIRE_RUNTIME_DIR="$runtime" PIPEWIRE_REMOTE=gpukey-test XDG_RUNTIME_DIR="$runtime"
export GPUKEY_MEMORY_BINARY="$binary" GPUKEY_MEMORY_OUTPUT="$output"
export GPUKEY_MEMORY_TOOL="$PWD/tools/measure-gpu-memory.py"
pipewire -c "$PWD/tests/pipewire.conf" >"$output/server.log" 2>&1 &
server=$!
for ((i=0;i<100;i++)); do [[ -S "$runtime/gpukey-test" ]] && break; sleep 0.02; done
[[ -S "$runtime/gpukey-test" ]]
sha256sum "$binary" "$fixture" >"$output/binaries.sha256"
printf '%s\n' "${GPUKEY_MEMORY_LIBVA_DRIVERS_PATH:-system default}" >"$output/driver.txt"
cat >"$output/record" <<'SH'
#!/usr/bin/env bash
set -euo pipefail
if [[ ${1:-} = save ]]; then exec "$GPUKEY_MEMORY_BINARY" "$@"; fi
if [[ -n ${GPUKEY_MEMORY_LIBVA_DRIVERS_PATH:-} ]]; then
    export LIBVA_DRIVERS_PATH="$GPUKEY_MEMORY_LIBVA_DRIVERS_PATH"
fi
extra=(--fps "${GPUKEY_FIXTURE_RATE:-120}")
[[ -n ${GPUKEY_SURFACES:-} ]] && extra+=(--surfaces "$GPUKEY_SURFACES")
[[ -n ${GPUKEY_ENCODE_DEPTH:-} ]] && extra+=(--encode-depth "$GPUKEY_ENCODE_DEPTH")
[[ -n ${GPUKEY_CAPTURE_BUFFERS:-} ]] && extra+=(--capture-buffers "$GPUKEY_CAPTURE_BUFFERS")
[[ -n ${GPUKEY_REPLAY_STORAGE:-} ]] && extra+=(--replay-storage "$GPUKEY_REPLAY_STORAGE")
[[ -n ${GPUKEY_PACKET_READOUT:-} ]] && extra+=(--packet-readout "$GPUKEY_PACKET_READOUT")
exec python3 "$GPUKEY_MEMORY_TOOL" --output "$GPUKEY_MEMORY_OUTPUT/memory.json" --observe "$PPID" -- \
    "$GPUKEY_MEMORY_BINARY" "$@" "${extra[@]}"
SH
chmod 700 "$output/record"
GPUKEY_FIXTURE_STATS="$output/recording.json" GPUKEY_FIXTURE_REPORT="$output/producer.json" \
    "$fixture" "$output/record" "$output/clip.mkv" "$mode" >"$output/recording.log" 2>&1
jq -e '.success and .replay_saved == 2 and .replay_failed == 0' "$output/recording.json" >/dev/null
jq -e '.returncode == 0 and (.samples | length) > 0' "$output/memory.json" >/dev/null
sha256sum -c "$output/binaries.sha256" >"$output/binaries-verified.log"
jq '[.samples[] | [.clients[]."total-vram"] | add] | {min:min,max:max}' "$output/memory.json"

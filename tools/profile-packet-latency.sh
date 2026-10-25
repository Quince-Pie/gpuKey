#!/usr/bin/env bash
# Run in nix develop. Trace public encoder API timing in a private recorder.
# BINARY FIXTURE TRACE_LIBRARY NEW_OUTPUT DRIVER_DIRECTORY READOUT [FIXTURE_MODE]
set -euo pipefail
binary=$(realpath "$1") fixture=$(realpath "$2") tracer=$(realpath "$3")
output=$4 driver=$(realpath "$5") readout=$6 mode=${7:---gpu-portal-replay-static}
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
export GPUKEY_TRACE_BINARY="$binary" GPUKEY_TRACE_LIBRARY="$tracer"
export GPUKEY_TRACE_DRIVER="$driver" GPUKEY_TRACE_READOUT="$readout"
pipewire -c "$PWD/tests/pipewire.conf" >"$output/server.log" 2>&1 &
server=$!
for ((i=0;i<100;i++)); do [[ -S "$runtime/gpukey-test" ]] && break; sleep 0.02; done
[[ -S "$runtime/gpukey-test" ]]
cat >"$output/record" <<'SH'
#!/usr/bin/env bash
set -euo pipefail
if [[ ${1:-} = save ]]; then exec "$GPUKEY_TRACE_BINARY" "$@"; fi
export LIBVA_DRIVERS_PATH="$GPUKEY_TRACE_DRIVER/lib/dri:/run/opengl-driver/lib/dri"
args=("$@" --packet-readout "$GPUKEY_TRACE_READOUT")
[[ -n ${GPUKEY_TRACE_BACKEND:-} ]] && args+=(--backend "$GPUKEY_TRACE_BACKEND")
[[ -n ${GPUKEY_TRACE_DEVICE:-} ]] && args+=(--device "$GPUKEY_TRACE_DEVICE")
exec env LD_PRELOAD="$GPUKEY_TRACE_LIBRARY" "$GPUKEY_TRACE_BINARY" "${args[@]}"
SH
chmod 700 "$output/record"
sha256sum "$binary" "$fixture" "$tracer" "$driver/lib/dri/radeonsi_drv_video.so" >"$output/binaries.sha256"
GPUKEY_FIXTURE_STATS="$output/recording.json" GPUKEY_FIXTURE_REPORT="$output/producer.json" \
    "$fixture" "$output/record" "$output/clip.mkv" "$mode" >"$output/recording.log" 2>&1
if grep -qE 'PACKET_LATENCY_(UNMATCHED|ABANDONED)' "$output/recording.log"; then
    printf 'Unmatched or abandoned packet latency trace\n' >&2
    exit 1
fi
grep '^PACKET_LATENCY ' "$output/recording.log" >"$output/latencies.txt"
[[ $(wc -l <"$output/latencies.txt") = $(jq .packets "$output/recording.json") ]]
sha256sum -c "$output/binaries.sha256" >"$output/binaries-verified.log"

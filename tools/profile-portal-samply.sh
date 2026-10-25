#!/usr/bin/env bash
# Run in nix develop. Launch a private recorder; never SIGSTOP an interactive job.
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
    if [[ -n "$server" ]]; then
        kill "$server" 2>/dev/null || true
        wait "$server" 2>/dev/null || true
    fi
    rm -rf "$runtime"
}
trap cleanup EXIT
export PIPEWIRE_RUNTIME_DIR="$runtime" PIPEWIRE_REMOTE=gpukey-test XDG_RUNTIME_DIR="$runtime"
export GPUKEY_PROFILE_BINARY="$binary" GPUKEY_PROFILE_OUTPUT="$output"
export GPUKEY_SAMPLER=${GPUKEY_SAMPLER:-samply}
pipewire -c "$PWD/tests/pipewire.conf" >"$output/server.log" 2>&1 &
server=$!
for ((i=0;i<100;i++)); do [[ -S "$runtime/gpukey-test" ]] && break; sleep 0.02; done
[[ -S "$runtime/gpukey-test" ]]
cat >"$output/record" <<'SH'
#!/usr/bin/env bash
set -euo pipefail
if [[ ${1:-} = save ]]; then
    exec "$GPUKEY_PROFILE_BINARY" "$@"
fi
if [[ -n ${GPUKEY_PROFILE_LIBVA_DRIVERS_PATH:-} ]]; then
    export LIBVA_DRIVERS_PATH="$GPUKEY_PROFILE_LIBVA_DRIVERS_PATH"
fi
# A sampler compatibility preload must not enter the measured application.
exec "$GPUKEY_SAMPLER" record --save-only --rate 1000 --unstable-presymbolicate \
    --output "$GPUKEY_PROFILE_OUTPUT/profile.json.gz" -- \
    env -u LD_PRELOAD "$GPUKEY_PROFILE_BINARY" "$@"
SH
chmod 700 "$output/record"
GPUKEY_FIXTURE_STATS="$output/recording.json" \
    "$fixture" "$output/record" "$output/clip.mkv" "$mode" >"$output/recording.log" 2>&1
gzip -cd "$output/profile.json.gz" | jq \
    '[.threads[] | {name, pid, tid, samples:.samples.length,
      sample_weight:(.samples.weight|add), cpu_delta_us:(.samples.threadCPUDelta|add)}]' \
    >"$output/sample-counts.json"
jq -e '.success and .replay_saved == 2 and .replay_failed == 0' "$output/recording.json" >/dev/null
ffmpeg -v error -i "$output/clip.mkv" -map 0 -f null -
ffmpeg -v error -i "$output/clip.mkv.second.mkv" -map 0 -f null -
sha256sum "$binary" "$fixture" >"$output/binaries.sha256"
echo "Samply private-portal recording and independent decoding passed: $output"

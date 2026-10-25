#!/usr/bin/env bash
# In nix develop: RECORDER FIXTURE RESOURCE_INTERPOSER NEW_DIRECTORY
# Deliberately fail restoration after a private-source backlog, then decode the
# valid recording prefix. This is a failure check, not a performance benchmark.
set -euo pipefail
binary=$(realpath "$1") fixture=$(realpath "$2") interposer=$(realpath "$3") output=$4
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
export GPUKEY_FAILURE_BINARY="$binary" GPUKEY_FAILURE_INTERPOSER="$interposer" GPUKEY_FAILURE_OUTPUT="$output"
unset GPUKEY_FIXTURE_RATE GPUKEY_FIXTURE_PATTERN GPUKEY_FIXTURE_BUFFER_MIN GPUKEY_FIXTURE_SECONDS
unset GPUKEY_FIXTURE_LATE_SAVES GPUKEY_FIXTURE_EARLY_SAVES GPUKEY_RESOURCE_STALL_REPEAT
cat >"$output/record" <<'SH'
#!/usr/bin/env bash
set -u
env LD_PRELOAD="$GPUKEY_FAILURE_INTERPOSER" GPUKEY_RESOURCE_TRACE="$GPUKEY_FAILURE_OUTPUT/trace.log" \
    GPUKEY_RESOURCE_STALL_AFTER=10 GPUKEY_RESOURCE_FAIL_RESTORE=1 \
    "$GPUKEY_FAILURE_BINARY" "$@" --gpu-memory compact
status=$?
printf '%s\n' "$status" >"$GPUKEY_FAILURE_OUTPUT/exit-status"
exit "$status"
SH
chmod 700 "$output/record"
pipewire -c "$PWD/tests/pipewire.conf" >"$output/server.log" 2>&1 &
server=$!
for ((i=0;i<100;i++)); do [[ -S "$runtime/gpukey-test" ]] && break; sleep 0.02; done
[[ -S "$runtime/gpukey-test" ]]
set +e
GPUKEY_FIXTURE_SIZE=2560x2880 GPUKEY_FIXTURE_STATS="$output/stats.json" \
    "$fixture" "$output/record" "$output/partial.mp4" --gpu >"$output/recording.log" 2>&1
fixture_status=$?
set -e
[[ $fixture_status = 1 && $(cat "$output/exit-status") = 1 ]]
grep -q 'restore encoder input capacity' "$output/recording.log"
if grep -qE 'ERROR: (AddressSanitizer|LeakSanitizer)|runtime error:' "$output/recording.log"; then
    printf 'Sanitizer failure in capacity cleanup\n' >&2
    exit 1
fi
jq -e '.success == false and .submitted >= 10 and .packets == .submitted' "$output/stats.json" >/dev/null
ffmpeg -v error -i "$output/partial.mp4" -map 0 -f null -
ffprobe -v error -count_frames -select_streams v:0 -show_entries stream=nb_read_frames \
    -of json "$output/partial.mp4" >"$output/decoded.json"
python3 - "$output" <<'PY'
import json, sys
from pathlib import Path
root=Path(sys.argv[1])
stats=json.loads((root/'stats.json').read_text())
decoded=json.loads((root/'decoded.json').read_text())
assert int(decoded['streams'][0]['nb_read_frames']) == stats['submitted']-stats['video_boundary_omitted']
PY
sha256sum "$binary" "$fixture" "$interposer" "$output/record" >"$output/binaries.sha256"
echo 'Injected restore failure: explicit error, clean teardown, all earlier accepted pictures decode.'

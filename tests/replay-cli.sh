#!/usr/bin/env bash
set -euo pipefail
binary=$1
scratch=$(mktemp -d)
recorder=
cleanup() {
    if [[ -n "$recorder" ]]; then
        kill -INT "$recorder" 2>/dev/null || true
        wait "$recorder" 2>/dev/null || true
    fi
    rm -rf "$scratch"
}
trap cleanup EXIT
export XDG_RUNTIME_DIR="$scratch"
cd "$scratch"
common=(--backend software --test-source pattern --test-audio --test-audio
        --size 64x48 --realtime --session test)
expect_error() {
    local expected=$1 status=0
    shift
    "$binary" "$@" >stdout 2>stderr || status=$?
    [[ $status = "$expected" ]]
}
expect_error 1 save --session test
grep -q "No running replay session" stderr
expect_error 2 replay 0
expect_error 2 replay --static-interval 3
expect_error 2 replay -o invalid.mkv
expect_error 2 replay --session ../invalid
expect_error 2 replay --replay-storage invalid
"$binary" replay 1 "${common[@]}" --duration 15 --directory clips --stats run.json >recording.log 2>&1 &
recorder=$!
for ((attempt=0; attempt<500; attempt++)); do
    grep -q "Replay 'test' ready" recording.log && break
    kill -0 "$recorder"
    sleep 0.02
done
grep -q "Replay 'test' ready" recording.log
[[ -d clips && $(stat -c %a gpukey) = 700 && $(stat -c %a gpukey/test.sock) = 600 ]]
expect_error 1 replay 1 "${common[@]}" --duration 0.1
grep -q "already running" stderr
sleep 2.4
"$binary" save "first clip" --session test >saved
[[ -s "first clip.mkv" ]]
grep -q "Saved " saved
expect_error 1 save wrong.mp4 --session test
[[ ! -e wrong.mp4 ]]
sha256sum "first clip.mkv" >before
expect_error 1 save "first clip.mkv" --session test
sha256sum -c before
expect_error 1 save absent/clip.mkv --session test
if grep -q "No running replay session" stderr; then
    printf 'Running replay session was not found\n' >&2
    exit 1
fi
sleep 1.2
"$binary" save --session test >saved
clips=(clips/gpukey-replay-*.mkv)
[[ ${#clips[@]} = 1 ]]
kill -INT "$recorder"
wait "$recorder"
recorder=
frames=$(sed -n 's/.*"submitted": \([0-9]*\).*/\1/p' run.json)
[[ $frames -lt 720 ]] # The stop signal, not the safety duration, ended the session.
[[ ! -S gpukey/test.sock ]]
grep -q '"replay_saved": 2, "replay_failed": 2' run.json
grep -q '"replay_copied_bytes": 0' run.json
for clip in "first clip.mkv" "${clips[0]}"; do
    [[ $(ffprobe -v error -show_entries stream=index -of csv=p=0 "$clip" | wc -l) = 3 ]]
    ffmpeg -v error -i "$clip" -map 0 -f null -
done
start=$(ffprobe -v error -show_entries format_tags=GPUKEY_REPLAY_START_NS -of default=nw=1:nk=1 "${clips[0]}")
[[ $start -ge 2000000000 ]]
# Reopening the same session does not split ownership across a replaced lock inode.
"$binary" replay 1 "${common[@]}" --replay-storage reference --duration 15 --stats restart.json >restart.log 2>&1 &
recorder=$!
for ((attempt=0; attempt<500; attempt++)); do
    grep -q "Replay 'test' ready" restart.log && break
    kill -0 "$recorder"
    sleep 0.02
done
grep -q "Replay 'test' ready" restart.log
sleep 0.1
kill -TERM "$recorder"
wait "$recorder"
recorder=
frames=$(sed -n 's/.*"submitted": \([0-9]*\).*/\1/p' restart.json)
[[ $frames -lt 720 ]]
grep -q '"replay_shared_keyframes": 0, "replay_shared_key_bytes": 0' restart.json
expect_error 1 save --session test
echo 'replay cli: repeated saves, private trigger, session collision, failures, stop and restart pass'

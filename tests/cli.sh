#!/usr/bin/env bash
set -euo pipefail
binary=$1
scratch=$(mktemp -d)
trap 'rm -rf "$scratch"' EXIT
cd "$scratch"
common=(--backend software --test-source static --size 64x48 --frames 6 --fps 60000/1001)
expect_error() {
    local expected=$1
    shift
    local status=0
    "$binary" "$@" >stdout 2>stderr || status=$?
    [[ $status = "$expected" ]]
}
"$binary" --help >help
grep -q -- '--audio' help
grep -q -- '--display' help
"$binary" --help-all >all
grep -q -- '--output-buffer-mib' all
grep -q -- '--video-clock' all
grep -q -- '--encode-depth' all
grep -q -- '--packet-readout' all
grep -q -- '--static-encoding' all
grep -q -- '--gpu-memory' all
"$binary" devices --help >devices
grep -q 'does not start capture' devices
expect_error 2 devices extra
expect_error 2 --fps 0
grep -q -- 'Invalid --fps' stderr
expect_error 2 --fps 60000/0
expect_error 2 --capture-buffers 1
expect_error 2 --surfaces 1 --encode-depth 2
expect_error 2 --surfaces 1 --backend software
expect_error 2 --encode-depth 1 --backend software
expect_error 2 --gpu-memory invalid
grep -q -- 'Invalid --gpu-memory' stderr
expect_error 2 --gpu-memory compact --backend software
grep -q -- 'vaapi or vulkan' stderr
expect_error 2 --video-clock invalid
grep -q -- 'Invalid --video-clock' stderr
expect_error 2 --packet-readout invalid
grep -q -- 'Invalid --packet-readout' stderr
expect_error 2 --static-encoding invalid
grep -q -- 'Invalid --static-encoding' stderr
expect_error 2 --test-source static --video-clock source
expect_error 2 a.mkv --container mp4
expect_error 2 --display --window
expect_error 2 a.mp4 b.mp4
expect_error 2 a.mp4 --test-source static --audio selected-device
expect_error 2 a.mp4 --audio-codec invalid
expect_error 2 a.mp4 --audio-codec opus --test-source static --test-audio

# Everyday positional output and extension inference; two independently decoded tracks.
"$binary" "two tracks.mkv" "${common[@]}" --test-audio --test-audio >stdout 2>stderr
[[ $(ffprobe -v error -select_streams a -show_entries stream=codec_name -of csv=p=0 "two tracks.mkv" | wc -l) = 2 ]]
[[ $(ffprobe -v error -select_streams a:0 -show_entries stream=codec_name -of csv=p=0 "two tracks.mkv") = opus ]]
for track in 0 1; do
    [[ $(ffmpeg -v error -i "two tracks.mkv" -map "0:a:$track" -f f32le - | wc -c) = 38440 ]]
done

# The original explicit output flag still works; existing bytes survive failure.
"$binary" -o original.mp4 "${common[@]}" --static-encoding always >stdout 2>stderr
sha256sum original.mp4 >before
expect_error 1 -o original.mp4 "${common[@]}"
sha256sum -c before >stdout

# No output argument uses a dated filename in the current directory.
"$binary" "${common[@]}" >stdout 2>stderr
dated=(gpukey-*.mp4)
[[ ${#dated[@]} = 1 && -f ${dated[0]} ]]
ffmpeg -v error -i "${dated[0]}" -f null -
"$binary" "${common[@]}" --test-audio --audio-codec pcm >stdout 2>stderr
dated_audio=(gpukey-*.mkv)
[[ ${#dated_audio[@]} = 1 && -f ${dated_audio[0]} ]]
[[ $(ffprobe -v error -select_streams a:0 -show_entries stream=codec_name -of csv=p=0 "${dated_audio[0]}") = pcm_f32le ]]
printf 'cli: positional/default output, format inference, repeatable audio, rational FPS, errors and nonoverwrite pass\n'

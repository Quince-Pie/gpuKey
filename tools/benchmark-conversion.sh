#!/usr/bin/env bash
# In nix develop: NEW_DIRECTORY REPETITIONS DRIVER NAME BINARY [NAME BINARY ...]
set -euo pipefail
output=$1 repetitions=$2 driver=$(realpath "$3")
shift 3
[[ $repetitions =~ ^[1-9][0-9]*$ ]]
(( $# > 0 && $# % 2 == 0 ))
names=() binaries=()
while (( $# )); do
    [[ $1 =~ ^[a-z][a-z0-9]*$ ]]
    names+=("$1") binaries+=("$(realpath "$2")")
    shift 2
done
mkdir "$output"
output=$(realpath "$output")
export LIBVA_DRIVERS_PATH="$driver/lib/dri:/run/opengl-driver/lib/dri"
sha256sum "${binaries[@]}" "$driver/lib/dri/radeonsi_drv_video.so" >"$output/binaries.sha256"
read -r -a cases <<<"${GPUKEY_CONVERSION_CASES:-small hd portrait odd hd10}"
for ((run=1;run<=repetitions;++run)); do
    for ((step=0;step<${#cases[@]};++step)); do
        ci=$step
        ((run%2)) || ci=$((${#cases[@]}-1-step))
        name=${cases[ci]} bits=8
        case $name in
            small) size=320x240;;
            hd) size=1920x1080;;
            portrait) size=2560x2880;;
            odd) size=641x481;;
            hd10) size=1920x1080 bits=10;;
            *) exit 2;;
        esac
        for ((v=0;v<${#names[@]};++v)); do
            vi=$(((v+run-1)%${#names[@]}))
            directory="$output/$name-${names[vi]}-$run"
            mkdir "$directory"
            printf '%s %s %s\n' "$name" "${names[vi]}" "$run"
            python3 tools/measure-gpu-memory.py --output "$directory/memory.json" -- \
                "${binaries[vi]}" --test-source pattern --backend vaapi --size "$size" \
                --bits "$bits" --fps 60 --frames 240 --output "$directory/clip.mkv" \
                --stats "$directory/recording.json" >"$directory/recording.log" 2>&1
            jq -e '.success and .submitted == 240 and .packets == 240 and
                .accepted_bytes == .written_bytes' "$directory/recording.json" >/dev/null
            printf '%s\n' '{"kind":"inprocess_synthetic","produced":240,"empty_processes":0,"coalesced_ticks":0,"peak_source_bytes":0}' \
                >"$directory/producer.json"
        done
    done
done
sha256sum -c "$output/binaries.sha256" >"$output/binaries-verified.log"
# Decoding/quality analysis is intentionally performed after timing.
echo 'Timed conversion/upload/encode lifetimes complete; evaluate decoded quality separately.'

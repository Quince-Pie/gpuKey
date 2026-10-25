#!/usr/bin/env bash
# In nix develop: FIXTURE NEW_DIRECTORY DRIVER REPETITIONS NAME BINARY [NAME BINARY ...]
# Rotate complete recorder lifetimes; do not overlap with builds or profilers.
set -euo pipefail
fixture=$(realpath "$1") root=$2 driver=$(realpath "$3") repetitions=$4
shift 4
[[ $repetitions =~ ^[1-9][0-9]*$ ]]
(( $# > 0 && $# % 2 == 0 ))
names=() binaries=()
while (( $# )); do
    [[ $1 =~ ^[a-z][a-z0-9]*$ ]]
    names+=("$1") binaries+=("$(realpath "$2")")
    shift 2
done
resume=${GPUKEY_PROGRAM_RESUME:-0}
if [[ $resume = 1 ]]; then
    [[ -d $root ]]
else
    mkdir "$root"
fi
root=$(realpath "$root")
export GPUKEY_MEMORY_LIBVA_DRIVERS_PATH="$driver/lib/dri:/run/opengl-driver/lib/dri"
if [[ $resume = 1 ]]; then
    sha256sum -c "$root/binaries.sha256" >"$root/resume-binaries-verified.log"
else
    sha256sum "$fixture" "${binaries[@]}" "$driver/lib/dri/radeonsi_drv_video.so" >"$root/binaries.sha256"
fi
read -r -a cases <<<"${GPUKEY_PROGRAM_CASES:-static scene60 scene120 hd240 hd1000}"
config=$(printf '%s\n' "repetitions=$repetitions" "paced_seconds=${GPUKEY_PROGRAM_SECONDS:-4}" \
    "saturated_seconds=${GPUKEY_PROGRAM_SATURATED_SECONDS:-10}" "cases=${cases[*]}" \
    "fixture=$fixture" "driver=$driver" "buffer_min=${GPUKEY_FIXTURE_BUFFER_MIN:-}" \
    "libva_driver_name=${LIBVA_DRIVER_NAME:-}")
if [[ $resume = 1 ]]; then
    [[ -f $root/workload.txt && $(cat "$root/workload.txt") = "$config" ]]
else
    printf '%s\n' "$config" >"$root/workload.txt"
fi
for ((run=1;run<=repetitions;++run)); do
    for ((step=0;step<${#cases[@]};++step)); do
        index=$step
        ((run%2)) || index=$((${#cases[@]}-1-step))
        scenario=${cases[index]}
        export GPUKEY_FIXTURE_SIZE=2560x2880 GPUKEY_FIXTURE_RATE=60
        export GPUKEY_FIXTURE_SECONDS=${GPUKEY_PROGRAM_SECONDS:-4}
        unset GPUKEY_FIXTURE_PATTERN GPUKEY_FIXTURE_LATE_SAVES GPUKEY_FIXTURE_EARLY_SAVES
        unset GPUKEY_CAPTURE_BUFFERS GPUKEY_SURFACES GPUKEY_ENCODE_DEPTH GPUKEY_REPLAY_STORAGE GPUKEY_PACKET_READOUT
        mode=--gpu-portal-replay
        case $scenario in
            static) mode=--gpu-portal-replay-static;;
            scene60) export GPUKEY_FIXTURE_PATTERN=scene;;
            scene120) export GPUKEY_FIXTURE_PATTERN=scene GPUKEY_FIXTURE_RATE=120
                      export GPUKEY_FIXTURE_SECONDS=${GPUKEY_PROGRAM_SATURATED_SECONDS:-10};;
            hd240) export GPUKEY_FIXTURE_PATTERN=scene GPUKEY_FIXTURE_RATE=240 GPUKEY_FIXTURE_SIZE=1920x1080;;
            hd1000) export GPUKEY_FIXTURE_PATTERN=scene GPUKEY_FIXTURE_RATE=1000 GPUKEY_FIXTURE_SIZE=1920x1080
                    export GPUKEY_FIXTURE_SECONDS=${GPUKEY_PROGRAM_SATURATED_SECONDS:-10};;
            *) exit 2;;
        esac
        for ((v=0;v<${#names[@]};++v)); do
            vi=$(((v+run-1)%${#names[@]}))
            output="$root/$scenario-${names[vi]}-$run"
            if [[ $resume = 1 && -d $output ]]; then
                jq -e '.success and .replay_saved == 2 and .replay_failed == 0' "$output/recording.json" >/dev/null
                jq -e '.returncode == 0 and (.samples | length) > 0' "$output/memory.json" >/dev/null
                jq -e '.produced > 0' "$output/producer.json" >/dev/null
                sha256sum -c "$output/binaries.sha256" >"$output/resume-binaries-verified.log"
                printf '%s %s %s (verified existing)\n' "$scenario" "${names[vi]}" "$run"
                continue
            fi
            printf '%s %s %s\n' "$scenario" "${names[vi]}" "$run"
            if ! bash tools/benchmark-gpu-memory.sh "${binaries[vi]}" "$fixture" "$output" "$mode" \
                >"$root/last.log" 2>&1; then
                cat "$root/last.log"
                tail -n 40 "$output/recording.log"
                exit 1
            fi
        done
    done
done
sha256sum -c "$root/binaries.sha256" >"$root/binaries-verified.log"
echo 'Complete native recorder lifetimes measured; independent profiles/analysis follow separately.'

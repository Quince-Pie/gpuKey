#!/usr/bin/env bash
# BINARY FIXTURE NEW_OUTPUT REPETITIONS NAME DRIVER [NAME DRIVER ...]
# One executable and workload contract; rotate driver order on complete runs.
set -euo pipefail
binary=$(realpath "$1") fixture=$(realpath "$2") root=$3 repetitions=$4
shift 4
[[ $repetitions =~ ^[1-9][0-9]*$ ]]
(( $# > 0 && $# % 2 == 0 ))
names=() drivers=()
while (( $# )); do
    [[ $1 =~ ^[a-z][a-z0-9]*$ ]]
    names+=("$1") drivers+=("$(realpath "$2")")
    shift 2
done
read -r -a scenarios <<<"${GPUKEY_DRIVER_CASES:-static scene60 scene120 hd240 hd1000}"
for scenario in "${scenarios[@]}"; do
    case $scenario in static|scene60|scene120|hd240|hd1000) ;; *) exit 2;; esac
done
mkdir "$root"
root=$(realpath "$root")
sha256sum "$binary" "$fixture" >"$root/binaries.sha256"
for driver in "${drivers[@]}"; do
    sha256sum "$driver/lib/dri/radeonsi_drv_video.so" >>"$root/binaries.sha256"
done
for ((run=1; run<=repetitions; ++run)); do
    for ((step=0; step<${#scenarios[@]}; ++step)); do
        index=$step
        ((run % 2)) || index=$((${#scenarios[@]}-1-step))
        scenario=${scenarios[index]}
        export GPUKEY_FIXTURE_SIZE=2560x2880 GPUKEY_FIXTURE_RATE=60
        export GPUKEY_FIXTURE_SECONDS=${GPUKEY_DRIVER_SECONDS:-4}
        unset GPUKEY_FIXTURE_PATTERN GPUKEY_FIXTURE_LATE_SAVES GPUKEY_FIXTURE_EARLY_SAVES
        unset GPUKEY_CAPTURE_BUFFERS GPUKEY_SURFACES GPUKEY_ENCODE_DEPTH GPUKEY_REPLAY_STORAGE GPUKEY_PACKET_READOUT
        mode=--gpu-portal-replay
        case $scenario in
            static) mode=--gpu-portal-replay-static;;
            scene60) export GPUKEY_FIXTURE_PATTERN=scene;;
            scene120) export GPUKEY_FIXTURE_PATTERN=scene GPUKEY_FIXTURE_RATE=120
                      export GPUKEY_FIXTURE_SECONDS=${GPUKEY_DRIVER_SATURATED_SECONDS:-10};;
            hd240) export GPUKEY_FIXTURE_PATTERN=scene GPUKEY_FIXTURE_RATE=240 GPUKEY_FIXTURE_SIZE=1920x1080;;
            hd1000) export GPUKEY_FIXTURE_PATTERN=scene GPUKEY_FIXTURE_RATE=1000 GPUKEY_FIXTURE_SIZE=1920x1080
                    export GPUKEY_FIXTURE_SECONDS=${GPUKEY_DRIVER_SATURATED_SECONDS:-10};;
        esac
        for ((v=0; v<${#names[@]}; ++v)); do
            index=$(((v+run-1)%${#names[@]}))
            variant=${names[index]}
            export GPUKEY_MEMORY_LIBVA_DRIVERS_PATH="${drivers[index]}/lib/dri:/run/opengl-driver/lib/dri"
            output="$root/$scenario-$variant-$run"
            printf '%s %s %s\n' "$scenario" "$variant" "$run"
            if ! bash tools/benchmark-gpu-memory.sh "$binary" "$fixture" "$output" "$mode" \
                >"$root/last.log" 2>&1; then
                cat "$root/last.log"
                tail -n 40 "$output/recording.log"
                exit 1
            fi
        done
    done
done
sha256sum -c "$root/binaries.sha256" >"$root/binaries-verified.log"

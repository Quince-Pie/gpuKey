#!/usr/bin/env bash
# Matched complete captures; keep builds and profilers outside these runs.
# BINARY FIXTURE NEW_DIR CONTROL_DRIVER VIEWS_DRIVER PAIRED_DRIVER [REPETITIONS]
set -euo pipefail
binary=$(realpath "$1")
fixture=$(realpath "$2")
root=$3
drivers=("$(realpath "$4")" "$(realpath "$5")" "$(realpath "$6")")
repetitions=${7:-5}
[[ $repetitions =~ ^[1-9][0-9]*$ ]]
read -r -a selected <<<"${GPUKEY_VPP_VARIANTS:-control views paired}"
for variant in "${selected[@]}"; do
    [[ $variant = control || $variant = views || $variant = paired ]]
done
read -r -a scenarios <<<"${GPUKEY_VPP_CASES:-static scene60 scene120 hd240}"
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
    for ((case_index=0; case_index<${#scenarios[@]}; ++case_index)); do
        ordered=$case_index
        ((run % 2)) || ordered=$((${#scenarios[@]}-1-case_index))
        scenario=${scenarios[ordered]}
        export GPUKEY_FIXTURE_SIZE=2560x2880 GPUKEY_FIXTURE_RATE=60
        export GPUKEY_FIXTURE_SECONDS=${GPUKEY_VPP_SECONDS:-4}
        unset GPUKEY_FIXTURE_PATTERN GPUKEY_FIXTURE_LATE_SAVES
        unset GPUKEY_CAPTURE_BUFFERS GPUKEY_SURFACES GPUKEY_ENCODE_DEPTH GPUKEY_REPLAY_STORAGE
        mode=--gpu-portal-replay
        case $scenario in
            static) mode=--gpu-portal-replay-static;;
            scene60) export GPUKEY_FIXTURE_PATTERN=scene;;
            scene120) export GPUKEY_FIXTURE_PATTERN=scene GPUKEY_FIXTURE_RATE=120
                      export GPUKEY_FIXTURE_SECONDS=${GPUKEY_VPP_SATURATED_SECONDS:-4};;
            hd240) export GPUKEY_FIXTURE_PATTERN=scene GPUKEY_FIXTURE_RATE=240 GPUKEY_FIXTURE_SIZE=1920x1080;;
            hd1000) export GPUKEY_FIXTURE_PATTERN=scene GPUKEY_FIXTURE_RATE=1000 GPUKEY_FIXTURE_SIZE=1920x1080
                    export GPUKEY_FIXTURE_SECONDS=${GPUKEY_VPP_SATURATED_SECONDS:-4};;
        esac
        for ((step=0; step<${#selected[@]}; ++step)); do
            index=$(((step+run-1)%${#selected[@]}))
            variant=${selected[index]}
            case $variant in control) index=0;; views) index=1;; paired) index=2;; esac
            export GPUKEY_MEMORY_LIBVA_DRIVERS_PATH="${drivers[index]}/lib/dri:/run/opengl-driver/lib/dri"
            output="$root/$scenario-$variant-$run"
            printf '%s %s %s\n' "$scenario" "$variant" "$run"
            if ! bash tools/benchmark-gpu-memory.sh "$binary" "$fixture" "$output" "$mode" \
                >"$root/last.log" 2>&1; then
                cat "$root/last.log"
                tail -40 "$output/recording.log"
                exit 1
            fi
        done
    done
done
sha256sum -c "$root/binaries.sha256" >"$root/binaries-verified.log"

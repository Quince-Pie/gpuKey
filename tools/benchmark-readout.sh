#!/usr/bin/env bash
# Matched complete recording lifetimes, with no profiler or concurrent build.
# CONTROL CANDIDATE FIXTURE NEW_DIR CONTROL_DRIVER EVENT_DRIVER [REPETITIONS]
set -euo pipefail
control=$(realpath "$1") candidate=$(realpath "$2") fixture=$(realpath "$3")
root=$4 old_driver=$(realpath "$5") event_driver=$(realpath "$6")
repetitions=${7:-4}
[[ $repetitions =~ ^[1-9][0-9]*$ ]]
read -r -a variants <<<"${GPUKEY_READOUT_VARIANTS:-control events}"
read -r -a scenarios <<<"${GPUKEY_READOUT_CASES:-static scene60 scene120 hd240 hd1000}"
for variant in "${variants[@]}"; do
    case $variant in control|buffered|available|events) ;; *) exit 2;; esac
done
for scenario in "${scenarios[@]}"; do
    case $scenario in static|scene60|scene120|hd240|hd1000) ;; *) exit 2;; esac
done
mkdir "$root"
root=$(realpath "$root")
sha256sum "$control" "$candidate" "$fixture" \
    "$old_driver/lib/dri/radeonsi_drv_video.so" \
    "$event_driver/lib/dri/radeonsi_drv_video.so" >"$root/binaries.sha256"
for ((run=1;run<=repetitions;++run)); do
    for ((step=0;step<${#scenarios[@]};++step)); do
        index=$step
        ((run % 2)) || index=$((${#scenarios[@]}-1-step))
        scenario=${scenarios[index]}
        export GPUKEY_FIXTURE_SIZE=2560x2880 GPUKEY_FIXTURE_RATE=60
        export GPUKEY_FIXTURE_SECONDS=${GPUKEY_READOUT_SECONDS:-4}
        unset GPUKEY_FIXTURE_PATTERN GPUKEY_FIXTURE_LATE_SAVES GPUKEY_FIXTURE_EARLY_SAVES
        unset GPUKEY_CAPTURE_BUFFERS GPUKEY_SURFACES GPUKEY_ENCODE_DEPTH GPUKEY_REPLAY_STORAGE
        mode=--gpu-portal-replay
        case $scenario in
            static) mode=--gpu-portal-replay-static;;
            scene60) export GPUKEY_FIXTURE_PATTERN=scene;;
            scene120) export GPUKEY_FIXTURE_PATTERN=scene GPUKEY_FIXTURE_RATE=120
                      export GPUKEY_FIXTURE_SECONDS=${GPUKEY_READOUT_SATURATED_SECONDS:-10};;
            hd240) export GPUKEY_FIXTURE_PATTERN=scene GPUKEY_FIXTURE_RATE=240 GPUKEY_FIXTURE_SIZE=1920x1080;;
            hd1000) export GPUKEY_FIXTURE_PATTERN=scene GPUKEY_FIXTURE_RATE=1000 GPUKEY_FIXTURE_SIZE=1920x1080
                    export GPUKEY_FIXTURE_SECONDS=${GPUKEY_READOUT_SATURATED_SECONDS:-10};;
        esac
        for ((v=0;v<${#variants[@]};++v)); do
            variant=${variants[(v+run-1)%${#variants[@]}]}
            binary=$candidate driver=$event_driver
            export GPUKEY_PACKET_READOUT=$variant
            if [[ $variant = control ]]; then
                binary=$control driver=$old_driver
                unset GPUKEY_PACKET_READOUT
            fi
            export GPUKEY_MEMORY_LIBVA_DRIVERS_PATH="$driver/lib/dri:/run/opengl-driver/lib/dri"
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

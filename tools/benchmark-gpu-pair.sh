#!/usr/bin/env bash
# CONTROL CANDIDATE FIXTURE NEW_OUTPUT DRIVER_DIRECTORY [REPETITIONS]
# Compare complete lifetimes with one driver; keep builds/profilers separate.
set -euo pipefail
control=$(realpath "$1") candidate=$(realpath "$2") fixture=$(realpath "$3")
root=$4 driver=$(realpath "$5") repetitions=${6:-4}
[[ $repetitions =~ ^[1-9][0-9]*$ ]]
read -r -a scenarios <<<"${GPUKEY_PAIR_CASES:-static scene60 scene120 hd240 hd1000}"
for scenario in "${scenarios[@]}"; do
    case $scenario in static|scene60|scene120|hd240|hd1000|upload-small|upload-hd|upload-portrait) ;;
        *) exit 2;; esac
done
mkdir "$root"
root=$(realpath "$root")
export GPUKEY_MEMORY_LIBVA_DRIVERS_PATH="$driver/lib/dri:/run/opengl-driver/lib/dri"
sha256sum "$control" "$candidate" "$fixture" "$driver/lib/dri/radeonsi_drv_video.so" \
    >"$root/binaries.sha256"
for ((run=1; run<=repetitions; ++run)); do
    for ((step=0; step<${#scenarios[@]}; ++step)); do
        index=$step
        ((run % 2)) || index=$((${#scenarios[@]}-1-step))
        scenario=${scenarios[index]}
        export GPUKEY_FIXTURE_SIZE=2560x2880 GPUKEY_FIXTURE_RATE=60
        export GPUKEY_FIXTURE_SECONDS=${GPUKEY_PAIR_SECONDS:-4}
        unset GPUKEY_FIXTURE_PATTERN GPUKEY_FIXTURE_LATE_SAVES GPUKEY_FIXTURE_EARLY_SAVES
        unset GPUKEY_CAPTURE_BUFFERS GPUKEY_SURFACES GPUKEY_ENCODE_DEPTH GPUKEY_REPLAY_STORAGE GPUKEY_PACKET_READOUT
        mode=--gpu-portal-replay
        case $scenario in
            static) mode=--gpu-portal-replay-static;;
            scene60) export GPUKEY_FIXTURE_PATTERN=scene;;
            scene120) export GPUKEY_FIXTURE_PATTERN=scene GPUKEY_FIXTURE_RATE=120
                      export GPUKEY_FIXTURE_SECONDS=${GPUKEY_PAIR_SATURATED_SECONDS:-10};;
            hd240) export GPUKEY_FIXTURE_PATTERN=scene GPUKEY_FIXTURE_RATE=240 GPUKEY_FIXTURE_SIZE=1920x1080;;
            hd1000) export GPUKEY_FIXTURE_PATTERN=scene GPUKEY_FIXTURE_RATE=1000 GPUKEY_FIXTURE_SIZE=1920x1080
                    export GPUKEY_FIXTURE_SECONDS=${GPUKEY_PAIR_SATURATED_SECONDS:-10};;
            upload-small) export GPUKEY_FIXTURE_SIZE=320x240;;
            upload-hd) export GPUKEY_FIXTURE_SIZE=1920x1080;;
        esac
        order='control candidate'
        ((run % 2)) || order='candidate control'
        for variant in $order; do
            binary=$control
            [[ $variant = candidate ]] && binary=$candidate
            output="$root/$scenario-$variant-$run"
            printf '%s %s %s\n' "$scenario" "$variant" "$run"
            if [[ $scenario = upload-* ]]; then
                mkdir "$output"
                LIBVA_DRIVERS_PATH="$GPUKEY_MEMORY_LIBVA_DRIVERS_PATH" \
                    python3 tools/measure-gpu-memory.py --output "$output/memory.json" -- \
                    "$binary" --test-source pattern --backend vaapi --size "$GPUKEY_FIXTURE_SIZE" \
                    --fps 60 --frames 240 --output "$output/clip.mp4" --stats "$output/recording.json" \
                    >"$output/recording.log" 2>&1
                jq -e '.success and .submitted == 240 and .packets == 240 and
                    .accepted_bytes == .written_bytes' "$output/recording.json" >/dev/null
                # There is no external producer or GPU capture pool. CPU source
                # pixels, conversion, upload, encoding and output are all timed.
                printf '%s\n' '{"kind":"inprocess_synthetic","produced":240,"empty_processes":0,"coalesced_ticks":0,"peak_source_bytes":0}' \
                    >"$output/producer.json"
            elif ! bash tools/benchmark-gpu-memory.sh "$binary" "$fixture" "$output" "$mode" \
                >"$root/last.log" 2>&1; then
                cat "$root/last.log"
                tail -n 40 "$output/recording.log"
                exit 1
            fi
        done
    done
done
# Independent decoding stays outside the performance series.
for scenario in "${scenarios[@]}"; do
    [[ $scenario = upload-* ]] || continue
    for variant in control candidate; do
        stem="$root/$scenario-$variant-1"
        ffmpeg -v error -i "$stem/clip.mp4" -map 0:v -fps_mode passthrough -f framemd5 \
            "$stem/decoded.md5"
        [[ $(grep -c '^[0-9]' "$stem/decoded.md5") = 240 ]]
    done
    cmp "$root/$scenario-control-1/decoded.md5" "$root/$scenario-candidate-1/decoded.md5"
done
sha256sum -c "$root/binaries.sha256" >"$root/binaries-verified.log"

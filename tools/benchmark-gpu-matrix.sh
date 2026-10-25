#!/usr/bin/env bash
# Matched, alternating complete lifecycles. No concurrent profilers or builds.
# CONTROL CANDIDATE FIXTURE NEW_OUTPUT MODERN_DRIVER_DIR COMPACT_DRIVER_DIR [REPS] [MEDIA_DRIVER_DIR] [ACO_DRIVER_DIR]
set -euo pipefail
control=$(realpath "$1")
candidate=$(realpath "$2")
fixture=$(realpath "$3")
root=$4
modern=$(realpath "$5")
compact=$(realpath "$6")
repetitions=${7:-5}
media=${8:-}
aco=${9:-}
[[ -z $media ]] || media=$(realpath "$media")
[[ -z $aco ]] || aco=$(realpath "$aco")
read -r -a variants <<<"${GPUKEY_MATRIX_VARIANTS:-control application modern compact ${media:+media} ${aco:+aco}}"
for variant in "${variants[@]}"; do
    case "$variant" in
        control|application|modern|compact) ;;
        media) [[ -n $media ]];;
        aco) [[ -n $aco ]];;
        *) exit 2;;
    esac
done
[[ $repetitions =~ ^[1-9][0-9]*$ ]]
mkdir "$root"
root=$(realpath "$root")
sha256sum "$control" "$candidate" "$fixture" \
    "$modern/radeonsi_drv_video.so" "$compact/radeonsi_drv_video.so" >"$root/binaries.sha256"
[[ -z $media ]] || sha256sum "$media/radeonsi_drv_video.so" >>"$root/binaries.sha256"
[[ -z $aco ]] || sha256sum "$aco/radeonsi_drv_video.so" >>"$root/binaries.sha256"
for ((run=1; run<=repetitions; ++run)); do
    cases='static scene60 scene120 hd240'
    ((run % 2 == 0)) && cases='hd240 scene120 scene60 static'
    for case_name in $cases; do
        export GPUKEY_FIXTURE_SIZE=2560x2880 GPUKEY_FIXTURE_RATE=60
        unset GPUKEY_FIXTURE_PATTERN
        mode=--gpu-portal-replay
        case "$case_name" in
            static) mode=--gpu-portal-replay-static;;
            scene60) export GPUKEY_FIXTURE_PATTERN=scene;;
            scene120) export GPUKEY_FIXTURE_RATE=120 GPUKEY_FIXTURE_PATTERN=scene;;
            hd240) export GPUKEY_FIXTURE_SIZE=1920x1080 GPUKEY_FIXTURE_RATE=240 GPUKEY_FIXTURE_PATTERN=scene;;
        esac
        for ((j=0; j<${#variants[@]}; ++j)); do
            index=$(((j + run - 1) % ${#variants[@]}))
            ((run % 2)) || index=$((${#variants[@]} - 1 - index))
            variant=${variants[index]}
            unset GPUKEY_CAPTURE_BUFFERS GPUKEY_SURFACES GPUKEY_ENCODE_DEPTH GPUKEY_MEMORY_LIBVA_DRIVERS_PATH
            binary=$candidate
            case "$variant" in
                control) binary=$control;;
                modern) export GPUKEY_MEMORY_LIBVA_DRIVERS_PATH="$modern";;
                compact) export GPUKEY_MEMORY_LIBVA_DRIVERS_PATH="$compact";;
                media) export GPUKEY_MEMORY_LIBVA_DRIVERS_PATH="$media";;
                aco) export GPUKEY_MEMORY_LIBVA_DRIVERS_PATH="$aco";;
            esac
            out="$root/$case_name-$variant-$run"
            printf '%s %s %s\n' "$case_name" "$variant" "$run"
            if ! bash tools/benchmark-gpu-memory.sh "$binary" "$fixture" "$out" "$mode" \
                >"$root/last.log" 2>&1; then
                cat "$root/last.log"
                tail -50 "$out/recording.log"
                exit 1
            fi
        done
    done
done
sha256sum -c "$root/binaries.sha256" >"$root/binaries-verified.log"

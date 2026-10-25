#!/usr/bin/env bash
# In nix develop: gpukey|gsr BINARY FIXTURE NEW_DIRECTORY
# A common private portal source; no desktop capture or interactive job signals.
set -euo pipefail
app=$1 binary=$(realpath "$2") fixture=$(realpath "$3") output=$4
[[ $app = gpukey || $app = gsr ]]
mkdir "$output"
output=$(realpath "$output")
runtime=$(mktemp -d)
server= saver=
cleanup() {
    if [[ -n $saver ]]; then kill "$saver" 2>/dev/null || true; wait "$saver" 2>/dev/null || true; fi
    if [[ -n $server ]]; then kill "$server" 2>/dev/null || true; wait "$server" 2>/dev/null || true; fi
    rm -rf "$runtime"
}
trap cleanup EXIT
export GPUKEY_RECORDER_APP="$app" GPUKEY_RECORDER_BINARY="$binary"
export GPUKEY_RECORDER_OUTPUT="$output" GPUKEY_RECORDER_TOOL="$PWD/tools/measure-gpu-memory.py"
export GPUKEY_RECORDER_MODE=${GPUKEY_RECORDER_MODE:-record}
[[ $GPUKEY_RECORDER_MODE = record || $GPUKEY_RECORDER_MODE = replay ]]
export GPUKEY_RECORDER_DISPLAY=${DISPLAY:-}
export GPUKEY_RECORDER_WAYLAND=${WAYLAND_DISPLAY:-}
if [[ -n $GPUKEY_RECORDER_WAYLAND && $GPUKEY_RECORDER_WAYLAND != /* ]]; then
    export GPUKEY_RECORDER_WAYLAND="${XDG_RUNTIME_DIR:?}/$GPUKEY_RECORDER_WAYLAND"
fi
export PIPEWIRE_RUNTIME_DIR="$runtime" PIPEWIRE_REMOTE=gpukey-test XDG_RUNTIME_DIR="$runtime"
export GPUKEY_FIXTURE_SIZE=${GPUKEY_FIXTURE_SIZE:-2560x2880}
export GPUKEY_FIXTURE_RATE=${GPUKEY_FIXTURE_RATE:-60}
export GPUKEY_FIXTURE_SECONDS=${GPUKEY_FIXTURE_SECONDS:-8}
export GPUKEY_FIXTURE_PATTERN=${GPUKEY_FIXTURE_PATTERN:-scene}
unset GPUKEY_FIXTURE_LATE_SAVES GPUKEY_FIXTURE_EARLY_SAVES
if [[ -n ${GPUKEY_RECORDER_DRIVER:-} ]]; then
    driver=$(realpath "$GPUKEY_RECORDER_DRIVER")
    export LIBVA_DRIVERS_PATH="$driver/lib/dri" LIBGL_DRIVERS_PATH="$driver/lib/dri"
    export GBM_BACKENDS_PATH="$driver/lib/gbm"
    export __EGL_VENDOR_LIBRARY_FILENAMES="$driver/share/glvnd/egl_vendor.d/50_mesa.json"
    export LD_LIBRARY_PATH="$driver/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
    sha256sum "$driver/lib/dri/radeonsi_drv_video.so" > "$output/driver.sha256"
fi
cat > "$output/record" <<'SH'
#!/usr/bin/env bash
set -euo pipefail
output=$1 seconds=$2 width=$3 height=$4 fps=$5
export DISPLAY="$GPUKEY_RECORDER_DISPLAY" WAYLAND_DISPLAY="$GPUKEY_RECORDER_WAYLAND"
qp=${GPUKEY_RECORDER_QP:-32} depth=${GPUKEY_RECORDER_DEPTH:-2}
if [[ $GPUKEY_RECORDER_APP = gpukey ]]; then
    args=(--display --fps "$fps" --qp "$qp" --encode-depth "$depth" --container mkv
          --output "$output" --stats "$GPUKEY_RECORDER_OUTPUT/recording.json")
    if [[ $GPUKEY_RECORDER_MODE = replay ]]; then
        args=(replay 30 --display --fps "$fps" --qp "$qp" --encode-depth "$depth"
              --container mkv --session fixture --stats "$GPUKEY_RECORDER_OUTPUT/recording.json")
    fi
    if [[ -n ${GPUKEY_RECORDER_BACKEND:-} ]]; then
        args+=(--backend "$GPUKEY_RECORDER_BACKEND")
    fi
    if [[ -n ${GPUKEY_RECORDER_DEVICE:-} ]]; then
        args+=(--device "$GPUKEY_RECORDER_DEVICE")
    fi
    if [[ -n ${GPUKEY_RECORDER_STATIC_INTERVAL:-} ]]; then
        args+=(--static-interval "$GPUKEY_RECORDER_STATIC_INTERVAL")
    fi
    if [[ -n ${GPUKEY_RECORDER_STATIC_ENCODING:-} ]]; then
        args+=(--static-encoding "$GPUKEY_RECORDER_STATIC_ENCODING")
    fi
    if [[ -n ${GPUKEY_RECORDER_GPU_MEMORY:-} ]]; then
        args+=(--gpu-memory "$GPUKEY_RECORDER_GPU_MEMORY")
    fi
    if [[ -n ${GPUKEY_RECORDER_BITS:-} ]]; then
        args+=(--bits "$GPUKEY_RECORDER_BITS")
    fi
    if [[ -n ${GPUKEY_RECORDER_SHM:-} ]]; then
        args+=(--shm)
    fi
else
    target=$output
    [[ $GPUKEY_RECORDER_MODE = replay ]] && target=$GPUKEY_RECORDER_OUTPUT
    args=(-w portal -f "$fps" -k av1 -c matroska -o "$target" -bm qp -q ultra
          -fm "${GPUKEY_RECORDER_CADENCE:-content}" -cr limited -keyint 2 -cursor no
          -restore-portal-session no -fallback-cpu-encoding no -tune performance
          -ffmpeg-video-opts "qp=$qp;global_quality=$qp;async_depth=$depth;bf=0;color_trc=iec61966-2-1")
    if [[ $GPUKEY_RECORDER_MODE = replay ]]; then
        args+=(-r 30 -replay-storage ram -df no -ipc "$XDG_RUNTIME_DIR/gsr")
    fi
fi
observer=()
[[ -n ${GPUKEY_RECORDER_SMAPS_AFTER:-} ]] && observer=(--smaps-after "$GPUKEY_RECORDER_SMAPS_AFTER")
exec python3 "$GPUKEY_RECORDER_TOOL" "${observer[@]}" --output "$GPUKEY_RECORDER_OUTPUT/memory.json" \
    --observe "$PPID" --stop-after "$seconds" --stop-timeout 5 -- "$GPUKEY_RECORDER_BINARY" "${args[@]}"
SH
chmod 700 "$output/record"
pipewire -c "$PWD/tests/pipewire.conf" > "$output/server.log" 2>&1 &
server=$!
for ((i=0;i<100;i++)); do [[ -S "$runtime/gpukey-test" ]] && break; sleep 0.02; done
[[ -S "$runtime/gpukey-test" ]]
if [[ $GPUKEY_RECORDER_MODE = replay ]]; then
    (( GPUKEY_FIXTURE_SECONDS >= 40 ))
    (
        sleep "$((GPUKEY_FIXTURE_SECONDS-5))"
        for n in 1 2; do
            clip="$output/clip.mkv"
            [[ $n = 2 ]] && clip="$output/clip.mkv.second.mkv"
            if [[ $app = gpukey ]]; then
                "$binary" save "$clip" --session fixture > "$output/save-$n.log" 2>&1
            else
                saved=$("$(dirname "$binary")/gsr-cli" -ipc "$runtime/gsr" save-replay 2> "$output/save-$n.log")
                printf '%s\n' "$saved" >> "$output/save-$n.log"
                [[ -f $saved && ! -e $clip ]]
                cp -- "$saved" "$clip"
            fi
            if [[ $n = 1 ]]; then sleep 3; fi
        done
    ) &
    saver=$!
fi
sha256sum "$binary" "$fixture" "$output/record" "$GPUKEY_RECORDER_TOOL" > "$output/binaries.sha256"
source_flag=--gpu-portal-source
[[ -n ${GPUKEY_FIXTURE_SHM:-} ]] && source_flag=--shm-portal-source
GPUKEY_FIXTURE_REPORT="$output/producer.json" \
    "$fixture" "$output/record" "$output/clip.mkv" "$source_flag" > "$output/recording.log" 2>&1
if [[ -n $saver ]]; then wait "$saver"; saver=; fi
jq -e '.returncode == 0 and (.stop_killed | not) and (.samples | length) > 0' "$output/memory.json" >/dev/null
ffprobe -v error -count_frames -show_streams -show_format -of json "$output/clip.mkv" > "$output/decoded.json"
jq -e '.streams | length == 1 and .[0].codec_name == "av1" and (.[0].nb_read_frames | tonumber) > 0' "$output/decoded.json" >/dev/null
ffmpeg -v error -xerror -err_detect explode -i "$output/clip.mkv" -map 0 -fps_mode:v passthrough -enc_time_base:v demux -f null -
if [[ $GPUKEY_RECORDER_MODE = replay ]]; then
    ffmpeg -v error -xerror -err_detect explode -i "$output/clip.mkv.second.mkv" -map 0 \
        -fps_mode:v passthrough -enc_time_base:v demux -f null -
fi
sha256sum -c "$output/binaries.sha256" > "$output/binaries-verified.log"
printf '%s\n' "$app private capture and independent AV1 decoding passed: $output"

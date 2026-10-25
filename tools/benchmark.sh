#!/usr/bin/env bash
# Run after builds/tests stop. This measures synthetic complete paths, not a desktop/game benchmark.
set -euo pipefail
control=$1
candidate=$2
output=$3
mkdir -p "$output"
sha256sum "$control" "$candidate" > "$output/binaries.sha256"
printf 'case\tvariant\trun\telapsed_ns\tprepare_mean_ns\tprepare_p99_upper_ns\tencode_mean_ns\tbytes\tcpu_us\n' > "$output/results.tsv"
run() {
    local case_name=$1 variant=$2 repetition=$3 binary=$4 source=$5 size=$6 frames=$7 sink=$8
    local stem="$output/$case_name-$variant-$repetition"
    "$binary" --test-source "$source" --size "$size" --frames "$frames" --fps 240 \
        --sink "$sink" --output "$stem.mp4" --stats "$stem.json" > "$stem.log" 2>&1
    jq -e --argjson frames "$frames" '.success and .submitted == $frames and .packets == $frames and .prepare.count == $frames and .encode_and_receive.count == $frames and .accepted_bytes == .written_bytes' "$stem.json" >/dev/null
    jq -r --arg c "$case_name" --arg v "$variant" --argjson r "$repetition" \
        '[$c,$v,$r,.elapsed_ns,.prepare.mean_ns,.prepare.p99_bucket_upper_ns,.encode_and_receive.mean_ns,.written_bytes,(.cpu_user_us+.cpu_system_us)] | @tsv' \
        "$stem.json" >> "$output/results.tsv"
}
for case_name in small hd uhd; do
    case "$case_name" in
        small) source=dmabuf-static; size=320x240; frames=240;;
        hd) source=dmabuf; size=1920x1080; frames=180;;
        uhd) source=dmabuf-static; size=3840x2160; frames=120;;
    esac
    for repetition in 1 2 3 4 5; do
        if ((repetition % 2)); then order='control pooled'; else order='pooled control'; fi
        for variant in $order; do
            binary=$candidate
            [[ $variant = control ]] && binary=$control
            run "$case_name" "$variant" "$repetition" "$binary" "$source" "$size" "$frames" buffered
        done
    done
    # Compare independent decoded pixels and serialized presentation times.
    ffmpeg -v error -i "$output/$case_name-control-1.mp4" -f framemd5 "$output/$case_name-control.framemd5"
    ffmpeg -v error -i "$output/$case_name-pooled-1.mp4" -f framemd5 "$output/$case_name-pooled.framemd5"
    cmp "$output/$case_name-control.framemd5" "$output/$case_name-pooled.framemd5"
done
for repetition in 1 2 3 4 5; do
    if ((repetition % 2)); then order='buffered uring'; else order='uring buffered'; fi
    for sink in $order; do
        run output "$sink" "$repetition" "$candidate" dmabuf 1920x1080 180 "$sink"
    done
done
printf 'Raw runs and decoder comparisons saved in %s\n' "$output"

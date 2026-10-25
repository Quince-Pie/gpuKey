#!/usr/bin/env bash
# Fixed real packet inputs; preload is outside the replay lifecycle timer.
# Run in nix develop: BENCH UNSHARED_CONTROL NEW_OUTPUT_DIR CASE INPUT [CASE INPUT ...].
set -euo pipefail
bench=$(realpath "$1")
control=$(realpath "$2")
output=$3
shift 3
(( $# > 0 && $# % 2 == 0 ))
mkdir "$output"
sha256sum "$bench" "$control" >"$output/binaries.sha256"
printf 'case\tvariant\trun\tjson\n' >"$output/runs.tsv"
while (( $# )); do
    name=$1 input=$(realpath "$2")
    shift 2
    [[ $name =~ ^[a-z0-9-]+$ ]]
    sha256sum "$input" >>"$output/inputs.sha256"
    for variant in control reference auto; do
        runner=$bench mode=$variant
        [[ $variant = control ]] && runner=$control mode=auto
        "$runner" "$input" "$mode" "$output/warmup.mkv" >/dev/null
        rm "$output/warmup.mkv"
    done
    for repetition in 1 2 3 4 5 6 7; do
        case $((repetition % 3)) in
            0) order='auto control reference';;
            1) order='control reference auto';;
            2) order='reference auto control';;
        esac
        for variant in $order; do
            runner=$bench mode=$variant
            [[ $variant = control ]] && runner=$control mode=auto
            stem="$output/$name-$variant-$repetition"
            "$runner" "$input" "$mode" "$stem.mkv" >"$stem.json"
            printf '%s\t%s\t%s\t%s\n' "$name" "$variant" "$repetition" "$stem.json" >>"$output/runs.tsv"
            ffprobe -v error -show_packets -show_data_hash sha256 \
                -show_entries packet=stream_index,pts,dts,duration,size,flags,data_hash,side_data_list \
                -of json "$stem.mkv" >"$stem.packets.json"
            if (( repetition == 1 )); then
                ffmpeg -v error -i "$stem.mkv" -map 0 -f framemd5 "$stem.decoded.md5"
            fi
        done
        for variant in reference auto; do
            cmp "$output/$name-control-$repetition.packets.json" "$output/$name-$variant-$repetition.packets.json"
            if (( repetition == 1 )); then
                cmp "$output/$name-control-1.decoded.md5" "$output/$name-$variant-1.decoded.md5"
            fi
        done
    done
done
sha256sum -c "$output/binaries.sha256" >"$output/binaries-verified.log"
sha256sum -c "$output/inputs.sha256" >"$output/inputs-verified.log"

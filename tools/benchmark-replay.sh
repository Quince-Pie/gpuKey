#!/usr/bin/env bash
# Fixed packets and full replay lifetime; source production is measured separately.
# Run in nix develop, with no other builds or benchmarks in progress.
set -euo pipefail
binary=$1
bench=$2
output=$3
scan_control=${4-}
event_control=${5-}
if [[ -n "$event_control" && -z "$scan_control" ]]; then
    echo 'Pass the initial control as argument 4 before the eventfd control.' >&2
    exit 2
fi
mkdir "$output"
sha256sum "$binary" "$bench" >"$output/binaries.sha256"
if [[ -n "$scan_control" ]]; then sha256sum "$scan_control" >>"$output/binaries.sha256"; fi
if [[ -n "$event_control" ]]; then sha256sum "$event_control" >>"$output/binaries.sha256"; fi
printf 'case\tvariant\trun\telapsed_ns\tcpu_us\tadmission_total_ns\tadmission_p99_upper_ns\tadmission_max_ns\tsnapshot_ns\tlive_bytes\towned_bytes\tcopied_bytes\tsaved_bytes\n' >"$output/results.tsv"
for case_name in static changing opus pcm; do
    source=dmabuf-static
    audio=()
    extension=mp4
    [[ $case_name = changing ]] && source=dmabuf
    [[ $case_name = pcm ]] && audio=(--test-audio --test-audio)
    if [[ $case_name = opus ]]; then audio=(--test-audio --test-audio); extension=mkv; fi
    input="$output/$case_name.$extension"
    "$binary" --test-source "$source" --size 1920x1080 --frames 1200 --fps 60 \
        "${audio[@]}" --output "$input" >"$output/$case_name.log" 2>&1
    # A separate warm-up is not included in confirmation results.
    "$bench" "$input" auto "$output/warmup.mkv" >/dev/null
    rm "$output/warmup.mkv"
    for repetition in 1 2 3 4 5 6 7; do
        order='auto copy'
        ((repetition % 2 == 0)) && order='copy auto'
        if [[ -n "$scan_control" ]]; then
            order='scan copy auto'
            ((repetition % 2 == 0)) && order='auto copy scan'
        fi
        if [[ -n "$event_control" ]]; then
            order='scan event copy auto'
            ((repetition % 2 == 0)) && order='auto copy event scan'
        fi
        for variant in $order; do
            stem="$output/$case_name-$variant-$repetition"
            runner=$bench mode=$variant
            if [[ $variant = scan ]]; then runner=$scan_control mode=auto; fi
            if [[ $variant = event ]]; then runner=$event_control mode=auto; fi
            "$runner" "$input" "$mode" "$stem.mkv" >"$stem.json"
            jq -r --arg c "$case_name" --arg v "$variant" --argjson r "$repetition" \
                '[$c,$v,$r,.elapsed_ns,.cpu_us,.admission_total_ns,.admission_p99_upper_ns,
                  .admission_max_ns,.snapshot_ns,.live_bytes,.owned_bytes,.copied_bytes,
                  .saved_bytes] | @tsv' "$stem.json" >>"$output/results.tsv"
            ffmpeg -v error -i "$stem.mkv" -map 0 -f framemd5 "$stem.md5"
            if [[ $variant = auto ]]; then reference="$stem.md5"; fi
        done
        cmp "$output/$case_name-auto-$repetition.md5" "$output/$case_name-copy-$repetition.md5"
        cmp "$output/$case_name-auto-1.md5" "$reference"
        if [[ -n "$scan_control" ]]; then
            cmp "$reference" "$output/$case_name-scan-$repetition.md5"
        fi
        if [[ -n "$event_control" ]]; then
            cmp "$reference" "$output/$case_name-event-$repetition.md5"
        fi
    done
done
awk -F '\t' '
NR>1 {
    k=$1 " " $2; n[k]++; x=$4/1000000; sum[k]+=x; squares[k]+=x*x;
    if (x>maximum[k]) maximum[k]=x;
    cpu[k]+=$5/1000; add[k]+=$6/1000000; copy[k]+=$12; live[k]+=$10; owned[k]+=$11;
}
END {
    print "case variant: mean_ms sample_sd_ms max_ms mean_cpu_ms admission_ms copied_bytes live_bytes owned_bytes";
    for(k in n) {
        m=sum[k]/n[k]; v=(squares[k]-n[k]*m*m)/(n[k]-1);
        printf "%s: %.3f %.3f %.3f %.3f %.3f %.0f %.0f %.0f\n", k,m,sqrt(v>0?v:0),
            maximum[k],cpu[k]/n[k],add[k]/n[k],copy[k]/n[k],live[k]/n[k],owned[k]/n[k];
    }
}' "$output/results.tsv" >"$output/summary.txt"
cat "$output/summary.txt"

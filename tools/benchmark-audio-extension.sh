#!/usr/bin/env bash
# Matched complete sessions, including device lifetime; run without other builds.
set -euo pipefail
control=$1
candidate=$2
output=$3
mkdir "$output"
sha256sum "$control" "$candidate" >"$output/binaries.sha256"
printf 'case\tvariant\trun\telapsed_ns\tcpu_us\tmax_rss_kib\tprepare_p99_upper_ns\tencode_p99_upper_ns\tbytes\n' >"$output/results.tsv"
for case_name in static-video changing-video stereo; do
    source=dmabuf-static
    audio=()
    [[ $case_name = changing-video ]] && source=dmabuf
    [[ $case_name = stereo ]] && audio=(--test-audio)
    for repetition in 1 2 3 4 5; do
        order='control candidate'
        ((repetition % 2 == 0)) && order='candidate control'
        for variant in $order; do
            binary=$candidate
            [[ $variant = control ]] && binary=$control
            stem="$output/$case_name-$variant-$repetition"
            "$binary" --test-source "$source" --size 1920x1080 --frames 240 --fps 240 \
                "${audio[@]}" --output "$stem.mp4" --stats "$stem.json" >"$stem.log" 2>&1
            jq -e '.success and .submitted == 240 and .packets == 240 and
                .accepted_bytes == .written_bytes' "$stem.json" >/dev/null
            jq -r --arg c "$case_name" --arg v "$variant" --argjson r "$repetition" \
                '[$c,$v,$r,.elapsed_ns,(.cpu_user_us+.cpu_system_us),.max_rss_kib,
                  .prepare.p99_bucket_upper_ns,.encode_and_receive.p99_bucket_upper_ns,
                  .written_bytes] | @tsv' "$stem.json" >>"$output/results.tsv"
        done
    done
    for variant in control candidate; do
        ffmpeg -v error -i "$output/$case_name-$variant-1.mp4" -map 0:v:0 \
            -f framemd5 "$output/$case_name-$variant.framemd5"
        if [[ $case_name = stereo ]]; then
            ffmpeg -v error -i "$output/$case_name-$variant-1.mp4" -map 0:a:0 \
                -f f32le "$output/$case_name-$variant.f32"
        fi
    done
    cmp "$output/$case_name-control.framemd5" "$output/$case_name-candidate.framemd5"
    if [[ $case_name = stereo ]]; then
        cmp "$output/$case_name-control.f32" "$output/$case_name-candidate.f32"
    fi
done
awk -F '\t' '
NR > 1 {
    k = $1 " " $2; n[k]++; x=$4/1000000; total[k]+=x; squares[k]+=x*x;
    cpu[k]+=$5/1000; rss[k]+=$6;
    if (x > maximum[k]) maximum[k]=x
}
END {
    print "case variant: mean_ms sample_sd_ms max_ms mean_cpu_ms mean_peak_rss_kib";
    for (k in n) {
        m=total[k]/n[k]; variance=(squares[k]-n[k]*m*m)/(n[k]-1);
        printf "%s: %.3f %.3f %.3f %.3f %.1f\n", k,m,
            sqrt(variance>0?variance:0),maximum[k],cpu[k]/n[k],rss[k]/n[k]
    }
}' "$output/results.tsv" >"$output/summary.txt"
cat "$output/summary.txt"

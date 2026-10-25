#!/usr/bin/env bash
set -euo pipefail
control=$1
candidate=$2
fixture=$3
output=$4
mkdir "$output"
runtime=$(mktemp -d)
server=
cleanup() {
    if [[ -n "$server" ]]; then kill "$server" 2>/dev/null || true; wait "$server" 2>/dev/null || true; fi
    rm -rf "$runtime"
}
trap cleanup EXIT
export PIPEWIRE_RUNTIME_DIR="$runtime" PIPEWIRE_REMOTE=gpukey-test
pipewire -c "$PWD/tests/pipewire.conf" >"$output/server.log" 2>&1 &
server=$!
for ((i=0;i<100;i++)); do [[ -S "$runtime/gpukey-test" ]] && break; sleep 0.02; done
[[ -S "$runtime/gpukey-test" ]]
sha256sum "$control" "$candidate" "$fixture" >"$output/binaries.sha256"
printf 'case\tvariant\trun\telapsed_ns\tcpu_us\tmax_rss_kib\tlease_p99_upper_ns\tbytes\tframes\n' >"$output/results.tsv"
run() {
    local case_name=$1 variant=$2 repetition=$3 binary=$4 mode=$5 extension=$6
    local stem="$output/$case_name-$variant-$repetition"
    GPUKEY_FIXTURE_STATS="$stem.json" "$fixture" "$binary" "$stem.$extension" "$mode" >"$stem.log" 2>&1
    jq -e '.success and .accepted_bytes == .written_bytes and .skipped == 0' "$stem.json" >/dev/null
    jq -r --arg c "$case_name" --arg v "$variant" --argjson r "$repetition" \
        '[$c,$v,$r,.elapsed_ns,(.cpu_user_us+.cpu_system_us),.max_rss_kib,
          .source_lease.p99_bucket_upper_ns,.written_bytes,.submitted] | @tsv' \
        "$stem.json" >>"$output/results.tsv"
}
for repetition in 1 2 3 4 5; do
    order='control candidate'
    ((repetition % 2 == 0)) && order='candidate control'
    for variant in $order; do
        binary=$candidate
        [[ $variant = control ]] && binary=$control
        run video "$variant" "$repetition" "$binary" --gpu mp4
    done
    order='opus pcm'
    ((repetition % 2 == 0)) && order='pcm opus'
    for codec in $order; do
        mode=--aux
        [[ $codec = pcm ]] && mode=--aux-pcm
        run aux "$codec" "$repetition" "$candidate" "$mode" mkv
    done
done
awk -F '\t' '
NR > 1 {
    k=$1 " " $2; n[k]++; x=$5/1000; total[k]+=x; squares[k]+=x*x;
    elapsed[k]+=$4/1000000; bytes[k]+=$8; rss[k]+=$6;
    if (x>maximum[k]) maximum[k]=x
}
END {
    print "case variant: mean_cpu_ms sample_sd_ms max_cpu_ms mean_elapsed_ms mean_bytes mean_peak_rss_kib";
    for(k in n) {
        m=total[k]/n[k]; v=(squares[k]-n[k]*m*m)/(n[k]-1);
        printf "%s: %.3f %.3f %.3f %.3f %.1f %.1f\n",k,m,sqrt(v>0?v:0),
            maximum[k],elapsed[k]/n[k],bytes[k]/n[k],rss[k]/n[k]
    }
}' "$output/results.tsv" >"$output/summary.txt"
cat "$output/summary.txt"

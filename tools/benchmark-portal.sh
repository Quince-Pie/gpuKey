#!/usr/bin/env bash
# Matched full portal -> PipeWire -> GPU -> replay lifecycles, without profilers.
set -euo pipefail
control=$(realpath "$1")
ready_time=$(realpath "$2")
candidate=$(realpath "$3")
fixture=$(realpath "$4")
output=$5
mkdir "$output"
output=$(realpath "$output")
runtime=$(mktemp -d)
server=
cleanup() {
    if [[ -n "$server" ]]; then kill "$server" 2>/dev/null || true; wait "$server" 2>/dev/null || true; fi
    rm -rf "$runtime"
}
trap cleanup EXIT
export PIPEWIRE_RUNTIME_DIR="$runtime" PIPEWIRE_REMOTE=gpukey-test XDG_RUNTIME_DIR="$runtime"
pipewire -c "$PWD/tests/pipewire.conf" >"$output/server.log" 2>&1 &
server=$!
for ((i=0;i<100;i++)); do [[ -S "$runtime/gpukey-test" ]] && break; sleep 0.02; done
[[ -S "$runtime/gpukey-test" ]]
sha256sum "$control" "$ready_time" "$candidate" "$fixture" >"$output/binaries.sha256"
printf 'case\tvariant\trun\tcpu_us\telapsed_ns\trss_kib\toffered\tsubmitted\tskipped\tlease_p99_upper_ns\tlease_max_ns\tsaved_bytes\n' >"$output/results.tsv"
for repetition in 1 2 3 4 5; do
    case "$repetition" in
        1) order='control ready_time candidate';;
        2) order='candidate control ready_time';;
        3) order='ready_time candidate control';;
        4) order='candidate ready_time control';;
        5) order='control candidate ready_time';;
    esac
    cases='static changing'
    ((repetition % 2 == 0)) && cases='changing static'
    for case_name in $cases; do
        mode=--gpu-portal-replay
        [[ $case_name = static ]] && mode=--gpu-portal-replay-static
        for variant in $order; do
            binary=$candidate
            [[ $variant = control ]] && binary=$control
            [[ $variant = ready_time ]] && binary=$ready_time
            stem="$output/$case_name-$variant-$repetition"
            GPUKEY_FIXTURE_STATS="$stem.json" "$fixture" "$binary" "$stem.mkv" "$mode" \
                >"$stem.log" 2>&1
            jq -e '.success and .skipped == 0 and .fps_limited == 0
                   and .replay_saved == 2 and .replay_failed == 0' "$stem.json" >/dev/null
            jq -r --arg c "$case_name" --arg v "$variant" --argjson r "$repetition" \
                '[$c,$v,$r,(.cpu_user_us+.cpu_system_us),.elapsed_ns,.max_rss_kib,
                  .offered,.submitted,.skipped,.source_lease.p99_bucket_upper_ns,
                  .source_lease.max_ns,.replay_saved_bytes] | @tsv' "$stem.json" >>"$output/results.tsv"
        done
    done
done
awk -F '\t' '
NR>1 {
    k=$1 " " $2; n[k]++; x=$4/1000; total[k]+=x; squares[k]+=x*x;
    pct[k]+=100*$4*1000/$5; rss[k]+=$6;
    if(x>maximum[k]) maximum[k]=x;
    if($11>tail[k]) tail[k]=$11;
}
END {
    print "case variant: mean_cpu_ms sample_sd_ms max_cpu_ms mean_one_core_percent mean_rss_kib worst_lease_ns";
    for(k in n) {
        m=total[k]/n[k]; v=(squares[k]-n[k]*m*m)/(n[k]-1);
        printf "%s: %.3f %.3f %.3f %.4f %.1f %.0f\n",
               k,m,sqrt(v>0?v:0),maximum[k],pct[k]/n[k],rss[k]/n[k],tail[k];
    }
}' "$output/results.tsv" >"$output/summary.txt"
cat "$output/summary.txt"

#!/usr/bin/env bash
# In nix develop: NEW_DIRECTORY DIRECTORY_WITH_base_fixed_adaptive_exact_shared_TREES
# Extract, compile and validate first; then rotate complete helper-batch timings.
set -euo pipefail
output=$1 sources=$2
mkdir "$output"
output=$(realpath "$output")
bash tools/check-command-hints.sh "$output/cores" "$sources" >"$output/check.log" 2>&1
names=(base fixed adaptive exact shared)
for ((run=1; run<=3; ++run)); do
    for ((step=0; step<${#names[@]}; ++step)); do
        index=$(((step+run-1)%${#names[@]}))
        name=${names[index]}
        "$output/cores/$name/probe" bench >"$output/$name-$run.csv"
    done
done
sha256sum "$output"/cores/*/probe >"$output/binaries.sha256"
echo 'Complete insertion, lookup, release and reset batches measured; native confirmation remains separate.'

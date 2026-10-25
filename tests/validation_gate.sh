#!/usr/bin/env bash
# Exercise the validation gates without a GPU or decoder.
set -euo pipefail
checker=$(realpath "$1")
scratch=$(mktemp -d)
trap 'rm -rf "$scratch"' EXIT
export GPUKEY_GATE_EVENTS="$scratch/events"
{
    printf '#!%s\n' "$BASH"
    cat <<'SH'
set -euo pipefail
if [[ $GPUKEY_GATE_MODE = positive-error ]]; then
    printf 'Validation Error: injected positive-path gate regression\n'
elif [[ $GPUKEY_GATE_MODE = restore-error ]]; then
    if [[ ${GPUKEY_RGB_FAIL_RECON_RESTORE:-} = 1 ]]; then
        printf 'Injected reconstruction-view failure after reclamation.\n'
        printf 'VUID-test: injected cleanup-path gate regression\n'
        exit 1
    fi
    if [[ $GPUKEY_RGB_TRIM = 0 ]]; then
        printf 'DPB_AUDIT created=3 retired=0 live=3\n'
    else
        printf 'DPB_AUDIT created=11 retired=8 live=3\n'
    fi
fi
SH
} > "$scratch/probe"
{
    printf '#!%s\n' "$BASH"
    cat <<'SH'
set -euo pipefail
printf 'decode\n' >> "$GPUKEY_GATE_EVENTS"
[[ $GPUKEY_GATE_MODE != positive-clean ]] || exit 42
output=${!#}
: > "$output"
for ((i=0;i<250;i++)); do printf 'fixture frame\n' >> "$output"; done
SH
} > "$scratch/ffmpeg"
chmod 700 "$scratch/probe" "$scratch/ffmpeg"
export PATH="$scratch:$PATH"
for mode in positive-error positive-clean restore-error; do
    export GPUKEY_GATE_MODE=$mode
    rm -f "$GPUKEY_GATE_EVENTS"
    status=0
    bash "$checker" "$scratch/probe" "$scratch/$mode" > "$scratch/$mode.log" 2>&1 || status=$?
    if [[ $mode = positive-clean ]]; then
        [[ $status = 42 && $(wc -l < "$GPUKEY_GATE_EVENTS") = 1 ]]
    elif [[ $mode = positive-error ]]; then
        [[ $status = 1 && ! -e $GPUKEY_GATE_EVENTS ]]
        grep -q 'Validation failed: 8-0' "$scratch/$mode.log"
    else
        [[ $status = 1 && $(wc -l < "$GPUKEY_GATE_EVENTS") = 2 ]]
        grep -q 'Failure cleanup validation failed: 8' "$scratch/$mode.log"
    fi
done
printf 'Validation gates reject positive/cleanup errors and admit a clean log.\n'

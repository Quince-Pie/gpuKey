#!/usr/bin/env bash
# In nix develop: NEW_DIRECTORY DIRECTORY_WITH_base_fixed_adaptive_exact_TREES
# Extract actual core functions; only the allocation binding changes for faults.
set -euo pipefail
output=$1 sources=$(realpath "$2")
mkdir "$output"
output=$(realpath "$output")
extra=()
[[ ${GPUKEY_HINT_SANITIZE:-0} != 1 ]] || extra=(-fsanitize=address,undefined -fno-omit-frame-pointer)
[[ ${GPUKEY_HINT_TSAN:-0} != 1 ]] || extra=(-fsanitize=thread -fno-omit-frame-pointer)
for spec in base:0 fixed:1 adaptive:2 exact:3 shared:4; do
    name=${spec%:*} mode=${spec#*:}
    directory="$output/$name"
    mkdir "$directory"
    source="$sources/$name/src/gallium/winsys/amdgpu/drm/amdgpu_cs.cpp"
    python3 - "$source" "$directory/command_hint_core.h" <<'PY'
import sys
from pathlib import Path
s=Path(sys.argv[1]).read_text()
markers=['static void amdgpu_grow_buffer_hash(', 'static uint32_t amdgpu_buffer_map_hash(',
         'static struct amdgpu_cs_buffer *\namdgpu_lookup_buffer(']
start=min(s.index(m) for m in markers if m in s)
end=s.index('static unsigned amdgpu_cs_add_buffer(',start)
part=s[start:end]
split=part.index('static struct amdgpu_cs_buffer *\namdgpu_lookup_buffer(')
part=part[:split].replace('REALLOC(', 'HINT_REALLOC(')+part[split:]
Path(sys.argv[2]).write_text(part)
PY
    ${CC:-cc} -std=c23 -O2 -g -Wall -Wextra -Wpedantic -Werror -Wno-unused-parameter \
        "${extra[@]}" -DGK_HINT_MODE="$mode" -I"$directory" tests/command_hint_probe.c \
        -pthread -o "$directory/probe" >"$directory/build.log" 2>&1
    if [[ ${GPUKEY_HINT_TSAN:-0} = 1 ]]; then
        if [[ $mode = 0 ]]; then
            if TSAN_OPTIONS=halt_on_error=1 "$directory/probe" race >"$directory/race.log" 2>&1; then
                echo 'Shared-table race negative control unexpectedly passed.' >&2
                exit 1
            fi
            grep -q 'ThreadSanitizer: data race' "$directory/race.log"
        else
            TSAN_OPTIONS=halt_on_error=1 "$directory/probe" race >"$directory/race.log" 2>&1
        fi
    else
        "$directory/probe" >"$directory/check.log" 2>&1
    fi
    if [[ $mode = 0 ]]; then
        if "$directory/probe" handoff >"$directory/handoff.log" 2>&1; then
            echo 'Shared-table negative control unexpectedly passed.' >&2
            exit 1
        fi
        grep -q 'amdgpu_lookup_buffer_any_type' "$directory/handoff.log"
    else
        "$directory/probe" handoff >"$directory/handoff.log" 2>&1
    fi
    sha256sum "$source" tests/command_hint_probe.c "$directory/command_hint_core.h" "$directory/probe" >"$directory/inputs.sha256"
done
echo 'Actual hint cores: shared-table negative control, independent ownership, membership, collisions, growth, failure and teardown pass.'

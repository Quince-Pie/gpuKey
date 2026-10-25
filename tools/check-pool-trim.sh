#!/usr/bin/env bash
# In nix develop: NEW_DIRECTORY PATCHED_FFMPEG_SOURCE
# Compile the actual pool implementations, including their existing teardown.
set -euo pipefail
ulimit -c 0
output=$1 source=$(realpath "$2")
mkdir "$output"
output=$(realpath "$output")
mkdir "$output/config"
(
    cd "$output/config"
    bash "$source/configure" --disable-everything --disable-autodetect --disable-x86asm \
        --disable-programs --disable-doc --disable-network
) >"$output/configure.log" 2>&1
grep -q '^#define HAVE_PTHREADS 1$' "$output/config/config.h"
extra=()
case ${GPUKEY_POOL_SANITIZE:-none} in
    none) ;;
    address) extra=(-fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer);;
    thread) extra=(-fsanitize=thread -fno-omit-frame-pointer);;
    *) exit 2;;
esac
common=(-std=c23 -O2 -g -I"$output/config" -I"$source")
units="$source/libavutil"
if [[ ${GPUKEY_POOL_BREAK_PIN:-0} = 1 ]]; then
    mkdir "$output/negative"
    python3 - "$source" "$output/negative" <<'PY'
from pathlib import Path
import sys
source, target = map(Path, sys.argv[1:])
for name, end, finalizer in [('buffer', 'void av_buffer_pool_uninit', 'buffer_pool_free'),
                             ('refstruct', 'static void pool_return_entry', 'pool_free')]:
    text = (source/'libavutil'/f'{name}.c').read_text()
    start = text.index(f'size_t av_{name}_pool_trim(')
    stop = text.index(end, start)
    part = text[start:stop]
    pin = '    if (entry)\n        atomic_fetch_add_explicit(&pool->refcount, 1, memory_order_relaxed);\n'
    unpin = ('    if (atomic_fetch_sub_explicit(&pool->refcount, 1, memory_order_acq_rel) == 1)\n'
             f'        {finalizer}(pool);\n')
    assert part.count(pin) == part.count(unpin) == 1
    part = part.replace(pin, '').replace(unpin, '')
    (target/f'{name}.c').write_text(text[:start]+part+text[stop:])
PY
    units="$output/negative"
    common+=(-I"$source/libavutil")
fi
${CC:-cc} "${common[@]}" "${extra[@]}" -Wall -Wextra -Wpedantic -Wconversion -Werror \
    $(pkg-config --cflags libavutil) -c tests/pool_trim_probe.c -o "$output/probe.o" \
    >"$output/build.log" 2>&1
for unit in buffer refstruct; do
    ${CC:-cc} "${common[@]}" "${extra[@]}" $(pkg-config --cflags libavutil) \
        -c "$units/$unit.c" -o "$output/$unit.o" >>"$output/build.log" 2>&1
done
${CC:-cc} "${extra[@]}" "$output/probe.o" "$output/buffer.o" "$output/refstruct.o" \
    $(pkg-config --libs libavutil) -pthread -o "$output/probe" >>"$output/build.log" 2>&1
if [[ ${GPUKEY_POOL_BREAK_PIN:-0} = 1 ]]; then
    for kind in buffer refstruct; do
        if "$output/probe" "$kind" >"$output/$kind-negative.log" 2>&1; then
            echo 'Missing lifetime pin unexpectedly passed.' >&2
            exit 1
        fi
        grep -q 'atomic_load(&a->destroyed) == atomic_load(&a->created)' "$output/$kind-negative.log"
    done
    echo 'Both negative controls expose closing the pool before all detached entries are destroyed.' >"$output/check.log"
else
    "$output/probe" >"$output/check.log" 2>&1
fi
sha256sum tests/pool_trim_probe.c "$source"/libavutil/{buffer,refstruct}.{c,h} \
    "${BASH_SOURCE[0]}" "$output/config/config.h" "$output/probe" >"$output/inputs.sha256"
if [[ ${GPUKEY_POOL_BREAK_PIN:-0} = 1 ]]; then
    sha256sum "$units"/{buffer,refstruct}.c >>"$output/inputs.sha256"
fi
cat "$output/check.log"

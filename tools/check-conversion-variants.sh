#!/usr/bin/env bash
# In nix develop: NEW_DIRECTORY. Exercise the exact generated SIMD clone code.
set -euo pipefail
output=$1
mkdir "$output"
output=$(realpath "$output")
cat >"$output/capability.c" <<'C'
int main(void) {
#if defined(__x86_64__)
    __builtin_cpu_init();
    return __builtin_cpu_supports("x86-64-v4") ? 0 : 77;
#else
    return 77;
#endif
}
C
${CC:-cc} -std=c23 -Wall -Wextra -Wpedantic -Werror "$output/capability.c" -o "$output/capability"
if ! "$output/capability"; then
    echo 'This check executes every clone and requires an x86-64-v4 host.' >&2
    exit 77
fi
build="build/${TOOLCHAIN:-gcc}-release"
make -s PROFILE=release CFLAGS=-Werror "$build/tests/conversion_test"
nm --defined-only "$build/convert.o" >"$output/symbols.txt"
python3 - "$output/symbols.txt" >"$output/variants.txt" <<'PY'
import re,sys
from pathlib import Path
names=[]
for line in Path(sys.argv[1]).read_text().splitlines():
    parts=line.split()
    if len(parts)!=3 or parts[1] not in ('t','T'): continue
    name=parts[2]
    if name.startswith('gk_rgb_to_yuv_rows.') and ('default' in name or 'arch_x86' in name):
        assert re.fullmatch(r'[A-Za-z0-9_.-]+',name)
        names.append(name)
assert len(names)==3,names
print('\n'.join(names))
PY
index=0
while read -r symbol; do
    object="$output/variant-$index.o"
    objcopy --redefine-sym="$symbol=gk_variant" "$build/convert.o" "$object"
    objcopy --globalize-symbol=gk_variant "$object"
    # Renaming/exporting a symbol must not change the compiled instruction bytes.
    objcopy --dump-section .text="$output/original.text" "$build/convert.o"
    objcopy --dump-section .text="$output/variant.text" "$object"
    cmp "$output/original.text" "$output/variant.text"
    ${CC:-cc} -std=c23 -O2 -g -Wall -Wextra -Wpedantic -Wconversion -Werror \
        -Isrc $(pkg-config --cflags libavutil libva) \
        -Dgk_rgb_to_yuv_rows=gk_variant tests/conversion_test.c "$object" \
        $(pkg-config --libs libavutil) -lm -o "$output/variant-$index"
    "$output/variant-$index" >"$output/variant-$index.log" 2>&1
    index=$((index+1))
done <"$output/variants.txt"
sha256sum "$build/convert.o" "$build/tests/conversion_test" >"$output/binaries.sha256"
echo 'Exact default, x86-64-v3 and x86-64-v4 conversion clones pass the independent pixel oracle.'

#!/usr/bin/env bash
# In a Nix compiler shell. NEW_DIRECTORY; optional SANITIZE=address,undefined|thread.
set -euo pipefail
output=$1
mkdir "$output"
python3 - "$output" <<'PY'
import hashlib
from pathlib import Path
import sys
patch = Path('nix/ffmpeg-vulkan-rgb.patch').read_text()
added = '\n'.join(line[1:] for line in patch.splitlines()
                  if line.startswith('+') and not line.startswith('+++')) + '\n'
start = added.index('enum VulkanFrameExecution {')
end = added.index('\n}\n', added.index('static int vulkan_frame_execution(', start)) + 3
helper = added[start:end]
output = Path(sys.argv[1])
(output/'vulkan-frame-execution.inc').write_text(helper)
(output/'helper.sha256').write_text(hashlib.sha256(helper.encode()).hexdigest()+'\n')
PY
flags=()
[[ -n ${SANITIZE:-} ]] && flags=(-fsanitize="$SANITIZE" -fno-sanitize-recover=all)
"${CC:-cc}" -std=c23 -O1 -g -fno-omit-frame-pointer -Wall -Wextra -Wpedantic -Werror \
    "${flags[@]}" -I"$output" tests/vulkan_pool_probe.c -pthread -o "$output/probe"
timeout 10 "$output/probe"

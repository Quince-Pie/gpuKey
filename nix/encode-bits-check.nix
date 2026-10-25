# Test the actual original/patched driver helpers without requiring a GPU.
{
  lib,
  stdenv,
  python3,
  vulkan-headers,
  mesa,
  writerPatch,
  cumulative ? false,
}:
stdenv.mkDerivation {
  pname = "gpukey-encode-bits-check";
  version = "1";
  src = lib.fileset.toSource {
    root = ../.;
    fileset = lib.fileset.unions [
      ../tests/encode_bits_probe.c
      ../tools/extract-encode-bits.py
    ];
  };
  nativeBuildInputs = [ python3 ];
  buildInputs = [ vulkan-headers ];
  dontConfigure = true;
  buildPhase = ''
    runHook preBuild
    mkdir -p candidate/src/amd/vulkan generated
    cp ${mesa.src}/src/amd/vulkan/radv_video_enc.c candidate/src/amd/vulkan/
    cp ${mesa.src}/src/amd/vulkan/radv_cmd_buffer.h candidate/src/amd/vulkan/
    chmod -R u+w candidate
    patch -d candidate -p1 < ${writerPatch}
    python3 tools/extract-encode-bits.py ${mesa.src} candidate generated
    $CC ${lib.optionalString cumulative "-DGK_BITS_CUMULATIVE"} -std=c23 -O2 -g -UNDEBUG -Wall -Wextra -Wpedantic -Wconversion -Werror \
      -Igenerated tests/encode_bits_probe.c -o ordinary
    $CC ${lib.optionalString cumulative "-DGK_BITS_CUMULATIVE"} -std=c23 -O1 -g -UNDEBUG -Wall -Wextra -Wpedantic -Wconversion -Werror \
      -fsanitize=address,undefined -fno-sanitize-recover=all \
      -Igenerated tests/encode_bits_probe.c -o sanitized
    ./ordinary
    ./sanitized
    runHook postBuild
  '';
  installPhase = ''
    mkdir -p $out
    ./ordinary > $out/result.txt
  '';
}

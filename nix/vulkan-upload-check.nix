# Actual FFmpeg transfer and gpuKey upload/staging operations.
{
  lib,
  stdenv,
  python3,
  ffmpeg-headless,
}:
stdenv.mkDerivation {
  pname = "gpukey-vulkan-upload-check";
  version = "1";
  src = lib.fileset.toSource {
    root = ../.;
    fileset = lib.fileset.unions [
      ../nix/ffmpeg-pool-trim.patch
      ../nix/ffmpeg-vulkan-buffer-owner.patch
      ../nix/ffmpeg-vulkan-rgb.patch
      ../nix/ffmpeg-vulkan-transfer.patch
      ../src/media.c
      ../tests/vulkan_upload_probe.c
      ../tools/extract-vulkan-upload.py
    ];
  };
  nativeBuildInputs = [ python3 ];
  dontConfigure = true;
  buildPhase = ''
    runHook preBuild
    mkdir candidate control
    python3 tools/extract-vulkan-upload.py ${ffmpeg-headless.src} src/media.c \
      candidate/vulkan-upload-operations.inc --patch ${./ffmpeg-vulkan-transfer.patch}
    python3 tools/extract-vulkan-upload.py ${ffmpeg-headless.src} src/media.c \
      control/vulkan-upload-operations.inc
    $CC -std=c23 -O2 -g -UNDEBUG -Wall -Wextra -Wpedantic -Wconversion -Werror \
      -Icandidate tests/vulkan_upload_probe.c -o ordinary
    $CC -std=c23 -O1 -g -UNDEBUG -Wall -Wextra -Wpedantic -Wconversion -Werror \
      -fsanitize=address,undefined -fno-sanitize-recover=all \
      -Icandidate tests/vulkan_upload_probe.c -o sanitized
    ./ordinary
    ./sanitized
    $CC -std=c23 -O2 -g -UNDEBUG -Wall -Wextra -Wpedantic -Wconversion -Werror \
      -Icontrol tests/vulkan_upload_probe.c -o original
    if ./original > control.log 2>&1; then
      echo 'Original unexpectedly satisfies synchronous transfer ownership' >&2
      exit 1
    else
      test "$?" = 2
    fi
    grep -Fq 'swf.buf[0]->refs == 1' control.log
    runHook postBuild
  '';
  installPhase = ''
    mkdir -p $out
    ./ordinary > $out/result.txt
    cp control.log $out/
  '';
}

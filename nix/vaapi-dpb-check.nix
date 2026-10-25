# Actual Mesa ownership operations; GPU synchronization is tested natively.
{
  lib,
  stdenv,
  python3,
  mesa,
}:
stdenv.mkDerivation {
  pname = "gpukey-vaapi-dpb-check";
  version = "1";
  src = lib.fileset.toSource {
    root = ../.;
    fileset = lib.fileset.unions [
      ../tests/vaapi_dpb_owner_probe.c
      ../tools/extract-vaapi-dpb.py
    ];
  };
  nativeBuildInputs = [ python3 ];
  dontConfigure = true;
  buildPhase = ''
    runHook preBuild
    mkdir candidate control
    python3 tools/extract-vaapi-dpb.py ${mesa.src} candidate/vaapi-dpb-operations.inc \
      --patch ${./mesa-vaapi-surface-dpb.patch}
    python3 tools/extract-vaapi-dpb.py ${mesa.src} control/vaapi-dpb-operations.inc
    $CC -std=c23 -O2 -g -UNDEBUG -Wall -Wextra -Wpedantic -Wconversion -Werror \
      -Icandidate tests/vaapi_dpb_owner_probe.c -o ordinary
    $CC -std=c23 -O1 -g -UNDEBUG -Wall -Wextra -Wpedantic -Wconversion -Werror \
      -fsanitize=address,undefined -fno-sanitize-recover=all \
      -Icandidate tests/vaapi_dpb_owner_probe.c -o sanitized
    ./ordinary
    ./sanitized
    $CC -std=c23 -O2 -g -UNDEBUG -Wall -Wextra -Wpedantic -Wconversion -Werror \
      -Icontrol tests/vaapi_dpb_owner_probe.c -o original
    if ./original > control.log 2>&1; then
      echo 'Original unexpectedly satisfies surface ownership' >&2
      exit 1
    else
      test "$?" = 2
    fi
    grep -Fq 'owners[i] == (allocated[i] ? 1U : 0U)' control.log
    runHook postBuild
  '';
  installPhase = ''
    mkdir -p $out
    ./ordinary > $out/result.txt
    cp control.log $out/
  '';
}

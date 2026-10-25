# Hardware-independent regression gate for the actual pinned Mesa lookup cores.
{
  stdenv,
  python3,
  mesa,
}:
stdenv.mkDerivation {
  name = "gpukey-command-hints-check";
  dontUnpack = true;
  dontConfigure = true;
  nativeBuildInputs = [ python3 ];
  buildPhase = ''
    runHook preBuild
    mesa_cs_path=src/gallium/winsys/amdgpu/drm
    mkdir -p sources/base/$mesa_cs_path tests
    cp ${mesa.src}/$mesa_cs_path/amdgpu_cs.{cpp,h} sources/base/$mesa_cs_path/
    cp ${../tests/command_hint_probe.c} tests/command_hint_probe.c
    for variant in fixed adaptive exact shared; do
      cp -R sources/base sources/$variant
      chmod -R u+w sources/$variant
    done
    patch -d sources/fixed -p1 < ${./mesa-hints-fixed.patch}
    patch -d sources/adaptive -p1 < ${./mesa-hints-adaptive.patch}
    patch -d sources/exact -p1 < ${./mesa-hints-exact.patch}
    patch -d sources/shared -p1 < ${./mesa-hints-shared.patch}
    bash ${../tools/check-command-hints.sh} results sources
    for variant in fixed adaptive exact shared; do
      results/$variant/probe race > results/$variant/race.log
    done
    runHook postBuild
  '';
  installPhase = ''
    mv results "$out"
  '';
}

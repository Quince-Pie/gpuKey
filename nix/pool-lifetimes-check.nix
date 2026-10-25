# Exercise the real pinned pool code without requiring graphics hardware.
{
  stdenv,
  python3,
  pkg-config,
  ffmpeg,
}:
stdenv.mkDerivation {
  name = "gpukey-pool-lifetimes-check";
  src = ffmpeg.src;
  patches = ffmpeg.patches;
  dontConfigure = true;
  dontStrip = true;
  dontPatchELF = true;
  nativeBuildInputs = [
    python3
    pkg-config
  ];
  buildInputs = [ ffmpeg ];
  buildPhase = ''
    runHook preBuild
    pool_source=$PWD
    mkdir -p probe/tests
    cp ${../tests/pool_trim_probe.c} probe/tests/pool_trim_probe.c
    cd probe
    bash ${../tools/check-pool-trim.sh} checked "$pool_source"
    GPUKEY_POOL_BREAK_PIN=1 bash ${../tools/check-pool-trim.sh} negative "$pool_source"
    runHook postBuild
  '';
  installPhase = ''
    mkdir "$out"
    for variant in checked negative; do
      mkdir -p "$out/$variant/config"
      cp "$variant/probe" "$variant/"*.log "$variant/inputs.sha256" "$out/$variant/"
      cp "$variant/config/config.h" "$variant/configure.log" "$out/$variant/config/"
    done
    cp -R negative/negative "$out/negative/sources"
  '';
}

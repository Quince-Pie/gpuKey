{
  lib,
  stdenv,
  python3,
}:
stdenv.mkDerivation {
  pname = "gpukey-vulkan-execution-check";
  version = "1";
  src = lib.fileset.toSource {
    root = ../.;
    fileset = lib.fileset.unions [
      ../nix/ffmpeg-vulkan-rgb.patch
      ../tests/vulkan_pool_probe.c
      ../tools/check-vulkan-pool.sh
    ];
  };
  nativeBuildInputs = [ python3 ];
  hardeningDisable = [ "fortify" ];
  buildPhase = ''
    runHook preBuild
    bash tools/check-vulkan-pool.sh ordinary
    SANITIZE=address,undefined bash tools/check-vulkan-pool.sh sanitized
    runHook postBuild
  '';
  installPhase = ''
    mkdir -p "$out"
    cp ordinary/helper.sha256 "$out/"
    touch "$out/passed"
  '';
}

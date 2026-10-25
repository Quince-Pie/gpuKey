# Opt-in ACO Radeon profile: avoid initializing an unused LLVM compiler stack.
{ lib, mesa }:
let
  base = import ./mesa-media.nix { inherit lib mesa; };
in
base.overrideAttrs (old: {
  pname = "mesa-gpukey-aco";
  mesonFlags = old.mesonFlags ++ [
    "-Damd-use-llvm=false"
    "-Ddraw-use-llvm=false"
    "-Dllvm=disabled"
    # Generic Mesa tooling pulls in the CLC/LLVM compiler even for a Radeon-only
    # runtime. These tools are not used by gpuKey's VAAPI or graphics probes.
    "-Dtools="
    "-Dinstall-mesa-clc=false"
    "-Dinstall-precomp-compiler=false"
    "-Dteflon=false"
    "-Dintel-rt=disabled"
  ];
  postInstall = old.postInstall + ''
    mkdir -p "$cross_tools"
  '';
})

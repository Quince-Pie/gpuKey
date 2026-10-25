# Exact chroma-grid spacing and guarded partial tiles for NV12/P010 conversion.
{ lib, mesa }:
let
  base = import ./mesa-workspace.nix { inherit lib mesa; };
in
base.overrideAttrs (old: {
  pname = "mesa-gpukey-chroma";
  patches = old.patches ++ [
    (builtins.path {
      path = ./mesa-chroma-grid.patch;
      name = "gpukey-mesa-chroma-grid.patch";
    })
  ];
})

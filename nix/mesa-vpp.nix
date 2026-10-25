# Full-frame RGB conversion in one dispatch, with general VPP retained as fallback.
{ lib, mesa }:
let
  base = import ./mesa-views.nix { inherit lib mesa; };
in
base.overrideAttrs (old: {
  pname = "mesa-gpukey-vpp";
  patches = old.patches ++ [
    (builtins.path {
      path = ./mesa-paired-vpp.patch;
      name = "gpukey-mesa-paired-vpp.patch";
    })
  ];
})

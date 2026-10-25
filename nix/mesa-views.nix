# Comparison profile: texture views share the source buffer's lifetime.
{ lib, mesa }:
let
  base = import ./mesa-aco.nix { inherit lib mesa; };
in
base.overrideAttrs (old: {
  pname = "mesa-gpukey-views";
  patches = old.patches ++ [
    (builtins.path {
      path = ./mesa-owned-views.patch;
      name = "gpukey-mesa-owned-views.patch";
    })
  ];
})

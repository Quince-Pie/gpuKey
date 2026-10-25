# Preserve graphics support while deferring its unused ring for compute/video users.
{ lib, mesa }:
let
  base = import ./mesa-compact.nix { inherit lib mesa; };
in
base.overrideAttrs (old: {
  pname = "mesa-gpukey-media";
  patches = old.patches ++ [
    (builtins.path {
      path = ./mesa-lazy-ring.patch;
      name = "gpukey-mesa-lazy-ring.patch";
    })
  ];
})

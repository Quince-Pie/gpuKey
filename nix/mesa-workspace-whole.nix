# Alternate layout retaining a 2 MiB workspace and one shared BO.
{ lib, mesa }:
let
  base = import ./mesa-workspace.nix {
    inherit lib mesa;
    packed = true;
  };
in
base.overrideAttrs (old: {
  pname = "mesa-gpukey-workspace-whole";
  patches = old.patches ++ [
    (builtins.path {
      path = ./mesa-workspace-whole.patch;
      name = "gpukey-mesa-workspace-whole.patch";
    })
  ];
})

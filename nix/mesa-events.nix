# Coded-buffer readiness export; selected only by the process-local AMD profile.
{ lib, mesa }:
let
  base = import ./mesa-vpp.nix { inherit lib mesa; };
in
base.overrideAttrs (old: {
  pname = "mesa-gpukey-events";
  patches = old.patches ++ [
    (builtins.path {
      path = ./mesa-coded-buffer-export.patch;
      name = "gpukey-mesa-coded-buffer-export.patch";
    })
  ];
})

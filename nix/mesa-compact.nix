# Opt-in, process-local allocator candidate for the measured media workload.
# Keep the original slab/reclamation algorithm and large-allocation policy.
{ lib, mesa }:
let
  base = import ./mesa-color.nix { inherit lib mesa; };
in
base.overrideAttrs (old: {
  pname = "mesa-gpukey-compact";
  patches = old.patches ++ [
    (builtins.path {
      path = ./mesa-compact-slabs.patch;
      name = "gpukey-mesa-compact-slabs.patch";
    })
  ];
})

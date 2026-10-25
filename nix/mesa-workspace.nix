# Compare dedicated singleton buffers with a packed CDF/session allocation.
{
  lib,
  mesa,
  packed ? false,
}:
let
  base = import ./mesa-events.nix { inherit lib mesa; };
in
base.overrideAttrs (old: {
  pname = if packed then "mesa-gpukey-workspace-packed" else "mesa-gpukey-workspace";
  patches = old.patches ++ [
    (builtins.path {
      path = ./mesa-workspace.patch;
      name = "gpukey-mesa-workspace.patch";
    })
  ];
  mesonFlags = old.mesonFlags ++ lib.optional packed "-Dc_args=-DGPUKEY_PACKED_WORKSPACE";
})

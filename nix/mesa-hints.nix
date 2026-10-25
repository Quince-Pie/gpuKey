# Context-owned compact command hints, with fixed/exact/shared comparison controls.
{
  lib,
  mesa,
  fixed ? false,
  exact ? false,
  shared ? false,
}:
let
  base = import ./mesa-chroma.nix { inherit lib mesa; };
in
base.overrideAttrs (old: {
  pname =
    "mesa-gpukey-hints"
    + lib.optionalString fixed "-fixed"
    + lib.optionalString exact "-exact"
    + lib.optionalString shared "-shared";
  patches = old.patches ++ [
    (builtins.path {
      path =
        if fixed then
          ./mesa-hints-fixed.patch
        else if exact then
          ./mesa-hints-exact.patch
        else if shared then
          ./mesa-hints-shared.patch
        else
          ./mesa-hints-adaptive.patch;
      name = "gpukey-mesa-hints.patch";
    })
  ];
})

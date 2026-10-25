# Surface-owned AV1 reconstructions; retain the existing driver as a control.
{ lib, mesa }:
let
  base = import ./mesa-hints.nix { inherit lib mesa; };
in
base.overrideAttrs (old: {
  pname = "mesa-gpukey-vaapi-surface-dpb";
  patches = old.patches ++ [ ./mesa-vaapi-surface-dpb.patch ];
})

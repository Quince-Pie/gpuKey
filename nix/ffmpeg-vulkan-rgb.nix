{
  lib,
  ffmpeg-headless,
  synchronousTransfer ? true,
}:
let
  base = import ./ffmpeg-capacity.nix {
    inherit lib ffmpeg-headless;
    returnPolicy = true;
  };
in
base.overrideAttrs (old: {
  pname = "ffmpeg-gpukey-vulkan-rgb";
  patches =
    old.patches
    ++ [
      ./ffmpeg-vulkan-rgb.patch
      ./ffmpeg-quantizer-vulkan.patch
      ./ffmpeg-dpb-idle.patch
    ]
    ++ lib.optionals synchronousTransfer [
      ./ffmpeg-vulkan-transfer.patch
    ];
})

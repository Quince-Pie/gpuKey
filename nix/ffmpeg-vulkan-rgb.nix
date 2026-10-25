{
  lib,
  ffmpeg-headless,
}:
let
  base = import ./ffmpeg.nix {
    inherit lib ffmpeg-headless;
    withVaapiDpb = false;
  };
in
base.overrideAttrs (old: {
  pname = "ffmpeg-gpukey-vulkan-rgb";
  patches = old.patches ++ [
    ./ffmpeg-vulkan-rgb.patch
    ./ffmpeg-quantizer-vulkan.patch
    ./ffmpeg-dpb-idle.patch
    ./ffmpeg-vulkan-transfer.patch
  ];
})

# Optional live-pool trimming and completion-FD generation reporting.
{
  ffmpeg-headless,
  lib,
  returnPolicy ? false,
}:
let
  base = import ./ffmpeg-headers.nix { inherit ffmpeg-headless lib; };
in
base.overrideAttrs (old: {
  pname = "ffmpeg-gpukey-capacity" + lib.optionalString returnPolicy "-return";
  patches =
    old.patches
    ++ [
      ./ffmpeg-quantizer-vaapi.patch
      ./ffmpeg-vulkan-buffer-owner.patch
      (builtins.path {
        path = ./ffmpeg-pool-trim.patch;
        name = "gpukey-ffmpeg-pool-trim.patch";
      })
      (builtins.path {
        path = ./ffmpeg-idle-output.patch;
        name = "gpukey-ffmpeg-idle-output.patch";
      })
    ]
    ++ lib.optionals returnPolicy [
      (builtins.path {
        path = ./ffmpeg-pool-single-idle.patch;
        name = "gpukey-ffmpeg-pool-single-idle.patch";
      })
    ]
    ++ [ ./ffmpeg-pool-standby.patch ];
})

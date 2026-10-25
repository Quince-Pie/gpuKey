# Lazy VAAPI mappings and checked synchronous transfers with borrowed plane views.
{ ffmpeg-headless }:
let
  base = import ./ffmpeg-recorder.nix { inherit ffmpeg-headless; };
in
base.overrideAttrs (old: {
  pname = "ffmpeg-gpukey-mapping";
  patches = old.patches ++ [
    (builtins.path {
      path = ./ffmpeg-video-duration.patch;
      name = "gpukey-ffmpeg-video-duration.patch";
    })
    (builtins.path {
      path = ./ffmpeg-parser-input.patch;
      name = "gpukey-ffmpeg-parser-input.patch";
    })
    (builtins.path {
      path = ./ffmpeg-initial-output.patch;
      name = "gpukey-ffmpeg-initial-output.patch";
    })
    (builtins.path {
      path = ./ffmpeg-lazy-mapping.patch;
      name = "gpukey-ffmpeg-lazy-mapping.patch";
    })
  ];
})

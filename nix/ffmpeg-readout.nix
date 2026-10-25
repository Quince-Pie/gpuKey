# Opt-in VAAPI completion readout; no extra input, drain, thread or polling timer.
{ ffmpeg-headless }:
ffmpeg-headless.overrideAttrs (old: {
  pname = "ffmpeg-gpukey-readout";
  patches = (old.patches or [ ]) ++ [
    (builtins.path {
      path = ./ffmpeg-ready-output.patch;
      name = "gpukey-ffmpeg-ready-output.patch";
    })
  ];
})

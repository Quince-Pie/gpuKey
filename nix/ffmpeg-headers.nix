# AV1 packed-header construction without unused CQP tracing, temporary fragment
# buffers or retained raw OBUs. Keep the trace-only control independently usable.
{
  ffmpeg-headless,
  lib,
  traceOnly ? false,
}:
let
  base = import ./ffmpeg-mapping.nix { inherit ffmpeg-headless; };
in
base.overrideAttrs (old: {
  pname = "ffmpeg-gpukey-headers" + lib.optionalString traceOnly "-trace";
  patches =
    old.patches
    ++ [
      (builtins.path {
        path = ./ffmpeg-header-trace.patch;
        name = "gpukey-ffmpeg-header-trace.patch";
      })
    ]
    ++ lib.optionals (!traceOnly) [
      (builtins.path {
        path = ./ffmpeg-header-direct.patch;
        name = "gpukey-ffmpeg-header-direct.patch";
      })
    ];
})

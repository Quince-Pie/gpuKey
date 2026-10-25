# Minimal FFmpeg library configuration and patches for gpuKey's VAAPI, Vulkan,
# NVENC, software AV1, Opus/PCM audio, and MP4/Matroska recording pipeline.
{
  ffmpeg-headless,
  lib,
  withVaapiDpb ? false,
}:
let
  base = ffmpeg-headless.override {
    ffmpegVariant = "headless";
    withHeadlessDeps = false;
    withSmallDeps = false;
    withFullDeps = false;
    buildAvcodec = true;
    buildAvformat = true;
    buildAvutil = true;
    buildSwresample = true;
    buildSwscale = true;
    withAom = true;
    withDav1d = true;
    withOpus = true;
    withVaapi = true;
    withVulkan = true;
    withDrm = true;
    withNvcodec = true;
    withNvenc = true;
    withCuda = true;
    withHardcodedTables = true;
    withSafeBitstreamReader = true;
    withPixelutils = true;
  };
  readout = base.overrideAttrs (old: {
    pname = "ffmpeg-gpukey-readout";
    patches = (old.patches or [ ]) ++ [
      (builtins.path {
        path = ./ffmpeg-ready-output.patch;
        name = "gpukey-ffmpeg-ready-output.patch";
      })
    ];
  });
  recorder = readout.overrideAttrs (old: {
    pname = "ffmpeg-gpukey-recorder";
    configureFlags = old.configureFlags ++ [
      "--disable-everything"
      "--enable-encoder=libaom_av1,av1_vaapi,av1_nvenc,av1_vulkan,libopus,pcm_f32le"
      "--enable-decoder=libdav1d,opus,libopus,pcm_f32le"
      "--enable-parser=av1,opus"
      "--enable-muxer=matroska,mp4"
      "--enable-demuxer=matroska,mov"
      "--enable-protocol=file,pipe"
      "--enable-bsf=av1_metadata,av1_frame_merge,av1_frame_split,extract_extradata,null"
    ];
    checkPhase = builtins.replaceStrings [ "make check -j" ] [ "make fate -j" ] old.checkPhase;
  });
  mapping = recorder.overrideAttrs (old: {
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
  });
  headers = mapping.overrideAttrs (old: {
    pname = "ffmpeg-gpukey-headers";
    patches = old.patches ++ [
      (builtins.path {
        path = ./ffmpeg-header-trace.patch;
        name = "gpukey-ffmpeg-header-trace.patch";
      })
      (builtins.path {
        path = ./ffmpeg-header-direct.patch;
        name = "gpukey-ffmpeg-header-direct.patch";
      })
    ];
  });
  capacityReturn = headers.overrideAttrs (old: {
    pname = "ffmpeg-gpukey-capacity-return";
    patches = old.patches ++ [
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
      (builtins.path {
        path = ./ffmpeg-pool-single-idle.patch;
        name = "gpukey-ffmpeg-pool-single-idle.patch";
      })
      ./ffmpeg-pool-standby.patch
    ];
  });
  vaapiDpb = capacityReturn.overrideAttrs (old: {
    pname = "ffmpeg-gpukey-vaapi-dpb";
    patches = old.patches ++ [ ./ffmpeg-vaapi-dpb-idle.patch ];
  });
in
if withVaapiDpb then vaapiDpb else capacityReturn

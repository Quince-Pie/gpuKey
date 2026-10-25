# Preserve every gpuKey codec/backend/container consumer while excluding unused
# codec, filter, device and network families from the linked library closure.
# Full ffmpeg/ffprobe tools remain separate check/development dependencies.
{ ffmpeg-headless }:
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
  readout = import ./ffmpeg-readout.nix { ffmpeg-headless = base; };
in
readout.overrideAttrs (old: {
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
  # "check" unconditionally builds developer tools which require avfilter,
  # even when that library is disabled. Keep the configuration-selected FATE
  # tests; gpuKey separately runs its complete codec/mux/decode integration.
  checkPhase = builtins.replaceStrings [ "make check -j" ] [ "make fate -j" ] old.checkPhase;
})

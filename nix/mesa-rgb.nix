{
  mesa,
  lib,
  withLLVM ? false,
  fullRange ? false,
  bitWriter ? "compact",
}:
assert builtins.elem bitWriter [
  "compact"
  "word"
  "reference"
];
(mesa.override {
  galliumDrivers = [ "radeonsi" ];
  vulkanDrivers = [ "amd" ];
  vulkanLayers = [ ];
  withValgrind = false;
}).overrideAttrs
  (old: {
    pname =
      "mesa-gpukey-rgb"
      + lib.optionalString fullRange "-full-range"
      + lib.optionalString (bitWriter == "word") "-word";
    patches =
      (old.patches or [ ])
      ++ [
        ./mesa-rgb-formats.patch
        ./mesa-rgb-construction.patch
      ]
      ++ lib.optional fullRange ./mesa-rgb-full-range.patch
      ++ lib.optional (bitWriter == "compact") ./mesa-encode-bits.patch
      ++ lib.optional (bitWriter == "word") ./mesa-encode-bits-word.patch;
    mesonFlags =
      old.mesonFlags
      ++ [ "-Dgallium-rusticl=false" ]
      ++ lib.optionals (!withLLVM) [
        "-Damd-use-llvm=false"
        "-Ddraw-use-llvm=false"
        "-Dllvm=disabled"
        "-Dtools="
        "-Dinstall-mesa-clc=false"
        "-Dinstall-precomp-compiler=false"
        "-Dteflon=false"
        "-Dintel-rt=disabled"
      ];
    postInstall =
      old.postInstall
      + ''
        rm -f $opencl/etc/OpenCL/vendors/rusticl.icd
        mkdir -p $spirv2dxil
      ''
      + lib.optionalString (!withLLVM) ''
        mkdir -p "$cross_tools"
      '';
    postFixup = lib.replaceStrings [ " $opencl/lib/libRusticlOpenCL.so" ] [ "" ] old.postFixup;
  })

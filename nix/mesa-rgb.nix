{
  mesa,
  lib,
  fullRange ? false,
}:
(mesa.override {
  galliumDrivers = [ "radeonsi" ];
  vulkanDrivers = [ "amd" ];
  vulkanLayers = [ ];
  withValgrind = false;
}).overrideAttrs
  (old: {
    pname = "mesa-gpukey-rgb" + lib.optionalString fullRange "-full-range";
    patches =
      (old.patches or [ ])
      ++ [
        ./mesa-rgb-formats.patch
        ./mesa-rgb-construction.patch
      ]
      ++ lib.optional fullRange ./mesa-rgb-full-range.patch
      ++ [ ./mesa-encode-bits.patch ];
    mesonFlags = old.mesonFlags ++ [
      "-Dgallium-rusticl=false"
      "-Damd-use-llvm=false"
      "-Ddraw-use-llvm=false"
      "-Dllvm=disabled"
      "-Dtools="
      "-Dinstall-mesa-clc=false"
      "-Dinstall-precomp-compiler=false"
      "-Dteflon=false"
      "-Dintel-rt=disabled"
    ];
    postInstall = old.postInstall + ''
      rm -f $opencl/etc/OpenCL/vendors/rusticl.icd
      mkdir -p $spirv2dxil
      mkdir -p "$cross_tools"
    '';
    postFixup = lib.replaceStrings [ " $opencl/lib/libRusticlOpenCL.so" ] [ "" ] old.postFixup;
  })

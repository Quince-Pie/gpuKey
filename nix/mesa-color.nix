# Optional, process-local VAAPI driver for the verified RGB8 -> P010 range fix.
# It does not install or replace the system's graphics drivers.
{ lib, mesa }:
(mesa.override {
  galliumDrivers = [ "radeonsi" ];
  vulkanDrivers = [ ];
  vulkanLayers = [ ];
  withValgrind = false;
}).overrideAttrs
  (old: {
    pname = "mesa-gpukey-color";
    patches = (old.patches or [ ]) ++ [
      (builtins.path {
        path = ./mesa-output-range.patch;
        name = "gpukey-mesa-output-range.patch";
      })
    ];
    mesonFlags = old.mesonFlags ++ [ "-Dgallium-rusticl=false" ];
    postInstall = old.postInstall + ''
      rm -f $opencl/etc/OpenCL/vendors/rusticl.icd
      mkdir -p $spirv2dxil
    '';
    postFixup = lib.replaceStrings [ " $opencl/lib/libRusticlOpenCL.so" ] [ "" ] old.postFixup;
  })

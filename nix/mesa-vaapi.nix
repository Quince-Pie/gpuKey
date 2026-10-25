# Process-local Radeon VAAPI driver with ACO, P010 output-range fix, single-pass
# paired-row VPP, completion fence export, dedicated AV1 workspaces, adaptive
# command-buffer lookup hints, and surface-owned reusable DPB reconstructions.
{ lib, mesa }:
let
  color =
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
      });
  compact = color.overrideAttrs (old: {
    pname = "mesa-gpukey-compact";
    patches = old.patches ++ [
      (builtins.path {
        path = ./mesa-compact-slabs.patch;
        name = "gpukey-mesa-compact-slabs.patch";
      })
    ];
  });
  media = compact.overrideAttrs (old: {
    pname = "mesa-gpukey-media";
    patches = old.patches ++ [
      (builtins.path {
        path = ./mesa-lazy-ring.patch;
        name = "gpukey-mesa-lazy-ring.patch";
      })
    ];
  });
  aco = media.overrideAttrs (old: {
    pname = "mesa-gpukey-aco";
    mesonFlags = old.mesonFlags ++ [
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
      mkdir -p "$cross_tools"
    '';
  });
  views = aco.overrideAttrs (old: {
    pname = "mesa-gpukey-views";
    patches = old.patches ++ [
      (builtins.path {
        path = ./mesa-owned-views.patch;
        name = "gpukey-mesa-owned-views.patch";
      })
    ];
  });
  vpp = views.overrideAttrs (old: {
    pname = "mesa-gpukey-vpp";
    patches = old.patches ++ [
      (builtins.path {
        path = ./mesa-paired-vpp.patch;
        name = "gpukey-mesa-paired-vpp.patch";
      })
    ];
  });
  events = vpp.overrideAttrs (old: {
    pname = "mesa-gpukey-events";
    patches = old.patches ++ [
      (builtins.path {
        path = ./mesa-coded-buffer-export.patch;
        name = "gpukey-mesa-coded-buffer-export.patch";
      })
    ];
  });
  workspace = events.overrideAttrs (old: {
    pname = "mesa-gpukey-workspace";
    patches = old.patches ++ [
      (builtins.path {
        path = ./mesa-workspace.patch;
        name = "gpukey-mesa-workspace.patch";
      })
    ];
  });
  chroma = workspace.overrideAttrs (old: {
    pname = "mesa-gpukey-chroma";
    patches = old.patches ++ [
      (builtins.path {
        path = ./mesa-chroma-grid.patch;
        name = "gpukey-mesa-chroma-grid.patch";
      })
    ];
  });
  hints = chroma.overrideAttrs (old: {
    pname = "mesa-gpukey-hints";
    patches = old.patches ++ [
      (builtins.path {
        path = ./mesa-hints-adaptive.patch;
        name = "gpukey-mesa-hints.patch";
      })
    ];
  });
in
hints.overrideAttrs (old: {
  pname = "mesa-gpukey-vaapi-surface-dpb";
  patches = old.patches ++ [ ./mesa-vaapi-surface-dpb.patch ];
})

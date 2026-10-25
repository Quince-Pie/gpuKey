{
  description = "gpuKey: a C23 Wayland GPU recorder with Tracy profiling";

  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";
    tracy-flake = {
      url = "github:Quince-Pie/tracy-flake";
      inputs.nixpkgs.follows = "nixpkgs";
    };
  };

  outputs =
    { nixpkgs, tracy-flake, ... }:
    let
      inherit (nixpkgs) lib;

      systems = [
        "x86_64-linux"
        "aarch64-linux"
      ];

      perSystem = lib.genAttrs systems (
        system:
        let
          pkgs = nixpkgs.legacyPackages.${system};
          tracy = tracy-flake.packages.${system};
          toolchains = import ./nix/toolchains.nix { inherit pkgs; };
          toolchainTest = pkgs.callPackage ./nix/toolchain-test.nix { };

          ffmpegBase = pkgs.callPackage ./nix/ffmpeg.nix { };
          ffmpegVaapi = pkgs.callPackage ./nix/ffmpeg.nix { withVaapiDpb = true; };
          ffmpegRgb = pkgs.callPackage ./nix/ffmpeg-vulkan-rgb.nix { };

          mesaVaapi = pkgs.callPackage ./nix/mesa-vaapi.nix { };
          mesaRgb = pkgs.callPackage ./nix/mesa-rgb.nix { };
          mesaRgbFull = pkgs.callPackage ./nix/mesa-rgb.nix { fullRange = true; };
          validationRgb = pkgs.callPackage ./nix/vulkan-validation-rgb.nix { };

          packages = lib.mapAttrs (
            _: toolchain:
            pkgs.callPackage ./nix/package.nix {
              inherit (toolchain) stdenv;
              ffmpeg-headless = ffmpegBase;
              ffmpeg-tools = pkgs.ffmpeg-headless;
              tracy-client = tracy.tracy-client;
            }
          ) toolchains;

          fullPackage = pkgs.callPackage ./nix/package.nix {
            inherit (toolchains.gcc) stdenv;
            tracy-client = tracy.tracy-client;
          };
          vaapiPackage = packages.gcc.override { ffmpeg-headless = ffmpegVaapi; };
          rgbPackage = pkgs.callPackage ./nix/package.nix {
            inherit (toolchains.gcc) stdenv;
            ffmpeg-headless = ffmpegRgb;
            ffmpeg-tools = pkgs.ffmpeg-headless;
            tracy-client = tracy.tracy-client;
            withVulkanRgb = true;
          };

          amdFor =
            name: driver: package:
            pkgs.symlinkJoin {
              inherit name;
              paths = [ package ];
              nativeBuildInputs = [ pkgs.makeWrapper ];
              postBuild = ''
                wrapProgram $out/bin/gpukey \
                  --set-default LIBVA_DRIVERS_PATH "${driver}/lib/dri:/run/opengl-driver/lib/dri"
              '';
              meta = package.meta // {
                mainProgram = "gpukey";
                description = "gpuKey with a process-local Radeon VAAPI driver";
              };
              passthru = {
                unwrapped = package;
                inherit driver;
              };
            };

          rgbFor =
            name: driver:
            pkgs.symlinkJoin {
              inherit name;
              paths = [ rgbPackage ];
              nativeBuildInputs = [ pkgs.makeWrapper ];
              postBuild = ''
                wrapProgram $out/bin/gpukey \
                  --set-default LIBVA_DRIVERS_PATH "${mesaVaapi}/lib/dri:/run/opengl-driver/lib/dri" \
                  --set-default VK_DRIVER_FILES "${driver}/share/vulkan/icd.d/radeon_icd.${pkgs.stdenv.hostPlatform.parsed.cpu.name}.json"
              '';
              meta = rgbPackage.meta;
              passthru = {
                unwrapped = rgbPackage;
                inherit driver;
              };
            };

          amdPackage = amdFor "gpukey-vaapi-dpb-amd" mesaVaapi vaapiPackage;
          rgbAmd = rgbFor "gpukey-rgb-amd" mesaRgb;
          rgbFullAmd = rgbFor "gpukey-rgb-full-amd" mesaRgbFull;

          devShells = lib.mapAttrs (
            name: toolchain:
            import ./nix/shell.nix {
              inherit pkgs name;
              inherit (toolchain) stdenv;
              package = packages.${name};
              tracy = tracy.tracy;
            }
          ) toolchains;

          formatter = pkgs.nixfmt-tree.override {
            nixfmtPackage = pkgs.nixfmt-rs;
            runtimeInputs = [ pkgs.clang-tools ];
            settings = {
              tree-root-file = "flake.nix";
              formatter.clang-format = {
                command = "clang-format";
                options = [ "-i" ];
                includes = [
                  "*.c"
                  "*.h"
                ];
              };
            };
          };

          formattedSources = lib.fileset.toSource {
            root = ./.;
            fileset = lib.fileset.unions [
              ./.clang-format
              (lib.fileset.fileFilter (file: file.hasExt "nix" || file.hasExt "c" || file.hasExt "h") ./.)
            ];
          };
        in
        {
          packages = packages // {
            default = rgbAmd;
            full = fullPackage;
            amd = amdPackage;
            vaapi-dpb = vaapiPackage;
            vaapi-dpb-amd = amdPackage;
            rgb = rgbPackage;
            rgb-amd = rgbAmd;
            rgb-full-amd = rgbFullAmd;
            ffmpeg = ffmpegVaapi;
            ffmpeg-rgb = ffmpegRgb;
            mesa-vaapi = mesaVaapi;
            mesa-rgb = mesaRgb;
            mesa-rgb-full = mesaRgbFull;
            validation-rgb = validationRgb;
          };
          devShells = devShells // {
            default = devShells.gcc;
            full = import ./nix/shell.nix {
              inherit pkgs;
              name = "gcc-full";
              inherit (toolchains.gcc) stdenv;
              package = fullPackage;
              tracy = tracy.tracy;
            };
            vaapi-dpb = import ./nix/shell.nix {
              inherit pkgs;
              name = "gcc";
              inherit (toolchains.gcc) stdenv;
              package = vaapiPackage;
              tracy = tracy.tracy;
            };
            rgb = import ./nix/shell.nix {
              inherit pkgs;
              name = "gcc";
              inherit (toolchains.gcc) stdenv;
              package = rgbPackage;
              tracy = tracy.tracy;
            };
            rgb-clang = import ./nix/shell.nix {
              inherit pkgs;
              name = "clang";
              inherit (toolchains.clang) stdenv;
              package = rgbPackage.override { inherit (toolchains.clang) stdenv; };
              tracy = tracy.tracy;
            };
          };
          inherit formatter;
          checks =
            lib.mapAttrs' (name: package: lib.nameValuePair "build-${name}" package) packages
            // lib.mapAttrs' (
              name: toolchain:
              lib.nameValuePair "toolchain-${name}" (toolchainTest {
                inherit (toolchain) compiler linker;
                package = packages.${name};
              })
            ) toolchains
            // {
              command-hints = pkgs.callPackage ./nix/command-hints-check.nix {
                inherit (toolchains.gcc) stdenv;
              };
              vaapi-dpb = pkgs.callPackage ./nix/vaapi-dpb-check.nix {
                inherit (toolchains.gcc) stdenv;
              };
              vaapi-dpb-clang = pkgs.callPackage ./nix/vaapi-dpb-check.nix {
                inherit (toolchains.clang-mold) stdenv;
              };
              encode-bits = pkgs.callPackage ./nix/encode-bits-check.nix {
                inherit (toolchains.gcc) stdenv;
                writerPatch = ./nix/mesa-encode-bits.patch;
              };
              encode-bits-clang = pkgs.callPackage ./nix/encode-bits-check.nix {
                inherit (toolchains.clang-mold) stdenv;
                writerPatch = ./nix/mesa-encode-bits.patch;
              };
              pool-lifetimes = pkgs.callPackage ./nix/pool-lifetimes-check.nix {
                inherit (toolchains.gcc) stdenv;
                ffmpeg = ffmpegBase;
              };
              vulkan-execution = pkgs.callPackage ./nix/vulkan-execution-check.nix {
                inherit (toolchains.gcc) stdenv;
              };
              vulkan-upload = pkgs.callPackage ./nix/vulkan-upload-check.nix {
                inherit (toolchains.gcc) stdenv;
              };
              vulkan-upload-clang = pkgs.callPackage ./nix/vulkan-upload-check.nix {
                inherit (toolchains.clang-mold) stdenv;
              };
              tracy = packages.gcc.tracy;
              formatting =
                pkgs.runCommandLocal "formatting"
                  {
                    nativeBuildInputs = [ formatter ];
                  }
                  ''
                    cp -R --no-preserve=mode ${formattedSources} source
                    treefmt --ci --working-dir source
                    touch "$out"
                  '';
            };
        }
      );
    in
    {
      packages = lib.mapAttrs (_: s: s.packages) perSystem;
      devShells = lib.mapAttrs (_: s: s.devShells) perSystem;
      checks = lib.mapAttrs (_: s: s.checks) perSystem;
      formatter = lib.mapAttrs (_: s: s.formatter) perSystem;

      overlays.default = lib.composeManyExtensions [
        tracy-flake.overlays.default
        (final: _prev: {
          gpukey = final.callPackage ./nix/package.nix {
            tracy-client = final.tracy.client;
          };
        })
      ];
    };
}

{
  description = "gpuKey: a C23 Wayland GPU recorder with Tracy profiling";

  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";
    tracy-flake = {
      url = "github:Quince-Pie/tracy-flake";
      # One nixpkgs instantiation for both flakes, and one glibc/libstdc++ for
      # the program and the Tracy client it links against.
      inputs.nixpkgs.follows = "nixpkgs";
    };
  };

  outputs =
    { nixpkgs, tracy-flake, ... }:
    let
      inherit (nixpkgs) lib;

      # mold cannot emit Mach-O and wild only links Linux ELF, so the toolchain
      # matrix is Linux-only.  Darwin needs its own toolchains.nix.
      systems = [
        "x86_64-linux"
        "aarch64-linux"
      ];

      # Everything system-specific is computed once here and projected into the
      # outputs below.  `legacyPackages` is the instantiation tracy-flake uses
      # too (see `follows`), so nixpkgs is evaluated once per system.
      perSystem = lib.genAttrs systems (
        system:
        let
          pkgs = nixpkgs.legacyPackages.${system};
          tracy = tracy-flake.packages.${system};
          toolchains = import ./nix/toolchains.nix { inherit pkgs; };
          toolchainTest = pkgs.callPackage ./nix/toolchain-test.nix { };
          mesaColor = pkgs.callPackage ./nix/mesa-color.nix { };
          mesaCompact = pkgs.callPackage ./nix/mesa-compact.nix { };
          mesaMedia = pkgs.callPackage ./nix/mesa-media.nix { };
          mesaAco = pkgs.callPackage ./nix/mesa-aco.nix { };
          ffmpegReadout = pkgs.callPackage ./nix/ffmpeg-readout.nix { };
          ffmpegRecorder = pkgs.callPackage ./nix/ffmpeg-recorder.nix { };
          ffmpegMapping = pkgs.callPackage ./nix/ffmpeg-mapping.nix { };
          ffmpegHeaderTrace = pkgs.callPackage ./nix/ffmpeg-headers.nix { traceOnly = true; };
          ffmpegHeaders = pkgs.callPackage ./nix/ffmpeg-headers.nix { };
          ffmpegCapacity = pkgs.callPackage ./nix/ffmpeg-capacity.nix { };
          ffmpegCapacityReturn = pkgs.callPackage ./nix/ffmpeg-capacity.nix { returnPolicy = true; };
          ffmpegVaDpb = ffmpegCapacityReturn.overrideAttrs (old: {
            pname = "ffmpeg-gpukey-vaapi-dpb";
            patches = old.patches ++ [ ./nix/ffmpeg-vaapi-dpb-idle.patch ];
          });
          ffmpegRgb = pkgs.callPackage ./nix/ffmpeg-vulkan-rgb.nix { };
          ffmpegRgbTransferReference = pkgs.callPackage ./nix/ffmpeg-vulkan-rgb.nix {
            synchronousTransfer = false;
          };
          mesaRgb = pkgs.callPackage ./nix/mesa-rgb.nix { };
          mesaRgbWord = pkgs.callPackage ./nix/mesa-rgb.nix { bitWriter = "word"; };
          mesaRgbByteReference = pkgs.callPackage ./nix/mesa-rgb.nix { bitWriter = "reference"; };
          mesaRgbFull = pkgs.callPackage ./nix/mesa-rgb.nix { fullRange = true; };
          mesaRgbReference = pkgs.callPackage ./nix/mesa-rgb.nix { withLLVM = true; };
          validationRgb = pkgs.callPackage ./nix/vulkan-validation-rgb.nix { };
          mesaViews = pkgs.callPackage ./nix/mesa-views.nix { };
          mesaVpp = pkgs.callPackage ./nix/mesa-vpp.nix { };
          mesaEvents = pkgs.callPackage ./nix/mesa-events.nix { };
          mesaWorkspace = pkgs.callPackage ./nix/mesa-workspace.nix { };
          mesaWorkspacePacked = pkgs.callPackage ./nix/mesa-workspace.nix { packed = true; };
          mesaWorkspaceWhole = pkgs.callPackage ./nix/mesa-workspace-whole.nix { };
          mesaChroma = pkgs.callPackage ./nix/mesa-chroma.nix { };
          mesaHints = pkgs.callPackage ./nix/mesa-hints.nix { };
          mesaVaDpb = pkgs.callPackage ./nix/mesa-vaapi-surface-dpb.nix { };
          mesaHintsFixed = pkgs.callPackage ./nix/mesa-hints.nix { fixed = true; };
          mesaHintsExact = pkgs.callPackage ./nix/mesa-hints.nix { exact = true; };
          mesaHintsShared = pkgs.callPackage ./nix/mesa-hints.nix { shared = true; };
          mesaChromaFault = mesaChroma.overrideAttrs (old: {
            pname = "mesa-gpukey-chroma-fault";
            mesonFlags = old.mesonFlags ++ [ "-Dc_args=-DGPUKEY_VPP_TEST_FAULTS" ];
          });
          mesaVppFault = mesaVpp.overrideAttrs (old: {
            pname = "mesa-gpukey-vpp-fault";
            mesonFlags = old.mesonFlags ++ [ "-Dc_args=-DGPUKEY_VPP_TEST_FAULTS" ];
          });

          packages = lib.mapAttrs (
            _: toolchain:
            pkgs.callPackage ./nix/package.nix {
              inherit (toolchain) stdenv;
              ffmpeg-headless = ffmpegCapacityReturn;
              ffmpeg-tools = pkgs.ffmpeg-headless;
              tracy-client = tracy.tracy-client;
            }
          ) toolchains;

          fullPackage = pkgs.callPackage ./nix/package.nix {
            inherit (toolchains.gcc) stdenv;
            tracy-client = tracy.tracy-client;
          };
          vaDpbPackage = packages.gcc.override { ffmpeg-headless = ffmpegVaDpb; };
          rgbPackage = pkgs.callPackage ./nix/package.nix {
            inherit (toolchains.gcc) stdenv;
            ffmpeg-headless = ffmpegRgb;
            ffmpeg-tools = pkgs.ffmpeg-headless;
            tracy-client = tracy.tracy-client;
            withVulkanRgb = true;
          };
          rgbShell = import ./nix/shell.nix {
            inherit pkgs;
            name = "gcc";
            inherit (toolchains.gcc) stdenv;
            package = rgbPackage;
            tracy = tracy.tracy;
          };
          rgbClangShell = import ./nix/shell.nix {
            inherit pkgs;
            name = "clang";
            inherit (toolchains.clang) stdenv;
            package = rgbPackage.override { inherit (toolchains.clang) stdenv; };
            tracy = tracy.tracy;
          };
          fullShell = import ./nix/shell.nix {
            inherit pkgs;
            name = "gcc-full";
            inherit (toolchains.gcc) stdenv;
            package = fullPackage;
            tracy = tracy.tracy;
          };
          readoutPackage = pkgs.callPackage ./nix/package.nix {
            inherit (toolchains.gcc) stdenv;
            ffmpeg-headless = ffmpegReadout;
            tracy-client = tracy.tracy-client;
          };
          readoutShell = import ./nix/shell.nix {
            inherit pkgs;
            name = "gcc-readout";
            inherit (toolchains.gcc) stdenv;
            package = readoutPackage;
            tracy = tracy.tracy;
          };
          recorderPackage = pkgs.callPackage ./nix/package.nix {
            inherit (toolchains.gcc) stdenv;
            ffmpeg-headless = ffmpegRecorder;
            ffmpeg-tools = pkgs.ffmpeg-headless;
            tracy-client = tracy.tracy-client;
          };
          mappingPackage = pkgs.callPackage ./nix/package.nix {
            inherit (toolchains.gcc) stdenv;
            ffmpeg-headless = ffmpegMapping;
            ffmpeg-tools = pkgs.ffmpeg-headless;
            tracy-client = tracy.tracy-client;
          };
          headerTracePackage = pkgs.callPackage ./nix/package.nix {
            inherit (toolchains.gcc) stdenv;
            ffmpeg-headless = ffmpegHeaderTrace;
            ffmpeg-tools = pkgs.ffmpeg-headless;
            tracy-client = tracy.tracy-client;
          };
          headersPackage = pkgs.callPackage ./nix/package.nix {
            inherit (toolchains.gcc) stdenv;
            ffmpeg-headless = ffmpegHeaders;
            ffmpeg-tools = pkgs.ffmpeg-headless;
            tracy-client = tracy.tracy-client;
          };
          capacityPackage = pkgs.callPackage ./nix/package.nix {
            inherit (toolchains.gcc) stdenv;
            ffmpeg-headless = ffmpegCapacity;
            ffmpeg-tools = pkgs.ffmpeg-headless;
            tracy-client = tracy.tracy-client;
          };
          mappingShell = import ./nix/shell.nix {
            inherit pkgs;
            name = "gcc-mapping";
            inherit (toolchains.gcc) stdenv;
            package = mappingPackage;
            tracy = tracy.tracy;
          };
          recorderShell = import ./nix/shell.nix {
            inherit pkgs;
            name = "gcc-recorder";
            inherit (toolchains.gcc) stdenv;
            package = recorderPackage;
            tracy = tracy.tracy;
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
          amdPackage = amdFor "gpukey-amd" mesaHints packages.gcc;
          vaDpbAmd = amdFor "gpukey-vaapi-dpb-amd" mesaVaDpb vaDpbPackage;
          rgbFor =
            name: driver:
            pkgs.symlinkJoin {
              inherit name;
              paths = [ rgbPackage ];
              nativeBuildInputs = [ pkgs.makeWrapper ];
              postBuild = ''
                wrapProgram $out/bin/gpukey \
                  --set-default LIBVA_DRIVERS_PATH "${mesaHints}/lib/dri:/run/opengl-driver/lib/dri" \
                  --set-default VK_DRIVER_FILES "${driver}/share/vulkan/icd.d/radeon_icd.${pkgs.stdenv.hostPlatform.parsed.cpu.name}.json"
              '';
              meta = rgbPackage.meta;
              passthru = {
                unwrapped = rgbPackage;
                inherit driver;
              };
            };
          rgbAmd = rgbFor "gpukey-rgb-amd" mesaRgb;
          rgbWordAmd = rgbFor "gpukey-rgb-word-amd" mesaRgbWord;
          rgbFullAmd = rgbFor "gpukey-rgb-full-amd" mesaRgbFull;
          capacityAmd = amdFor "gpukey-capacity-amd" mesaHints capacityPackage;
          capacityReturnPackage = packages.gcc;
          capacityReturnAmd = amdFor "gpukey-capacity-return-amd" mesaHints capacityReturnPackage;
          amdCapacityReference = amdFor "gpukey-amd-capacity-reference" mesaHints headersPackage;
          amdHintsReference = amdFor "gpukey-amd-hints-reference" mesaChroma packages.gcc;
          amdHintsFixed = amdFor "gpukey-amd-hints-fixed" mesaHintsFixed packages.gcc;
          amdHintsExact = amdFor "gpukey-amd-hints-exact" mesaHintsExact packages.gcc;
          amdHintsShared = amdFor "gpukey-amd-hints-shared" mesaHintsShared packages.gcc;
          amdHeadersReference = amdFor "gpukey-amd-headers-reference" mesaHints mappingPackage;
          amdChromaReference = amdFor "gpukey-amd-chroma-reference" mesaWorkspace packages.gcc;
          amdWorkspaceReference = amdFor "gpukey-amd-workspace-reference" mesaEvents packages.gcc;
          amdWorkspacePacked = amdFor "gpukey-amd-workspace-packed" mesaWorkspacePacked packages.gcc;
          amdWorkspaceWhole = amdFor "gpukey-amd-workspace-whole" mesaWorkspaceWhole packages.gcc;
          amdReference = amdFor "gpukey-amd-reference" mesaAco fullPackage;
          amdVppReference = amdFor "gpukey-amd-vpp-reference" mesaVpp fullPackage;
          amdReadoutReference = amdFor "gpukey-amd-readout-reference" mesaEvents recorderPackage;

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
            # Byte-identical Rust port: the Haskell nixfmt drags the Haskell
            # package set into every `nix flake check` evaluation.
            nixfmtPackage = pkgs.nixfmt-rs;
            runtimeInputs = [ pkgs.clang-tools ];
            settings = {
              # Anchor treefmt here even when the project is not a git repository.
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

          # Only what the formatters read; never build/ or result/.
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
            default = packages.gcc;
            full = fullPackage;
            rgb = rgbPackage;
            rgb-amd = rgbAmd;
            rgb-word-amd = rgbWordAmd;
            rgb-full-amd = rgbFullAmd;
            ffmpeg-rgb = ffmpegRgb;
            ffmpeg-rgb-transfer-reference = ffmpegRgbTransferReference;
            mesa-rgb = mesaRgb;
            mesa-rgb-word = mesaRgbWord;
            mesa-rgb-byte-reference = mesaRgbByteReference;
            mesa-rgb-full = mesaRgbFull;
            mesa-rgb-reference = mesaRgbReference;
            validation-rgb = validationRgb;
            mesa-color = mesaColor;
            mesa-compact = mesaCompact;
            mesa-media = mesaMedia;
            mesa-aco = mesaAco;
            ffmpeg-readout = ffmpegReadout;
            ffmpeg-recorder = ffmpegRecorder;
            ffmpeg-mapping = ffmpegMapping;
            ffmpeg-header-trace = ffmpegHeaderTrace;
            ffmpeg-headers = ffmpegHeaders;
            ffmpeg-capacity = ffmpegCapacity;
            ffmpeg-capacity-return = ffmpegCapacityReturn;
            ffmpeg-vaapi-dpb = ffmpegVaDpb;
            mesa-vaapi-surface-dpb = mesaVaDpb;
            vaapi-dpb = vaDpbPackage;
            vaapi-dpb-amd = vaDpbAmd;
            header-trace = headerTracePackage;
            headers = headersPackage;
            capacity = capacityPackage;
            capacity-amd = capacityAmd;
            capacity-return = capacityReturnPackage;
            capacity-return-amd = capacityReturnAmd;
            mapping = mappingPackage;
            readout = readoutPackage;
            recorder = recorderPackage;
            mesa-views = mesaViews;
            mesa-vpp = mesaVpp;
            mesa-events = mesaEvents;
            mesa-workspace = mesaWorkspace;
            mesa-workspace-packed = mesaWorkspacePacked;
            mesa-workspace-whole = mesaWorkspaceWhole;
            mesa-chroma = mesaChroma;
            mesa-hints = mesaHints;
            mesa-hints-fixed = mesaHintsFixed;
            mesa-hints-exact = mesaHintsExact;
            mesa-hints-shared = mesaHintsShared;
            mesa-chroma-fault = mesaChromaFault;
            mesa-vpp-fault = mesaVppFault;
            amd = amdPackage;
            amd-capacity-reference = amdCapacityReference;
            amd-hints-reference = amdHintsReference;
            amd-hints-fixed = amdHintsFixed;
            amd-hints-exact = amdHintsExact;
            amd-hints-shared = amdHintsShared;
            amd-headers-reference = amdHeadersReference;
            amd-chroma-reference = amdChromaReference;
            amd-workspace-reference = amdWorkspaceReference;
            amd-workspace-packed = amdWorkspacePacked;
            amd-workspace-whole = amdWorkspaceWhole;
            amd-reference = amdReference;
            amd-vpp-reference = amdVppReference;
            amd-readout-reference = amdReadoutReference;
          };
          devShells = devShells // {
            vaapi-dpb = import ./nix/shell.nix {
              inherit pkgs;
              name = "gcc";
              inherit (toolchains.gcc) stdenv;
              package = vaDpbPackage;
              tracy = tracy.tracy;
            };
            capacity-return = import ./nix/shell.nix {
              inherit pkgs;
              name = "gcc-capacity-return";
              inherit (toolchains.gcc) stdenv;
              package = capacityReturnPackage;
              tracy = tracy.tracy;
            };
            capacity-return-clang = import ./nix/shell.nix {
              inherit pkgs;
              name = "clang-capacity-return";
              inherit (toolchains.clang) stdenv;
              package = capacityReturnPackage;
              tracy = tracy.tracy;
            };
            default = devShells.gcc;
            full = fullShell;
            readout = readoutShell;
            recorder = recorderShell;
            rgb = rgbShell;
            rgb-clang = rgbClangShell;
            mapping = mappingShell;
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
              encode-bits-word = pkgs.callPackage ./nix/encode-bits-check.nix {
                inherit (toolchains.gcc) stdenv;
                writerPatch = ./nix/mesa-encode-bits-word.patch;
                cumulative = true;
              };
              pool-lifetimes = pkgs.callPackage ./nix/pool-lifetimes-check.nix {
                inherit (toolchains.gcc) stdenv;
                ffmpeg = ffmpegCapacityReturn;
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
              # One instrumented build whose tests run linked against libTracyClient.
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

      # `nixpkgs.overlays = [ gpukey.overlays.default ]` gives `pkgs.gpukey`,
      # built with that nixpkgs' default toolchain.
      overlays.default = lib.composeManyExtensions [
        tracy-flake.overlays.default
        (final: _prev: {
          gpukey = final.callPackage ./nix/package.nix {
            tracy-client = final.tracy.client;
            ffmpeg-headless = final.callPackage ./nix/ffmpeg-capacity.nix { returnPolicy = true; };
            ffmpeg-tools = final.ffmpeg-headless;
          };
        })
      ];
    };
}

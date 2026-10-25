args@{
  lib,
  stdenv,
  callPackage,
  pkg-config,
  pipewire,
  glib,
  ffmpeg-headless,
  ffmpeg-tools ? ffmpeg-headless,
  libva,
  libdrm,
  liburing,
  vulkan-headers,
  vulkan-loader,
  dbus,
  # Tracy client library: headers plus libTracyClient.so.  Deliberately not
  # named `tracy`: callPackage would fill that from nixpkgs' own `tracy`, the
  # profiler GUI with its wayland/freetype/... closure.
  tracy-client,
  # Instrumented build: defines TRACY_ENABLE and links the client.
  withTracy ? false,
  withVulkanRgb ? false,
}:
stdenv.mkDerivation {
  pname = "gpukey" + lib.optionalString withTracy "-tracy";
  version = "0.1.0";

  # Exactly the files the Makefile reads.  Editing flake.nix, nix/, README.md
  # or .envrc leaves the source hash, and therefore the build, untouched.
  src = lib.fileset.toSource {
    root = ../.;
    fileset = lib.fileset.unions [
      ../Makefile
      (lib.fileset.fileFilter (file: file.hasExt "c" || file.hasExt "h") ../src)
      (lib.fileset.fileFilter (file: file.hasExt "c" || file.hasExt "h") ../tests)
      ../tests/integration.sh
      ../tests/cli.sh
      ../tests/replay-cli.sh
      ../tests/validation_gate.sh
      ../tools/check-vulkan-dpb.sh
      ../tests/pipewire.conf
    ];
  };

  __structuredAttrs = true;
  strictDeps = true;

  nativeBuildInputs = [ pkg-config ];
  nativeCheckInputs = [
    dbus
    (lib.getBin pipewire)
    (lib.getBin ffmpeg-tools)
  ];
  # TracyC.h expands to nothing without TRACY_ENABLE; a plain build references
  # neither the library nor its store path.
  buildInputs = [
    tracy-client
    pipewire
    glib
    ffmpeg-headless
    libva
    libdrm
    liburing
  ]
  ++ lib.optionals withVulkanRgb [
    vulkan-headers
    vulkan-loader
  ];

  makeFlags = [
    "PROFILE=release"
    # stdenv only sets a shell variable `prefix`; Make sees exported variables.
    "prefix=${placeholder "out"}"
  ]
  ++ lib.optional withTracy "TRACY=1"
  ++ lib.optional withVulkanRgb "VULKAN_RGB=1";
  enableParallelBuilding = true;

  doCheck = true;
  checkTarget = "check";
  checkPhase = ''
    runHook preCheck
    make "''${makeFlags[@]}" check
    make "''${makeFlags[@]}" feature-check
    make "''${makeFlags[@]}" integration
    runHook postCheck
  '';

  passthru = {
    # nix build .#default.tracy -- same toolchain and sources, TRACY_ENABLE on.
    tracy = callPackage ./package.nix (args // { withTracy = true; });
  };

  meta = {
    description = "Capability-driven Wayland GPU recorder";
    license = lib.licenses.mit;
    platforms = lib.platforms.linux;
    mainProgram = "gpukey";
  };
}

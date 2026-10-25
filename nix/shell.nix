# Development shell for one toolchain variant.  Compiler, linker and hardening
# all come from `stdenv`, so `nix develop .#clang-mold` is clang linking through
# mold in the shell exactly as in `nix build .#clang-mold`.
{
  pkgs,
  name,
  stdenv,
  package,
  tracy,
}:
(pkgs.mkShell.override { inherit stdenv; }) {
  # pkg-config, the Tracy client and GNU Make, as the package sees them.
  inputsFrom = [ package ];

  packages = [
    # tracy-profiler (GUI), tracy-capture, tracy-csvexport, ...  Dependency
    # lists take a package's `dev` output, and tracy's `dev` deliberately
    # propagates only `lib` (no profiler in library consumers' closures), so
    # the binaries have to be asked for explicitly.
    (pkgs.lib.getBin tracy)
    pkgs.samply
    # clangd, clang-format, clang-tidy; gcc builds are served through compile_commands.json.
    pkgs.clang-tools
    pkgs.gdb
    pkgs.libva-utils
    pkgs.vulkan-tools
    pkgs.vulkan-loader
    pkgs.vulkan-headers
    # Headless graphics checks for the optional process-local Mesa changes.
    pkgs.libglvnd
    pkgs.libgbm
    pkgs.python3
    pkgs.jq
    (pkgs.lib.getBin pkgs.ffmpeg-headless)
  ];

  # The nixpkgs hardening set (-D_FORTIFY_SOURCE=3, -fstack-protector-strong,
  # PIE, ...) is right for `nix build` and wrong for a debug loop: fortify warns
  # at -O0 and hides overflows from ASan.  Here the Makefile's flags are the flags.
  hardeningDisable = [ "all" ];

  env = {
    # Keys the Makefile's build tree (build/<toolchain>-<profile>), so switching
    # shells never reuses objects from another compiler or linker.
    TOOLCHAIN = name;
    # stdenv's preHook sets this to 1 unless already set; 1 makes the wrapper drop
    # -march=native (an impurity in `nix build`, a feature in a shell).
    NIX_ENFORCE_NO_NATIVE = "0";
  };
}

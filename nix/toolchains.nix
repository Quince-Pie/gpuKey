# The compiler x linker matrix, as complete stdenvs.  Every flake output
# (packages, devShells, checks) is derived from this set, so adding a compiler
# or a linker here is the whole change:
#
#   compilers.gcc15 = pkgs.gcc15Stdenv;
#   compilers.clang22 = pkgs.llvmPackages_22.stdenv;
#
# Measured with nix 2.34.8 on nixpkgs 26.11pre (x86_64-linux): one plain
# package evaluates in ~142k thunks, the whole six-entry matrix in ~399k, less
# than three single variants, because the adapters only re-wrap bintools and
# cc; the compiler, mold and wild derivations are shared.
{ pkgs }:
let
  inherit (pkgs) lib stdenvAdapters;

  compilers = {
    # The same derivation as pkgs.stdenv on Linux: no extra wrapper to build.
    gcc = pkgs.gccStdenv;
    # clang + GNU binutils + libstdc++, as nixpkgs builds its own clang packages.
    clang = pkgs.clangStdenv;
  };

  # `ld` is whatever the stdenv ships (GNU ld.bfd).  The nixpkgs adapters swap
  # the bintools wrapper: mold is exposed as ld.mold plus -fuse-ld=mold, wild
  # replaces `ld` itself, so neither compiler needs a flag it does not know.
  linkers = {
    ld = lib.id;
    mold = stdenvAdapters.useMoldLinker;
    wild = stdenvAdapters.useWildLinker;
  };

  name = compiler: linker: if linker == "ld" then compiler else "${compiler}-${linker}";
in
lib.concatMapAttrs (
  compiler: stdenv:
  lib.mapAttrs' (
    linker: adapt:
    lib.nameValuePair (name compiler linker) {
      inherit compiler linker;
      stdenv = adapt stdenv;
    }
  ) linkers
) compilers

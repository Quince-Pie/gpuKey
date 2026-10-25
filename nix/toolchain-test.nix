# Proves a built binary came from the advertised compiler and linker by reading
# the .comment section both of them stamp, then runs it.
{
  lib,
  runCommand,
  binutils-unwrapped,
}:
{
  package,
  compiler,
  linker,
}:
runCommand "${package.pname}-toolchain-${compiler}-${linker}"
  {
    nativeBuildInputs = [ binutils-unwrapped ];
  }
  ''
    bin=${lib.getExe package}
    comment=$(readelf --string-dump=.comment "$bin")
    printf '%s\n' "$comment"
    expect() { grep -q -- "$1" <<< "$comment" || { echo "expected '$1' in .comment" >&2; exit 1; }; }
    forbid() { ! grep -q -- "$1" <<< "$comment" || { echo "unexpected '$1' in .comment" >&2; exit 1; }; }
    # glibc's crt objects always contribute a "GCC:" line, so gcc is proven negatively.
    case ${compiler} in
      gcc) forbid "clang version" ;;
      clang) expect "clang version" ;;
    esac
    case ${linker} in
      ld) forbid "mold "; forbid "Wild" ;;
      mold) expect "mold " ;;
      wild) expect "Wild" ;;
    esac
    "$bin" --version > /dev/null
    touch "$out"
  ''

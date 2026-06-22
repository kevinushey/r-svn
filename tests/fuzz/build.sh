#!/bin/sh
#
# Build the in-tree R fuzz targets listed in fuzz_tests.txt.
#
# Each target is compiled from fuzzer.c into its own binary with
#   -D_R_FUZZ_ONE -D_R_FUZZ_<target>
# so the binary contains exactly one fuzz target, matching how OSS-Fuzz
# builds them.
#
# Requirements:
#   - clang with libFuzzer (-fsanitize=fuzzer).  On macOS this is the
#     LLVM clang from Homebrew (`brew install llvm`), not Apple clang.
#   - An R built with --enable-R-shlib (so libR is available to link).
#     Point the script at it with R_HOME, or have `R` on PATH.
#
# Environment overrides:
#   CC                  compiler (default: clang)
#   R_HOME              R installation/build to link against
#   SANITIZERS          -fsanitize list (default: fuzzer,address,undefined)
#   CFLAGS              if set, used verbatim instead of the default flags
#                       (the caller is then responsible for the fuzzing
#                       engine, e.g. via -fsanitize=fuzzer)
#   LIB_FUZZING_ENGINE  if set, appended to the link line instead of
#                       relying on -fsanitize=fuzzer (OSS-Fuzz sets this)
#   OUT                 output directory for the binaries (default: here)
#
set -eu

here=$(cd "$(dirname "$0")" && pwd)
out=${OUT:-$here}

CC=${CC:-clang}
SANITIZERS=${SANITIZERS:-fuzzer,address,undefined}

# Locate R.
if [ -n "${R_HOME:-}" ]; then
    rhome=$R_HOME
elif command -v R >/dev/null 2>&1; then
    rhome=$(R RHOME)
else
    echo "error: set R_HOME or put R on PATH" >&2
    exit 1
fi

rinc="$rhome/include"
rlib="$rhome/lib"
if [ ! -f "$rinc/Rinternals.h" ]; then
    echo "error: R headers not found at $rinc" >&2
    exit 1
fi

# Compile flags: honor a caller-supplied CFLAGS, else use our defaults.
if [ -n "${CFLAGS:-}" ]; then
    cflags="$CFLAGS -I$rinc"
else
    cflags="-g -O1 -fno-omit-frame-pointer -fsanitize=$SANITIZERS -I$rinc"
fi

# Link flags: shared libR, with an rpath so the binary finds it at run
# time without LD_LIBRARY_PATH / DYLD_LIBRARY_PATH.
ldflags="-L$rlib -lR -Wl,-rpath,$rlib ${LIB_FUZZING_ENGINE:-}"

mkdir -p "$out"

while read -r target; do
    case $target in
        ''|\#*) continue ;;
    esac
    echo "building $target ..."
    # shellcheck disable=SC2086
    $CC $cflags -D_R_FUZZ_ONE -D"_R_FUZZ_$target" \
        "$here/fuzzer.c" -o "$out/$target" \
        $ldflags
done < "$here/fuzz_tests.txt"

echo "done."
echo "run e.g.:  R_HOME=$rhome $out/fuzz_parse $here/fuzz_parse_corpus"

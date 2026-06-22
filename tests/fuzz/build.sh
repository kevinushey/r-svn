#!/bin/sh
#
#  R : A Computer Language for Statistical Data Analysis
#  Copyright (C) 1995--2026  The R Core Team
#
#  This program is free software; you can redistribute it and/or modify
#  it under the terms of the GNU General Public License as published by
#  the Free Software Foundation; either version 2 of the License, or
#  (at your option) any later version.
#
#  This program is distributed in the hope that it will be useful,
#  but WITHOUT ANY WARRANTY; without even the implied warranty of
#  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
#  GNU General Public License for more details.
#
#  You should have received a copy of the GNU General Public License
#  along with this program; if not, a copy is available at
#  https://www.R-project.org/Licenses/
#
# Build the in-tree R fuzz targets listed in fuzz_tests.txt.
#
# Each target is compiled from fuzzer.c into its own binary with
#
#   -D_R_FUZZ_ONE -D_R_FUZZ_<target>
#
# so the binary contains exactly one fuzz target, matching how OSS-Fuzz
# builds them.
#
# Requirements:
#   - clang with libFuzzer (-fsanitize=fuzzer). Not Apple Clang.
#   - An R built with --enable-R-shlib (so libR is available to link).
#     Point the script at it with R_HOME, or have `R` on PATH.
#
# The sanitizer and coverage instrumentation are NOT chosen here: they
# come from how R itself was configured.  R records its build flags in
# etc/Makeconf, and we read them back with `R CMD config`, so the fuzz
# targets are compiled with the same flags as libR and the two stay in
# sync.  Build R for fuzzing with e.g.
#
#   export CC="clang -fsanitize=address,fuzzer-no-link"
#   export CFLAGS="-g -O1 -fno-omit-frame-pointer"
#   ./configure --enable-R-shlib
#
# See README.md.
#
# Environment overrides:
#
#   R_HOME              R installation/build to link against (else `R` on
#                       PATH)
#   CC                  compiler; default: R's own (`R CMD config CC`),
#                       carrying R's sanitizer/coverage flags
#   CFLAGS              compile flags; default: R's own (`R CMD config
#                       CFLAGS` plus `CPPFLAGS`)
#   LIB_FUZZING_ENGINE  if set, linked in as the fuzzing engine instead of
#                       -fsanitize=fuzzer (OSS-Fuzz sets this)
#   OUT                 output directory for the binaries (default: here)
#
set -eu

here=$(cd "$(dirname "$0")" && pwd)
out=${OUT:-$here}

# Locate R and a binary to query its build flags with.
if [ -n "${R_HOME:-}" ]; then
    rhome=$R_HOME
elif command -v R >/dev/null 2>&1; then
    rhome=$(R RHOME)
else
    echo "error: set R_HOME or put R on PATH" >&2
    exit 1
fi

rbin="$rhome/bin/R"
[ -x "$rbin" ] || rbin=R

# Ask R itself for its include and library directories rather than
# assuming the layout under R_HOME.
rinc=$("$rbin" -s -e 'cat(R.home("include"))')
rlib=$("$rbin" -s -e 'cat(R.home("lib"))')
if [ ! -f "$rinc/Rinternals.h" ]; then
    echo "error: R headers not found at $rinc" >&2
    exit 1
fi

# Inherit the compiler and flags R was built with, so the targets get the
# same sanitizer/coverage instrumentation as libR (see README).  A
# caller-supplied CC/CFLAGS (e.g. from the OSS-Fuzz environment) wins.
#
# --no-user-files/--no-site-files is essential: it makes `R CMD config`
# report the flags from etc/Makeconf (how R was actually built) rather
# than the developer's personal ~/.R/Makevars, which carries unrelated
# package-build preferences and would not match libR.
r_cmd_config() { "$rbin" CMD config --no-user-files --no-site-files "$@"; }

CC=${CC:-$(r_cmd_config CC)}

if [ -n "${CFLAGS:-}" ]; then
    cflags=$CFLAGS
else
    cflags="$(r_cmd_config CFLAGS) $(r_cmd_config CPPFLAGS)"
fi
cflags="$cflags -I$rinc"

# Link against libR with an rpath (resolves libR and its private libs on
# ELF) and add the fuzzing engine: OSS-Fuzz supplies its own via
# LIB_FUZZING_ENGINE, otherwise use libFuzzer.
ldflags="-L$rlib -lR -Wl,-rpath,$rlib"
if [ -n "${LIB_FUZZING_ENGINE:-}" ]; then
    ldflags="$ldflags $LIB_FUZZING_ENGINE"
else
    cflags="$cflags -fsanitize=fuzzer"
    # Local build (not OSS-Fuzz): bake in R_HOME so the binary runs
    # without the caller setting it.  Non-overwriting at run time; see
    # fuzzer.c.
    cflags="$cflags -DR_FUZZ_R_HOME=\"$rhome\""
fi

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

    # Under OSS-Fuzz, stage the dictionary and seed corpus beside the
    # binary in the layout ClusterFuzz looks for: <target>.dict (applied
    # automatically) and <target>_seed_corpus.zip.  Locally these are
    # found by the launcher instead (see below), so skip the staging.
    if [ -n "${LIB_FUZZING_ENGINE:-}" ]; then
        if [ -f "$here/dictionaries/$target.dict" ]; then
            cp "$here/dictionaries/$target.dict" "$out/$target.dict"
        fi
        if [ -d "$here/${target}_corpus" ]; then
            rm -f "$out/${target}_seed_corpus.zip"
            (cd "$here/${target}_corpus" && zip -q -r "$out/${target}_seed_corpus.zip" .)
        fi
    fi
done < "$here/fuzz_tests.txt"

# Generate the 'fuzz' launcher.  It runs a target with its default
# dictionary and seed corpus baked in, so a session needs no arguments:
#     fuzz <target> [libFuzzer args...]
# It also sets R's runtime library search path before exec -- R's private
# dylibs (libRblas, ...) use bare install names that the loader resolves
# through that path, which on macOS must be set before the process starts
# (dyld loads the whole graph at launch, so the binary cannot set it
# itself).  The build-time paths are emitted as quoted assignments; the
# rest of the script is literal.  Skipped under OSS-Fuzz, which runs the
# binaries directly.
if [ -z "${LIB_FUZZING_ENGINE:-}" ]; then
{
    echo '#!/bin/sh'
    echo '# Generated by build.sh.  Usage: fuzz <target> [libFuzzer args...]'
    echo "r_home='$rhome'"
    echo "r_lib='$rlib'"
    echo "bindir='$out'"
    echo "srcdir='$here'"
    cat <<'EOF'
set -eu

export R_HOME="$r_home"
case "$(uname -s)" in
    Darwin) export DYLD_FALLBACK_LIBRARY_PATH="$r_lib${DYLD_FALLBACK_LIBRARY_PATH:+:$DYLD_FALLBACK_LIBRARY_PATH}" ;;
    *)      export LD_LIBRARY_PATH="$r_lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}" ;;
esac

if [ $# -eq 0 ]; then
    echo "usage: fuzz <target> [libFuzzer args...]" >&2
    exit 2
fi
target=$1; shift

bin="$bindir/$target"
if [ ! -x "$bin" ]; then
    echo "fuzz: unknown target '$target' (no $bin)" >&2
    exit 2
fi

# Per-target defaults.  New inputs are written to a working corpus under
# the build output, never to the tracked seed corpus, which is read-only
# input here.
dict="$srcdir/dictionaries/$target.dict"
seeds="$srcdir/${target}_corpus"
work="$bindir/corpus/$target"

# Apply the defaults only where the caller did not override them: skip the
# dictionary if a -dict= was passed, and skip the corpus dirs if any
# positional argument (a file or directory) was given.
have_dict=no
have_positional=no
for a in "$@"; do
    case $a in
        -dict=*) have_dict=yes ;;
        -*)      ;;
        *)       have_positional=yes ;;
    esac
done

if [ "$have_dict" = no ] && [ -f "$dict" ]; then
    set -- -dict="$dict" "$@"
fi
if [ "$have_positional" = no ]; then
    mkdir -p "$work"
    set -- "$@" "$work" "$seeds"
fi

exec "$bin" "$@"
EOF
} > "$out/fuzz"
chmod +x "$out/fuzz"
fi

echo "done."
if [ -z "${LIB_FUZZING_ENGINE:-}" ]; then
    echo "run e.g.:  $out/fuzz parse"
fi


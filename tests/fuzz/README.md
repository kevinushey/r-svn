# R fuzz test harnesses

This directory contains coverage-guided fuzz targets for R's internal
parsers and decoders.  It is modeled on CPython's `Modules/_xxtestfuzz`
and is intended to be run by [OSS-Fuzz](https://github.com/google/oss-fuzz),
which continuously fuzzes the targets and reports findings to maintainers.

The harnesses are **self-contained**: they build against an unmodified R
and nothing here is referenced by R's build system, so a normal `make` /
`make check` ignores this directory entirely.

## Layout

```
fuzzer.c                 all fuzz targets + the libFuzzer entry points
fuzz_tests.txt           manifest: one target name per line
build.sh                 build each target against an installed R; also
                         emits the 'fuzz' launcher
dictionaries/            per-target token dictionaries (<name>.dict)
<name>_corpus/           per-target seed corpus
```

## How a target works

Each target is a function `static int fuzz_<name>(const char *data,
size_t size)` in `fuzzer.c`.  R is embedded once in
`LLVMFuzzerInitialize()` and reused across inputs; the libFuzzer entry
point `LLVMFuzzerTestOneInput()` dispatches to the targets.

Compiling with `-D_R_FUZZ_ONE -D_R_FUZZ_<name>` selects exactly one
target, producing one binary per target -- this is how OSS-Fuzz builds
them, one per name in `fuzz_tests.txt`.  Compiling without `_R_FUZZ_ONE`
includes every target and runs them all on each input, which is
convenient for a local smoke test.

The current targets:

| Target        | Exercises                                              |
|---------------|--------------------------------------------------------|
| `parse`       | the R parser (`R_ParseVector`), no evaluation          |
| `grep`        | the regex engines (TRE and PCRE2) via `grep`/`sub`     |
| `unserialize` | the deserializer (`unserialize`, i.e. `.rds`/`.RData`) |
| `dcf`         | the DCF reader (`read.dcf`, DESCRIPTION/PACKAGES)      |

## Adding a target

1. Add the name to `fuzz_tests.txt`.
2. In `fuzzer.c`, write `static int fuzz_<name>(const char *data, size_t
   size)` and, if the target needs reusable R objects or warmup, an
   `init_<name>(void)`.  Keep lifetime objects alive with
   `R_PreserveObject` (they must survive across inputs); PROTECT/UNPROTECT
   only per-input allocations.  Wrap any R evaluation in `eval_safe()` so
   R errors unwind through a top-level context.
3. Add a dispatch block in `LLVMFuzzerTestOneInput()` mirroring the
   existing ones (`#if !defined(_R_FUZZ_ONE) || defined(_R_FUZZ_<name>)`).
4. Optionally add a `<name>_corpus/` seed corpus and a
   `dictionaries/<name>.dict`; the `fuzz` launcher picks both up by name.

## Building and running locally

Prerequisites: a clang with libFuzzer (`-fsanitize=fuzzer`; on macOS use
the LLVM clang from `brew install llvm`, not Apple clang).

### Build R as a linkable library

The harness links against `libR`, so R must be built with
`--enable-R-shlib` (shared `libR`) or `--enable-R-static-lib` (static
`libR.a`).  `R_HOME` may point at either an installed R or an uninstalled
build tree; only `$R_HOME/include` and `$R_HOME/lib` are used, so
`make install` is not required.

For coverage-guided fuzzing to do anything useful, **R itself must be
instrumented** -- instrumenting only the harness is not enough (see "Why
R must be instrumented" below).  Configure R with the same sanitizer and
coverage flags the harness uses:

```sh
cd /path/to/R          # the R source tree
CC="clang -fsanitize=address,fuzzer-no-link" \
CFLAGS="-g -O1 -fno-omit-frame-pointer" \
    ./configure --enable-R-shlib
make
```

Recommended flags, and why:

- `-fsanitize=address` -- AddressSanitizer, the bug detector.  Keep it in
  `CC` (not just `CFLAGS`) so it is also applied when `libR` is linked.
- `-fsanitize=fuzzer-no-link` -- the SanitizerCoverage instrumentation
  that feeds libFuzzer its coverage signal, *without* adding a fuzzer
  `main` (that comes from the harness's `-fsanitize=fuzzer`).  This is
  what makes the fuzzing coverage-guided rather than black-box.
- `-g` -- debug info, so reports carry symbolized `file:line` frames.
- `-O1` -- the AddressSanitizer-recommended optimization level: `-O0`
  fuzzes too slowly to be useful, while `-O2`/`-O3` build more slowly,
  give noisier traces, and can fold away the undefined behaviour UBSAN
  would otherwise see.  This matches the harness (`build.sh` uses
  `-g -O1`).
- `-fno-omit-frame-pointer` -- fast, reliable stack unwinding for the
  sanitizer traces.

On macOS add `CC=/opt/homebrew/opt/llvm/bin/clang ...` (Apple clang ships
neither libFuzzer nor the coverage runtime).  Keep the sanitizer set
consistent between this R build and `build.sh`: if you add `undefined`
(UBSAN) to one, add it to both.  For a fully instrumented R you can also
set `CXX` (and `FC` with a sanitizer-capable Fortran); the current
targets are all C, so `CC`/`CFLAGS` already cover the code they reach.

A plain `./configure --enable-R-shlib` with no sanitizer flags still
links and runs as a quick smoke test, but with an uninstrumented `libR`
libFuzzer gets no coverage signal and ASan cannot see bugs inside R's own
code.

### Build the fuzz targets and run

```sh
cd tests/fuzz

# build one binary per target, plus the 'fuzz' launcher, into this
# directory (set OUT=<dir> to write elsewhere)
R_HOME=/path/to/R ./build.sh

# fuzz a target -- dictionary and seed corpus are supplied automatically
./fuzz parse

# replay the seed corpus once and exit (smoke test: nonzero if any input
# crashes)
./fuzz parse -runs=0

# extra libFuzzer flags pass straight through
./fuzz parse -max_total_time=60
```

`./fuzz <target>` runs the target with its default dictionary
(`dictionaries/<target>.dict`) and seed corpus (`<target>_corpus/`) baked
in, so no paths are needed.  Newly discovered inputs are written to a
separate working corpus under the build output (`<OUT>/corpus/<target>`),
never to the tracked seed corpus, which is read-only input -- so a fuzzing
run never dirties the checked-in seeds.  Passing `-dict=` or any
positional file/directory argument overrides the corresponding default.

`R_HOME` is only needed by `build.sh`, to locate the R to build against.
At run time the targets need no environment setup: `build.sh` bakes the
build's `R_HOME` into each binary, and the `fuzz` launcher puts R's
private libraries (`libRblas`, ...) on the loader path before exec --
something that on macOS must happen before the process starts, so it
cannot be baked into the binary.  (On ELF the rpath alone resolves those
libraries, so the binaries can also be run directly there; `fuzz` works
everywhere.)

`build.sh` does not pick the sanitizers itself.  It reads the compiler
and flags R was built with (`R CMD config --no-user-files --no-site-files
CC` / `CFLAGS` / `CPPFLAGS`, which R records in `etc/Makeconf`) and reuses
them, so the targets get exactly the same instrumentation as `libR`.  The
`--no-user-files`/`--no-site-files` matters: without it `R CMD config`
folds in the developer's personal `~/.R/Makevars`, which carries
unrelated package-build flags that would not match `libR`.  `build.sh`
then adds only R's headers and the fuzzing engine (`-fsanitize=fuzzer`,
or `LIB_FUZZING_ENGINE` if set).  Consequences:

- To change sanitizers (e.g. add `undefined` for UBSAN -- off by default
  because R's integer/float cast paths emit a high volume of reports that
  would drown out other findings for now), change how *R* is configured;
  the fuzz build follows automatically.
- `CC` and `CFLAGS` still override the inherited values (this is how the
  OSS-Fuzz environment drives the build), and `OUT` sets the output
  directory.

### Why R must be instrumented

Both mechanisms that make this useful are inserted per translation unit
at compile time, so they only cover code that was built with the flags:

- **Coverage.**  If only the harness carries SanitizerCoverage, libFuzzer
  sees coverage for the few hundred lines of harness (which never change
  across inputs) and nothing inside R.  It cannot tell which inputs reach
  new parser/deserializer states, so it degenerates into random,
  black-box fuzzing.
- **Detection.**  ASan's out-of-bounds and use-after-free checks are
  likewise inserted around the loads and stores in instrumented code.
  The bugs here live inside R's parser, regex engine, and deserializer,
  so those translation units must be instrumented or the accesses are
  simply not checked.  (ASan's global `malloc`/`free` interceptors still
  catch a few things process-wide, but not the out-of-bounds reads that
  are the main quarry.)

## OSS-Fuzz integration

OSS-Fuzz discovers the targets from `fuzz_tests.txt` and builds one
binary per name.  The OSS-Fuzz project's `build.sh` (which lives in the
oss-fuzz repository, not here) builds R first -- preferably as a static
library (`--enable-R-static-lib`) so the fuzz binaries are
self-contained -- then compiles each target with the project's
`$CC`/`$CFLAGS`/`$LIB_FUZZING_ENGINE`, e.g. by invoking this `build.sh`.

When `LIB_FUZZING_ENGINE` is set (the OSS-Fuzz case), `build.sh` also
stages each target's dictionary and seed corpus beside its binary in
`$OUT` under the names ClusterFuzz expects -- `<target>.dict` (applied
automatically) and `<target>_seed_corpus.zip` -- and skips the local
`fuzz` launcher.  So the oss-fuzz `build.sh` only has to build R and call
this script.

## Relationship to AFL++

The targets use the libFuzzer entry-point contract
(`LLVMFuzzerInitialize` / `LLVMFuzzerTestOneInput`).  AFL++ consumes the
same contract through its libFuzzer-compatible driver, so the same
`fuzzer.c` can be driven by either engine.

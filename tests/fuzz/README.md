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

# build (defaults to $R_HOME/tests/fuzz, i.e. under the R build tree;
# set OUT=<dir> to write elsewhere)
R_HOME=/path/to/R ./build.sh
cd /path/to/R/tests/fuzz

# fuzz a target -- dictionary and seed corpus are supplied automatically
./fuzz parse

# replay the corpus once and exit (smoke test: nonzero if any input
# crashes)
./fuzz parse -runs=0

# extra libFuzzer flags pass straight through
./fuzz parse -max_total_time=60
```

The output directory defaults to `$R_HOME/tests/fuzz` -- under the R build
tree, beside the `libR` the targets link against, and (for the usual
out-of-tree R build) outside the source checkout.  An in-tree R build
puts it back in the source `tests/fuzz`, where a `.gitignore` keeps the
artifacts out of git.  Override with `OUT`.

`build.sh` makes that directory a self-contained bundle: each target's
binary, its `<target>.dict`, its `corpus/<target>/` (the seed corpus,
copied from the tree), and the `fuzz` and `campaign` helpers all live
there.  The helpers resolve everything relative to their own location, so
the bundle can be moved or run from anywhere (only R itself is referenced
by an absolute path).

`./fuzz <target>` runs the target with that dictionary and corpus, so no
paths are needed.  Newly discovered inputs accumulate in
`corpus/<target>/`, so a later run resumes from them rather than starting
cold -- and because that corpus is the build's own copy, fuzzing never
touches the checked-in seeds in the source tree.  Passing `-dict=` or any
positional file/directory argument overrides the corresponding default.

`R_HOME` is only needed by `build.sh`, to locate the R to build against.
At run time the targets need no environment setup: `build.sh` bakes the
build's `R_HOME` into each binary, and the `fuzz` launcher puts R's
private libraries (`libRblas`, ...) on the loader path before exec --
something that on macOS must happen before the process starts, so it
cannot be baked into the binary.  (On ELF the rpath alone resolves those
libraries, so the binaries can also be run directly there; `fuzz` works
everywhere.)

### Running a campaign

For a real, indefinite, crash-tolerant run use the `campaign` helper:

```sh
./campaign parse        # 4 workers (default)
./campaign grep 8       # 8 workers
                        # Ctrl-C to stop
```

It launches N independent `fuzz` workers that share the one corpus
(libFuzzer cross-pollinates new inputs via `-reload`) and restarts any
worker that exits, so a crash is recorded and fuzzing continues rather
than stopping at the first finding.  Crash-inducing inputs
(`crash-*`/`oom-*`/`timeout-*`) and a per-worker log land in
`findings/<target>/`.

This deliberately does **not** use libFuzzer's own `-fork`/`-jobs`: those
spawn their workers through `/bin/sh`, and on macOS SIP strips the
`DYLD_*` library-path the workers need, so they abort.  Driving `fuzz`
directly (which sets the path and `exec`s the binary itself) sidesteps
that, so the same campaign works on Linux and macOS.  On Linux you may
still use `./fuzz <target> -jobs=N -workers=N` if you prefer libFuzzer's
built-in parallelism.

To get a symbolized report for a saved finding, replay it through the
target:

```sh
./fuzz grep findings/grep/crash-XXXX 2>&1 | sed -n '/ERROR: /,/SUMMARY/p'
```

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

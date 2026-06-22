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
build.sh                 build each target against an installed R
dictionaries/            per-target token dictionaries (fuzz_<name>.dict)
fuzz_<name>_corpus/       per-target seed corpus
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

| Target             | Exercises                                            |
|--------------------|------------------------------------------------------|
| `fuzz_parse`       | the R parser (`R_ParseVector`), no evaluation        |
| `fuzz_grep`        | the regex engines (TRE and PCRE2) via `grep`/`sub`   |
| `fuzz_unserialize` | the deserializer (`unserialize`, i.e. `.rds`/`.RData`) |
| `fuzz_dcf`         | the DCF reader (`read.dcf`, DESCRIPTION/PACKAGES)    |

## Adding a target

1. Add the name to `fuzz_tests.txt`.
2. In `fuzzer.c`, write `static int fuzz_<name>(const char *data, size_t
   size)` and, if the target needs reusable R objects or warmup, an
   `init_<name>(void)`.  Keep lifetime objects alive with
   `R_PreserveObject` (they must survive across inputs); PROTECT/UNPROTECT
   only per-input allocations.  Wrap any R evaluation in `eval_safe()` so
   R errors unwind through a top-level context.
3. Add a dispatch block in `LLVMFuzzerTestOneInput()` mirroring the
   existing ones (`#if !defined(_R_FUZZ_ONE) || defined(_R_FUZZ_fuzz_<name>)`).
4. Optionally add `fuzz_<name>_corpus/` seeds and a
   `dictionaries/fuzz_<name>.dict`.

## Building and running locally

Prerequisites: a clang with libFuzzer (`-fsanitize=fuzzer`; on macOS use
the LLVM clang from `brew install llvm`, not Apple clang) and an R built
with `--enable-R-shlib`.

```sh
# point at the R to link against (or have `R` on PATH)
R_HOME=/path/to/R ./build.sh

# replay a corpus once (smoke test): exits 0 if no input crashes
R_HOME=/path/to/R ./fuzz_parse fuzz_parse_corpus/*

# run a fuzzing session
R_HOME=/path/to/R ./fuzz_parse -dict=dictionaries/fuzz_parse.dict fuzz_parse_corpus
```

`build.sh` defaults to `-fsanitize=fuzzer,address`.  UBSAN is off by
default (`SANITIZERS=fuzzer,address,undefined` adds it): R's integer and
float cast paths emit a high volume of reports that would drown out other
findings for now.  Set `SANITIZERS`, `CC`, or `CFLAGS` to override; the
script also honors `LIB_FUZZING_ENGINE` and `OUT` so the OSS-Fuzz build
environment can reuse it after building R.

## OSS-Fuzz integration

OSS-Fuzz discovers the targets from `fuzz_tests.txt` and builds one
binary per name.  The OSS-Fuzz project's `build.sh` (which lives in the
oss-fuzz repository, not here) builds R first -- preferably as a static
library (`--enable-R-static-lib`) so the fuzz binaries are
self-contained -- then compiles each target with the project's
`$CC`/`$CFLAGS`/`$LIB_FUZZING_ENGINE`, e.g. by invoking this `build.sh`.

## Relationship to AFL++

The targets use the libFuzzer entry-point contract
(`LLVMFuzzerInitialize` / `LLVMFuzzerTestOneInput`).  AFL++ consumes the
same contract through its libFuzzer-compatible driver, so the same
`fuzzer.c` can be driven by either engine.

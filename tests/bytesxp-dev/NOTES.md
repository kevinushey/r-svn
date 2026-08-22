# BYTESXP: current design and invariants

`BYTESXP` is the internal representation for vectors whose elements have
a per-vector byte width. It supports opaque byte strings and signed or
unsigned fixed-width integers. This note records the current design;
the commit history contains the discarded prototypes and implementation
stages.

The user-facing reference is `?bytes`. The package-facing C interface
is documented under “bytes vectors” in *Writing R Extensions*.

## Representation

A vector has four defining properties:

| Property | Representation |
| --- | --- |
| element count | `XLENGTH(x)` |
| width, 1–255 bytes | `gp` bits 8–15 |
| kind: opaque, unsigned, signed | `gp` bits 0–1 |
| whether an `NA` sentinel is reserved | inverse flag in `gp` bit 2 |

`XLENGTH()` counts elements, not bytes. The payload therefore occupies
`XLENGTH(x) * BYTEVEC_WIDTH(x)` bytes. This is why the representation is
a distinct `SEXPTYPE` rather than a strided `RAWSXP`, whose length is a
byte count.

Width, kind and the sentinel policy together form the element type.
Generic allocation with `allocVector(BYTESXP, n)` is invalid because it
does not provide those properties. Code that needs another vector like
an existing one uses `R_allocVectorLike()` or `R_allocMatrixLike()`.

The `gp` fields are serialized before allocation, so the reader knows
the element size when it allocates the payload. The “no NA” flag is
inverted: a clear bit means the default policy, with an `NA` sentinel.

## R-level identity

All variants share the structural type reported by `typeof()`:

| Kind | Example `storage.mode()` | `mode()` | implicit `class()` |
| --- | --- | --- | --- |
| opaque | `"bytes16"` | `"bytes"` | `c("bytes16", "bytes")` |
| unsigned | `"uint64"` | `"numeric"` | `"uint64"` |
| signed | `"int64"` | `"numeric"` | `"int64"` |

For every row, `typeof(x)` is `"bytes"`, and both `is.bytes(x)` and
`is.fixedwidth(x)` are true. `bytesKind()` or `is.numeric()` separates
opaque and numeric values.

The detailed storage-mode name contains the kind and width but not the
sentinel policy. Interfaces that must preserve the complete element
type accept a prototype vector. For example, `vector(x, n)` preserves
`bytesHasNA(x)`, whereas `vector(storage.mode(x), n)` uses the default
sentinel policy. Plain `"bytes"` is incomplete and is not accepted as
a storage mode.

Assigning an implicit class back is a no-op. In particular,
`class(x) <- class(x)` leaves a bare vector bare. The two-element
implicit class belongs only to the opaque kind; on a numeric vector,
`class(x) <- c("uint64", "bytes")` intentionally installs an explicit
S3 class.

## Kinds and storage order

Opaque elements are byte strings. They are stored verbatim, ordered
lexicographically by unsigned byte value and rendered as hexadecimal.

Signed and unsigned elements are integers of `8 * width` bits. They
are stored in native byte order so a native value can be copied into
the payload directly, but they are ordered by numeric value and rendered
as decimal. Serialization normalizes numeric payloads to most-
significant-byte first; opaque payloads are written verbatim. Thus
sorting and serialized values are portable even though in-memory
numeric storage is native-endian.

Numeric `NA` sentinels follow the interpretation:

| Kind | Reserved value when `na = TRUE` |
| --- | --- |
| opaque | all bits set |
| unsigned | maximum unsigned value |
| signed | most negative signed value |

With `na = FALSE`, every bit pattern is a value. An operation that
would need to create a missing value then raises an error. This
includes out-of-range or missing subscripts, `length<-` growth, join
misses and arithmetic overflow. Vectors with different sentinel
policies are different element types and cannot be combined, compared
or matched without explicit conversion.

## Coercion and operations

Two `BYTESXP` operands must have the same width, kind and sentinel
policy unless an explicit conversion is requested. Widths are not
implicitly promoted.

Logical and integer operands narrow into a numeric fixed-width operand.
For arithmetic, an unrepresentable value becomes `NA` with a warning;
for comparison and matching, it is ordered outside the representable
range or treated as absent. Double and complex operands promote the
fixed-width value to the ordinary R type. Conversion to double warns
only when an actual value loses precision.

Arithmetic is defined for signed and unsigned widths 1, 2, 4, 8 and 16
bytes. Wider numeric elements remain exact storage values but do not
support fixed-width arithmetic. Bitwise operations work at every width
and for the opaque kind as well.

Opaque values support the operations that need only equality or order,
including subsetting, concatenation, comparison, matching, hashing,
sorting, tables and factors. Numeric kinds additionally support
arithmetic, summaries, numeric coercion, `Math`, exact unit-step
sequences and exact accumulation for `mean()` before its final
conversion to double.

Character conversion is reversible: hexadecimal for opaque values and
decimal for numeric values. `bytesRaw()` exposes storage order and is
therefore intended for round trips and native interfaces, not as a
portable numeric encoding.

## Allocation and C API

Packages allocate with:

```c
SEXP R_allocBytesVector(R_xlen_t n, int width, int kind,
                        Rboolean hasNA);
```

The public kinds are `BYTES_OPAQUE`, `BYTES_UNSIGNED` and
`BYTES_SIGNED`. `R_bytesWidth()`, `R_bytesKind()` and
`R_bytesHasNA()` inspect the element type. `R_bytesElt()` and
`R_bytesEltRO()` address one element; `R_bytesIsNA()` and
`R_bytesSetNA()` handle missingness. `BYTES()` and `BYTES_RO()` expose
the whole payload.

Ordinary typed accessors (`INTEGER`, `REAL`, `RAW` and their element
forms) reject `BYTESXP` by type. The untyped `DATAPTR` family also
rejects it so code cannot read a payload without first accounting for
its width and kind. `BYTESXP` is not ALTREP.

## Serialization

Streams containing `BYTESXP` require serialization version 4. Writers
select version 4 automatically when no version was specified; an
explicit older version is rejected before a destination file is opened.
The save magic remains `RDX3` (or its ASCII/XDR sibling) because the
serialization header that follows carries the actual version.

Numeric payloads are normalized to big-endian element order on the
wire. Opaque payloads are unchanged. Processing is chunked in whole
elements so an element never straddles a serialization chunk.

## Implementation invariants

- Never allocate a result with `allocVector(TYPEOF(x), n)` when `x`
  may be `BYTESXP`; use the “like” allocators so width, kind and sentinel
  policy survive.
- Never infer compatibility from `TYPEOF()` alone. Use
  `R_bytesCheckSameType()` or the corresponding settlement helper.
- Check `BYTEVEC_HAS_NA(x)` before interpreting a sentinel pattern.
  Under `na = FALSE`, the same bits are an ordinary value.
- Numeric payloads use native byte order; opaque payloads do not.
- Preserve attributes through the same outer machinery used by the
  ordinary atomic types. Low-level kernels return bare vectors.
- A `BYTESXP` result must retain width, kind and sentinel policy through
  subsetting, iteration, binding, matrix operations and serialization.

## Test suites

`make test-BytesXP` runs:

- `gauntlet.R`: public behavior and regression cases;
- `endcheck.R`: storage and wire byte order;
- `pcheck.R` and `xcheck.R`: value, text and ordering cross-checks;
- `archeck.R`: exact arithmetic and native/general kernel agreement;
- `realcheck.R`: correctly rounded conversion to double;
- `rxcheck.R`: radix ordering and stability with heavy ties.

The reference arithmetic in `bignum.R` uses decimal digit vectors and
shares no implementation with the binary-byte kernels. Each suite
runs its self-test before using it as an oracle. `make test-BytesFFI`
separately checks the package-facing C boundary and guarded accessors.

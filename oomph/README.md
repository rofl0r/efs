# Minimal Perfect Hash Function Toolkit

A collection of tools that build and consume **minimal perfect hash
functions (MPHF)** based on the BBHash / BoomPHF algorithm
(arXiv:1702.03154). They take a set of keys and emit a compact structure that
maps each key to a unique index in `1..n` (a permutation), in O(1) time.

There are two "eras" here:

- The legacy **numeric-key** generator (`boomphf.c`) and the **string-key**
  generators that emit C source (`soomph.c`, `goomph.c`).
- The factored-out **single-header library** (`mph.h`) plus its generator
  (`mph_builder.c`), which is the recommended path going forward.

---

## Repository layout

| File             | Role                                                                 |
|------------------|----------------------------------------------------------------------|
| `tlist.h`        | Single-header treap (randomized BST) used as a sorted list. API/IMPL macros. |
| `boomphf.c`      | Legacy numeric-key MPHF generator. Reads `uint64` per line, emits C.  |
| `soomph.c`       | String-key generator emitting **named tables** (`bv_i`/`rank_i`).     |
| `goomph.c`       | String-key generator emitting a **single `uint8_t` blob** + decoder.  |
| `mph.h`          | **The library.** Single-header MPHF: producer (`mph_build`) + consumer (`mph_lookup`). |
| `mph_builder.c`  | String-key generator that uses `mph.h` and emits a `mph_blob[]` + `#include "mph.h"`. |
| `test_soomph.sh` | Test harness: generate -> compile `-DTEST` -> re-pipe -> verify.      |
| `Makefile`       | Builds all generators and runs the harness.                           |

---

## Quick start

Build everything:

```sh
make
```

Run the full test suite (verifies every generator produces a valid MPHF for
`N_INPUTS` keys, default 10000; override with `N_INPUTS=...`):

```sh
make test
make test N_INPUTS=50000
```

Generate a blob for a set of string keys (one per line) and produce a
self-contained C table:

```sh
seq 10000 | ./mph_builder > table.c
cc -DTEST -o table table.c      # table.c reads keys from stdin, prints "<key>\t<rank>"
./table < keys.txt
```

---

## The `mph.h` library (recommended)

`mph.h` is a "single header" library. It has two roles controlled by macros:

- **Producer** — define `MPH_IMPL` (in exactly one translation unit) before
  including. This exposes `mph_build()`.
- **Consumer** — just `#include "mph.h"`. This exposes `mph_lookup()` (plus the
  small portable decoder helpers).

`MPH_API` / `MPH_INTERNAL` mirror the `tlist.h` convention: `MPH_API`
defaults to `extern`; define it to `static` for internal linkage.

### Contracts (important!)

- **Input to `mph_build()` MUST BE UNIQUE.** Duplicate (non-unique) keys are a
  contract violation. `mph_build()` detects them and returns `NULL` (printing a
  message to stderr). A minimal perfect hash is only well-defined for a set of
  distinct keys.
- **`mph_lookup()` returns 1-BASED indices.** `0` means "key not found". To get
  a 0-based index, subtract 1 — but only when the result is non-zero:

  ```c
  uint64_t r = mph_lookup(table, tablen, key, keylen);
  size_t idx = (r == 0) ? SENTINEL : (size_t)(r - 1);
  ```

### Producer API

```c
uint8_t *mph_build(const char * const *keys, size_t n_keys, size_t *out_len);
```

- `keys`     : array of `n_keys` NUL-terminated strings. **Must be unique.**
- `n_keys`   : number of keys.
- `out_len`  : out param; receives the blob byte length (set to 0 on failure).
- Returns    : a `malloc`'d `uint8_t` blob the caller owns (free it), or `NULL`
               on OOM or non-unique input.

### Consumer API

```c
uint64_t mph_lookup(const uint8_t *table, size_t tablen,
                    const char *key, size_t keylen);
```

- `table`    : the blob returned by `mph_build()` (or embedded as
               `static const uint8_t mph_blob[]`).
- `tablen`   : its byte length (e.g. `sizeof(mph_blob)`). Used for bounds
               checking against a truncated/corrupt blob.
- `key`/`keylen` : the key to look up (no NUL-termination assumed; pass length
               explicitly to avoid a `strlen` in hot paths).
- Returns    : `0` if not found, else the 1-based rank.

### Blob format (packed, little-endian, endian-agnostic)

```
[ u8  num_levels ]
[ u32 seed ]                        // baked-in hash seed (fits 2^20)
per level:
    [ u32 bitsize ]                 // in bits
    [ (bitsize+63)/64 * 8 bytes ]   bv  bitvector  (u64 words, LE)
    [ ((bitsize+511)/512+1)*8 ]     rank table    (u64 words, LE)
```

`size`/`rank_size` are recomputed by the decoder from `bitsize` and are **not**
stored. The number of keys `n` lives **outside** the blob (the caller/embedding
code carries it, e.g. `static const uint32_t mph_n;`). Typical size is ~0.5
bytes per key (the bitvectors dominate; ranks add ~12%).

### Multi-table / multi-language use

Because the decoder is generic over the blob, a program handling several key
sets (e.g. keyword tables for different languages) just keeps an array of
blobs and dispatches by id:

```c
static const uint8_t *const LANGS[] = { en_blob, fr_blob, de_blob };
uint64_t r = mph_lookup(LANGS[lang], sizeof(LANGS[lang]), key, keylen);
```

One `mph.h` decoder serves all of them.

---

## Legacy generators

### `boomphf.c` (numeric keys)

Reads `uint64` values (one per line, any base via `strtoul`) on stdin and emits
C source implementing `boomphf_query(uint64_t)`. The emitted code has a
`#ifdef TEST` `main()` that reads values from stdin and prints their rank.
This is the original, simplest generator and the seed of everything here.

### `soomph.c` (string keys, named tables)

Reads string keys (one per line), hashes each with a seeded FNV-1a + xorshift
mixer, and emits C source with per-level `static const uint64_t bv_i[]` /
`rank_i[]` tables and an unrolled `boomphf_query_string()`. Uses `tlist.h` to
keep the key set sorted during the collision-free build.

### `goomph.c` (string keys, blob form)

Same engine as `soomph.c` but emits the **single `uint8_t` blob** plus a
generic decoder instead of named tables. Predecessor to `mph.h`.

### `mph_builder.c` (string keys, via `mph.h`)

The `mph.h`-based generator: behaves like `soomph.c` but emits a
`static const uint8_t mph_blob[]` literal, `#include "mph.h"`, and a `-DTEST`
`main()` that exercises the real `mph_lookup()` consumer path. Prefer this over
`soomph.c`/`goomph.c` for new work.

---

## Testing

`test_soomph.sh` is the shared harness. It:

1. generates `N_INPUTS` distinct keys (`seq N_INPUTS`),
2. pipes them into a generator, capturing the emitted C,
3. compiles that C with `-DTEST`,
4. re-pipes the **same** keys into the test executable (it prints
   `<key>\t<rank>`),
5. asserts: exactly `N_INPUTS` unique input lines, output line count == `N_INPUTS`,
   every rank in `[1..N_INPUTS]` (none exceeds `N_INPUTS+1`), and all ranks
   distinct (a true permutation).

It accepts the generator executable as `argv[1]` (default `./soomph`), so
`make test` runs it against `boomphf`, `soomph`, `goomph`, and `mph_builder`.
Timing is printed per run (portable, no `time(1)` dependency).

---

## Implementation notes

- **Hash/mix**: `mph_hash_string` (seeded FNV-1a 64-bit) finalized with
  `mph_mix64` (the xorshift-multiply finalizer). The seed is baked into the
  blob and retried (`0,1,2,...`) until the build is collision-free.
- **Popcount**: `mph_popcountll` uses `__builtin_popcountll` when
  `__GNUC__ >= 3` (auto-`#define HAVE_POPCOUNTLL`), else a portable bit-twiddle
  fallback. Defined under `MPH_IMPL`.
- **Portability**: every multi-byte value is read/written byte-by-byte with
  shift-and-or (little-endian *in the blob*), so the produced tables work on
  any host endianness.
- **`tlist.h`** provides the sorted, binary-searchable key collection used by
  the string generators' build step (and by `mph.h`'s producer).

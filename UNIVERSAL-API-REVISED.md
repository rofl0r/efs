# UNIVERSAL-API (REVISED) — One MPH API Shape, Two Standalone Implementations

This supersedes `UNIVERSAL-API.md`. That revision proposed a runtime vtable
with per-algorithm adapters, a self-describing blob, and a new EFS v2
directory header. The project constraints have changed:

- **One algorithm will be chosen for the final EFS design.** The point of a
  unified API is only to let us *swap implementations to measure which is
  more space-efficient* (see `ALGO-COMPARISON.md`), not to support both at
  once.
- **No adapter layer.** Either one API is changed to match the other, or a
  common middle ground is found.
- **Each implementation stays usable on its own**, regardless of which one
  EFS adopts.
- **No backward-compatibility burden** — this is all new code.
- **No extra per-directory bytes.** No descriptor byte, no +12-byte header
  growth. The existing EFS v1 header is the budget.

The revised design follows directly: a **single API shape**, adopted by
**both** headers natively, selected at **compile time**. No vtable, no
descriptor, no format change.

---

## 1. Design principles

1. **Same signature, two headers.** `jmph.h` and `oomph/mph.h` each expose
   the *same* small set of functions with the *same* C signatures, under
   their existing `jmph_` / `mph_` prefixes. Callers written against the
   shape work with either.
2. **Compile-time selection.** The EFS builder/reader picks the algorithm by
   which header it includes and which prefix it uses — via one indirection
   header (§4). Nothing is decided at runtime, so there is **nothing to store
   in the image** to identify the algorithm. The image *is* a jmph image or
   an oomph image because you built it that way.
3. **Parameters the consumer needs travel in the directory header** — exactly
   as EFS v1 already does (`blen, salt, shift, w`). This is why jmph is the
   natural fit: its parameters *are* the v1 header fields. oomph is adapted
   to the same parameter model so it can fill the same header.
4. **0-based indexing everywhere.** EFS already works in 0-based indices; the
   unified shape returns 0-based. oomph's native 1-based rank is converted
   inside its own header (still "usable on its own", just with the unified
   contract).
5. **Explicit key lengths in, length-delimited lookups.** No `strlen` in hot
   paths; matches how EFS already stores names.

---

## 2. The unified API shape

Five functions + one input struct + one output struct. This is essentially
**jmph's existing API, promoted to the contract** (with one tweak: the output
also reports its byte length so oomph can participate without a `blen/w`
size helper).

```c
/* ---- input: n unique, length-delimited string keys ---- */
struct X_in {
    uint32_t          n;      /* number of keys                          */
    const char *const *keys;  /* key bytes (NUL-terminated for C use)    */
    const uint32_t    *kl;    /* key lengths; kl[i] = length of keys[i]  */
};

/* ---- output: a packed table + the params the consumer must store ---- */
struct X_out {
    uint8_t  *data;  /* packed table; caller frees with free()           */
    uint32_t  len;   /* byte length of data                              */
    uint32_t  blen;  /* algorithm parameter(s) the consumer needs...     */
    uint32_t  shift; /*      ...to decode the table. The exact meaning   */
    uint32_t  salt;  /*      of these four slots is per-algorithm, but   */
    uint32_t  w;     /*      the *set* is fixed so one header fits all.  */
};

/* Build a minimal perfect hash. Returns 1 on success (out->data malloc'd),
 * 0 on failure (duplicate keys / OOM; out zeroed). Keys must be unique. */
int      X_build(const struct X_in *in, struct X_out *out);

/* 0-based index of key[0..klen) in [0, n-1], reading the packed table.
 * `p` points at the table bytes; the four params are the ones X_build
 * emitted (the caller stored them alongside, e.g. in the efs_dir header).
 * Behaviour for a key not in the build set is undefined (EFS verifies the
 * matched name, so this is safe). */
uint32_t X_index(const uint8_t *p, uint32_t blen, uint32_t shift,
                 uint32_t salt, uint32_t w,
                 const char *key, uint32_t klen);

/* Exact serialized size of a table built with (blen, w). Lets the consumer
 * skip past the table to the name/entry arrays without a descriptor. */
uint32_t X_bytes(uint32_t blen, uint32_t w);
```

`X` is `jmph` in `jmph.h` and `mph` in `oomph/mph.h`. A caller that wants to
be algorithm-agnostic uses the indirection header in §4; a caller that knows
its algorithm calls the prefixed functions directly.

### Why this shape (and not a blob-with-descriptor)

- **Zero added bytes.** The four `uint32_t` params are precisely EFS v1's
  `blen/shift/salt/w` header fields (already 12 bytes of the existing 20-byte
  header; `shift`/`w` pack into the two bytes v1 already reserves). Nothing
  new is stored.
- **`X_index` is a pure function of `(params, table bytes)`**, which is all
  the reader has after `mmap`. No length-prefix to trust, no descriptor to
  version.
- **`X_bytes(blen, w)`** replaces the old "call `jmph_bytes`" coupling with a
  contract every algorithm provides, so the reader's layout math
  (`dir_name_off`) is algorithm-independent.

---

## 3. How each implementation maps onto the shape

### 3.1 `jmph.h` — already conforms (trivial changes only)

jmph is the reference. To fully match the shape it needs only:

- `jmph_out` gains a `len` field (`= jmph_bytes(blen, w)`), so callers don't
  need to know the formula. (Pure addition; existing fields keep their
  meaning.)
- `jmph_in.kl` widens `uint16_t` → `uint32_t` for a uniform signature. The
  builder keeps its internal 16-bit fast path if desired, but the public
  contract is 32-bit lengths.
- `jmph_index()` gets a sibling matching the unified signature. The cleanest
  is to make the canonical decode take the four params directly (this is also
  exactly what the EFS reader wants — it reads params from the `efs_dir`
  header, not from a `jmph_out`):

```c
uint32_t jmph_index_p(const uint8_t *tab, uint32_t blen, uint32_t shift,
                      uint32_t salt, uint32_t w,
                      const char *key, uint32_t klen);
```

  Today's `jmph_index(const jmph_out*, key, klen)` becomes a one-line wrapper
  over it. The body already exists — it is the decode currently inlined in
  `efs.h` (`mph_lookup3` + `tab_load`), which **moves into `jmph.h`** so it
  lives next to the builder that produces the table. `efs.h` then just calls
  it and stops carrying its own copy.

jmph's parameter semantics: `blen`=table buckets, `shift`=hash shift,
`salt`=hash seed, `w`=bytes per displacement. `jmph_bytes(blen,w)` =
`blen>=USE_SCRAMBLE ? blen + 256*w : blen*w`. Unchanged.

### 3.2 `oomph/mph.h` — adopt the shape natively (real changes)

oomph is re-expressed behind the identical shape, **in its own header**, so
it remains a standalone library. The BBHash engine is untouched; only the
public surface and serialization change.

- **Same structs/signature** with the `mph_` prefix:
  `mph_in{ n, keys, kl }`, `mph_out{ data, len, blen, shift, salt, w }`,
  `mph_build`, `mph_index_p`, `mph_bytes`.
- **Map BBHash onto the four params** so one `efs_dir` header serves both:
  - `salt`  → the collision-free hash seed (BBHash already finds one).
  - `blen`  → number of BBHash levels.
  - `shift` → gamma numerator/scale used (so the consumer sizes level 0), or
    0 if the decoder recomputes from `n`.
  - `w`     → unused (set 0) — BBHash has no displacement width.
  The consumer needs `n` too, which EFS already stores as `efs_dir.count`.
- **`mph_bytes(blen, w)`** returns the serialized size of a `blen`-level
  table. For BBHash the per-level `bitsize`s must be derivable without a
  descriptor, so fix the level sizing rule to a pure function of `(n, level)`
  — e.g. `bitsize_i = round_up_64(gamma * remaining_i)` with the standard
  geometric decay — letting the decoder walk the levels and compute the total
  size. (This is the one place oomph gives up its current self-describing
  blob in exchange for zero header bytes.)
- **0-based contract:** `mph_index_p` returns `rank - 1` (BBHash's internal
  1-based rank), reconciling the APIs without a wrapper layer.
- **Empty keys / empty set:** handled per the unified contract — `n == 0`
  builds a valid empty table; empty-string keys are supported (the current
  silent-skip behaviour in `mph_build_keys` is removed).

### 3.3 Pre-existing oomph bugs that must be fixed for this to work

While adapting oomph, three independent bugs in the checked-in `oomph/mph.h`
surface (all reproducible today; all algorithm-neutral, so they don't affect
the size numbers in `ALGO-COMPARISON.md`). EFS adoption of oomph is blocked
until these are fixed; they are called out here because the unified-API work
is the natural time to fix them:

1. **Double-free** in `mph_new_boomphf`/`mph_build` (the caller's key array
   is freed twice). Fix: don't free the caller's buffer on the final level
   (`if (to_free != keys) free(to_free);`).
2. **Endianness mismatch:** `mph_build`'s `mph_emit_u32/u64` write
   little-endian but `mph_rd_u32/u64`/`mph_rd_u64_at` read big-endian, so
   `mph_lookup` mis-decodes `bitsize` and never matches. Fix one side (make
   the emitters big-endian to match the readers, or vice versa) and correct
   the self-contradictory blob comment.
3. **Builder/consumer hash mismatch:** the builder indexes with
   `mix64(mix64(FNV(key,seed)))` (seed mixed, then a second `mix64` in
   `mph_new_boomphf`), while the consumer computes `FNV(key,seed)` only.
   They can never agree, so lookups return "not found" for keys that were
   just built. Fix: make both compute the same function (e.g.
   `mix64(FNV(key,seed))`).

These are exactly the kind of bugs the unified shape prevents in future:
with one `X_index` decode path shared by the builder's self-check and the
consumer, builder and consumer cannot drift apart.

---

## 4. The EFS integration (compile-time swap, no new bytes)

### 4.1 One indirection header: `efs_mph.h`

This is the only "glue", and it is a *header*, not an adapter TU — it just
maps generic names onto the chosen implementation at compile time. No code,
no runtime cost, no descriptor.

```c
/* efs_mph.h - select the MPH implementation EFS is built against.
 * Define EFS_MPH_OOMPH to use oomph/mph.h; default is jmph.h.            */
#if defined(EFS_MPH_OOMPH)
#  include "oomph/mph.h"
#  define efs_mph_in      mph_in
#  define efs_mph_out     mph_out
#  define efs_mph_build   mph_build
#  define efs_mph_index   mph_index_p
#  define efs_mph_bytes   mph_bytes
#else
#  include "jmph.h"
#  define efs_mph_in      jmph_in
#  define efs_mph_out     jmph_out
#  define efs_mph_build   jmph_build
#  define efs_mph_index   jmph_index_p
#  define efs_mph_bytes   jmph_bytes
#endif
```

`efs.h` includes `efs_mph.h` instead of `jmph.h`. Swapping algorithms for a
measurement is `cc -DEFS_MPH_OOMPH …` — no source edits.

### 4.2 `efs.h` — unchanged header, simplified reader

- `struct efs_dir` **stays exactly the v1 layout** (`count, blen, salt,
  names_len, shift, w, reserved`). No new fields, no descriptor, no size
  change. For oomph, `blen/shift/salt/w` carry the BBHash-mapped params of
  §3.2.
- The inline jmph decode (`mph_lookup3`, `tab_disp`, `mph_lookup`,
  `USE_SCRAMBLE`) is **deleted**. The reader's per-name index becomes:

```c
static uint32_t mph_lookup(const struct efs_dir *d, const char *key){
    return efs_mph_index(dir_hashtab(d), d->blen, d->shift, d->salt, d->w,
                         key, (uint32_t)strlen(key));
}
```

- `dir_name_off()` uses `efs_mph_bytes(d->blen, d->w)` instead of
  `jmph_bytes(...)` — identical for jmph, defined for oomph.
- The stale trailer comment promising `gen_mph()` is corrected to describe
  the unified build call.

### 4.3 `efsbuilder.c`

- Build with the generic names; the only algorithm-specific artefact is that
  the four `X_out` params are copied into the header (same for both):

```c
struct efs_mph_in  in = { n, keys, kl };     /* kl now uint32_t */
struct efs_mph_out mo;
if (!efs_mph_build(&in, &mo)) { fprintf(stderr, "mph fail in %s\n", path); exit(1); }

/* order entries by MPH index (insertion sort, unchanged) */
... efs_mph_index(mo.data, mo.blen, mo.shift, mo.salt, mo.w, name, len) ...

hdr.blen = mo.blen; hdr.shift = mo.shift; hdr.salt = mo.salt; hdr.w = mo.w;
fwrite(mo.data, 1, mo.len, out);            /* mo.len == efs_mph_bytes(blen,w) */
```

- `uint16_t kl[]` → `uint32_t kl[]` to match the unified input struct.

### 4.4 `efsreader.c`

`entry_len()` keeps its arithmetic but swaps `jmph_bytes` → `efs_mph_bytes`
(and can drop the hardcoded `20` by using `sizeof(struct efs_dir)`).

### 4.5 `Makefile`

```make
# default: jmph. Swap with: make CFLAGS='-O2 -DEFS_MPH_OOMPH'
efsbuilder.o: efsbuilder.c efs.h efs_mph.h jmph.h oomph/mph.h
```

No new objects; `efs_mph.h` is header-only and each MPH stays single-header.

---

## 5. Summary of changes vs. today

| File | Change |
|------|--------|
| `jmph.h` | add `len` to `jmph_out`; widen `jmph_in.kl` to `uint32_t`; add `jmph_index_p()` (the decode, moved in from `efs.h`); keep `jmph_index()` as a wrapper. No algorithm change. |
| `oomph/mph.h` | adopt the unified shape natively (`mph_in`/`mph_out`/`mph_build`/`mph_index_p`/`mph_bytes`); 0-based indices; derive level sizes so no descriptor is needed; **fix the three pre-existing bugs** (double-free, endianness, hash mismatch). Engine unchanged. |
| `efs_mph.h` | **new, header-only** compile-time selector (§4.1). |
| `efs.h` | include `efs_mph.h`; delete inline jmph decode; `mph_lookup` + layout helpers call the unified functions; `struct efs_dir` unchanged. |
| `efsbuilder.c` | use generic names; `uint32_t kl[]`; copy the four params to the header. |
| `efsreader.c` | `efs_mph_bytes`; use `sizeof(struct efs_dir)`. |
| `Makefile` | header dep on `efs_mph.h`; document the `-DEFS_MPH_OOMPH` swap. |

**What is deliberately gone from the previous revision:** the `umph_*`
runtime vtable, the two adapter `.c` files, the registry, the self-describing
blob, the `mph_id` descriptor byte, and the EFS v2 header. All of that was
machinery for *runtime* algorithm pluralism, which the project does not want.

**What is kept:** the core insight that EFS only needs *build*, *index
(build-time order)*, and *index (read-time lookup)* — now expressed as the
smallest possible shared signature, with jmph as the zero-cost default and
oomph a `-D` away.

**Selection guidance:** per `ALGO-COMPARISON.md`, **jmph is the recommended
default** — smaller for the small-to-medium directories that dominate real
filesystems (2–4× below ~300 entries), a simpler 32-bit decoder suited to
EFS's ROM targets, no header/format changes, and currently-correct code.
oomph remains available behind the same API should very large directories
become the priority (it wins above ~500 entries at ~0.5 B/key).

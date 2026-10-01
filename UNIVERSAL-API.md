# UNIVERSAL-API — A Swappable Minimal Perfect Hash API for EFS

This document proposes a **unified MPH (minimal perfect hash) API** so the
EFS image builder/reader can swap the underlying MPH algorithm without code
changes beyond a compile-time switch, and drafts the code changes required
to get there.

It compares the two MPH implementations in this repository, identifies every
point where the EFS code is coupled to one of them, defines the unified
interface, specifies the (small) on-disk format evolution it requires, and
lays out the concrete diffs/rewrites per file.

---

## 1. The two MPH implementations today

### 1.1 Main directory: `jmph.h` (Jenkins `perfect.c` port)

Used by `efsbuilder.c` / `efsreader.c` via `efs.h`.

| Aspect            | Today |
|-------------------|-------|
| Build input       | `struct jmph_in { uint32_t n; const char **keys; uint16_t *kl; }` (explicit 16-bit lengths) |
| Build output      | `struct jmph_out { uint32_t blen, shift, salt, w; uint8_t *data; }` — parameters travel **outside** the packed blob |
| Build function    | `int jmph_build(const struct jmph_in *in, struct jmph_out *out)` → 1/0, `out->data` is `malloc`'d |
| Blob size         | `uint32_t jmph_bytes(uint32_t blen, uint32_t w)` — needs the external parameters |
| Index (builder)   | `uint32_t jmph_index(const struct jmph_out *mo, const uint8_t *key, uint32_t klen)` → **0-based** index in `[0, n-1]`; undefined for foreign keys |
| Lookup (reader)   | **Inline in `efs.h`** (`mph_lookup()`/`tab_disp()`/`mph_lookup3()`), not in `jmph.h`; reads the blob with `(blen, shift, salt, w)` taken from the `efs_dir` header |
| Blob format       | `blen*w` bytes of big-endian `w`-byte displacements; or, when `blen >= USE_SCRAMBLE` (4096), `blen` 1-byte scramble indices followed by a 256-entry `w`-byte scramble table. Formula: `idx = (hash >> shift) ^ disp`, `hash = lookup2(key, salt·0x9e3779b9)` |
| Build gating      | `#if defined(MPH_IMPL) \|\| defined(EFS_BUILDER)` |
| Key limits        | unique, length < 4096 (fits `uint16_t`) |

### 1.2 `oomph/` subdirectory: `mph.h` (BBHash / BoomPHF, arXiv:1702.03154)

Not currently used by EFS; standalone library + generators.

| Aspect            | Today |
|-------------------|-------|
| Build input       | `const char * const *keys, size_t n_keys` (NUL-terminated, lengths re-derived) |
| Build output      | A single **self-describing** `uint8_t *blob` (+ `size_t out_len`); all parameters are *inside* the blob |
| Build function    | `uint8_t *mph_build(const char * const *keys, size_t n_keys, size_t *out_len)` → blob or `NULL` |
| Blob size         | known from `out_len`; no helper needed |
| Index (builder)   | none — the producer uses the consumer `mph_lookup()` for self-checks |
| Lookup (reader)   | `uint64_t mph_lookup(const uint8_t *table, size_t tablen, const char *key, size_t keylen)` → **1-based** rank, `0` = "not in set"; bounds-checked against `tablen` |
| Blob format       | `[u8 num_levels][u32 seed]` then per level `[u32 bitsize][bitvector][rank table]`, little-endian, endian-agnostic |
| Build gating      | `#ifdef MPH_IMPL` |
| Key limits        | unique, non-empty (empty lines are silently **skipped** by `mph_build_keys`) |

### 1.3 Semantic differences that any unified API must reconcile

| # | Difference | jmph | oomph/mph.h | Unified choice (§3) |
|---|------------|------|-------------|---------------------|
| 1 | Result base | 0-based | 1-based, 0 = not-found | 0-based; explicit `found` flag |
| 2 | Foreign-key behaviour | undefined (may return a valid-looking slot) | returns 0 ("not found") | undefined *by design*; EFS always verifies the name, so neither side needs guarantees |
| 3 | Parameters | outside blob (`blen/shift/salt/w`) | inside blob | inside blob (self-describing) |
| 4 | Lookup context | raw `efs_dir` header fields | `table` + `tablen` | opaque `struct umph` view `{data, len}` |
| 5 | Key lengths | caller-supplied `uint16_t` array | `strlen()` internally | caller-supplied `uint32_t` array (no re-scan, no 16-bit ceiling baked into the API) |
| 6 | Empty key set | builds a trivial 1-slot table | fails / skips | allowed; builds a valid empty table (`n == 0` → every lookup is "not found") |
| 7 | Duplicate keys | build fails (0) | build fails (NULL) | build fails (0) — unchanged contract |
| 8 | Empty-string key | supported | silently dropped | supported |
| 9 | Seed retry | implicit in builder | implicit in builder | stays an implementation detail |

---

## 2. Coupling inventory: where EFS depends on jmph specifics

Everything that must change is confined to three files; the reader binary
(`efsreader.c`) only touches the MPH indirectly.

| File | Coupling |
|------|----------|
| `efs.h` | `#include "jmph.h"`; hardcodes `USE_SCRAMBLE 4096`; `struct efs_dir` carries the jmph parameters `blen, salt, shift, w`; EFS_IMPL contains the jmph-specific decoder (`mph_lookup3`, `tab_disp`, `mph_lookup`); layout helper `dir_name_off()` calls `jmph_bytes(blen, w)` to skip the table; the comment block promises `EFS_BUILDER` exposes `gen_mph()` (stale — the actual API is `jmph_build()` in `jmph.h`). |
| `efsbuilder.c` | builds `struct jmph_in` (with `uint16_t kl[]`); calls `jmph_build()`; sorts entries with `jmph_index()`; copies jmph parameters into the header (`hdr.blen/shift/salt/w`); writes `jmph_bytes(mo.blen, mo.w)` bytes of table. |
| `efsreader.c` | `entry_len()` duplicates the layout math with a hardcoded header size (`20`) and `jmph_bytes(d->blen, d->w)`. |
| `EFS.md` | documents the header fields and the `mph_bytes()`-based layout. |

The good news: **EFS itself only needs three things from an MPH** —

1. **build** a table from the directory's names,
2. **query the index** of a known-set key at build time (to order
   `name_offset[]` / `entry_offset[]` and the names blob), and
3. **query the index** of an arbitrary string at read time (then verify the
   name — EFS already does an exact `strcmp`, so false positives from a
   foreign key are filtered out regardless of algorithm).

That maps onto a very small interface.

---

## 3. The unified MPH API (`umph`)

One header, `umph.h`, in the repository root. Each algorithm provides an
adapter translation unit. The names use the `umph_` prefix ("universal
MPH"); the per-algorithm adapters keep their native internals untouched.

### 3.1 Design principles

1. **Self-describing blobs.** The produced table carries every parameter it
   needs (seed, sizes, layout). The consumer never reconstructs state from
   side channels. This matches `oomph/mph.h` today and requires jmph to
   gain a tiny serialized header (§4, §6.2).
2. **Opaque view, not ownership, for lookups.** The reader operates on
   memory it does not own (an `mmap`'d image), so the lookup side is a
   plain `{pointer, length}` view. The builder side returns an owning
   object the caller frees.
3. **0-based indices with an explicit found flag.** No `+1`/`-1` rank
   games at call sites; foreign keys are "not found" or "undefined" per
   algorithm, and EFS's existing name verification is the safety net.
4. **Explicit key lengths.** Keys are length-delimited (`uint32_t`), not
   required to be NUL-terminated; the EFS builder already has the lengths
   at hand, avoiding O(total) re-scans.
5. **Function-pointer vtable for the algorithm.** Build-time selection
   (`--mph=jenkins|bbhash`, default jenkins). The algorithm id is stored
   in the image header so the **reader dispatches automatically** (§4).
6. **Same single-header ergonomics as today.** Algorithms keep their
   `*_IMPL` build gating; the adapters are the only place both worlds meet.

### 3.2 API surface

```c
#ifndef UMPH_H
#define UMPH_H

#include <stdint.h>
#include <stddef.h>

/* ---- algorithm registry ---- */

enum umph_algo {
    UMPH_JENKINS = 1,   /* jmph.h   — Jenkins perfect.c port (displacement) */
    UMPH_BBHASH  = 2    /* oomph/mph.h — BBHash/BoomPHF (level bitvectors)  */
    /* room for future algorithms; 0 is reserved/invalid */
};

/* ---- input ---- */

struct umph_in {
    uint32_t          n;      /* number of keys                          */
    const char *const *keys;  /* key bytes; need not be NUL-terminated   */
    const uint32_t    *kl;    /* key lengths, kl[i] = length of keys[i]  */
};
/* Contract: keys must be pairwise distinct (a perfect hash is only defined
 * for a set). Empty string keys are valid. n may be 0 (empty table). */

/* ---- build output (owning) ---- */

struct umph_out {
    uint8_t  *data;   /* self-describing packed table; free with free()  */
    uint32_t  len;    /* byte length of data                             */
    uint32_t  n;      /* number of keys the table was built for          */
    uint8_t   algo;   /* enum umph_algo that produced it                 */
};

/* ---- lookup view (non-owning) ---- */

struct umph {
    const uint8_t *data;  /* table bytes (e.g. into an mmap'd image)     */
    uint32_t       len;   /* byte length of the table                    */
    uint32_t       n;     /* number of keys (bounds the result)          */
    uint8_t        algo;  /* enum umph_algo; selects the decode path     */
};

/* ---- operations ---- */

/* Build a minimal perfect hash table for in->keys.
 * Returns 1 on success (*out filled, out->data malloc'd; free with
 * umph_free_out()), 0 on failure (duplicate keys or OOM; out zeroed). */
int  umph_build(uint8_t algo, const struct umph_in *in, struct umph_out *out);

void umph_free_out(struct umph_out *out);   /* frees out->data, zeroes *out */

/* Index of key[0..klen) in [0, m->n-1].
 * Returns 1 and sets *idx when found / assigned; returns 0 when the key is
 * definitely not in the set. For foreign keys some algorithms (Jenkins)
 * cannot detect the miss and may return 1 with an arbitrary in-range slot:
 * callers that admit foreign keys MUST verify the key themselves (EFS
 * always does an exact name comparison). */
int  umph_index(const struct umph *m, const char *key, uint32_t klen,
                uint32_t *idx);

/* Convenience: resolve the vtable for an algorithm id (NULL if unknown). */
const struct umph_vtable *umph_algo_vtable(uint8_t algo);

struct umph_vtable {
    uint8_t  id;
    const char *name;   /* "jenkins", "bbhash" — CLI/parser spelling       */
    int  (*build)(const struct umph_in *in, struct umph_out *out);
    int  (*index)(const struct umph *m, const char *key, uint32_t klen,
                  uint32_t *idx);
};

#endif /* UMPH_H */
```

Notes on the shape:

* `umph_index()` takes the view, not the owning object, so the **same
  function** serves the builder (ordering entries) and the reader
  (lookup in the image). This removes the current duplication where
  `efs.h` re-implements the jmph decode inline.
* The vtable is deliberately tiny. `umph_build`/`umph_index` are thin
  dispatchers; image embedding code may equally call
  `umph_algo_vtable(id)->index(...)` directly to skip one indirection.
* No `tablen`-style bounds parameter is needed on `umph` beyond `len`:
  the BBHash decoder already bounds-checks; the Jenkins decoder is
  branch-free and image offsets are trusted (as today).

### 3.3 Algorithm adapters

Both adapters are new, thin files. Neither algorithm's core file needs
changes to its *algorithm*; jmph gains a serialization wrapper, BBHash
gains a rank-adjust wrapper.

#### jenkins adapter (`umph_jenkins.c`)

* Build: translate `umph_in` → `jmph_in` (lengths are widened
  `uint32_t`→`uint16_t` with an overflow check; EFS name components are
  < 4096 so this never fires for EFS), call `jmph_build()`, then serialize
  `{blen, shift, salt, w} + data` into the self-describing v2 blob (§6.2).
* Index: parse the v2 blob header, run the exact decode currently inlined
  in `efs.h` (`mph_lookup3` + displacement read). The inline copy in
  `efs.h` is **deleted**; this becomes the single implementation.

#### bbhash adapter (`umph_bbhash.c`)

* Build: `mph_build()` already produces a self-describing blob. It is used
  as-is, with two pre-filters to match the unified contract: reject empty
  keys (`kl[i] == 0`) explicitly instead of letting them be skipped, and
  handle `n == 0` as a valid empty table (5-byte header with
  `num_levels = 0` decodes to "always not found" in today's decoder — no
  format change needed).
* Index: `r = mph_lookup(data, len, key, klen)`; `r == 0` → return 0,
  else `*idx = (uint32_t)r - 1; return (*idx < m->n)`.
* Caveat documented in the adapter: BBHash's bitvectors use
  `unsigned` bit positions, so per-directory entry counts above `UINT_MAX/2`
  are out of scope (irrelevant for EFS).

---

## 4. On-disk format evolution: EFS v1 → EFS v2

Swapping the algorithm changes the table bytes and kills the jmph-specific
header fields, so the **image format must carry the algorithm id**. This is
a deliberate, versioned change — the magic already contains a version byte
(`"EFS\x01"`), which is what it is for.

### 4.1 v2 directory header (16 bytes, was 20)

```c
struct efs_dir {              /* EFS v2 */
    uint32_t count;           /* number of entries                        */
    uint32_t names_len;       /* byte length of names blob (padded to 4)  */
    uint32_t hashtab_len;     /* byte length of the MPH table             */
    uint8_t  mph_id;          /* enum umph_algo: 1 = jenkins, 2 = bbhash  */
    uint8_t  flags;           /* reserved, must be 0                      */
    uint8_t  reserved1;
    uint8_t  reserved2;
};
```

* `blen`, `salt`, `shift`, `w` **leave the header** — they are jmph
  internals and move into the jenkins table blob (§6.2).
* `hashtab_len` replaces the `jmph_bytes(blen, w)` computation, so the
  layout helpers no longer know anything about any algorithm:

```c
dir_hashtab(d)  = (const uint8_t*)(d + 1)
dir_name_off(d) = (const uint32_t*)(dir_hashtab(d) + d->hashtab_len)
dir_entry_off(d)= dir_name_off(d) + d->count
dir_names(d)    = (const uint8_t*)(dir_entry_off(d) + d->count + 1)
```

* On-disk layout after the header is unchanged in structure:
  `[ hashtab (hashtab_len bytes) ][ name_offset[count] ][ entry_offset[count+1] ][ names ]`.

### 4.2 Magic / versioning and reader strategy

* Builder emits `"EFS\x02"`.
* The reader keys the header interpretation off the 4th magic byte:
  * `0x01` → legacy v1 header (20 bytes, jenkins-only) — optional
    compatibility shim (§8); a minimal reader may simply reject it.
  * `0x02` → v2 header, dispatch via `mph_id`.
* `mph_id` values are fixed for all time; new algorithms append new ids.
  A reader facing an unknown `mph_id` fails with "unsupported MPH
  algorithm" at image-open time rather than mis-decoding.

### 4.3 Size impact

Per directory, v2 costs `hashtab_len` (4 bytes) + `mph_id` (amortized in
the padding v1 already reserved) and the jenkins blob grows by its own
8-byte parameter header — net +8…12 bytes per directory versus v1, in
exchange for algorithm independence.

---

## 5. How each current consumer maps onto the new API

### 5.1 `efsbuilder.c` (per directory)

Before (jmph-specific):

```c
struct jmph_in mi = {n, (const char**)keys, kl16};
struct jmph_out mo;
if (!jmph_build(&mi, &mo)) { ... }
... jmph_index(&mo, name, len) ...          /* insertion sort */
hdr.blen = mo.blen; hdr.shift = mo.shift; hdr.salt = mo.salt; hdr.w = mo.w;
fwrite(mo.data, 1, jmph_bytes(mo.blen, mo.w), out);
free(mo.data);
```

After (algorithm-agnostic; `g_algo` from `--mph=`):

```c
struct umph_in  in  = { n, keys, kl };
struct umph_out mo;
if (!umph_build(g_algo, &in, &mo)) { fprintf(stderr, "mph fail in %s\n", path); exit(1); }

/* order entries by MPH index */
struct umph view = { mo.data, mo.len, mo.n, mo.algo };
... umph_index(&view, es[k].name, kl[k], &idx) ...   /* same insertion sort */

struct efs_dir hdr = {0};
hdr.count = n; hdr.names_len = names_len;
hdr.hashtab_len = mo.len; hdr.mph_id = mo.algo;
fwrite(&hdr, 1, sizeof hdr, out);
fwrite(mo.data, 1, mo.len, out);
...
umph_free_out(&mo);
```

The `uint16_t kl[]` array becomes `uint32_t kl[]` (the jenkins adapter
does the range check; BBHash stops re-`strlen`ing every key, a build-time
speedup).

### 5.2 `efs.h` reader path

Before: ~80 lines of jmph decode (`mph_lookup3`, `tab_disp`, `mph_lookup`,
`USE_SCRAMBLE`) compiled into every `EFS_IMPL` consumer, plus
`#include "jmph.h"` for `jmph_bytes()`.

After: the reader builds a view from the header and dispatches:

```c
static uint32_t mph_lookup(const struct efs_dir *d, const char *key){
    struct umph m = { dir_hashtab(d), d->hashtab_len, d->count, d->mph_id };
    uint32_t idx;
    if (!umph_index(&m, key, (uint32_t)strlen(key), &idx)) return d->count; /* miss */
    return idx;                             /* in [0, count-1]; caller verifies name */
}
```

`efs.h` shrinks to the layout helpers + path-walking logic. The decode
code lives exactly once per algorithm, in its adapter.

### 5.3 `efsreader.c`

`entry_len()` stops hardcoding `20` and `jmph_bytes()`:

```c
static uint32_t entry_len(const struct efs_dir *d, uint32_t idx){
    const uint32_t *eo = (const uint32_t*)
        ((const uint8_t*)(d + 1) + d->hashtab_len + 4*d->count);
    return eo[idx+1] - eo[idx];
}
```

plus the magic check accepts `\x02` (and `\x01` only if the compat shim is
kept).

---

## 6. Draft code changes, file by file

### 6.1 NEW `umph.h`

Exactly the interface from §3.2, plus a `UMPH_API` linkage macro following
the repo's `JMPH_API`/`MPH_API` convention. Pure interface — no `#ifdef
IMPL` section needed, since the adapters are separate TUs (they are tiny
and the repo already builds multiple objects into `efsbuilder`).

### 6.2 NEW `umph_jenkins.c` — jmph adapter (+ blob v2)

The jmph v2 blob is the v1 blob with a parameter header:

```
[ u32 blen ][ u8 shift ][ u8 w ][ u16 reserved ]   (8 bytes, LE)
[ jmph table bytes: jmph_bytes(blen, w) ]           (unchanged v1 layout)
```

```c
#define MPH_IMPL
#include "jmph.h"
#include "umph.h"
#include <stdlib.h>
#include <string.h>

static void put_u32(uint8_t **p, uint32_t v){
    for (int i = 0; i < 4; i++){ **p = (uint8_t)(v & 0xff); (*p)++; v >>= 8; }
}
static uint32_t get_u32(const uint8_t **p){
    uint32_t v = 0;
    for (int i = 0; i < 4; i++) v |= (uint32_t)*(*p)++ << (8*i);
    return v;
}

static int jenkins_build(const struct umph_in *in, struct umph_out *out){
    /* jmph key lengths are uint16_t; enforce with a hard failure */
    uint16_t *kl = malloc((in->n ? in->n : 1) * sizeof *kl);
    if (!kl) return 0;
    for (uint32_t i = 0; i < in->n; i++){
        if (in->kl[i] > 0xffff){ free(kl); return 0; }
        kl[i] = (uint16_t)in->kl[i];
    }
    struct jmph_in  ji = { in->n, in->keys, kl };
    struct jmph_out jo;
    int ok = jmph_build(&ji, &jo);
    free(kl);
    if (!ok) return 0;

    uint32_t tab = jmph_bytes(jo.blen, jo.w);
    out->data = malloc(8 + tab);
    if (!out->data){ free(jo.data); return 0; }
    uint8_t *p = out->data;
    put_u32(&p, jo.blen);
    *p++ = (uint8_t)jo.shift;
    *p++ = (uint8_t)jo.w;
    *p++ = 0; *p++ = 0;                 /* reserved */
    memcpy(p, jo.data, tab);
    free(jo.data);
    out->len = 8 + tab;
    out->n   = in->n;
    out->algo = UMPH_JENKINS;
    return 1;
}
```

The index path is the code currently inlined in `efs.h`
(`mph_lookup3` hash + `tab_load` displacement read), moved verbatim and
operating on the parsed header:

```c
static int jenkins_index(const struct umph *m, const char *key, uint32_t klen,
                         uint32_t *idx){
    if (m->len < 8) return 0;
    const uint8_t *p = m->data;
    uint32_t blen  = get_u32(&p);
    uint32_t shift = p[0], w = p[1];
    const uint8_t *tab = m->data + 8;
    if (m->len < 8 + jmph_bytes(blen, w)) return 0;   /* truncated table */

    uint32_t v    = jmph_lookup_k((const uint8_t*)key, klen,
                                  get_u32_salt(m) * 0x9e3779b9); /* see below */
    ...
}
```

> **Design note — salt placement:** jmph's salt is a `uint32_t`. The v2
> jenkins header above shows `blen/shift/w`; `salt` needs 4 more bytes,
> making the parameter header 12 bytes:
> `[ u32 blen ][ u32 salt ][ u8 shift ][ u8 w ][ u16 reserved ]`.
> (Kept byte-leveled little-endian; `jmph_lookup_k` and the displacement
> table body remain big-endian per-entry exactly as v1 — only the new
> header is specified LE, matching oomph's blob style. Alternatively the
> whole header can be big-endian; pick one and document it in EFS.md.)

So the final jenkins v2 blob:

```
[ u32 blen ][ u32 salt ][ u8 shift ][ u8 w ][ u16 reserved ]  (12 bytes)
[ jmph table bytes: jmph_bytes(blen, w) ]                      (v1 body, unchanged)
```

`jenkins_index()` then reproduces today's `efs.h` formula:
`a = v >> shift; b = v & (blen-1); disp = tab[b]-decode; idx = a ^ disp`,
returning `(*idx < m->n)` as the found flag (Jenkins cannot detect foreign
keys — documented, and harmless for EFS).

> **Naming clean-up folded into the move:** the hash helper `jmph_lookup_k`
> and `jmph_tab_load` are currently `JMPH_INTERNAL` (builder-only). The
> adapter needs them on the decode path, so they move to a small
> always-visible section of `jmph.h` (like `jmph_bytes` already is), or are
> duplicated as `static` in the adapter. Moving them is preferred — one
> hash implementation, no drift.

### 6.3 NEW `umph_bbhash.c` — oomph adapter

```c
#define MPH_IMPL
#include "oomph/mph.h"          /* relative to repo root */
#include "umph.h"
#include <stdlib.h>
#include <string.h>

static int bbhash_build(const struct umph_in *in, struct umph_out *out){
    /* mph.h silently skips empty keys and has no 0-key build; enforce the
     * unified contract at the adapter boundary. */
    for (uint32_t i = 0; i < in->n; i++)
        if (in->kl[i] == 0) return 0;                 /* empty key unsupported */
    if (in->n == 0){                                  /* valid empty table */
        out->data = malloc(5);
        if (!out->data) return 0;
        out->data[0] = 0;                             /* num_levels = 0 */
        memset(out->data + 1, 0, 4);                  /* seed */
        out->len = 5; out->n = 0; out->algo = UMPH_BBHASH;
        return 1;
    }
    size_t len = 0;
    uint8_t *blob = mph_build(in->keys, in->n, &len); /* strlen's the keys */
    if (!blob) return 0;
    if (len > UINT32_MAX){ free(blob); return 0; }
    out->data = blob; out->len = (uint32_t)len;
    out->n = in->n; out->algo = UMPH_BBHASH;
    return 1;
}

static int bbhash_index(const struct umph *m, const char *key, uint32_t klen,
                        uint32_t *idx){
    if (klen == 0) return 0;
    uint64_t r = mph_lookup(m->data, m->len, key, klen);
    if (r == 0 || r > m->n) return 0;
    *idx = (uint32_t)r - 1;                           /* 1-based -> 0-based */
    return 1;
}
```

> **Optional optimization (not required):** add `mph_build_keys_len()`
> to `oomph/mph.h` taking explicit lengths so the adapter can skip the
> internal `strlen` pass. The unified API passes lengths regardless, so
> this is a contained, backward-compatible addition to mph.h.

### 6.4 NEW `umph.c` — registry/dispatch

```c
#include "umph.h"
#include <string.h>

/* adapters register their vtables here */
extern const struct umph_vtable umph_jenkins_vt;   /* umph_jenkins.c */
extern const struct umph_vtable umph_bbhash_vt;    /* umph_bbhash.c  */

static const struct umph_vtable *const vts[] = {
    &umph_jenkins_vt, &umph_bbhash_vt,
};

const struct umph_vtable *umph_algo_vtable(uint8_t algo){
    for (size_t i = 0; i < sizeof vts / sizeof *vts; i++)
        if (vts[i]->id == algo) return vts[i];
    return 0;
}

const struct umph_vtable *umph_algo_by_name(const char *name){
    for (size_t i = 0; i < sizeof vts / sizeof *vts; i++)
        if (!strcmp(vts[i]->name, name)) return vts[i];
    return 0;
}

int umph_build(uint8_t algo, const struct umph_in *in, struct umph_out *out){
    const struct umph_vtable *vt = umph_algo_vtable(algo);
    *out = (struct umph_out){0};
    return vt ? vt->build(in, out) : 0;
}

int umph_index(const struct umph *m, const char *key, uint32_t klen,
               uint32_t *idx){
    const struct umph_vtable *vt = umph_algo_vtable(m->algo);
    return vt ? vt->index(m, key, klen, idx) : 0;
}

void umph_free_out(struct umph_out *out){
    free(out->data);            /* <stdlib.h> */
    *out = (struct umph_out){0};
}
```

(For a ROM-constrained reader that only needs one algorithm, each adapter
can instead be compiled with `-DUMPH_SINGLE=UMPH_JENKINS` to make
`umph_index` a direct call — optional, documented in the header.)

### 6.5 `efs.h` — v2 header + slim reader

1. Replace `struct efs_dir` with the v2 layout from §4.1 and change
   `EFS_MAGIC` to `"EFS\x02"`.
2. Delete: `#include "jmph.h"`, the `USE_SCRAMBLE` import, `tab_load`,
   `struct mph_data`, `tab_disp`, `mph_lookup3`, and the jmph-coupled
   `mph_lookup`. Replace with `#include "umph.h"` and the 6-line
   dispatching `mph_lookup` from §5.2 (kept `static` under `EFS_IMPL`).
3. Update `dir_name_off()` to use `d->hashtab_len` (removes the
   `jmph_bytes()` call).
4. Update the big comment block: the builder section now points at
   `umph.h` instead of promising `gen_mph()`.
5. Update the `EFS.md` header/layout documentation to v2 (fields,
   `hashtab_len`-based helpers, algorithm byte, the `"EFS\x01"` vs
   `"EFS\x02"` story).

`efs_lookup`'s path walking, the two-pass `name`/`/name` directory logic,
and `efs_readdir` are **unchanged** — they only needed an index.

### 6.6 `efsbuilder.c`

1. `#include "umph.h"` (already via `efs.h`), add a tiny CLI:

```c
/* usage: efsbuilder [--mph=jenkins|bbhash] out.efs dir */
static uint8_t parse_algo(const char *s){
    const struct umph_vtable *vt = umph_algo_by_name(s);
    if (!vt){ fprintf(stderr, "unknown MPH algorithm: %s\n", s); exit(1); }
    return vt->id;
}
```

   Default stays `UMPH_JENKINS`, so existing invocations keep working and
   produce v2-jenkins images.
2. `build_dir()` changes per §5.1: `uint32_t kl[]`, `umph_build`,
   `umph_index` in the insertion sort (the sort itself is untouched),
   v2 header fill, `fwrite(mo.data, 1, mo.len, out)`,
   `umph_free_out(&mo)`.
3. `main()` emits `EFS_MAGIC` (now `EFS\x02`).

### 6.7 `efsreader.c`

1. `entry_len()` per §5.3 (no hardcoded `20`, no `jmph_bytes`).
2. The magic check stays `memcmp(base, EFS_MAGIC, 4)` — it now means v2.
3. Link against `umph.o` + both adapter objects (or the single-algorithm
   variant) so `umph_index` resolves.

### 6.8 `Makefile`

```make
all: efsbuilder efsreader

UMPH_OBJS = umph.o umph_jenkins.o umph_bbhash.o

efsbuilder.o: efsbuilder.c efs.h umph.h jmph.h
efsreader.o: efsreader.c efs.h umph.h
umph_jenkins.o: umph_jenkins.c umph.h jmph.h
umph_bbhash.o: umph_bbhash.c umph.h oomph/mph.h oomph/tlist.h
umph.o: umph.c umph.h

umph_bbhash.o: CFLAGS += -Ioomph       /* mph.h includes "tlist.h" */

efsbuilder: efsbuilder.o $(UMPH_OBJS)
	$(CC) $(CFLAGS) $(LDFLAGS) $^ -o $@

efsreader: efsreader.o $(UMPH_OBJS)
	$(CC) $(CFLAGS) $(LDFLAGS) $^ -o $@
```

`jmph_test` and `make test` are untouched (they test `jmph.h` directly).

### 6.9 Tests

1. `test-dir.sh` works unchanged for the default algorithm; extend it to
   loop over both:

```sh
for algo in jenkins bbhash; do
    ./efsbuilder --mph=$algo "$EFS" "$DIR" || { echo "build failed ($algo)"; exit 1; }
    ... same per-file verify loop ...
done
```

2. New `test_umph.sh` (mirrors `test_jmph.sh`): pipe `seq $N_INPUTS` into
   a small `umph_test` driver that builds via `umph_build(algo, ...)` and
   prints `idx+1` per key; assert the permutation property **for each
   algorithm**, giving the same harness coverage oomph and jmph have
   individually today.
3. Cross-check test (cheap, high value): build the same tree with both
   algorithms and diff the *logical* listing (paths + contents via
   `efsreader`), proving image equivalence independent of table bytes.

---

## 7. Worked example: adding a third algorithm later

To show the API is actually universal, adding e.g. a CHD variant is:

1. Write `umph_chd.c` implementing `build` (produce a self-describing
   blob) and `index` (decode + return 0-based slot).
2. Assign `UMPH_CHD = 3`, add `&umph_chd_vt` to the registry, add one
   `--mph=chd` spelling.
3. Nothing in `efs.h`, `efsbuilder.c`, `efsreader.c`, or the on-disk
   layout changes; v2 images with `mph_id = 3` are readable by any reader
   linked with the new adapter, and cleanly rejected by old ones.

---

## 8. Migration & compatibility

| Concern | Decision |
|---------|----------|
| v1 images | The v1 reader code (jmph decode in `efs.h`) is deleted. If reading old images matters, keep a `efs_v1_lookup()` shim behind `#ifdef EFS_V1_COMPAT` that contains exactly today's code path; otherwise v1 images are rejected at the magic check with a clear message. Rebuilding an image from its source tree with the new builder is the migration path. |
| Default algorithm | jenkins (current behaviour, smallest tables at EFS's typical directory sizes; no scramble threshold surprises). `--mph=bbhash` opts into ~0.5–1.5 bytes/key bitvectors and 64-bit hashing. |
| ABI/API break | `struct efs_dir` changes size; it was never a public ABI (image-internal), so this is safe. The public functions `efs_lookup` / `efs_readdir` keep their signatures. |
| oomph standalone tools | Untouched. `oomph/mph.h` remains a standalone library; the adapter includes it. The `mph_lookup` 1-based contract is preserved inside oomph and translated at the adapter boundary. |
| jmph standalone | `jmph.h` keeps `jmph_build`/`jmph_index`/`jmph_bytes` exactly as they are; `jmph_test`/`test_jmph.sh` keep passing. Only the two decode helpers move visibility (§6.2). |

## 9. Summary of the change set

| File | Action |
|------|--------|
| `umph.h` | **new** — unified API (§3.2) |
| `umph.c` | **new** — vtable registry + dispatch (§6.4) |
| `umph_jenkins.c` | **new** — jmph adapter + v2 blob codec (§6.2) |
| `umph_bbhash.c` | **new** — oomph adapter, 1-based→0-based translation (§6.3) |
| `efs.h` | v2 header struct + magic; delete inline jmph decode; dispatch via `umph_index`; layout via `hashtab_len` (§6.5) |
| `efsbuilder.c` | `--mph=` option; `umph_build`/`umph_index`; v2 header fill (§6.6) |
| `efsreader.c` | `hashtab_len`-based `entry_len`; link adapters (§6.7) |
| `jmph.h` | expose `jmph_lookup_k`/`jmph_tab_load` outside the IMPL guard; no algorithm changes (§6.2) |
| `oomph/mph.h` | optional: length-taking build entry point; no required changes (§6.3) |
| `Makefile` | build/link the three new objects; `-Ioomph` for the bbhash adapter (§6.8) |
| `test-dir.sh`, new `test_umph.sh` | test both algorithms; cross-implementation equivalence check (§6.9) |
| `EFS.md` | document v2 header, `mph_id`, and the jenkins v2 blob (§6.5) |

Total: 4 new small files, 3 files meaningfully edited, 2 files with
one-line/visibility tweaks, tests extended — and afterwards the MPH
algorithm is a build-time and run-time (per-image) choice, not a property
of the filesystem code.

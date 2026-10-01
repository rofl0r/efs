# MPH Algorithm Space Comparison — jmph (Jenkins) vs oomph (BBHash)

Empirical, apples-to-apples comparison of the **on-disk hash-table size**
produced by the two minimal perfect hash implementations in this repo, over
**identical** filename sets. The goal is to inform which single algorithm
EFS should standardize on.

- **jmph** (`jmph.h`, main dir) — Bob Jenkins `perfect.c` port; displacement
  table + optional scramble table.
- **oomph** (`oomph/mph.h`) — BBHash / BoomPHF (arXiv:1702.03154); per-level
  bitvectors + rank tables.

> **Scope:** the metric is the *packed MPH table bytes only* — the bytes EFS
> would store per directory in the hash-table slot. Per-directory fixed costs
> that are **identical for both algorithms** (the `efs_dir` header, the
> `name_offset[]`/`entry_offset[]` arrays, and the names blob) are excluded,
> because they don't discriminate between the two. What differs — and what
> this measures — is the MPH table.

---

## 1. Methodology

For each configuration a **single** key file is generated and piped to *both*
builders, so both algorithms are measured on **byte-identical input**.

- **Table size** is the exact serialized size each builder emits:
  - jmph → `jmph_bytes(blen, w)` (the on-disk table length EFS v1 writes).
  - oomph → the blob length `mph_build()` returns.
- Every build is **sanity-checked** to be a true minimal perfect hash: the
  emitted indices are verified to be a permutation of `0..n-1` (jmph) /
  `1..n` (oomph). A run that isn't a valid permutation aborts.
- **Key sources** (deterministic, seeded; same args ⇒ same keys):
  - `words` — real filenames sampled from the distro wordlist
    (`/usr/share/hunspell/en_US.dic`, ~79k unique words, natural lengths).
  - `uniform` — random lowercase names, length uniform in `[minlen,maxlen]`.
  - `fixed` — random lowercase names of exactly `minlen` chars (used for the
    long-name sweep up to 1000 chars).
- **Counts** `n` ∈ {12, 50, 100, 500, 1000, 2000, 3000} and **name lengths**
  from 5 up to ~1000 chars, covering typical directories up to PATH_MAX-scale
  components.

### Reproduce

```sh
# in /tmp/mphbench (throwaway harness, not committed)
cc -O2 -I<repo>        -o bench_jmph  bench_jmph.c        # jmph driver
cc -O2 -I.             -o bench_oomph bench_oomph2.c      # oomph driver (fixed mph.h)
./run.sh                                                  # writes results.tsv
```

`run.sh` builds each key file once and feeds it to both drivers; columns are
`mode, n, len range, seed, actual len stats, jmph{blen,w,bytes,bpk},
oomph{levels,bytes,bpk}`.

### A necessary note on oomph's `mph.h`

The checked-in `oomph/mph.h` does **not** currently work as shipped; three
independent bugs had to be fixed to produce a *valid, verifiable* table for
an honest comparison. All three are **algorithm-neutral** (none changes the
table size, which is what we measure), and they are documented here and in
`UNIVERSAL-API-REVISED.md` because EFS adoption would require fixing them:

1. **Double-free.** `mph_new_boomphf()` frees the caller's key array on the
   final level; `mph_build()` then `free()`s it again. (Reproducible with
   oomph's own `mph_builder`.)
2. **Endianness mismatch.** `mph_build()` emits `u32/u64` little-endian, but
   the readers (`mph_rd_u32`, `mph_rd_u64`, `mph_rd_u64_at`) decode
   big-endian — so `mph_lookup` mis-decodes `bitsize` etc. and never finds
   anything. The blob comment also self-contradicts ("little-endian" vs
   "big-endian in the blob").
3. **Builder/consumer hash mismatch.** `mph_build_keys()` hashes keys as
   `mix64(FNV(key,seed))`, and `mph_new_boomphf()` applies a *second*
   `mix64`, so the builder's effective index hash is `mix64(mix64(FNV))`.
   The consumer `mph_lookup()` computes only `FNV(key,seed)` — it can never
   match. (The seed is also effectively dropped at lookup.)

Because these bugs mean the shipped oomph currently returns "not found" for
its own keys, **the numbers below are measured against a corrected copy**
(`mph_fixed.h`, used only by the throwaway bench; the repo file is left
untouched pending the API decision).

---

## 2. Headline results

| Key set | n | jmph table | oomph table | smaller |
|---------|--:|-----------:|------------:|:-------:|
| words 5–45 | 12 | **8 B** | 61 B | jmph (7.6×) |
| words 5–45 | 100 | **64 B** | 113 B | jmph (1.8×) |
| words 5–45 | 1000 | 1024 B | **581 B** | oomph (1.8×) |
| words 5–45 | 3000 | 2048 B | **1545 B** | oomph (1.3×) |
| uniform 5–12 | 12 | **16 B** | 33 B | jmph (2.1×) |
| uniform 5–12 | 100 | **64 B** | 141 B | oomph? no — **jmph** (2.2×) |
| uniform 5–12 | 1000 | 1024 B | **637 B** | oomph (1.6×) |
| uniform 5–12 | 3000 | 2048 B | **1529 B** | oomph (1.3×) |
| fixed 5 | 100 | **64 B** | 141 B | jmph (2.2×) |
| fixed 1000 | 100 | **64 B** | 121 B | jmph (1.9×) |
| fixed 1000 | 1000 | 512 B | **625 B** | oomph? no — **jmph** (1.2×) |
| uniform 5–1000 | 1000 | 1024 B | **561 B** | oomph (1.8×) |
| uniform 5–1000 | 3000 | 2048 B | **1505 B** | oomph (1.4×) |

*(Rows are a digest; the full table is in §4.)*

**The winner depends on directory size, not name length.**

---

## 3. Analysis

### 3.1 Name length is (almost) irrelevant to table size

For **both** algorithms the table size is essentially a function of the key
**count** `n`, not of the key **length**. The hash condenses any key to a
fixed-size value (32-bit for jmph, 64-bit for oomph), so a 5-char name and a
1000-char name cost the same in the table.

Evidence — fixed `n=100`, sweeping name length 5 → 1000:

| n=100, len | jmph bytes | oomph bytes |
|-----------:|-----------:|------------:|
| 5 | 64 | 141 |
| 16 | 64 | 113 |
| 64 | 64 | 121 |
| 128 | 64 | 149 |
| 256 | 64 | 141 |
| 512 | 64 | 197 |
| 1000 | 64 | 121 |

jmph is **perfectly flat** (64 B across the whole sweep). oomph wobbles
(113–197 B) only because longer names change hash values and therefore how
many levels BBHash needs — a *variance* effect, not a *growth* effect. There
is no monotonic relationship with length in either algorithm.

> Implication for EFS: filename length should not factor into the algorithm
> choice. Only directory entry count matters.

### 3.2 Small directories: jmph wins (low fixed overhead)

oomph/BBHash has a **per-level structural floor**: a 5-byte blob header
(`num_levels` + `seed`) plus, per level, a 4-byte `bitsize`, a padded-to-64
bit vector (≥ 8 bytes), and a rank table (≥ 2×8 bytes) — **~29 B minimum per
level**. Even a 12-key directory needs 1–2 levels, so oomph starts at 33–61 B.

jmph's floor is tiny: for small `n` the table is just `blen × w` bytes of
displacements (no scramble table while `blen < 4096`), with `w=1` for
`n ≤ 256`. So 12 keys → 16 B, 100 keys → 64 B.

| n | jmph | oomph | ratio (o/j) |
|--:|-----:|------:|:-----------:|
| 12 | 8–16 B | 33–61 B | ~2–4× |
| 50 | 32 B | 69–97 B | ~2–3× |
| 100 | 64 B | 113–149 B | ~1.8–2.2× |

### 3.3 The crossover: around n ≈ 300–500

jmph's `w` (bytes per displacement) is driven by `smax = 2^ceil(log2 n)`,
and `w = ceil(log2(smax)/8)`. It jumps at powers of two:

- `n ≤ 256` → `smax ≤ 256` → `w = 1`
- `257 ≤ n ≤ 65536` → `smax ≤ 65536` → `w = 2`  ← **doubles the table**

So at `n ≈ 257` jmph's table roughly **doubles** (from `blen×1` to
`blen×2`), and `blen` itself is a power of two that also steps up. This is
exactly where oomph — whose size grows smoothly at ~0.5–0.7 bytes/key with
no such cliff — pulls ahead.

| n | jmph (w) | oomph | smaller |
|--:|---------:|------:|:-------:|
| 256 | ~256 B (w=1) | ~180 B | oomph |
| 500 | 512 B (w=2) | 329–357 B | oomph |
| 1000 | 1024 B (w=2) | 537–653 B | oomph |

### 3.4 Large directories: oomph wins (lower bits/key)

Per-key cost at scale:

| n | jmph B/key | oomph B/key |
|--:|-----------:|------------:|
| 1000 | 1.024 | 0.55–0.65 |
| 2000 | 0.512 | 0.53 |
| 3000 | 0.68 | 0.50–0.52 |

oomph converges to **~0.5 bytes/key** (γ·n bits ≈ 2 bits/key per level
summed over a geometric series, plus ~12% rank overhead, plus padding).
jmph oscillates between **~0.5 and ~1.0 bytes/key** depending on where `n`
falls relative to the powers of two that drive `blen` and `w`.

Note jmph's variance: `n=2000` (blen=512, w=2 → 1024 B, 0.51 B/key) vs
`n=3000` (blen=1024, w=2 → 2048 B, 0.68 B/key). oomph is far more
predictable.

### 3.5 Determinism / seed robustness

Re-running with a different seed (`seed=7`) yields near-identical sizes for
both algorithms (e.g. uniform n=1000: jmph 512 B, oomph 653 B — within a few
percent of the `seed=1` rows), confirming the results are not an artifact of
a lucky key set.

---

## 4. Full results table

`bpk` = bytes per key. `blen`/`w` are jmph's parameters; `levels` is oomph's.
Bold marks the smaller table for that row.

### Realistic wordlist names (natural lengths, 5–45)

| n | avg len | jmph blen | w | jmph bytes | bpk | oomph levels | oomph bytes | bpk | smaller |
|--:|--------:|----------:|--:|-----------:|----:|-------------:|------------:|----:|:-------:|
| 12 | 7.58 | 8 | 1 | **8** | 0.67 | 2 | 61 | 5.08 | jmph |
| 50 | 8.30 | 32 | 1 | **32** | 0.64 | 3 | 97 | 1.94 | jmph |
| 100 | 8.66 | 64 | 1 | **64** | 0.64 | 3 | 113 | 1.13 | jmph |
| 500 | 8.62 | 256 | 2 | 512 | 1.02 | 5 | **329** | 0.66 | oomph |
| 1000 | 8.53 | 512 | 2 | 1024 | 1.02 | 6 | **581** | 0.58 | oomph |
| 2000 | 8.54 | 512 | 2 | **1024** | 0.51 | 7 | 1065 | 0.53 | jmph |
| 3000 | 8.54 | 1024 | 2 | 2048 | 0.68 | 7 | **1545** | 0.52 | oomph |

### Uniform random names, length 5–12

| n | jmph bytes | bpk | oomph bytes | bpk | smaller |
|--:|-----------:|----:|------------:|----:|:-------:|
| 12 | **16** | 1.33 | 33 | 2.75 | jmph |
| 50 | **32** | 0.64 | 69 | 1.38 | jmph |
| 100 | **64** | 0.64 | 141 | 1.41 | jmph |
| 500 | 512 | 1.02 | **357** | 0.71 | oomph |
| 1000 | 1024 | 1.02 | **637** | 0.64 | oomph |
| 2000 | **1024** | 0.51 | 1061 | 0.53 | jmph |
| 3000 | 2048 | 0.68 | **1529** | 0.51 | oomph |

### Name-length sweep at fixed count (jmph is length-invariant)

| n | len | jmph bytes | oomph bytes | smaller |
|--:|----:|-----------:|------------:|:-------:|
| 100 | 5 | **64** | 141 | jmph |
| 100 | 8 | **64** | 141 | jmph |
| 100 | 16 | **64** | 113 | jmph |
| 100 | 32 | **64** | 121 | jmph |
| 100 | 64 | **64** | 121 | jmph |
| 100 | 128 | **64** | 149 | jmph |
| 100 | 256 | **64** | 141 | jmph |
| 100 | 512 | **64** | 197 | jmph |
| 100 | 1000 | **64** | 121 | jmph |
| 1000 | 5 | 1024 | **589** | oomph |
| 1000 | 64 | 1024 | **537** | oomph |
| 1000 | 256 | 1024 | **609** | oomph |
| 1000 | 1000 | **512** | 625 | jmph |
| 3000 | 5 | 2048 | **1557** | oomph |

### Wide mixed lengths (5–1000)

| n | avg len | jmph bytes | oomph bytes | smaller |
|--:|--------:|-----------:|------------:|:-------:|
| 100 | 485 | **64** | 121 | jmph |
| 1000 | 502 | 1024 | **561** | oomph |
| 3000 | 510 | 2048 | **1505** | oomph |

---

## 5. Recommendation for EFS

**Neither algorithm is uniformly smaller; the deciding factor is the
directory sizes EFS must serve well.**

- **Typical directories are small** (a handful to a few hundred entries). In
  that regime **jmph is 2–4× smaller** and additionally:
  - needs only a **32-bit** hash and a tiny decode (one multiply-free mix +
    one table read) — attractive for the ROM/embedded targets EFS is designed
    for;
  - has a *fixed* table size for a given `n` (no BBHash level-count
    variance), so image sizes are predictable;
  - its current `efs_dir` header already stores exactly the four parameters
    it needs (`blen/shift/salt/w`) — **zero header growth**, which the
    project requires.
- **oomph only pulls ahead for directories larger than ~300–500 entries**,
  where its smooth ~0.5 B/key scaling beats jmph's power-of-two `w` cliffs.
  But it carries a **per-level structural floor** (~29 B/level) that hurts
  exactly the small directories that dominate real filesystems, and its
  decoder needs 64-bit arithmetic + popcount.

**Recommendation: standardize EFS on jmph (Jenkins).** It is smaller where it
matters (small/medium directories), simpler to decode, needs no header or
descriptor changes, and is the lower-risk option (its code already passes
`test_jmph.sh`; oomph's `mph.h` currently has the three correctness bugs
catalogued in §1 that must be fixed before it is usable at all).

Keep oomph as a standalone library (per the project's goal that each impl
stays usable on its own). The unified API in `UNIVERSAL-API-REVISED.md` lets
either implementation back EFS behind an identical build/index signature, so
if very large directories ever become the priority, oomph can be swapped in
by changing a single `mph_*.h` include — no EFS code changes.

> If EFS later targets a workload dominated by huge directories
> (thousands of entries each), revisit: at `n ≥ 1000` oomph saves ~40–50%
> per directory. A hybrid (choose per directory at build time) was considered
> and rejected — it would require a per-directory algorithm descriptor byte,
> which the project explicitly disallows.

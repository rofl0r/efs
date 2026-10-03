# BBHash Table Sizing in EFS — History, Background, and Why It Changed Back

This document explains, in plain terms, how the BBHash minimal-perfect-hash
builder (`bbhash.h`, formerly `oomph/mph.h`) decides **how big each level of
the hash table should be**, why a *deterministic* sizing scheme was tried for
a while, and why the tree has now returned to the original
*collision-dependent* sizing. You don't need to know the algorithms to follow
this — everything is explained from scratch.

---

## 1. Background: the problem a "minimal perfect hash" solves

Imagine you have a directory with, say, 1000 file names, and you want to
answer the question *"which slot does the name `report.txt` live in?"* as
fast as possible, with no searching.

A **perfect hash function** is like a magic numbering scheme: you feed it a
name and it gives you back a number, with the guarantee that **no two names
in your set ever get the same number** (no collisions). It's **minimal** if
the numbers are exactly `0, 1, 2, …, n-1` — one per name, no gaps.

EFS stores these numbers in the filesystem image, so looking up a file is a
single hash computation instead of a search. That's the whole point of the
"MPH" (minimal perfect hash) code.

## 2. How BBHash achieves this: levels of "raffles"

BBHash (the algorithm in `bbhash.h`) works in **levels**. Think of each
level as a raffle:

- **Level 0** is a big row of buckets (a "bitvector" — just a long row of
  yes/no flags). Every one of the 1000 names is tossed into a random bucket
  (the hash decides which).
  - If a name lands in a bucket **nobody else wanted**, it's a winner: it
    gets stored there, done.
  - If **two or more names** land in the same bucket, they *collide*. None
    of them can stay — they're all pushed to the next round.
- **Level 1** is a second, smaller row of buckets. Only the names that
  collided in level 0 play again. Same rules: unique landing = stored,
  collision = pushed further down.
- This repeats: level 2, level 3, … until **no names are left**.

Because each round only has to place the leftovers of the previous one, the
levels get smaller and smaller, and the process always finishes. The final
"which number is this name" answer is just *how many winners came before it*
across the levels — that count is the name's unique slot.

### The key knob: gamma (γ)

**Gamma** is simply *how many buckets per name* a level provides. `gamma = 2`
means "make the row twice as long as the number of names playing in it."

- A bigger gamma → fewer collisions per round → fewer leftovers trickle down
  → but each level wastes more space.
- A smaller gamma → tighter levels → but more collisions and more rounds.

`gamma = 2` is a well-known sweet spot for BBHash: with a 2:1 bucket-to-name
ratio, roughly **half** the names win in each round, so the leftover pile
roughly **halves** at every level. That's the "halving" intuition you'll see
below.

## 3. The two sizing schemes

### Collision-dependent (data-dependent) — *current*

Each level is sized using the **actual number of leftover names** that
happened to collide in the previous round:

```
bitsize(level i) = ceil64(gamma × actual leftovers reaching level i)
```

The catch: the number of leftovers is **random**. It depends on the exact
names and the exact hash seed. Run the builder twice on different sets of
1000 names and the levels come out with different sizes. There's no formula
you can compute in advance that tells you the total table size — you have to
actually build the thing to find out.

### Deterministic (planned sizes) — *reverted experiment*

For a while the code sized each level for the **planned** number of names,
computed up-front from the total count `n` alone, using the "halving" rule
of thumb:

```
planned names at level i   = ceil(n / 2^i)      (n, n/2, n/4, …)
bitsize(level i)           = ceil64(gamma × planned_i)   (gamma = 2)
```

So for `n = 1000`:

| level | planned names | buckets (gamma×planned, rounded up to 64) |
|------:|--------------:|-------------------------------------------:|
| 0 | 1000 | 2048 |
| 1 |  500 | 1024 |
| 2 |  250 |  512 |
| 3 |  125 |  256 |
| 4 |   63 |  128 (min 64 → 128) |
| … |   …  | … |

The crucial property of that experiment: **the sequence depended only on `n`
and the number of levels — nothing else.** Same `n`, same sizes, every single
time, on every machine, for any set of names — which made the total table
size computable by a pure formula, `mph_bytes(levels, n)`.

Note the subtlety: the level was *sized for* `planned_i` names, but it still
*received* however many names actually collided (usually fewer than
`planned_i`, because the previous level was itself roomy). Sizing for the
planned count just meant there was always more than enough room — never too
small.

## 4. Why deterministic sizing was *needed* — and why it isn't anymore

This wasn't originally an optimization — it was a **correctness requirement**
that came out of the unified-API design (`UNIVERSAL-API-REVISED.md`).

EFS stores, for each directory, a fixed-size header followed by the MPH
table, followed by the name array, followed by the file data. To read a file,
the reader must know **where the MPH table ends** so it can find the names
that come after it. That means it needs the table's exact length.

Two ways to get that length:

1. **Store it** in the header (a length field or a descriptor). The project
   initially rejected this — *no extra per-directory bytes, no descriptor.*
2. **Compute it** from values already in the header, via a small function
   `mph_bytes(levels, n)` — a pure formula, exactly like jmph's
   `jmph_bytes(blen, w)`. This is only possible if the table size is a
   function of known parameters, i.e. deterministic.

Option 2 is what drove the deterministic experiment, and it was verified
empirically (`mph_bytes()` matched the real blob length for every tested `n`
from 1 to 100,000).

**But option 1 turned out to be free after all.** The shared `efs_dir`
header already carries a 32-bit `w` slot whose meaning is
algorithm-specific — jmph uses it as its displacement width, and nothing
says BBHash must use it the same way. So the BBHash builder simply stores
the exact serialized table length (`mph_out.len`) in `w`, and
`mph_bytes(blen, w)` returns that stored value unchanged. No new bytes per
directory, no format change, and the level sizing could go back to the
original collision-dependent rule.

| Algorithm | `w` meaning | `mph_bytes(blen, w)` |
|---|---|---|
| jmph | Displacement width, in bytes | Computes the serialized table size |
| BBHash | Serialized table length, in bytes | Returns `w` |

The BBHash blob contains its own level count, level sizes, and hash seed, so
the reader can decode it without knowing the number of input keys; EFS
already stores the directory entry count separately in `efs_dir.count`. The
`mph_bytes(blen, w)` function intentionally ignores `blen`; it exists to
provide the same size-function signature as jmph.

## 5. Did the experiment pay off? (No.)

The deterministic scheme's theoretical costs and benefits were measured with
the committed benchmark harness (`./bench_bbhash.sh`, results in
`BBHASH-BENCHMARK.md`):

- **Table size:** for the fixed generated test sets, restored
  collision-dependent sizing did not yield the predicted average size
  increase — tables were unchanged through 100 files and differed by at most
  24 bytes at larger counts, sometimes shrinking and sometimes growing.
  (Deterministic sizing tends to be ~0.6 B/key at scale either way; tiny
  directories are dominated by per-level fixed costs in both schemes.)
- **Speed:** the measured MPH-builder times showed no meaningful difference
  between the two schemes on this workload.
- **Levels:** deterministic sizing often produced fewer levels (the wider
  level 0 absorbs more names up front), but not enough to matter here.

Since the only advantage of deterministic sizing — computing the table size
without a stored length — became unnecessary once `w` carried the length,
and the benchmarks showed no size or speed win worth keeping it for, the
collision-dependent sizing was restored.

## 6. API contract

The algorithm-selection header maps both implementations to the same EFS
interface:

```
efs_mph_build(...)
efs_mph_index(table, blen, shift, salt, w, key, key_len)
efs_mph_bytes(blen, w)
```

For jmph, `w` must keep its displacement-width meaning because both its
decoder and size calculation require that value. For BBHash, the decoder
needs the table byte length instead, so BBHash uses `w` for that length.
Compile-time selection means an image is read only by the matching
implementation; no algorithm identifier is required in the image.

## 7. One-paragraph summary

BBHash places names in a cascade of "raffle rounds" (levels), each a row of
buckets `gamma` (=2) times longer than the number of names playing in it.
The rows are sized by *how many names actually survive to that round*, a
random number you can only learn by running the build — so the table's total
size is not a closed-form function of the key count. An experiment that
sized each row by a *planned* count (`n / 2^i`, making the size a fixed
formula so the reader needed no stored length) was reverted: storing the
exact table length in the header's already-existing, algorithm-specific `w`
slot costs zero extra bytes, and benchmarks showed the deterministic scheme
was neither smaller nor faster. That's the whole story.

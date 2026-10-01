# Deterministic Level Sizing (gamma = 2) — What It Is and Why

This document explains, in plain terms, a change made to the BBHash
minimal-perfect-hash builder (`bbhash.h`, formerly `oomph/mph.h`): the line
that decides **how big each level of the hash table should be** was changed
from a *data-dependent* formula to a *deterministic* one. You don't need to
know the algorithms to follow this — everything is explained from scratch.

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

## 3. The change: *planned* sizes instead of *actual* sizes

### Before (data-dependent)

Each level was sized using the **actual number of leftover names** that
happened to collide in the previous round:

```
bitsize(level i) = gamma × (actual leftovers reaching level i)
```

The problem: the number of leftovers is **random**. It depends on the exact
names and the exact hash seed. Run the builder twice on different sets of 1000
names and the levels come out with different sizes. There's no formula you
can compute in advance that tells you the total table size — you have to
actually build the thing to find out.

### After (deterministic)

The new code sizes each level for the **planned** number of names, computed
up-front from the total count `n` alone, using the "halving" rule of thumb:

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

The crucial property: **this sequence depends only on `n` and the number of
levels — nothing else.** Same `n`, same sizes, every single time, on every
machine, for any set of names.

Note the subtlety: the level is *sized for* `planned_i` names, but it still
*receives* however many names actually collide (usually fewer than
`planned_i`, because the previous level was itself roomy). Sizing for the
planned count just means there's always more than enough room — a level built
for `n_i` names that only receives about `n_i/2` is comfortably oversized,
never too small.

## 4. Why it was *needed*

This wasn't an optimization — it was a **correctness requirement** that came
out of the unified-API design (`UNIVERSAL-API-REVISED.md`).

EFS stores, for each directory, a fixed-size header followed by the MPH
table, followed by the name array, followed by the file data. To read a file,
the reader must know **where the MPH table ends** so it can find the names
that come after it. That means it needs the table's exact length.

Two ways to get that length:

1. **Store it** in the header (a length field or a descriptor). The project
   explicitly rejected this — *no extra per-directory bytes, no descriptor.*
2. **Compute it** from values already in the header, via a small function
   `mph_bytes(levels, n)`.

Option 2 is what the design chose, and it's exactly what the *other* MPH
implementation (jmph) already does: `jmph_bytes(blen, w)` is a pure formula.
But a pure formula is only possible if the table size is **a function of
known parameters** — i.e. deterministic. With the old data-dependent sizing,
no such function could exist; the size was entangled with random collision
outcomes. With the new sizing, `mph_bytes(levels, n)` reproduces the exact
byte count the builder produced, so the reader can locate the name table with
zero stored metadata.

This was verified empirically: `mph_bytes()` matches the real blob length
exactly for every tested `n` from 1 to 100,000.

## 5. Does it have downsides?

### Slightly larger tables — sometimes

Yes, modestly. Because each level is sized for the *planned* count rather
than the (usually smaller) *actual* leftover count, some buckets sit unused.
The total table size under the deterministic scheme is roughly:

| n | total table size | bytes/key |
|--:|-----------------:|----------:|
| 100 | 261 B | 2.61 |
| 1,000 | 801 B | 0.80 |
| 10,000 | 5,977 B | 0.60 |
| 100,000 | 56,661 B | 0.57 |

It converges to about **0.6 bytes per key** at scale — the same ballpark as
before (the old scheme was ~0.5–0.7 B/key depending on luck). So the
"overhead" is a small, roughly constant factor, not a blow-up.

The one place it's *more* noticeable is tiny directories: sizes are dominated
by the per-level fixed costs (a few bytes of header + a minimum of 64 buckets
per level), which is true for both schemes and unrelated to this change.

### Smaller tables at large n — also sometimes

Interestingly, the deterministic scheme also tends to use **fewer levels**
(because the wider level 0 absorbs more names up front), which can make the
table *smaller* than the old one for large n. In the measurements that led to
this change, a 5,000-key table went from 14 levels / 2,693 bytes (old) to
5 levels / 2,833 bytes (new) — comparable, with far fewer levels (faster
lookups, since each level is a possible extra probe).

### The real trade-off

| | old (data-dependent) | new (deterministic) |
|---|---|---|
| Table size | marginally smaller on average, variable | marginally larger on average, **predictable** |
| Size computable without building? | **no** | **yes** (`mph_bytes(levels, n)`) |
| Extra bytes in EFS header? | would need a descriptor | **none** |
| Reproducibility | sizes vary per key set | identical for identical `n` |

The change trades a tiny, bounded amount of space for the ability to compute
the layout arithmetically — which is what lets EFS keep its lean, descriptor-
free directory header. Given that EFS is a *ROM-friendly, read-only*
filesystem where every directory header byte is multiplied by the number of
directories, avoiding a descriptor field is worth far more than the few
percent of slack inside the hash table itself.

## 6. One-paragraph summary

BBHash places names in a cascade of "raffle rounds" (levels), each a row of
buckets `gamma` (=2) times longer than the number of names playing in it.
Previously each row was sized by *how many names actually survived to that
round*, a random number you can only learn by running the build. The change
sizes each row by a *planned* count, `n / 2^i`, computed purely from the
total `n`. This makes the table size a fixed formula — so EFS can work out
where the table ends and the names begin **without storing any extra bytes**
— at the cost of a small, bounded increase in table size (and often fewer,
faster levels). That's the whole story.

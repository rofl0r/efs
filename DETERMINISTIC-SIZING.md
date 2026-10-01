# BBHash Table Sizing in EFS

BBHash (the implementation in `bbhash.h`) places keys in successive levels of
bitvectors. Each level is sized from the number of keys that actually remain
after collisions at the previous level. Those collision counts depend on the
keys and the hash seed, so the serialized table length is not a function of
only the key count and number of levels.

## Storing the table length

EFS needs the hash table's exact byte length to find the name-offset array that
follows it. The shared EFS directory header already has a 32-bit `w` field,
whose meaning is specific to the selected hash algorithm:

| Algorithm | `w` meaning | `mph_bytes(blen, w)` |
|---|---|---|
| jmph | Displacement width, in bytes | Computes the serialized table size |
| BBHash | Serialized table length, in bytes | Returns `w` |

The BBHash builder knows the exact output length (`mph_out.len`), so it stores
that value in `mph_out.w`. The EFS builder copies it into the existing header;
the reader passes it to `mph_bytes()` and to `mph_lookup()` as the table's
bounds. BBHash's blob contains its own level count, level sizes, and hash seed,
so the reader can decode it without knowing the number of input keys. EFS
already stores the directory entry count separately in `efs_dir.count`.

This uses no additional per-directory bytes and does not change the EFS header
or image layout. The BBHash `mph_bytes(blen, w)` function intentionally ignores
`blen`; it exists to provide the same size-function signature as jmph.

## Level sizing

The builder retains the original collision-dependent sizing rule:

```
bitsize(level) = round_up_to_64(gamma * remaining_keys)
```

where `gamma` is 2 and `remaining_keys` is the actual number of keys passed to
that level. This avoids sizing later levels from hypothetical counts such as
`ceil(n / 2^level)`. Deterministic sizing is not needed to locate the end of
the table, because BBHash's exact serialized length is stored in `w`.

The sizing rule can affect the number of levels and the total bytes, since
those depend on collisions. For this reason table sizes may vary across key
sets even when their key counts are equal.

## API contract

The algorithm-selection header maps both implementations to the same EFS
interface:

```
efs_mph_build(...)
efs_mph_index(table, blen, shift, salt, w, key, key_len)
efs_mph_bytes(blen, w)
```

For jmph, `w` must keep its displacement-width meaning because both its decoder
and size calculation require that value. For BBHash, the decoder needs the
table byte length instead, so BBHash uses `w` for that length. Compile-time
selection means an image is read only by the matching implementation; no
algorithm identifier is required in the image.

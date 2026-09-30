# EFS Minimal Read-Only Filesystem (v1)

## Image format
The EFS image begins with a 4-byte magic, immediately followed by the root
directory header:

    "EFS\x01"   (4 bytes, version 1)

All internal offsets in `efs_dir` are relative to the start of that
directory header (not the image start). The caller locates the magic,
then treats the next bytes as `struct efs_dir *root`.

## Design notes
EFS is a compact, read-only filesystem image built around a **minimal perfect
hash (MPH)** per directory. Each directory's entries are hashed into a dense
index space of size `blen` (a power of two), so a name lookup requires only one
hash computation, one table read for the displacement, and one array index
into `name_offset` / `entry_offset` - **O(1) with no chaining or search**.
All structural fields are 32-bit relative offsets from the directory header,
so the image is position-independent and can be concatenated to any binary or
mapped from a block device; the caller only adds a base pointer. Storage is
kept minimal: only `count`, `blen`, `salt`, `names_len`, `w`, and `shift` are
stored (24 bytes), while the use of a scramble table is derived, not
duplicated. Directory vs file is encoded in the leading `/` of the name,
avoiding a type field. The result is a ROM-friendly structure with
deterministic O(1) lookup, no dynamic allocation at read time, and a
straightforward linear on-disk layout.

The four MPH parameters (`blen`, `salt`, `w`, `shift`) are algorithm-generic
slots (see UNIVERSAL-API-REVISED.md): the MPH implementation is chosen at
compile time, so the header carries no algorithm descriptor. For jmph (the
default) `blen`=table buckets, `salt`=seed, `w`=displacement width (1..4),
`shift`=hash shift. For BBHash `blen`=level count, `salt`=seed, `w`=key
count, `shift`=0.

## Directory header (fixed fields, 24 bytes)
struct efs_dir {
    uint32_t count;      /* number of entries */
    uint32_t blen;       /* MPH param: table buckets (jmph) / levels (bbhash) */
    uint32_t salt;       /* MPH param: hash seed */
    uint32_t names_len;  /* byte length of names blob (padded to 4) */
    uint32_t w;          /* MPH param: displacement width (jmph) / n (bbhash) */
    uint8_t  shift;      /* MPH param: hash shift (jmph); unused (bbhash) */
    uint8_t  reserved[3];
};

## On-disk layout (immediately after header)
[ hashtab       ]  size = mph_bytes(blen, w)  (opaque MPH data; the unified
                                             per-algorithm size function --
                                             jmph_bytes / mph_bytes)
[ name_offset[] ]  u32[count]                 (indexed by MPH index)
[ entry_offset[]]  u32[count+1]               (data ranges)
[ names         ]  u8[names_len]              (NUL-terminated, 4-byte padded)

## File vs directory
A name beginning with '/' denotes a directory. Its entry_offset points to
another `efs_dir`. Other names are files; entry_offset points to contents.

## File length
len = entry_offset[i+1] - entry_offset[i]
(entries are stored in MPH index order, so offsets are ascending)

## Lookup (O(1))
1. idx = mph_lookup(dir, name)
2. if idx >= count -> not found
3. optional: verify name at names + name_offset[idx]
4. return dir + entry_offset[idx]

## readdir
Caller passes a u32 cursor = 0. Each call returns the name of entry at
cursor (if < count) and overwrites cursor with cursor+1. When cursor >=
count, returns NULL and sets cursor = 0.

## Helpers (internal)
dir_hashtab(d)   = (u8*)(d+1)
dir_name_off(d)  = (u32*)(dir_hashtab(d) + mph_bytes(d->blen, d->w))
dir_entry_off(d) = dir_name_off(d) + d->count
dir_names(d)     = (u8*)(dir_entry_off(d) + d->count + 1)


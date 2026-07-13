# EFS Minimal Read-Only Filesystem (v1)

## Image format
The EFS image begins with a 4-byte magic, immediately followed by the root
directory header:

    "EFS\x01"   (4 bytes, version 1)

All internal offsets in `efs_dir` are relative to the start of that
directory header (not the image start). The caller locates the magic,
then treats the next bytes as `struct efs_dir *root`.

## Directory header (fixed fields, 20 bytes)
struct efs_dir {
    uint32_t count;      /* number of entries */
    uint32_t blen;       /* hash table size (power of two, >= 2) */
    uint32_t salt;       /* MPH seed multiplier */
    uint32_t names_len;  /* byte length of names blob (padded to 4) */
    uint8_t  shift;      /* MPH shift (0..31) */
    uint8_t  w;          /* bytes per scramble entry (1..4) */
    uint8_t  reserved1;
    uint8_t  reserved2;
};

## On-disk layout (immediately after header)
[ hashtab       ]  size = mph_bytes(blen, w)   (opaque MPH data)
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


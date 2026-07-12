# EFS Minimal Read-Only Filesystem (v1)

## Overview
EFS is a flat, hash-indexed directory image. The image starts with a 4-byte
magic `"EFS\x01"`, immediately followed by the root directory header.
All internal offsets are `u32`, relative to the start of the directory
header they belong to. The caller is responsible for locating the magic
(e.g. in a concatenated binary) and passing a pointer to the root dir.

## Directory header (fixed fields)
struct efs_dir {
    u32 count;      // number of entries
    u32 blen;       // hash table size (power of two, >= count)
    u32 shift;      // MPH shift
    u32 salt;       // MPH salt
    u32 names_len;  // byte length of names blob (padded to 4)
};

## On-disk layout (immediately after header)
[ hashtab       ]  size = mph_bytes(blen)   (opaque MPH data)
[ name_offset[] ]  u32[count]               (indexed by MPH index)
[ entry_offset[]]  u32[count+1]             (data ranges)
[ names         ]  u8[names_len]            (NUL-terminated, 4-byte padded)

## File vs directory
A name beginning with '/' denotes a directory. Its entry_offset points to
another `efs_dir`. Other names are files; entry_offset points to contents.

## File length
len = entry_offset[i+1] - entry_offset[i]
(entries are stored in MPH index order, so offsets are ascending)

## Lookup (O(1))
1. idx = mph_lookup(dir, name)
2. if idx >= count -> not found
3. verify name at names + name_offset[idx]
4. return dir + entry_offset[idx]

## readdir
Caller passes a u32 cursor = 0. Each call returns the name of entry at
cursor (if < count) and overwrites cursor with cursor+1. When cursor >=
count, returns NULL and sets cursor = 0.

## Helpers (internal)
dir_hashtab(d)   = (u8*)(d+1)
dir_name_off(d)  = (u32*)(dir_hashtab(d) + mph_bytes(d->blen))
dir_entry_off(d) = dir_name_off(d) + d->count
dir_names(d)     = (u8*)(dir_entry_off(d) + d->count + 1)


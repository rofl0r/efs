#ifndef EFS_H
#define EFS_H

#include <stdint.h>
#include <string.h>

/* The MPH generator is selected at compile time by efs_mph.h (jmph.h by
 * default; oomph/mph.h when EFS_MPH_OOMPH is defined). We include it up
 * front so the on-disk consumer (EFS_IMPL) can call efs_mph_index()/
 * efs_mph_bytes() to locate and read the name table; the heavy builder code
 * is only compiled when MPH_IMPL or EFS_BUILDER is defined. */
#include "efs_mph.h"

#ifndef EFS_EXPORT
#define EFS_EXPORT
#endif

#define EFS_MAGIC "EFS\x01"

struct efs_dir {
    uint32_t count;
    uint32_t blen;
    uint32_t salt;
    uint32_t names_len;
    uint8_t shift;
    uint8_t w;
    uint8_t reserved1;
    uint8_t reserved2;
};

/* ---- API prototypes (always visible) ---- */
EFS_EXPORT const uint8_t *efs_lookup(const struct efs_dir *root, const char *path, uint32_t *len, int *is_dir);
EFS_EXPORT const char    *efs_readdir(const struct efs_dir *dir, uint32_t *cursor);

#ifdef EFS_IMPL

#include <stdint.h>
#include <string.h>

/* The MPH decode lives in the selected MPH header (jmph.h by default). The
 * reader supplies the four parameters stored in the directory header plus
 * the table bytes that follow it; behaviour for a key outside the build set
 * is undefined, so efs_lookup() verifies the matched name before trusting
 * the index. */
static uint32_t efs_dir_index(const struct efs_dir *d, const char *key){
    return efs_mph_index((const uint8_t*)(d + 1), d->blen, d->shift, d->salt,
                         d->w, key, (uint32_t)strlen(key));
}

/* ---- internal layout helpers ---- */
static const uint8_t  *dir_hashtab(const struct efs_dir *d){
    return (const uint8_t*)(d + 1);
}
/* Byte length of the on-disk MPH table for directory `d`, computed from the
 * four stored parameters without any descriptor (per-algorithm). */
static uint32_t efs_mph_tablen(const struct efs_dir *d){
#ifdef EFS_MPH_OOMPH
    /* BBHash blob: 5-byte header (num_levels + seed), then per level a u32
     * bitsize followed by its bitvector and rank table. Walk the headers. */
    const uint8_t *p = dir_hashtab(d), *q = p;
    uint32_t levels = *q++;
    q += 4; /* seed */
    for(uint32_t i = 0; i < levels; i++){
        uint32_t bitsize = ((uint32_t)q[0]<<24)|((uint32_t)q[1]<<16)|
                           ((uint32_t)q[2]<<8)|(uint32_t)q[3];
        q += 4;
        uint32_t size = (bitsize + 63) / 64, rank_size = (bitsize + 511) / 512 + 1;
        q += (size_t)size * 8 + (size_t)rank_size * 8;
    }
    return (uint32_t)(q - p);
#else
    return efs_mph_bytes(d->blen, d->w);
#endif
}
static const uint32_t *dir_name_off(const struct efs_dir *d){
    return (const uint32_t*)(dir_hashtab(d) + efs_mph_tablen(d));
}
static const uint32_t *dir_entry_off(const struct efs_dir *d){
    return dir_name_off(d) + d->count;
}
static const uint8_t  *dir_names(const struct efs_dir *d){
    return (const uint8_t*)(dir_entry_off(d) + d->count + 1);
}

/* ---- API implementation ---- */
EFS_EXPORT const uint8_t *efs_lookup(const struct efs_dir *root, const char *path, uint32_t *len, int *is_dir){
    if(is_dir) *is_dir = 0;
    const struct efs_dir *cur = root;
    const char *p = path;
    while(*p == '/') p++;
    if(!*p && p != path) {
        if(is_dir) *is_dir = 1;
        return (void*)root;
    }
    char buf[4096];
    buf[0] = '/';

    while(1){
        const char *slash = strchr(p, '/');
        size_t clen = slash ? (size_t)(slash - p) : strlen(p);
        if(clen > 4095) return 0; /* component too long */
        memcpy(buf + 1, p, clen);
        buf[clen + 1] = 0;

        const char *tryname = buf + 1;       /* start without leading '/' */
        const uint8_t *res = 0;
        for(int pass = 0; pass < 2; pass++){
            uint32_t idx = efs_dir_index(cur, tryname);
            if(idx < cur->count){
                const uint32_t *noff = dir_name_off(cur);
                const uint8_t *nm = dir_names(cur) + noff[idx];
                if(strcmp((const char*)nm, tryname) == 0){
                    const uint32_t *eoff = dir_entry_off(cur);
                    if(len) *len = eoff[idx+1] - eoff[idx];
                    res = (const uint8_t*)cur + eoff[idx];
                    if(is_dir) *is_dir = (nm[0] == '/');
                    break;
                }
            }
            tryname = buf;   /* second pass: with leading '/' */
        }
        if(!res) return 0;

        if(!slash){
            return res;
        }
        /* must be a directory */
        if(*tryname != '/') return 0;
        cur = (const struct efs_dir*)res;
        p = slash + 1;
    }
}

EFS_EXPORT const char *efs_readdir(const struct efs_dir *dir, uint32_t *cursor){
    if(*cursor >= dir->count){
        *cursor = 0;
        return 0;
    }
    const uint32_t *noff = dir_name_off(dir);
    const uint8_t *nm = dir_names(dir) + noff[*cursor];
    uint32_t cur = *cursor;
    (*cursor)++;
    return (const char*)nm;
}

#endif /* EFS_IMPL */

/*
 * =====================================================================
 *  MPH builder (EFS_BUILDER / MPH_IMPL)
 *
 *  The minimal perfect hash generator lives in the selected MPH header
 *  (jmph.h by default; see efs_mph.h / UNIVERSAL-API-REVISED.md for how to
 *  swap it). Define EFS_BUILDER (or MPH_IMPL) before including efs.h to
 *  compile that header's builder, which exposes:
 *
 *      int      X_build(const struct X_in *in, struct X_out *out);
 *      uint32_t X_index_p(tab, blen, shift, salt, w, key, klen);
 *      uint32_t X_bytes(blen, w);
 *
 *  The builder feeds directory names to X_build() and gets back a packed
 *  table plus the four parameters (blen/shift/salt/w) that are stored in
 *  struct efs_dir; the consumer above reads the table back with X_index_p()
 *  using those same parameters. There is no runtime algorithm descriptor:
 *  an image is built against exactly one MPH implementation, chosen at
 *  compile time.
 * =====================================================================
 */

#endif /* EFS_H */

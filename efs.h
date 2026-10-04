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

/* EFS_EXPORT controls the linkage of the EFS API functions, which are
 * defined in this header (under EFS_IMPL). The default `static inline` makes
 * the header safe to include from several TUs that later link together (e.g.
 * efstest.o + efstest_builder.o); a program that wants a single exported
 * definition instead can `#define EFS_EXPORT extern` in exactly one TU. */
#ifndef EFS_EXPORT
#define EFS_EXPORT static inline
#endif

#define EFS_MAGIC "EFS\x01"

struct efs_dir {
    uint32_t count;
    uint32_t blen;
    uint32_t salt;
    uint32_t names_len;
    /* Per-algorithm view of the 4 algorithm-specific parameter bytes. This
     * union is the single argument the unified size function takes: each
     * backend's *_bytes(p) reads its own angle of it (see efs_mph.h). */
    union mph_params {
        uint32_t bb_sz;               /* oomph/BBHash: serialized table length (full u32 range) */
        struct {
            uint8_t  w;               /* jmph: bytes per displacement (1..4) */
            uint8_t  shift;           /* jmph: hash shift (0..63) */
            uint8_t  reserved[2];     /* jmph: spare, always 0 */
        } jmph;
    } mph_params;                     /* named member: d->mph_params.jmph.w etc. */
};

#ifdef EFS_IMPL
/* The union is 4 bytes and naturally aligned after the four u32s, so the
 * header is exactly 20 bytes under either algorithm selection. */
_Static_assert(sizeof(struct efs_dir) == 20, "efs_dir header must be 20 bytes");
#endif

/* ---- API prototypes (always visible) ---- */
EFS_EXPORT const uint8_t *efs_lookup(const struct efs_dir *root, const char *path, uint32_t *len, int *is_dir);
EFS_EXPORT const char    *efs_readdir(const struct efs_dir *dir, uint32_t *cursor);
/* Build an EFS image of the directory tree at srcpath into outpath.
 * Implemented by efsbuilder.c; returns 0 on success, -1 on failure.
 * EFS_BUILD_API controls its linkage (extern by default so it can live in
 * the builder object; efstest defines it `static` to give the builder TU
 * internal linkage and avoid a duplicate symbol). */
#ifndef EFS_BUILD_API
#define EFS_BUILD_API
#endif
EFS_BUILD_API int         efs_build_path(const char *srcpath, const char *outpath);

#ifdef EFS_IMPL

#include <stdint.h>
#include <string.h>

/* The MPH decode lives in the selected MPH header (jmph.h by default). The
 * reader supplies the four parameters stored in the directory header plus
 * the table bytes that follow it; behaviour for a key outside the build set
 * is undefined, so efs_lookup() verifies the matched name before trusting
 * the index. */
static uint32_t efs_dir_index(const struct efs_dir *d, const char *key){
#ifdef EFS_MPH_OOMPH
    /* oomph: shift is unused; the table-length argument comes from bb_sz. */
    return efs_mph_index((const uint8_t*)(d + 1), d->blen, 0, d->salt,
                         d->mph_params.bb_sz, key, (uint32_t)strlen(key));
#else
    return efs_mph_index((const uint8_t*)(d + 1), d->blen, d->mph_params.jmph.shift,
                         d->salt, d->mph_params.jmph.w, key, (uint32_t)strlen(key));
#endif
}

/* ---- internal layout helpers ---- */
static const uint8_t  *dir_hashtab(const struct efs_dir *d){
    return (const uint8_t*)(d + 1);
}
static const uint32_t *dir_name_off(const struct efs_dir *d){
    /* efs_mph_bytes(d->blen, d->mph_params) is the unified size function:
     * it takes the shared bucket count plus the whole per-algorithm parameter
     * union, and the compile-time #ifdef in efs_mph.h maps it to the selected
     * backend's *_bytes(), which reads its own angle of the union. For jmph
     * the size is computed from (blen, p.jmph.w); for BBHash the level sizes
     * are collision-dependent data, so the builder stored the exact
     * serialized table length in p.bb_sz and mph_bytes() just returns it.
     * Either way the size is derived from the stored params with no extra
     * descriptor. */
    return (const uint32_t*)(dir_hashtab(d) + efs_mph_bytes(d->blen, d->mph_params));
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
 *      uint32_t X_bytes(...);   // per-algorithm table-size function; EFS
 *                               // reaches it through the
 *                               // efs_mph_bytes(blen, d->mph_params) macro
 *                               // in efs_mph.h
 *
 *  The builder feeds directory names to X_build() and gets back a packed
 *  table plus the algorithm parameters (blen/salt as shared u32s, plus the
 *  mph_params union: jmph's w/shift or BBHash's bb_sz) that are stored in
 *  struct efs_dir; the consumer above reads the table back with X_index_p()
 *  using those same parameters. There is no runtime algorithm descriptor:
 *  an image is built against exactly one MPH implementation, chosen at
 *  compile time.
 * =====================================================================
 */

#endif /* EFS_H */

#ifndef EFS_H
#define EFS_H

#include <stdint.h>
#include <string.h>

#ifndef EFS_EXPORT
#define EFS_EXPORT
#endif

#define EFS_MAGIC "EFS\x01"

/* The on-disk table layout and its scramble threshold are owned by the
 * MPH implementation (jmph.h). The consumer just reads back what the
 * builder wrote, so import that single constant here. */
#ifndef USE_SCRAMBLE
#define USE_SCRAMBLE 4096
#endif

/* ---- shared MPH helpers (used by the on-disk consumer decoder) ---- */
static uint32_t mph_bytes(uint32_t blen, uint32_t w){
    return (blen >= USE_SCRAMBLE) ? (blen + 256 * w) : (blen * w);
}

static uint32_t tab_load(const uint8_t *p, uint32_t w){
    uint32_t v = 0;
    while (w--) v = (v << 8) | *p++;
    return v;
}

/* The on-disk table layout and its scramble threshold are owned by the
 * MPH implementation (jmph.h). The consumer just reads back what the
 * builder wrote, so import that single constant here. */
#ifndef USE_SCRAMBLE
#define USE_SCRAMBLE 4096
#endif

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

/* ---- MPH consumer code (from CONSUMER_CODE) ---- */
#include <stdint.h>
#include <string.h>

struct mph_data {
    uint32_t blen;
    uint32_t shift;
    uint32_t salt;
    uint32_t w;
    uint8_t  data[];
};

static uint32_t tab_disp(const struct mph_data *m, uint32_t b, uint32_t w){
    if (m->blen >= 4096) {
        uint32_t idx = m->data[b];
        return tab_load(m->data + m->blen + idx * w, w);
    }
    return tab_load(m->data + b * w, w);
}

static uint32_t mph_lookup3(const uint8_t *k, uint32_t len, uint32_t level){
    uint32_t a,b,c,o=len;
    a=b=0x9e3779b9; c=level;
    while(len>=12){
        a+=(k[0]|k[1]<<8|k[2]<<16|k[3]<<24);
        b+=(k[4]|k[5]<<8|k[6]<<16|k[7]<<24);
        c+=(k[8]|k[9]<<8|k[10]<<16|k[11]<<24);
        a-=b;a-=c;a^=c>>13;b-=c;b-=a;b^=a<<8;c-=a;c-=b;c^=b>>13;
        a-=b;a-=c;a^=c>>12;b-=c;b-=a;b^=a<<16;c-=a;c-=b;c^=b>>5;
        a-=b;a-=c;a^=c>>3;b-=c;b-=a;b^=a<<10;c-=a;c-=b;c^=b>>15;
        k+=12; len-=12;
    }
    c+=o;
    switch(len){
    case 11:c+=k[10]<<24; case 10:c+=k[9]<<16; case 9:c+=k[8]<<8;
    case 8:b+=k[7]<<24; case 7:b+=k[6]<<16; case 6:b+=k[5]<<8; case 5:b+=k[4];
    case 4:a+=k[3]<<24; case 3:a+=k[2]<<16; case 2:a+=k[1]<<8; case 1:a+=k[0];
    }
    a-=b;a-=c;a^=c>>13;b-=c;b-=a;b^=a<<8;c-=a;c-=b;c^=b>>13;
    a-=b;a-=c;a^=c>>12;b-=c;b-=a;b^=a<<16;c-=a;c-=b;c^=b>>5;
    a-=b;a-=c;a^=c>>3;b-=c;b-=a;b^=a<<10;c-=a;c-=b;c^=b>>15;
    return c;
}

/* wrapper so we can use efs_dir directly */
static uint32_t mph_lookup(const struct efs_dir *d, const char *key){
    uint32_t len = (uint32_t)strlen(key);
    uint32_t v = mph_lookup3((const uint8_t*)key, len, d->salt * 0x9e3779b9);
    uint32_t a = v >> d->shift;
    uint32_t b = v & (d->blen - 1);
    uint32_t w = d->w;
    uint32_t disp;
    const uint8_t *tab = (const uint8_t*)(d + 1);
    if (d->blen >= 4096) {
        uint32_t idx = tab[b];
        disp = tab_load(tab + d->blen + idx*w, w);
    } else {
        disp = tab_load(tab + b*w, w);
    }
    return (a ^ disp);
}

/* ---- internal layout helpers ---- */
static const uint8_t  *dir_hashtab(const struct efs_dir *d){
    return (const uint8_t*)(d + 1);
}
static const uint32_t *dir_name_off(const struct efs_dir *d){
    return (const uint32_t*)(dir_hashtab(d) + mph_bytes(d->blen, d->w));
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
            uint32_t idx = mph_lookup(cur, tryname);
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
 *  EFS_BUILDER  -- Minimal perfect hash (MPH) generator.
 *
 *  Reusable, self-contained MPH builder. Define EFS_BUILDER before
 *  including efs.h to get `gen_mph()` plus its helpers. It has no
 *  dependency on the on-disk layout; the caller feeds keys and gets
 *  back a packed table (see struct mph_out) that the consumer code in
 *  EFS_IMPL reads back with the same parameters (blen/shift/salt/w).
 *
 *  This is a minimal port of Bob Jenkins' perfect.c (public domain),
 *  www.burtleburtle.net/bob/c/perfect.c, specialised to the string
 *  hash (lookup/Jenkins "lookup2"). Highlights:
 *    * scramble[] is computed once (a fixed permutation of 0..smax-1)
 *      and reused across every salt trial, not recomputed each trial.
 *    * Keys are mapped in descending bucket-size order (largest
 *      buckets first), which is what makes the O(n) augmenting-path
 *      matching fast instead of O(n^2).
 *    * The table size blen only grows when a perfect hash genuinely
 *      cannot be built for the current salt, never on a slow attempt.
 * =====================================================================
 */
#ifdef EFS_BUILDER

/* The MPH generator now lives in jmph.h (single-header library). Including
 * it here, with EFS_BUILDER defined, pulls in jmph_build() / jmph_index()
 * and the rest of the builder. The on-disk consumer (EFS_IMPL) reads back
 * the table jmph_build produces, using the shared USE_SCRAMBLE threshold. */
#include "jmph.h"

#endif /* EFS_BUILDER */

#endif /* EFS_H */

#ifndef EFS_H
#define EFS_H

#include <stdint.h>
#include <string.h>

#ifndef EFS_EXPORT
#define EFS_EXPORT
#endif

#define EFS_MAGIC "EFS\x01"

/* ---- shared MPH helpers (used by both consumer and builder) ---- */
static uint32_t mph_bytes(uint32_t blen, uint32_t w){
    return (blen >= 4096) ? (blen + 256 * w) : (blen * w);
}

static uint32_t tab_load(const uint8_t *p, uint32_t w){
    uint32_t v = 0;
    while (w--) v = (v << 8) | *p++;
    return v;
}

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

#include <stdlib.h>
#include <stdio.h>

#ifndef USE_SCRAMBLE
#define USE_SCRAMBLE 4096
#endif

/* Per-bucket working state for the matcher. */
typedef struct {
    uint32_t val_b;   /* scramble[] index assigned to this bucket */
    uint32_t len;     /* number of keys sharing this bucket */
    uint32_t off;     /* base offset into flat_list */
    uint32_t water;   /* traversal high-water mark for the matching */
} mph_Bstuff;

/* Queue node used by the augmenting-path search (a spanning tree). */
typedef struct {
    uint32_t b_q;        /* bucket index occupying this queue node */
    uint32_t parent_q;   /* queue position of the parent bucket */
    uint32_t newval_q;   /* val_b for the parent to reach this node */
    uint32_t oldval_q;   /* parent's previous val_b (for rollback) */
} mph_Qstuff;

static uint32_t mph_mylog2(uint32_t v){
    uint32_t i;
    for(i = 0; ((uint32_t)1 << i) < v; i++)
        ;
    return i;
}

/* Jenkins "lookup2" mix; matches the hash used by the consumer side. */
#define mph_mix(a,b,c) \
{ a-=b;a-=c;a^=c>>13;b-=c;b-=a;b^=a<<8;c-=a;c-=b;c^=b>>13; \
  a-=b;a-=c;a^=c>>12;b-=c;b-=a;b^=a<<16;c-=a;c-=b;c^=b>>5; \
  a-=b;a-=c;a^=c>>3;b-=c;b-=a;b^=a<<10;c-=a;c-=b;c^=b>>15; }

/* Hash a key to a 32-bit value; the consumer side uses the identical
 * mix, so the same (salt) reproduces the same hash there. */
static uint32_t mph_lookup_k(const uint8_t *k, uint32_t len, uint32_t level){
    uint32_t a, b, c, o = len;
    a = b = 0x9e3779b9;
    c = level;
    while(len >= 12){
        a += (k[0]|k[1]<<8|k[2]<<16|k[3]<<24);
        b += (k[4]|k[5]<<8|k[6]<<16|k[7]<<24);
        c += (k[8]|k[9]<<8|k[10]<<16|k[11]<<24);
        mph_mix(a,b,c);
        k += 12;
        len -= 12;
    }
    c += o;
    switch(len){
    case 11:c+=k[10]<<24; case 10:c+=k[9]<<16; case 9:c+=k[8]<<8;
    case 8:b+=k[7]<<24; case 7:b+=k[6]<<16; case 6:b+=k[5]<<8; case 5:b+=k[4];
    case 4:a+=k[3]<<24; case 3:a+=k[2]<<16; case 2:a+=k[1]<<8; case 1:a+=k[0];
    }
    mph_mix(a,b,c);
    return c;
}

/* permutation p(x) of 0..(1<<nbits)-1, used to fill scramble[] */
static uint32_t mph_permute(uint32_t x, uint32_t nbits){
    uint32_t mask = ((uint32_t)1 << nbits) - 1;
    int c2 = 1 + nbits/2, c3 = 1 + nbits/3, c4 = 1 + nbits/4, c5 = 1 + nbits/5;
    for(int i = 0; i < 20; i++){
        x = (x + (x << c2)) & mask;
        x = x ^ (x >> c3);
        x = (x + (x << c4)) & mask;
        x = x ^ (x >> c5);
    }
    return x;
}

struct mph_in {
    uint32_t n;        /* number of keys */
    char   **keys;     /* key strings (NUL-terminated) */
    uint32_t *kl;      /* key lengths; must all be < 256 */
};

struct mph_out {
    uint32_t blen;     /* table size (power of two, >= 2) */
    uint32_t shift;    /* a = hash >> shift */
    uint32_t salt;     /* seed passed to mph_lookup_k */
    uint32_t w;        /* bytes per scramble entry (1..4) */
    uint8_t *data;     /* packed table; free() with free() */
};

/*
 * Reusable index helper: given a finished table, map `key` to its unique
 * slot in 0..blen-1 (== 0..n-1 for a minimal perfect hash). This is the
 * exact inverse of the consumer-side hash, kept here so the builder and
 * the reader agree and so callers (e.g. efsbuilder) don't re-implement it.
 */
static uint32_t mph_index(const struct mph_out *mo, const uint8_t *key, uint32_t klen){
    uint32_t v = mph_lookup_k(key, klen, mo->salt * 0x9e3779b9);
    uint32_t a = v >> mo->shift;
    uint32_t b = v & (mo->blen - 1);
    uint32_t w = mo->w;
    uint32_t disp;
    if(mo->blen >= USE_SCRAMBLE){
        uint32_t idx = mo->data[b];
        disp = tab_load(mo->data + mo->blen + idx * w, w);
    } else {
        disp = tab_load(mo->data + b * w, w);
    }
    return a ^ disp;
}

/*
 * Walk the augmenting path from tail-1 up to the root, erasing and
 * re-applying val_b along the way. rollback==0 applies the path
 * (parent uses newval_q); rollback==1 undoes it (parent uses oldval_q).
 *
 * Mirrors perfect.c's apply(): the root bucket (parent_q == 0) has no
 * prior hash to erase, so in the erase pass it is skipped only on
 * rollback; in the apply pass its (a^scramble[val_b]) slots ARE recorded
 * into tabh so later buckets detect collisions with it.
 */
static int mph_apply(mph_Bstuff *tabb, uint32_t *tabh, mph_Qstuff *tabq,
                     uint32_t n, uint32_t tail, int rollback,
                     uint32_t *ka, uint32_t *kb, uint32_t *flat_list,
                     uint32_t *scramble){
    for(uint32_t child = tail - 1; child; child = tabq[child].parent_q){
        uint32_t parent = tabq[child].parent_q;
        uint32_t b = tabq[parent].b_q;
        uint32_t stabb = scramble[tabb[b].val_b];
        for(uint32_t j = 0; j < tabb[b].len; j++){
            uint32_t ki = flat_list[tabb[b].off + j];
            uint32_t h = ka[ki] ^ stabb;
            if(h >= n) continue;
            if(tabh[h] == ki)
                tabh[h] = 0xFFFFFFFF;
        }
        tabb[b].val_b = rollback ? tabq[child].oldval_q : tabq[child].newval_q;
        stabb = scramble[tabb[b].val_b];
        for(uint32_t j = 0; j < tabb[b].len; j++){
            uint32_t ki = flat_list[tabb[b].off + j];
            uint32_t h = ka[ki] ^ stabb;
            if(h >= n) continue;
            if(rollback){
                if(parent == 0) continue;          /* root never had a hash */
            } else if(tabh[h] != 0xFFFFFFFF){
                mph_apply(tabb, tabh, tabq, n, tail, 1, ka, kb, flat_list, scramble);
                return 0;
            }
            tabh[h] = ki;
        }
    }
    return 1;
}

/*
 * Find an augmenting path that maps bucket `item` to a free hash slot,
 * mirroring perfect.c's augment(). Returns 1 on success (the mapping is
 * applied); 0 if no perfect extension exists from this state.
 *
 * The transitive closure (BFS over buckets) is always built for a minimal
 * perfect hash; this is what makes the matching efficient. The trial range
 * is `smax` (or 256 when blen >= USE_SCRAMBLE) because val_b is an index
 * into scramble[].
 */
static int mph_augment(mph_Bstuff *tabb, uint32_t *tabh, mph_Qstuff *tabq,
                       uint32_t blen, uint32_t *scramble, uint32_t smax,
                       uint32_t item, uint32_t n, uint32_t highwater,
                       uint32_t *ka, uint32_t *kb, uint32_t *flat_list){
    uint32_t q, tail;
    uint32_t limit = (blen >= USE_SCRAMBLE) ? 256 : smax;

    tabq[0].b_q = item;
    tail = 1;

    for(q = 0; q < tail; q++){
        uint32_t b = tabq[q].b_q;
        uint32_t i;
        for(i = 0; i < limit; i++){
            uint32_t childb = 0xFFFFFFFF;
            int broke = 0;
            for(uint32_t j = 0; j < tabb[b].len; j++){
                uint32_t ki = flat_list[tabb[b].off + j];
                uint32_t h = ka[ki] ^ scramble[i];
                if(h >= n){ broke = 1; break; }      /* out of range */
                uint32_t ck = tabh[h];
                if(ck != 0xFFFFFFFF){
                    uint32_t cb = kb[ck];
                    if(childb != 0xFFFFFFFF){
                        if(childb != cb){ broke = 1; break; } /* >1 child */
                    } else {
                        if(tabb[cb].water == highwater){ broke = 1; break; }
                        childb = cb;
                    }
                }
            }
            if(broke) continue;                      /* multiple collisions */
            /* Enqueue the child (or a dummy for the no-collision case) so the
             * apply() path walks back and updates *this* bucket's val_b and
             * records its hashes. Mirror perfect.c's dummy node. */
            if(childb != 0xFFFFFFFF)
                tabb[childb].water = highwater;
            tabq[tail].b_q = childb;
            tabq[tail].newval_q = i;
            tabq[tail].oldval_q = tabb[b].val_b;
            tabq[tail].parent_q = q;
            ++tail;
            if(childb == 0xFFFFFFFF){
                if(mph_apply(tabb, tabh, tabq, n, tail, 0,
                             ka, kb, flat_list, scramble))
                    return 1;
                --tail;                              /* couldn't use this i */
            }
        }
    }
    return 0;
}

/* ascending order by bucket length (caller iterates reversed => descending) */
static void mph_heap_sift(mph_Bstuff *tabb, uint32_t *order, uint32_t n, uint32_t start){
    uint32_t root = start;
    for(;;){
        uint32_t child = 2*root + 1;
        if(child >= n) break;
        if(child+1 < n && tabb[order[child]].len < tabb[order[child+1]].len)
            child++;
        if(tabb[order[root]].len >= tabb[order[child]].len)
            break;
        uint32_t t = order[root]; order[root] = order[child]; order[child] = t;
        root = child;
    }
}

static void mph_heap_sort(mph_Bstuff *tabb, uint32_t *order, uint32_t n){
    if(n == 0) return;
    for(uint32_t i = n/2; i-- > 0; )
        mph_heap_sift(tabb, order, n, i);
    for(uint32_t end = n-1; end > 0; end--){
        uint32_t t = order[0]; order[0] = order[end]; order[end] = t;
        mph_heap_sift(tabb, order, end, 0);
    }
}

/*
 * Guess initial alen/blen for a minimal perfect hash, matching
 * perfect.c's initalen() for the slow/high-compaction settings.
 */
static void mph_initalen(uint32_t n, uint32_t smax, uint32_t *alen, uint32_t *blen){
    uint32_t sl = mph_mylog2(smax);
    if(sl <= 8){
        *alen = smax/2; *blen = smax/2;
    } else if(sl <= 17){
        *alen = (n <= smax*0.52) ? smax/8 : smax/4;
        *blen = (n <= smax*0.52) ? smax/8 : smax/4;
        if(*blen >= USE_SCRAMBLE) *blen = smax/4;
    } else if(sl == 18){
        *alen = smax/8;
        *blen = (n <= smax*5/8) ? smax/4 : smax/2;
    } else {
        *alen = (n <= smax*5/8) ? smax/8 : smax/2;
        *blen = (n <= smax*5/8) ? smax/4 : smax/2;
    }
    if(*blen < 2) *blen = 2;
    if(*alen < 2) *alen = 2;
}

/*
 * Build a minimal perfect hash for the given keys.
 * On success returns 1 and fills *out (caller must free(out->data)).
 * On failure returns 0 and out->data is set to NULL.
 *
 * The packed table is laid out so the EFS_IMPL consumer (and mph_index
 * above) read it back identically:
 *   - blen < USE_SCRAMBLE: blen*w bytes, each bucket's w-byte word is
 *     scramble[val_b] (the disp).
 *   - blen >= USE_SCRAMBLE: 1 byte per bucket holding val_b, followed by
 *     256*w bytes holding scramble[] (so disp = scramble[data[b]]).
 */
static int gen_mph(const struct mph_in *in, struct mph_out *out){
    *out = (struct mph_out){0};
    const uint32_t n = in->n;

    if(!n){
        out->blen = 1;
        out->shift = 31;
        out->salt = 0;
        out->w = 1;
        out->data = calloc(1, mph_bytes(1, 1));
        return out->data ? 1 : 0;
    }

    uint32_t smax = 1;
    while(smax < n) smax <<= 1;
    if(smax < 2) smax = 2;

    uint32_t alen, blen;
    mph_initalen(n, smax, &alen, &blen);

    uint32_t sl = mph_mylog2(smax);
    uint32_t shift = (alen > 1) ? 32 - mph_mylog2(alen) : 0;
    uint32_t mask = blen - 1;
    int use_scramble = (blen >= USE_SCRAMBLE);

    uint32_t *scramble = malloc(smax * sizeof(uint32_t));
    uint32_t *vbuf   = malloc(n * sizeof(uint32_t));
    uint32_t *ka     = malloc(n * sizeof(uint32_t));
    uint32_t *kb     = malloc(n * sizeof(uint32_t));
    mph_Bstuff *tabb = calloc(smax, sizeof(mph_Bstuff));
    uint32_t *flat_list = malloc(n * sizeof(uint32_t));
    uint32_t *counts = calloc(smax, sizeof(uint32_t));
    uint32_t *order  = malloc(smax * sizeof(uint32_t));
    uint32_t *tabh   = malloc(n * sizeof(uint32_t));
    mph_Qstuff *tabq = malloc((smax + 1) * sizeof(mph_Qstuff));
    if(!scramble || !vbuf || !ka || !kb || !tabb || !flat_list || !counts || !order || !tabh || !tabq){
        perror("malloc");
        goto fail;
    }

    /* scramble[] depends only on smax, so build it once for all trials. */
    for(uint32_t i = 0; i < smax; i++) scramble[i] = mph_permute(i, sl);

    int ok = 0;
    uint32_t salt = 0;
    uint32_t limit = use_scramble ? 256 : smax;

    if(n <= 1){
        /* A single key trivially has a unique slot. Pick a salt, then store
         * a table of blen=1 with disp == a so that idx = a^disp == 0. */
        salt = 1;
        uint32_t seed = salt * 0x9e3779b9;
        uint32_t v = mph_lookup_k((const uint8_t*)in->keys[0], in->kl[0], seed);
        uint32_t a0 = (alen > 1) ? v >> shift : 0;
        uint32_t vb = 0, disp0 = 0;
        for(uint32_t i = 0; i < smax; i++){
            if(scramble[i] == a0){ vb = i; disp0 = scramble[i]; break; }
        }
        uint32_t w = (sl + 7) / 8;
        uint32_t total_bytes = mph_bytes(1, w);
        out->data = malloc(total_bytes);
        if(!out->data){ free(scramble); free(vbuf); free(ka); free(kb);
            free(tabb); free(flat_list); free(counts); free(order);
            free(tabh); free(tabq); return 0; }
        memset(out->data, 0, total_bytes);
        if(1 >= USE_SCRAMBLE){
            out->data[0] = (uint8_t)vb;
            uint32_t o = 1;
            for(uint32_t i = 0; i < 256; i++)
                for(uint32_t j = 0; j < w; j++)
                    out->data[o + i*w + j] = (scramble[i] >> (8*(w-1-j))) & 0xFF;
        } else {
            for(uint32_t j = 0; j < w; j++)
                out->data[j] = (disp0 >> (8*(w-1-j))) & 0xFF;
        }
        out->w = w; out->blen = 1; out->shift = shift; out->salt = salt;
        free(scramble); free(vbuf); free(ka); free(kb); free(tabb);
        free(flat_list); free(counts); free(order); free(tabh); free(tabq);
        return 1;
    }

    for(uint32_t attempt = 1; attempt < 1000000 && !ok; attempt++){
        salt = attempt;
        uint32_t seed = salt * 0x9e3779b9;
        for(uint32_t i = 0; i < n; i++)
            vbuf[i] = mph_lookup_k((const uint8_t*)in->keys[i], in->kl[i], seed);
        for(uint32_t i = 0; i < n; i++){
            ka[i] = (alen > 1) ? vbuf[i] >> shift : 0;
            kb[i] = (blen > 1) ? vbuf[i] & mask : 0;
        }

        memset(tabb, 0, smax * sizeof(mph_Bstuff));
        memset(counts, 0, smax * sizeof(uint32_t));

        for(uint32_t i = 0; i < n; i++) counts[kb[i]]++;

        uint32_t total_entries = 0;
        for(uint32_t i = 0; i < smax; i++){
            tabb[i].off = total_entries;
            total_entries += counts[i];
        }

        /* bucket keys by kb, and reject this salt if any bucket holds
         * two keys with the same ka (a duplicate (a,b) pair). A bucket
         * whose own keys share `a` can never be given a single scramble
         * value, so the salt is unusable and must be retried. This mirrors
         * perfect.c's inittab() distinct-(a,b) check. */
        int distinct = 1;
        for(uint32_t i = 0; i < n; i++){
            uint32_t b = kb[i];
            for(uint32_t j = 0; j < tabb[b].len; j++){
                uint32_t other = flat_list[tabb[b].off + j];
                if(ka[other] == ka[i]){ distinct = 0; break; }
            }
            if(!distinct) break;
            flat_list[tabb[b].off + tabb[b].len++] = i;
        }
        if(!distinct) continue;

        /* process buckets largest-first (descending length) */
        uint32_t nb = 0;
        for(uint32_t i = 0; i < smax; i++)
            if(tabb[i].len) order[nb++] = i;
        mph_heap_sort(tabb, order, nb);

        for(uint32_t i = 0; i < n; i++) tabh[i] = 0xFFFFFFFF;

        uint32_t tag = 1;
        ok = 1;
        for(uint32_t x = nb; x-- > 0; ){
            uint32_t b = order[x];
            if(!tabb[b].len) continue;
            uint32_t ki = flat_list[tabb[b].off];
            if(tabh[ka[ki] ^ scramble[tabb[b].val_b]] == ki) continue;
            if(!mph_augment(tabb, tabh, tabq, blen, scramble, smax, b, n, b + 1,
                            ka, kb, flat_list)){
                ok = 0;
                break;
            }
        }

        if(ok) break;

        /* only grow the table when a perfect hash is truly impossible */
        if(!ok && blen < smax){
            blen *= 2;
            if(blen > smax) blen = smax;
            mask = blen - 1;
            if(blen >= USE_SCRAMBLE) use_scramble = 1;
            limit = use_scramble ? 256 : smax;
        }
    }

    if(!ok) goto fail;

    uint32_t w = (sl + 7) / 8;   /* based on smax */
    uint32_t total_bytes = mph_bytes(blen, w);
    out->data = malloc(total_bytes);
    if(!out->data) goto fail;
    memset(out->data, 0, total_bytes);

    if(use_scramble){
        for(uint32_t b = 0; b < blen; b++) out->data[b] = (uint8_t)tabb[b].val_b;
        uint32_t o = blen;
        for(uint32_t i = 0; i < 256; i++)
            for(uint32_t j = 0; j < w; j++)
                out->data[o + i*w + j] = (scramble[i] >> (8*(w-1-j))) & 0xFF;
    } else {
        for(uint32_t b = 0; b < blen; b++){
            uint32_t disp = scramble[tabb[b].val_b];
            for(uint32_t j = 0; j < w; j++)
                out->data[b*w + j] = (disp >> (8*(w-1-j))) & 0xFF;
        }
    }
    out->w = w;
    out->blen = blen;
    out->shift = shift;
    out->salt = salt;

    free(scramble);
    free(vbuf); free(ka); free(kb); free(tabb); free(flat_list);
    free(counts); free(order); free(tabh); free(tabq);
    return 1;

fail:
    free(scramble);
    free(vbuf); free(ka); free(kb); free(tabb); free(flat_list);
    free(counts); free(order); free(tabh); free(tabq);
    if(out->data) free(out->data);
    out->data = 0;
    return 0;
}

#endif /* EFS_BUILDER */

#endif /* EFS_H */

#ifndef JMPH_H
#define JMPH_H

#if defined(__cplusplus)
extern "C" {
#endif

#include <stdint.h>
#include <stddef.h>

/* ------------------------------------------------------------------ */
/* jmph.h - single-header minimal perfect hash (MPH) builder.         */
/*                                                                     */
/* This is the minimal-perfect-hash generator used by efs (factored    */
/* out of efs.h so the implementation can be swapped / compared). It   */
/* is a port of Bob Jenkins' perfect.c (public domain,                */
/* burtleburtle.net/bob/c/perfect.c) specialised to the Jenkins        */
/* "lookup2" string hash.                                             */
/*                                                                     */
/* Two roles (mirrors the single-header style of oomph/mph.h):        */
/*   * BUILDER  (define MPH_IMPL, or rely on EFS_BUILDER from efs.h):  */
/*       builds a packed table from a set of string keys via          */
/*       jmph_build().                                                 */
/*   * CONSUMER (just include, no MPH_IMPL): sees the types and the   */
/*       prototypes; jmph_index() is available too (handy for         */
/*       self-checks). The on-disk efs reader uses its own decoder    */
/*       and does not need the builder.                               */
/*                                                                     */
/* IMPORTANT CONTRACTS:                                                */
/*   * Input keys MUST BE UNIQUE. Passing duplicate keys is a         */
/*     contract violation; jmph_build() detects it and returns 0      */
/*     (with a message on stderr). A minimal perfect hash is only     */
/*     well-defined for a set of distinct keys.                       */
/*   * jmph_index() returns 0-BASED indices in [0, n-1]. To obtain a  */
/*     1-based rank (as mph.h does), add 1 -- but only when the       */
/*     result is < n.                                                 */
/*                                                                     */
/* Packed table layout (read back by the consumer with the same       */
/* blen/shift/salt/w):                                                */
/*   if blen < USE_SCRAMBLE:                                          */
/*       blen*w bytes; bucket b holds w-byte big-endian `disp`,       */
/*       disp = scramble[val_b].                                      */
/*   else:                                                            */
/*       1 byte per bucket holding val_b, followed by 256*w bytes     */
/*       holding scramble[] (so disp = scramble[data[b]]).            */
/*   idx(key) = (hash >> shift) ^ disp,  hash = lookup2(key, salt).   */
/* ------------------------------------------------------------------ */

#ifndef JMPH_API
#define JMPH_API
#endif

/* When the table needs the scramble table (large blen), use the byte
 * table layout. Kept at 4096 to match the efs on-disk consumer. */
#ifndef USE_SCRAMBLE
#define USE_SCRAMBLE 4096
#endif

/* ------------------------------------------------------------------ */
/* Public types and API (visible to every includer).                  */
/* ------------------------------------------------------------------ */

/* Build input: n keys, each a NUL-terminated string with length kl[i]
 * (< 256). keys/kl are caller-owned; the builder does not free them. */
struct jmph_in {
    uint32_t  n;     /* number of keys */
    const char **keys; /* key strings (NUL-terminated) */
    uint32_t *kl;    /* key lengths */
};

/* Build output: a packed MPH table. Caller frees data with free(). */
struct jmph_out {
    uint32_t  blen;  /* table size (power of two, >= 2) */
    uint32_t  shift; /* a = hash >> shift */
    uint32_t  salt;  /* seed passed to the string hash */
    uint32_t  w;     /* bytes per scramble/disp entry (1..4) */
    uint8_t  *data;  /* packed table; free() with free() */
};

/* Exact serialized size of a built table (blen/w). */
JMPH_API uint32_t jmph_bytes(uint32_t blen, uint32_t w);

/* Map `key` (klen bytes) to its unique slot in [0, blen-1], which is
 * [0, n-1] for a minimal perfect hash. This is the exact inverse of
 * the consumer-side decode and is what callers should use instead of
 * re-implementing it. */
JMPH_API uint32_t jmph_index(const struct jmph_out *mo, const uint8_t *key, uint32_t klen);

/* Build a minimal perfect hash for the given (unique) keys.
 * Returns 1 on success (out->data allocated; free with free()),
 * 0 on failure (out->data is NULL). Duplicate keys yield 0. */
JMPH_API int jmph_build(const struct jmph_in *in, struct jmph_out *out);

/* ------------------------------------------------------------------ */
/* Implementation (builder path only).                               */
/* ------------------------------------------------------------------ */

#if defined(MPH_IMPL) || defined(EFS_BUILDER)

#ifndef JMPH_INTERNAL
#define JMPH_INTERNAL static
#endif

#include <stdlib.h>
#include <stdio.h>
#include <string.h>

JMPH_INTERNAL uint32_t jmph_mylog2(uint32_t v){
    uint32_t i;
    for(i = 0; ((uint32_t)1 << i) < v; i++)
        ;
    return i;
}

/* Jenkins "lookup2" mix; must match the hash used by the consumer. */
#define jmph_mix(a,b,c) \
{ a-=b;a-=c;a^=c>>13;b-=c;b-=a;b^=a<<8;c-=a;c-=b;c^=b>>13; \
  a-=b;a-=c;a^=c>>12;b-=c;b-=a;b^=a<<16;c-=a;c-=b;c^=b>>5; \
  a-=b;a-=c;a^=c>>3;b-=c;b-=a;b^=a<<10;c-=a;c-=b;c^=b>>15; }

/* Hash a key to a 32-bit value; the consumer uses the identical mix so
 * the same salt reproduces the same hash on the read side. */
JMPH_INTERNAL uint32_t jmph_lookup_k(const uint8_t *k, uint32_t len, uint32_t level){
    uint32_t a, b, c, o = len;
    a = b = 0x9e3779b9;
    c = level;
    while(len >= 12){
        a += (k[0]|k[1]<<8|k[2]<<16|k[3]<<24);
        b += (k[4]|k[5]<<8|k[6]<<16|k[7]<<24);
        c += (k[8]|k[9]<<8|k[10]<<16|k[11]<<24);
        jmph_mix(a,b,c);
        k += 12;
        len -= 12;
    }
    c += o;
    switch(len){
    case 11:c+=k[10]<<24; case 10:c+=k[9]<<16; case 9:c+=k[8]<<8;
    case 8:b+=k[7]<<24; case 7:b+=k[6]<<16; case 6:b+=k[5]<<8; case 5:b+=k[4];
    case 4:a+=k[3]<<24; case 3:a+=k[2]<<16; case 2:a+=k[1]<<8; case 1:a+=k[0];
    }
    jmph_mix(a,b,c);
    return c;
}

/* permutation p(x) of 0..(1<<nbits)-1, used to fill scramble[] */
JMPH_INTERNAL uint32_t jmph_permute(uint32_t x, uint32_t nbits){
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

/* Read a big-endian w-byte word (w <= 4). */
JMPH_INTERNAL uint32_t jmph_tab_load(const uint8_t *p, uint32_t w){
    uint32_t v = 0;
    while(w--) v = (v << 8) | *p++;
    return v;
}

/* Per-bucket working state for the matcher. */
typedef struct {
    uint32_t val_b;   /* scramble[] index assigned to this bucket */
    uint32_t len;     /* number of keys sharing this bucket */
    uint32_t off;     /* base offset into flat_list */
    uint32_t water;   /* traversal high-water mark for the matching */
} jmph_Bstuff;

/* Queue node used by the augmenting-path search (a spanning tree). */
typedef struct {
    uint32_t b_q;        /* bucket index occupying this queue node */
    uint32_t parent_q;   /* queue position of the parent bucket */
    uint32_t newval_q;   /* val_b for the parent to reach this node */
    uint32_t oldval_q;   /* parent's previous val_b (for rollback) */
} jmph_Qstuff;

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
JMPH_INTERNAL int jmph_apply(jmph_Bstuff *tabb, uint32_t *tabh, jmph_Qstuff *tabq,
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
                jmph_apply(tabb, tabh, tabq, n, tail, 1, ka, kb, flat_list, scramble);
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
JMPH_INTERNAL int jmph_augment(jmph_Bstuff *tabb, uint32_t *tabh, jmph_Qstuff *tabq,
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
                if(jmph_apply(tabb, tabh, tabq, n, tail, 0,
                             ka, kb, flat_list, scramble))
                    return 1;
                --tail;                              /* couldn't use this i */
            }
        }
    }
    return 0;
}

/* ascending order by bucket length (caller iterates reversed => descending) */
JMPH_INTERNAL void jmph_heap_sift(jmph_Bstuff *tabb, uint32_t *order, uint32_t n, uint32_t start){
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

JMPH_INTERNAL void jmph_heap_sort(jmph_Bstuff *tabb, uint32_t *order, uint32_t n){
    if(n == 0) return;
    for(uint32_t i = n/2; i-- > 0; )
        jmph_heap_sift(tabb, order, n, i);
    for(uint32_t end = n-1; end > 0; end--){
        uint32_t t = order[0]; order[0] = order[end]; order[end] = t;
        jmph_heap_sift(tabb, order, end, 0);
    }
}

/*
 * Guess initial alen/blen for a minimal perfect hash, matching
 * perfect.c's initalen() for the slow/high-compaction settings.
 */
JMPH_INTERNAL void jmph_initalen(uint32_t n, uint32_t smax, uint32_t *alen, uint32_t *blen){
    uint32_t sl = jmph_mylog2(smax);
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

JMPH_API uint32_t jmph_bytes(uint32_t blen, uint32_t w){
    return (blen >= USE_SCRAMBLE) ? (blen + 256 * w) : (blen * w);
}

JMPH_API uint32_t jmph_index(const struct jmph_out *mo, const uint8_t *key, uint32_t klen){
    uint32_t v = jmph_lookup_k(key, klen, mo->salt * 0x9e3779b9);
    uint32_t a = v >> mo->shift;
    uint32_t b = v & (mo->blen - 1);
    uint32_t w = mo->w;
    uint32_t disp;
    if(mo->blen >= USE_SCRAMBLE){
        uint32_t idx = mo->data[b];
        disp = jmph_tab_load(mo->data + mo->blen + idx * w, w);
    } else {
        disp = jmph_tab_load(mo->data + b * w, w);
    }
    return a ^ disp;
}

JMPH_API int jmph_build(const struct jmph_in *in, struct jmph_out *out){
    *out = (struct jmph_out){0};
    const uint32_t n = in->n;

    if(!n){
        out->blen = 1;
        out->shift = 31;
        out->salt = 0;
        out->w = 1;
        out->data = calloc(1, jmph_bytes(1, 1));
        return out->data ? 1 : 0;
    }

    uint32_t smax = 1;
    while(smax < n) smax <<= 1;
    if(smax < 2) smax = 2;

    uint32_t alen, blen;
    jmph_initalen(n, smax, &alen, &blen);

    uint32_t sl = jmph_mylog2(smax);
    uint32_t shift = (alen > 1) ? 32 - jmph_mylog2(alen) : 0;
    uint32_t mask = blen - 1;
    int use_scramble = (blen >= USE_SCRAMBLE);

    uint32_t *scramble = malloc(smax * sizeof(uint32_t));
    uint32_t *vbuf   = malloc(n * sizeof(uint32_t));
    uint32_t *ka     = malloc(n * sizeof(uint32_t));
    uint32_t *kb     = malloc(n * sizeof(uint32_t));
    jmph_Bstuff *tabb = calloc(smax, sizeof(jmph_Bstuff));
    uint32_t *flat_list = malloc(n * sizeof(uint32_t));
    uint32_t *counts = calloc(smax, sizeof(uint32_t));
    uint32_t *order  = malloc(smax * sizeof(uint32_t));
    uint32_t *tabh   = malloc(n * sizeof(uint32_t));
    jmph_Qstuff *tabq = malloc((smax + 1) * sizeof(jmph_Qstuff));
    if(!scramble || !vbuf || !ka || !kb || !tabb || !flat_list || !counts || !order || !tabh || !tabq){
        perror("malloc");
        goto fail;
    }

    /* scramble[] depends only on smax, so build it once for all trials. */
    for(uint32_t i = 0; i < smax; i++) scramble[i] = jmph_permute(i, sl);

    int ok = 0;
    uint32_t salt = 0;
    uint32_t limit = use_scramble ? 256 : smax;

    if(n <= 1){
        /* A single key trivially has a unique slot. Pick a salt, then store
         * a table of blen=1 with disp == a so that idx = a^disp == 0. */
        salt = 1;
        uint32_t seed = salt * 0x9e3779b9;
        uint32_t v = jmph_lookup_k((const uint8_t*)in->keys[0], in->kl[0], seed);
        uint32_t a0 = (alen > 1) ? v >> shift : 0;
        uint32_t vb = 0, disp0 = 0;
        for(uint32_t i = 0; i < smax; i++){
            if(scramble[i] == a0){ vb = i; disp0 = scramble[i]; break; }
        }
        uint32_t w = (sl + 7) / 8;
        uint32_t total_bytes = jmph_bytes(1, w);
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
            vbuf[i] = jmph_lookup_k((const uint8_t*)in->keys[i], in->kl[i], seed);
        for(uint32_t i = 0; i < n; i++){
            ka[i] = (alen > 1) ? vbuf[i] >> shift : 0;
            kb[i] = (blen > 1) ? vbuf[i] & mask : 0;
        }

        memset(tabb, 0, smax * sizeof(jmph_Bstuff));
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
         * value, so the salt is unusable and must be retried. This also
         * catches genuinely duplicate keys (ka+kb identical). */
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
        if(!distinct){
            /* Two keys hashing to the same (a,b) at every salt mean the input
             * contains duplicates, which is a contract violation. */
            if(attempt >= 1000000 - 1){
                fprintf(stderr, "jmph_build: duplicate (or colliding) keys detected; "
                                "input must be unique\n");
            }
            continue;
        }

        /* process buckets largest-first (descending length) */
        uint32_t nb = 0;
        for(uint32_t i = 0; i < smax; i++)
            if(tabb[i].len) order[nb++] = i;
        jmph_heap_sort(tabb, order, nb);

        for(uint32_t i = 0; i < n; i++) tabh[i] = 0xFFFFFFFF;

        ok = 1;
        for(uint32_t x = nb; x-- > 0; ){
            uint32_t b = order[x];
            if(!tabb[b].len) continue;
            uint32_t ki = flat_list[tabb[b].off];
            if(tabh[ka[ki] ^ scramble[tabb[b].val_b]] == ki) continue;
            if(!jmph_augment(tabb, tabh, tabq, blen, scramble, smax, b, n, b + 1,
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
    uint32_t total_bytes = jmph_bytes(blen, w);
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

#endif /* MPH_IMPL || EFS_BUILDER */

#if defined(__cplusplus)
}
#endif

#endif /* JMPH_H */

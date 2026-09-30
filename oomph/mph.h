#ifndef MPH_H
#define MPH_H

#if defined(__cplusplus)
extern "C" {
#endif

#include <stdint.h>
#include <stddef.h>
#include <inttypes.h>

/* ------------------------------------------------------------------ */
/* Single-header minimal perfect hash (BBHash / BoomPHF, arXiv:1702.03154).
 *
 * IMPORTANT CONTRACTS:
 *   * The input to mph_build() MUST BE UNIQUE. Passing duplicate (non-unique)
 *     keys is a contract violation: mph_build() detects it and returns NULL
 *     (with a message on stderr). A minimal perfect hash is only well-defined
 *     for a set of distinct keys.
 *   * mph_lookup() returns 1-BASED indices. 0 means "key not found". To obtain
 *     a 0-based index, subtract 1 -- but ONLY when the result is non-zero:
 *         uint64_t r = mph_lookup(...);
 *         size_t idx = (r == 0) ? SOME_SENTINEL : (size_t)(r - 1);
 *
 * Two roles:
 *   * PRODUCER (define MPH_IMPL in exactly one TU): builds a packed uint8_t
 *     blob from a set of string keys via mph_build().
 *   * CONSUMER (just include): looks a key up in a blob via mph_lookup().
 *
 * The blob (little-endian, no header, endian-agnostic on every host):
 *     [ u8  num_levels ]
 *     [ u32 seed ]                        // baked-in hash seed
 *     per level:
 *         [ u32 bitsize ]                 // in bits
 *         [ (bitsize+63)/64 * 8 bytes ]   bv  bitvector (u64 words, LE)
 *         [ ((bitsize+511)/512+1)*8 ]     rank table (u64 words, LE)
 * size/rank_size are recomputed by the decoder from bitsize, so they are not
 * stored. n (number of keys) lives OUTSIDE the blob, in the consumer's code.
 * ------------------------------------------------------------------ */

#ifndef MPH_API
#define MPH_API
#endif

/* Consumer-facing lookup. Returns 0 on miss, else the 1-indexed rank. */
MPH_API uint64_t mph_lookup(const uint8_t *table, size_t tablen,
                            const char *key, size_t keylen);

#ifdef MPH_IMPL

#define MPH_INTERNAL static

#ifndef UINT_MAX
#define UINT_MAX 0xffffffffU
#endif

#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>

/* tlist.h is used only on the builder path. */
#define TLIST_API static
#define TLIST_IMPL
#include "tlist.h"

/* ------------------------------------------------------------------ */
/* Popcount: use the platform builtin when available, else footwork.  */
/* ------------------------------------------------------------------ */

#if defined(__GNUC__) && __GNUC__ >= 3
#define HAVE_POPCOUNTLL
#endif

#ifdef HAVE_POPCOUNTLL
#define mph_popcountll(x) __builtin_popcountll(x)
#else
MPH_INTERNAL int mph_popcountll(uint64_t x) {
    x = x - ((x >> 1) & 0x5555555555555555ULL);
    x = (x & 0x3333333333333333ULL) + ((x >> 2) & 0x3333333333333333ULL);
    x = (x + (x >> 4)) & 0x0f0f0f0f0f0f0f0fULL;
    return (int)((x * 0x0101010101010101ULL) >> 56);
}
#endif

/* ------------------------------------------------------------------ */
/* Hash / mix helpers (mph_foo_bar snake_case).                       */
/* ------------------------------------------------------------------ */

MPH_INTERNAL uint32_t mph_rotl(uint32_t v, uint32_t r) {
    return (r == 0) ? v : ((v << r) | (v >> (32 - r)));
}

MPH_INTERNAL uint64_t mph_mix64(uint64_t x) {
    x ^= x >> 12;
    x ^= x << 25;
    x ^= x >> 27;
    return x * 2685821657736338717ULL;
}

/* Seeded FNV-1a 64-bit, finalized with mph_mix64. The seed lets the builder
 * retry with a different mapping if a collision is detected. */
MPH_INTERNAL uint64_t mph_hash_string(const char *s, size_t len, uint64_t seed) {
    uint64_t h = 1469598103934665603ULL ^ seed;
    for (size_t i = 0; i < len; i++) {
        h ^= (unsigned char)s[i];
        h *= 1099511628211ULL;
    }
    return mph_mix64(h);
}

/* ------------------------------------------------------------------ */
/* BBHash engine (verbatim structure from soomph.c).                  */
/* ------------------------------------------------------------------ */

typedef struct {
    uint64_t *bits;
    unsigned size;
    unsigned bitsize;
} mph_bitvector_t;

typedef struct {
    mph_bitvector_t **bitvectors;
    uint64_t **ranks;
    unsigned num_bitvectors;
    unsigned *level_counts;
    unsigned total_keys;
} mph_boomphf_t;

MPH_INTERNAL mph_bitvector_t *mph_new_bv(unsigned bitsize) {
    mph_bitvector_t *b = calloc(1, sizeof(mph_bitvector_t));
    b->bitsize = bitsize;
    b->size = (bitsize + 63) / 64;
    b->bits = calloc((size_t)b->size, sizeof(uint64_t));
    return b->bits ? b : (free(b), NULL);
}

MPH_INTERNAL unsigned mph_bv_get(mph_bitvector_t *bv, unsigned bit) {
    return (unsigned)((bv->bits[bit / 64] >> (bit % 64)) & 1);
}

MPH_INTERNAL void mph_bv_set(mph_bitvector_t *bv, unsigned bit) {
    bv->bits[bit / 64] |= (1ULL << (bit % 64));
}

#define MPH_GAMMA 2.0

MPH_INTERNAL mph_boomphf_t *mph_new_boomphf(double gamma, uint64_t *keys,
                                            unsigned num_keys) {
    mph_boomphf_t *h = calloc(1, sizeof(mph_boomphf_t));
    if (!h) return NULL;

    h->total_keys = num_keys;
    unsigned level = 0;
    unsigned remaining = num_keys;
    uint64_t *current_keys = keys;

    while (remaining > 0) {
        unsigned bitsize = ((unsigned)(gamma * remaining) + 63) & ~63U;

        mph_bitvector_t *A = mph_new_bv(bitsize);
        mph_bitvector_t *collide = mph_new_bv(bitsize);
        uint64_t *redo = malloc((size_t)remaining * sizeof(uint64_t));
        unsigned redo_count = 0;

        for (unsigned i = 0; i < remaining; i++) {
            uint64_t v = current_keys[i];
            uint64_t hash = mph_mix64(v);
            uint32_t h1 = (uint32_t)hash, h2 = (uint32_t)(hash >> 32);
            unsigned idx = (h1 ^ mph_rotl(h2, level)) % bitsize;
            if (mph_bv_get(collide, idx)) continue;
            if (mph_bv_get(A, idx)) { mph_bv_set(collide, idx); continue; }
            mph_bv_set(A, idx);
        }

        mph_bitvector_t *bv = mph_new_bv(bitsize);
        h->level_counts = realloc(h->level_counts,
                                  (size_t)(h->num_bitvectors + 1) * sizeof(unsigned));
        unsigned stored_count = 0;
        for (unsigned i = 0; i < remaining; i++) {
            uint64_t v = current_keys[i];
            uint64_t hash = mph_mix64(v);
            uint32_t h1 = (uint32_t)hash, h2 = (uint32_t)(hash >> 32);
            unsigned idx = (h1 ^ mph_rotl(h2, level)) % bitsize;
            if (mph_bv_get(collide, idx)) {
                redo[redo_count++] = v;
                continue;
            }
            mph_bv_set(bv, idx);
            stored_count++;
        }
        h->level_counts[h->num_bitvectors] = stored_count;

        h->num_bitvectors++;
        h->bitvectors = realloc(h->bitvectors,
                                (size_t)h->num_bitvectors * sizeof(mph_bitvector_t *));
        h->bitvectors[h->num_bitvectors - 1] = bv;

        free(A->bits); free(A);
        free(collide->bits); free(collide);

        uint64_t *to_free = current_keys;
        current_keys = redo;
        /* `keys` is the caller's buffer (passed in on level 0); only the
         * per-level `redo` arrays are ours to free. Freeing the caller's
         * array here would double-free when mph_build() frees it again. */
        if (to_free != keys) free(to_free);

        remaining = redo_count;
        level++;
    }

    h->ranks = malloc((size_t)h->num_bitvectors * sizeof(uint64_t *));
    unsigned prev_total = 0;
    for (unsigned i = 0; i < h->num_bitvectors; i++) {
        unsigned rank_size = (h->bitvectors[i]->size + 7) / 8 + 1;
        h->ranks[i] = calloc((size_t)rank_size, sizeof(uint64_t));
        uint64_t pop = 0;
        for (unsigned j = 0; j < h->bitvectors[i]->size; j++) {
            if ((j % 8) == 0) h->ranks[i][j / 8] = pop + prev_total;
            pop += mph_popcountll(h->bitvectors[i]->bits[j]);
        }
        h->ranks[i][rank_size - 1] = pop + prev_total;
        prev_total += pop;
    }

    return h;
}

MPH_INTERNAL void mph_free_boomphf(mph_boomphf_t *h) {
    if (!h) return;
    for (unsigned i = 0; i < h->num_bitvectors; i++) {
        free(h->bitvectors[i]->bits);
        free(h->bitvectors[i]);
        free(h->ranks[i]);
    }
    free(h->bitvectors);
    free(h->ranks);
    free(h->level_counts);
    free(h);
}

/* ------------------------------------------------------------------ */
/* Sorted key collection (backed by tlist).                           */
/* ------------------------------------------------------------------ */

typedef struct {
    uint64_t hash;
    char *key;
} mph_key_node_t;

MPH_INTERNAL int mph_key_node_cmp(const void *a, const void *b) {
    const mph_key_node_t *ka = a, *kb = b;
    if (ka->hash < kb->hash) return -1;
    if (ka->hash > kb->hash) return 1;
    return 0;
}

MPH_INTERNAL int mph_key_list_insert(struct tlist *l, uint64_t hash, char *key) {
    size_t n = tlist_getsize(l);
    size_t lo = 0, hi = n;
    while (lo < hi) {
        size_t mid = (lo & hi) + ((lo ^ hi) >> 1);
        const mph_key_node_t *cur = tlist_get(l, mid);
        if (cur->hash < hash) lo = mid + 1;
        else hi = mid;
    }
    if (lo < n) {
        const mph_key_node_t *cur = tlist_get(l, lo);
        if (cur->hash == hash) {
            if (strcmp(cur->key, key) == 0) {
                /* Non-unique key (contract violation): signal failure so the
                 * builder returns NULL instead of silently producing a
                 * non-perfect hash. */
                return -2;
            }
            return 2;
        }
    }
    if (lo > 0) {
        const mph_key_node_t *prev = tlist_get(l, lo - 1);
        if (prev->hash == hash) {
            if (strcmp(prev->key, key) == 0) {
                return -2;
            }
            return 2;
        }
    }
    mph_key_node_t node = { hash, key };
    return tlist_insert_sorted(l, &node, mph_key_node_cmp) ? 0 : -1;
}

MPH_INTERNAL uint64_t mph_build_keys(const char * const *lines, size_t nlines,
                                     uint64_t **out_keys, size_t *out_n) {
    uint64_t seed = 0;
    const uint64_t max_tries = 1u << 20;

    for (;;) {
        struct tlist *lst = tlist_new(sizeof(mph_key_node_t));
        if (!lst) { fprintf(stderr, "fatal: out of memory\n"); exit(1); }

        unsigned collision = 0;
        for (size_t i = 0; i < nlines; i++) {
            size_t len = strlen(lines[i]);
            if (len == 0) continue;
            uint64_t h = mph_hash_string(lines[i], len, seed);
            int r = mph_key_list_insert(lst, h, (char *)lines[i]);
            if (r == 2) { collision = 1; break; }
            if (r == -2) { tlist_free(lst); return 0; } /* non-unique key */
            else if (r < 0) { fprintf(stderr, "fatal: out of memory\n"); exit(1); }
        }

        if (!collision) {
            *out_n = tlist_getsize(lst);
            *out_keys = malloc(*out_n * sizeof(uint64_t));
            for (size_t i = 0; i < *out_n; i++) {
                const mph_key_node_t *node = tlist_get(lst, i);
                (*out_keys)[i] = node->hash;
            }
            tlist_free(lst);
            return seed;
        }

        tlist_free(lst);
        seed++;
        if (seed >= max_tries) {
            fprintf(stderr, "fatal: could not find a collision-free seed\n");
            exit(1);
        }
        fprintf(stderr, "hash collision at seed %" PRIu64 ", retrying...\n", seed);
    }
}

/* ------------------------------------------------------------------ */
/* Blob serialization (into a malloc'd u8 buffer).                    */
/* ------------------------------------------------------------------ */

MPH_INTERNAL void mph_emit_u32(uint8_t **p, uint32_t v) {
    for (unsigned i = 0; i < 4; i++) { **p = (uint8_t)(v & 0xff); (*p)++; v >>= 8; }
}

MPH_INTERNAL void mph_emit_u64(uint8_t **p, uint64_t v) {
    for (unsigned i = 0; i < 8; i++) { **p = (uint8_t)(v & 0xff); (*p)++; v >>= 8; }
}

/* Compute the exact serialized blob size for a built boomphf. */
MPH_INTERNAL size_t mph_blob_size(mph_boomphf_t *h) {
    size_t sz = 1 /* num_levels u8 */ + 4 /* seed u32 */;
    for (unsigned i = 0; i < h->num_bitvectors; i++) {
        unsigned bitsize = h->bitvectors[i]->bitsize;
        unsigned size = (bitsize + 63) / 64;
        unsigned rank_size = (bitsize + 511) / 512 + 1;
        sz += 4 /* bitsize u32 */
            + (size_t)size * 8
            + (size_t)rank_size * 8;
    }
    return sz;
}

MPH_API uint8_t *mph_build(const char * const *keys, size_t n_keys,
                           size_t *out_len) {
    *out_len = 0;

    uint64_t *hkeys = NULL;
    size_t nkeys = 0;
    uint64_t seed = mph_build_keys(keys, n_keys, &hkeys, &nkeys);
    if (nkeys == 0) {
        free(hkeys);
        /* nkeys==0 with no OOM means a duplicate/non-unique key was found:
         * the builder contract requires unique keys, so fail loudly. */
        fprintf(stderr, "mph_build: duplicate/non-unique key detected; "
                        "input must be unique\n");
        return NULL;
    }

    mph_boomphf_t *h = mph_new_boomphf(MPH_GAMMA, hkeys, (unsigned)nkeys);
    free(hkeys);
    if (!h) return NULL;

    size_t sz = mph_blob_size(h);
    uint8_t *blob = malloc(sz);
    if (!blob) { mph_free_boomphf(h); return NULL; }

    uint8_t *p = blob;
    *p++ = (uint8_t)(h->num_bitvectors & 0xff); /* num_levels (u8) */
    mph_emit_u32(&p, (uint32_t)seed);           /* seed (u32, LE) */
    for (unsigned i = 0; i < h->num_bitvectors; i++) {
        unsigned bitsize = h->bitvectors[i]->bitsize;
        unsigned size = (bitsize + 63) / 64;
        unsigned rank_size = (bitsize + 511) / 512 + 1;
        mph_emit_u32(&p, (uint32_t)bitsize);
        for (unsigned j = 0; j < size; j++) mph_emit_u64(&p, h->bitvectors[i]->bits[j]);
        for (unsigned j = 0; j < rank_size; j++) mph_emit_u64(&p, h->ranks[i][j]);
    }
    mph_free_boomphf(h);

    *out_len = sz;
    return blob;
}

/* ------------------------------------------------------------------ */
/* Generic decoder (also used by the consumer-facing mph_lookup).     */
/* ------------------------------------------------------------------ */

MPH_INTERNAL uint8_t mph_rd_u8(const uint8_t **p) { return *(*p)++; }

MPH_INTERNAL uint32_t mph_rd_u32(const uint8_t **p) {
    uint32_t v = 0;
    for (unsigned i = 0; i < 4; i++) v = (v << 8) | mph_rd_u8(p);
    return v;
}

MPH_INTERNAL uint64_t mph_rd_u64(const uint8_t **p) {
    uint64_t v = 0;
    for (unsigned i = 0; i < 8; i++) v = (v << 8) | mph_rd_u8(p);
    return v;
}

/* Read a little-endian u64 from an arbitrary offset (no cursor advance). */
MPH_INTERNAL uint64_t mph_rd_u64_at(const uint8_t *p) {
    uint64_t v = 0;
    for (unsigned i = 0; i < 8; i++) v = (v << 8) | p[i];
    return v;
}

#endif /* MPH_IMPL */

/* Consumer-side decoder. Available to all includers (needs MPH_IMPL only
 * for the builder); we keep it outside the IMPL guard so consumers have it
 * without pulling the whole engine. */
#if defined(MPH_IMPL)
/* defined above inside MPH_IMPL */
#else
#include <stddef.h>
#include <stdint.h>
#include <string.h>

static inline uint32_t mph_rotl(uint32_t v, uint32_t r) {
    return (r == 0) ? v : ((v << r) | (v >> (32 - r)));
}
static inline uint64_t mph_mix64(uint64_t x) {
    x ^= x >> 12; x ^= x << 25; x ^= x >> 27;
    return x * 2685821657736338717ULL;
}
static inline uint64_t mph_hash_string(const char *s, size_t len, uint64_t seed) {
    uint64_t h = 1469598103934665603ULL ^ seed;
    for (size_t i = 0; i < len; i++) {
        h ^= (unsigned char)s[i];
        h *= 1099511628211ULL;
    }
    return mph_mix64(h);
}
static inline uint8_t  mph_rd_u8 (const uint8_t **p) { return *(*p)++; }
static inline uint32_t mph_rd_u32(const uint8_t **p) {
    uint32_t v = 0; for (unsigned i = 0; i < 4; i++) v = (v << 8) | mph_rd_u8(p);
    return v;
}
static inline uint64_t mph_rd_u64(const uint8_t **p) {
    uint64_t v = 0; for (unsigned i = 0; i < 8; i++) v = (v << 8) | mph_rd_u8(p);
    return v;
}
static inline uint64_t mph_rd_u64_at(const uint8_t *p) {
    uint64_t v = 0; for (unsigned i = 0; i < 8; i++) v = (v << 8) | p[i];
    return v;
}
#endif

MPH_API uint64_t mph_lookup(const uint8_t *table, size_t tablen,
                            const char *key, size_t keylen) {
    if (!table || tablen < 5) return 0; /* need num_levels(u8)+seed(u32) at least */
    const uint8_t *p = table;
    uint8_t num_levels = mph_rd_u8(&p);
    uint64_t seed = mph_rd_u32(&p);
    uint64_t h = mph_hash_string(key, keylen, seed);
    uint32_t h1 = (uint32_t)h, h2 = (uint32_t)(h >> 32);

    for (uint8_t level = 0; level < num_levels; level++) {
        if ((size_t)(p - table) + 4 > tablen) return 0;
        uint32_t bitsize = mph_rd_u32(&p);
        uint32_t size = (bitsize + 63) / 64;
        uint32_t rank_size = (bitsize + 511) / 512 + 1;
        uint32_t idx = (h1 ^ mph_rotl(h2, level)) % bitsize;

        if ((size_t)(p - table) + (size_t)size * 8 + (size_t)rank_size * 8 > tablen)
            return 0;

        const uint8_t *bp = p + (size_t)(idx >> 6) * 8;
        uint64_t bvword = mph_rd_u64_at(bp);
        if (((bvword >> (idx & 63)) & 1) == 0) {
            p += (size_t)size * 8 + (size_t)rank_size * 8;
            continue;
        }

        uint64_t rank = mph_rd_u64_at(p + (size_t)size * 8 + (size_t)(idx / 512) * 8);
        uint32_t word = idx / 64;
        uint64_t w = mph_rd_u64_at(p + (size_t)word * 8);
        for (uint32_t j = (idx / 512) * 8; j < word; j++) {
            uint64_t jw = mph_rd_u64_at(p + (size_t)j * 8);
            rank += mph_popcountll(jw);
        }
        if ((idx & 63) != 0)
            rank += mph_popcountll(w << (64 - (idx & 63)));
        return rank + 1;
    }
    return 0;
}

#if defined(__cplusplus)
}
#endif

#endif /* MPH_H */

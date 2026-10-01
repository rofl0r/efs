#ifndef BBHASH_H
#define BBHASH_H

/*
 * bbhash.h - single-header minimal perfect hash (BBHash / BoomPHF,
 * arXiv:1702.03154), with the unified MPH API (see UNIVERSAL-API-REVISED.md).
 *
 * This is the oomph/mph.h library moved into the main directory and made
 * self-contained (the treap from oomph/tlist.h is inlined below, on the
 * builder path only). The oomph/ copy is kept as the historical source.
 *
 * Roles (single-header convention):
 *   * PRODUCER : define MPH_IMPL (or EFS_BUILDER) in exactly one TU ->
 *                mph_build() / mph_build_u() + the engine.
 *   * CONSUMER : just include -> mph_lookup() / mph_index_p() / mph_bytes().
 *
 * On-disk byte order is BIG-ENDIAN (matching jmph's table storage, so both
 * MPH implementations in this repo share one byte order). Every multi-byte
 * value is read/written byte-by-byte, so the layout is host-endian agnostic.
 *
 * Unified API (shared shape with jmph.h):
 *   struct mph_in  { uint32_t n; const char *const *keys; const uint32_t *kl; }
 *   struct mph_out { uint8_t *data; uint32_t len, blen, shift, salt, w; }
 *   int      mph_build_u(const struct mph_in *, struct mph_out *);
 *   uint32_t mph_index_p(tab, blen, shift, salt, w, key, klen);   // 0-based
 *   uint32_t mph_bytes(levels, table_len);  // serialized size
 *
 * For BBHash the four consumer params map as: blen = num_levels, salt = hash
 * seed, w = serialized table length, shift = 0 (unused). BBHash levels are
 * sized from the actual collision counts, so the table length is stored in w.
 *
 * Standalone API (preserved): mph_build(keys, n, &len) -> blob, and
 * mph_lookup(blob, len, key, keylen) -> 1-based rank (0 = not found).
 */


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
 * The blob (big-endian throughout, no header, endian-agnostic on every
 * host — every multi-byte value is read/written byte-by-byte with
 * shift-and-or, so the layout is identical on LE and BE hosts. Big-endian
 * matches jmph's table storage, so both MPH implementations in this repo
 * share one on-disk byte order):
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

/* MPH_DEF controls the linkage of the functions *defined* in this header.
 * The default `static inline` makes the header safe to include from several
 * TUs that later link together (e.g. efstest.o + efstest_builder.o); a
 * standalone single-TU tool may `#define MPH_DEF extern` to export them. */
#ifndef MPH_DEF
#define MPH_DEF static inline
#endif

/* Consumer-facing lookup. Returns 0 on miss, else the 1-indexed rank. */
MPH_DEF uint64_t mph_lookup(const uint8_t *table, size_t tablen,
                            const char *key, size_t keylen);

/* The builder (BBHash engine + serializer) is compiled under MPH_IMPL.
 * EFS_BUILDER is honoured as an alias so the EFS builder TU (which defines
 * EFS_BUILDER, not MPH_IMPL, to stay algorithm-agnostic) also gets it. */
#if defined(MPH_IMPL) || defined(EFS_BUILDER)

#define MPH_INTERNAL static

#ifndef UINT_MAX
#define UINT_MAX 0xffffffffU
#endif

#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>

/* ---- tlist (treap) ------------------------------------------------
 * Inlined from oomph/tlist.h so this header is self-contained. It is used
 * only on the builder path (sorted key collection during the collision-free
 * build), so compile its implementation here (we are already inside the
 * MPH_IMPL/EFS_BUILDER builder section). */
#ifndef TLIST_API
#define TLIST_API static
#endif
#define TLIST_IMPL
typedef struct tlist tlist;

/* Public API declarations. When TLIST_API is not defined, these get plain
 * (extern) linkage so a consumer including this header only sees the prototypes.
 * Definitions are emitted only when TLIST_IMPL is defined (in exactly one TU). */
#ifndef TLIST_API
#define TLIST_API extern
#endif

TLIST_API struct tlist *tlist_new(unsigned itemsize);
TLIST_API size_t tlist_getsize(struct tlist *t);
TLIST_API void *tlist_get(struct tlist *t, size_t idx);
TLIST_API int tlist_insert(struct tlist *t, size_t idx, void *value);
TLIST_API int tlist_insert_sorted(struct tlist *t, void *value,
				  int (*cmp)(const void *, const void *));
TLIST_API int tlist_delete(struct tlist *t, size_t idx);
TLIST_API void tlist_free_items(struct tlist *t);
TLIST_API void *tlist_free(struct tlist *t);

#ifdef TLIST_IMPL

#define TLIST_INTERNAL static

#ifndef UINT_MAX
#define UINT_MAX 0xffffffffU
#endif

#include <stdlib.h>
#include <string.h>

TLIST_INTERNAL int tlist_mrand(unsigned *seed)
{
	return ((*seed =
		 (*seed + 1) * 1103515245 + 12345 - 1) + 1) & 0x7fffffff;
}

typedef struct item *pitem;
struct item {
	unsigned prior, cnt;
	pitem l, r;
};

TLIST_INTERNAL unsigned tlist_cnt(pitem it)
{
	return it ? it->cnt : 0;
}

TLIST_INTERNAL void tlist_upd_cnt(pitem it)
{
	if (it)
		it->cnt = tlist_cnt(it->l) + tlist_cnt(it->r) + 1;
}

TLIST_INTERNAL void tlist_merge(pitem * t, pitem l, pitem r)
{
	if (!l || !r)
		*t = l ? l : r;
	else if (l->prior > r->prior)
		tlist_merge(&l->r, l->r, r), *t = l;
	else
		tlist_merge(&r->l, l, r->l), *t = r;
	tlist_upd_cnt(*t);
}

TLIST_INTERNAL void tlist_split(pitem t, pitem * l, pitem * r, unsigned key,
				unsigned add)
{
	if (!t) {
		*l = *r = 0;
		return;
	}
	unsigned cur_key = add + tlist_cnt(t->l);
	if (key <= cur_key)
		tlist_split(t->l, l, &t->l, key, add), *r = t;
	else
		tlist_split(t->r, &t->r, r, key, add + 1 + tlist_cnt(t->l)),
		    *l = t;
	tlist_upd_cnt(t);
}

TLIST_INTERNAL pitem tlist_getitem(pitem t, unsigned idx, unsigned add)
{
	if (!t)
		return t;
	unsigned ls = tlist_cnt(t->l), cur_key = add + ls;
	if (cur_key == idx)
		return t;
	if (cur_key < idx)
		return tlist_getitem(t->r, idx, add + 1 + ls);
	else
		return tlist_getitem(t->l, idx, add);
}

TLIST_INTERNAL void tlist_insert_item(pitem * t, pitem n, unsigned idx)
{
	pitem t1, t2;
	tlist_split(*t, &t1, &t2, idx, 0);
	tlist_merge(t, t1, n);
	tlist_merge(t, *t, t2);
}

TLIST_INTERNAL void tlist_remove(pitem * t, unsigned idx, unsigned add)
{
	pitem n;
	if (!(*t))
		return;
	unsigned cur_key = add + tlist_cnt((*t)->l), new_add = cur_key + 1;
	unsigned lk = UINT_MAX, rk = UINT_MAX;
	if ((*t)->l)
		lk = tlist_cnt((*t)->l->l) + add;
	if ((*t)->r)
		rk = tlist_cnt((*t)->r->l) + new_add;
	if (cur_key == idx) {
		tlist_merge(t, (*t)->l, (*t)->r);
	} else if (lk == idx) {
		tlist_merge(&n, (*t)->l->l, (*t)->l->r);
		(*t)->l = n;
		tlist_upd_cnt(*t);
	} else if (rk == idx) {
		tlist_merge(&n, (*t)->r->l, (*t)->r->r);
		(*t)->r = n;
		tlist_upd_cnt(*t);
	} else if (cur_key < idx) {
		tlist_remove(&(*t)->r, idx, new_add);
		tlist_upd_cnt(*t);
	} else {
		tlist_remove(&(*t)->l, idx, add);
		tlist_upd_cnt(*t);
	}
}

TLIST_INTERNAL pitem tlist_new_item(void *value, unsigned valsz, unsigned *seed)
{
	pitem n = malloc(sizeof(struct item) + valsz);
	if (!n)
		return n;
	memcpy(n + 1, value, valsz);
	n->prior = tlist_mrand(seed);
	n->cnt = 1;
	n->l = n->r = 0;
	return n;
}

struct tlist {
	unsigned seed;
	unsigned itemsize;
	pitem root;
};

TLIST_API struct tlist *tlist_new(unsigned itemsize)
{
	struct tlist *new = malloc(sizeof(struct tlist));
	if (!new)
		return 0;
	new->seed = 385 - 1;
	new->itemsize = itemsize;
	new->root = 0;
	return new;
}

TLIST_INTERNAL void *tlist_data(pitem it)
{
	return it + 1;
}

TLIST_API size_t tlist_getsize(struct tlist *t)
{
	return tlist_cnt(t->root);
}

TLIST_API void *tlist_get(struct tlist *t, size_t idx)
{
	if (idx >= tlist_cnt(t->root))
		return 0;
	return tlist_data(tlist_getitem(t->root, idx, 0));
}

TLIST_API int tlist_insert(struct tlist *t, size_t idx, void *value)
{
	if (idx > tlist_cnt(t->root))
		return 0;
	pitem new = tlist_new_item(value, t->itemsize, &t->seed);
	if (!new)
		return 0;
	tlist_insert_item(&t->root, new, idx);
	return 1;
}

TLIST_API int tlist_insert_sorted(struct tlist *t, void *value,
			       int (*cmp)(const void *, const void *))
{
	size_t lo = 0, hi = tlist_getsize(t);
	while (lo < hi) {
		size_t mid = (lo & hi) + ((lo ^ hi) >> 1);
		void *cur = tlist_get(t, mid);
		if (cmp(value, cur) < 0)
			hi = mid;
		else
			lo = mid + 1;
	}
	return tlist_insert(t, lo, value);
}

TLIST_INTERNAL int tlist_delete_impl(struct tlist *t, size_t idx)
{
	if (idx >= tlist_cnt(t->root))
		return 0;
	pitem it = tlist_getitem(t->root, idx, 0);
	tlist_remove(&t->root, idx, 0);
	free(it);
	return 1;
}

TLIST_API int tlist_delete(struct tlist *t, size_t idx)
{
	return tlist_delete_impl(t, idx);
}

TLIST_API void tlist_free_items(struct tlist *t)
{
	while (tlist_cnt(t->root))
		tlist_delete_impl(t, 0);
}

TLIST_API void *tlist_free(struct tlist *t)
{
	tlist_free_items(t);
	free(t);
	return 0;
}

#undef TLIST_INTERNAL

#endif /* TLIST_IMPL */


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

/* Length-delimited core. kl may be NULL, in which case strlen(lines[i]) is
 * used (NUL-terminated keys). */
MPH_INTERNAL uint64_t mph_build_keys_len(const char * const *lines, size_t nlines,
                                        const uint32_t *kl,
                                        uint64_t **out_keys, size_t *out_n) {
    uint64_t seed = 0;
    const uint64_t max_tries = 1u << 20;

    for (;;) {
        struct tlist *lst = tlist_new(sizeof(mph_key_node_t));
        if (!lst) { fprintf(stderr, "fatal: out of memory\n"); exit(1); }

        unsigned collision = 0;
        for (size_t i = 0; i < nlines; i++) {
            size_t len = kl ? (size_t)kl[i] : strlen(lines[i]);
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

/* Back-compat wrapper: NUL-terminated keys, lengths derived with strlen. */
MPH_INTERNAL uint64_t mph_build_keys(const char * const *lines, size_t nlines,
                                      uint64_t **out_keys, size_t *out_n) {
    return mph_build_keys_len(lines, nlines, NULL, out_keys, out_n);
}

/* ------------------------------------------------------------------ */
/* Blob serialization (into a malloc'd u8 buffer).                    */
/* ------------------------------------------------------------------ */

MPH_INTERNAL void mph_emit_u32(uint8_t **p, uint32_t v) {
    for (int i = 3; i >= 0; i--) { **p = (uint8_t)(v >> (8 * i)); (*p)++; }
}

MPH_INTERNAL void mph_emit_u64(uint8_t **p, uint64_t v) {
    for (int i = 7; i >= 0; i--) { **p = (uint8_t)(v >> (8 * i)); (*p)++; }
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

/* Re-rank every level so the global rank of each key is its dense position
 * in [0, total_keys). The builder's ranks accumulate *stored* counts, which
 * is not guaranteed dense when a collision forces keys to a later level;
 * EFS (and the unified API) index their name/entry arrays by the result, so
 * the rank must be a true permutation of 0..n-1. We recompute per-level
 * bases from the popcounts of the emitted bitvectors (level l's base is the
 * total bits set in levels 0..l-1). */
MPH_INTERNAL void mph_rerank_boomphf(mph_boomphf_t *h) {
    uint64_t prev_total = 0;
    for (unsigned i = 0; i < h->num_bitvectors; i++) {
        mph_bitvector_t *bv = h->bitvectors[i];
        unsigned rank_size = (bv->size + 7) / 8 + 1;
        uint64_t pop = 0;
        for (unsigned j = 0; j < bv->size; j++) {
            if ((j % 8) == 0) h->ranks[i][j / 8] = prev_total + pop;
            pop += (uint64_t)mph_popcountll(bv->bits[j]);
        }
        h->ranks[i][rank_size - 1] = prev_total + pop;
        prev_total += pop;
    }
}

/* Serialize a built boomphf into a fresh malloc'd blob. Sets *out_len. */
MPH_INTERNAL uint8_t *mph_serialize(mph_boomphf_t *h, uint64_t seed, size_t *out_len) {
    size_t sz = mph_blob_size(h);
    uint8_t *blob = malloc(sz);
    if (!blob) return NULL;

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
    *out_len = sz;
    return blob;
}

/* Unified build core: length-delimited keys, fills the four consumer params
 * and returns a malloc'd blob. Returns NULL on duplicate keys / OOM. */
MPH_INTERNAL uint8_t *mph_build_impl(const char * const *keys, const uint32_t *kl,
                                     size_t n_keys, size_t *out_len,
                                     uint32_t *o_blen, uint32_t *o_salt) {
    *out_len = 0;
    *o_blen = 0;
    *o_salt = 0;

    /* n==0 is a valid empty MPH (EFS empty directories): emit a 5-byte blob
     * with num_levels=0 and seed=0. mph_lookup's level loop runs zero times
     * and returns 0 ("not found") for every key, which is correct. */
    if (n_keys == 0) {
        uint8_t *blob = calloc(5, 1);
        if (!blob) return NULL;
        *out_len = 5;
        return blob; /* o_blen/o_salt stay 0 */
    }

    uint64_t *hkeys = NULL;
    size_t nkeys = 0;
    uint64_t seed = mph_build_keys_len(keys, n_keys, kl, &hkeys, &nkeys);
    if (nkeys == 0) {
        free(hkeys);
        /* nkeys==0 with n_keys>0 and no OOM means a duplicate/non-unique key
         * was found: the builder contract requires unique keys, fail loudly. */
        fprintf(stderr, "mph_build: duplicate/non-unique key detected; "
                        "input must be unique\n");
        return NULL;
    }

    mph_boomphf_t *h = mph_new_boomphf(MPH_GAMMA, hkeys, (unsigned)nkeys);
    free(hkeys);
    if (!h) return NULL;

    mph_rerank_boomphf(h);

    uint8_t *blob = mph_serialize(h, seed, out_len);
    if (blob) {
        *o_blen = h->num_bitvectors;
        *o_salt = (uint32_t)seed;
    }
    mph_free_boomphf(h);
    return blob;
}

/* Standalone API (unchanged contract): blob from NUL-terminated keys. */
MPH_DEF uint8_t *mph_build(const char * const *keys, size_t n_keys,
                           size_t *out_len) {
    uint32_t blen, salt;
    return mph_build_impl(keys, NULL, n_keys, out_len, &blen, &salt);
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

/* Read a big-endian u64 from an arbitrary offset (no cursor advance). */
MPH_INTERNAL uint64_t mph_rd_u64_at(const uint8_t *p) {
    uint64_t v = 0;
    for (unsigned i = 0; i < 8; i++) v = (v << 8) | p[i];
    return v;
}

#endif /* MPH_IMPL || EFS_BUILDER */

/* Consumer-side decoder. Available to all includers (the full builder is
 * only compiled under MPH_IMPL/EFS_BUILDER); we keep it outside the IMPL
 * guard so consumers have it without pulling the whole engine. */
#if defined(MPH_IMPL) || defined(EFS_BUILDER)
/* decoder helpers defined above inside the builder section */
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
/* popcount is used by mph_lookup's rank decode on the consumer path too. */
#if defined(__GNUC__) && __GNUC__ >= 3
static inline int mph_popcountll(uint64_t x) { return __builtin_popcountll(x); }
#else
static inline int mph_popcountll(uint64_t x) {
    x = x - ((x >> 1) & 0x5555555555555555ULL);
    x = (x & 0x3333333333333333ULL) + ((x >> 2) & 0x3333333333333333ULL);
    x = (x + (x >> 4)) & 0x0f0f0f0f0f0f0f0fULL;
    return (int)((x * 0x0101010101010101ULL) >> 56);
}
#endif
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

MPH_DEF uint64_t mph_lookup(const uint8_t *table, size_t tablen,
                            const char *key, size_t keylen) {
    if (!table || tablen < 5) return 0; /* need num_levels(u8)+seed(u32) at least */
    const uint8_t *p = table;
    uint8_t num_levels = mph_rd_u8(&p);
    uint64_t seed = mph_rd_u32(&p);
    /* Must match the builder's index hash. mph_build_keys() maps each key to
     * mix64(FNV1a(key, seed)); mph_new_boomphf() then applies mph_mix64 to
     * that value to place it in a level, i.e. the effective index hash is
     *     mix64( mix64( FNV1a(key, seed) ) ).
     * The consumer previously computed only FNV1a(key, seed) (one fewer
     * mix64), so it could never find any key. Apply the same double mix. */
    uint64_t h = mph_mix64(mph_hash_string(key, keylen, seed));
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

/* ================================================================== */
/* Unified MPH API shape (see UNIVERSAL-API-REVISED.md).               */
/*                                                                     */
/* These five entry points mirror jmph.h's public surface so the EFS    */
/* builder/reader can be compiled against either implementation with an */
/* identical call signature, selected at compile time (efs_mph.h).      */
/* The four out-params (blen/shift/salt/w) are what the consumer must   */
/* store alongside the table to decode it later; for BBHash only blen   */
/* (num_levels) and salt (seed) are meaningful.                         */
/* ================================================================== */

struct mph_in {
    uint32_t          n;      /* number of keys                          */
    const char *const *keys;  /* key bytes (NUL-terminated for C use)    */
    const uint32_t    *kl;    /* key lengths; kl[i] = length of keys[i]  */
};

struct mph_out {
    uint8_t  *data;  /* packed table; caller frees with free()           */
    uint32_t  len;   /* byte length of data                              */
    uint32_t  blen;  /* BBHash num_levels                                */
    uint32_t  shift; /* unused (0)                                       */
    uint32_t  salt;  /* hash seed                                        */
    uint32_t  w;     /* serialized table length for the unified EFS API     */
};

/* Unified-API size function. BBHash level sizes depend on actual collisions,
 * so the builder stores the serialized length in w; unlike jmph_bytes(),
 * mph_bytes() returns that stored length unchanged. */
MPH_DEF uint32_t mph_bytes(uint32_t levels, uint32_t table_len);

/* Unified build: 1 on success (out filled, out->data malloc'd), 0 on
 * duplicate keys / OOM. Keys must be unique. */
MPH_DEF int mph_build_u(const struct mph_in *in, struct mph_out *out);

/* Unified decode: 0-based index of key[0..klen), reading the table bytes
 * with the four stored params. The blob's own 5-byte header carries
 * num_levels and seed; blen/salt are accepted (and validated against the
 * blob) for signature compatibility with the other MPH implementations. */
MPH_DEF uint32_t mph_index_p(const uint8_t *tab, uint32_t blen, uint32_t shift,
                             uint32_t salt, uint32_t w,
                             const char *key, uint32_t klen);

MPH_DEF uint32_t mph_bytes(uint32_t levels, uint32_t table_len){
    (void)levels;
    return table_len;
}

MPH_DEF uint32_t mph_index_p(const uint8_t *tab, uint32_t blen, uint32_t shift,
                             uint32_t salt, uint32_t w,
                             const char *key, uint32_t klen){
    (void)shift; (void)salt;
    if (!tab) return 0;
    if (blen && tab[0] != (uint8_t)blen) return 0;   /* params disagree with blob */
    /* w carries the serialized table length for mph_lookup's bounds checks. */
    size_t tablen = mph_bytes((uint32_t)tab[0], w);
    uint64_t r = mph_lookup(tab, tablen, key, klen);
    return r ? (uint32_t)(r - 1) : 0;
}

/* EFS defines EFS_BUILDER (not MPH_IMPL) on the builder path, so expose the
 * unified builder for either. The standalone mph_build() stays MPH_IMPL-only
 * to preserve oomph's original two-role model. */
#if defined(MPH_IMPL) || defined(EFS_BUILDER)

MPH_DEF int mph_build_u(const struct mph_in *in, struct mph_out *out){ /* builder */
    *out = (struct mph_out){0};
    if (!in) return 0;
    size_t len = 0;
    uint32_t blen = 0, salt = 0;
    uint8_t *blob = mph_build_impl(in->keys, in->kl, in->n, &len, &blen, &salt);
    if (!blob) return 0;
    if (len > 0xffffffffu){ free(blob); return 0; }
    out->data  = blob;
    out->len   = (uint32_t)len;
    out->blen  = blen;
    out->salt  = salt;
    out->shift = 0;
    out->w     = out->len;
    return 1;
}

#endif /* MPH_IMPL || EFS_BUILDER */

#if defined(__cplusplus)
}
#endif

#endif /* BBHASH_H */

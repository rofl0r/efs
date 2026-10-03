/*
 * goomph.c - String-keyed minimal perfect hash function generator.
 *
 * Same core algorithm as soomph.c (BBHash / BoomPHF, arXiv:1702.03154),
 * but the emitted C is a SINGLE portable uint8_t blob plus a small generic
 * decoder, instead of several named tables.
 *
 * The blob (little-endian, no header, endian-agnostic on every host):
 *
 *     [ u8  num_levels ]
 *     [ u32 seed ]                       // baked-in hash seed (fits 2^20)
 *     for each level:
 *         [ u32 bitsize ]                // in bits
 *         [ (bitsize+63)/64 * 8 bytes ]  bv  bitvector (u64 words, LE)
 *         [ ((bitsize+511)/512+1)*8 ]    rank table (u64 words, LE)
 *
 * size/rank_size are recomputed by the decoder from bitsize, so they are
 * not stored. `n` (number of keys) lives OUTSIDE the blob, as a companion
 * `static const uint32_t mph_n;`, so the blob stays minimal and the
 * decoder needs no range knowledge.
 *
 * The decoder (boomphf_query_blob) is written to be lifted later, unchanged,
 * into a one-header library: it is static, has no global state, and uses
 * only two runtime variables (a cursor pointer + the level index).
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>
#include <stddef.h>
#include <inttypes.h>

/* Use the tlist header with static linkage for this translation unit. */
#define TLIST_API static
#define TLIST_IMPL
#include "tlist.h"

#define GAMMA 2.0
#define MAX_KEYS 10000000

/* ------------------------------------------------------------------ */
/* Core MPHF engine (same as soomph.c)                                 */
/* ------------------------------------------------------------------ */

static inline uint32_t rotl(uint32_t v, uint32_t r) {
	return (v << r) | (v >> (32 - r));
}

static inline uint64_t xorshiftMult64(uint64_t x) {
	x ^= x >> 12;
	x ^= x << 25;
	x ^= x >> 27;
	return x * 2685821657736338717ULL;
}

typedef struct {
	uint64_t* bits;
	int size;
	int bitsize;
} bitvector_t;

typedef struct {
	bitvector_t** bitvectors;
	uint64_t** ranks;
	int num_bitvectors;
	int* level_counts;
	int total_keys;
} BoomPhF;

bitvector_t* newbv(int bitsize) {
	bitvector_t* b = calloc(1, sizeof(bitvector_t));
	b->bitsize = bitsize;
	b->size = (bitsize + 63) / 64;
	b->bits = calloc(b->size, sizeof(uint64_t));
	return b->bits ? b : (free(b), NULL);
}

static inline int bv_get(bitvector_t* bv, int bit) {
	return (bv->bits[bit / 64] >> (bit % 64)) & 1;
}

static inline void bv_set(bitvector_t* bv, int bit) {
	bv->bits[bit / 64] |= (1ULL << (bit % 64));
}

BoomPhF* NewBoomPhF(double gamma, uint64_t* keys, int num_keys) {
	BoomPhF* h = calloc(1, sizeof(BoomPhF));
	if (!h) return NULL;

	h->total_keys = num_keys;
	int level = 0;
	int remaining = num_keys;
	uint64_t* current_keys = keys;

	while (remaining > 0) {
		int bitsize = ((int)(gamma * remaining) + 63) & ~63;

		bitvector_t* A = newbv(bitsize);
		bitvector_t* collide = newbv(bitsize);
		uint64_t* redo = malloc(remaining * sizeof(uint64_t));
		int redo_count = 0;

		for (int i = 0; i < remaining; i++) {
			uint64_t v = current_keys[i];
			uint64_t hash = xorshiftMult64(v);
			uint32_t h1 = hash, h2 = hash >> 32;
			int idx = (h1 ^ rotl(h2, level)) % bitsize;

			if (bv_get(collide, idx)) continue;
			if (bv_get(A, idx)) { bv_set(collide, idx); continue; }
			bv_set(A, idx);
		}

		bitvector_t* bv = newbv(bitsize);
		h->level_counts = realloc(h->level_counts, (h->num_bitvectors + 1) * sizeof(int));
		int stored_count = 0;
		for (int i = 0; i < remaining; i++) {
			uint64_t v = current_keys[i];
			uint64_t hash = xorshiftMult64(v);
			uint32_t h1 = hash, h2 = hash >> 32;
			int idx = (h1 ^ rotl(h2, level)) % bitsize;

			if (bv_get(collide, idx)) {
				redo[redo_count++] = v;
				continue;
			}
			bv_set(bv, idx);
			stored_count++;
		}
		h->level_counts[h->num_bitvectors] = stored_count;

		h->num_bitvectors++;
		h->bitvectors = realloc(h->bitvectors, h->num_bitvectors * sizeof(bitvector_t*));
		h->bitvectors[h->num_bitvectors - 1] = bv;

		free(A->bits); free(A);
		free(collide->bits); free(collide);

		uint64_t* to_free = current_keys;
		current_keys = redo;
		free(to_free);

		remaining = redo_count;
		level++;
	}

	h->ranks = malloc(h->num_bitvectors * sizeof(uint64_t*));
	int prev_total = 0;
	for (int i = 0; i < h->num_bitvectors; i++) {
		int rank_size = (h->bitvectors[i]->size + 7) / 8 + 1;
		h->ranks[i] = calloc(rank_size, sizeof(uint64_t));
		uint64_t pop = 0;
		for (int j = 0; j < h->bitvectors[i]->size; j++) {
			if ((j % 8) == 0) h->ranks[i][j/8] = pop + prev_total;
			pop += __builtin_popcountll(h->bitvectors[i]->bits[j]);
		}
		h->ranks[i][rank_size - 1] = pop + prev_total;
		prev_total += pop;
	}

	return h;
}

uint64_t query(BoomPhF* h, uint64_t key) {
	uint64_t hash = xorshiftMult64(key);
	uint32_t h1 = hash, h2 = hash >> 32;

	for (int i = 0; i < h->num_bitvectors; i++) {
		int bitsize = h->bitvectors[i]->bitsize;
		int idx = (h1 ^ rotl(h2, i)) % bitsize;

		if (!bv_get(h->bitvectors[i], idx)) continue;

		uint64_t rank = h->ranks[i][idx/512];
		int word = idx / 64;
		int bit_in_word = idx % 64;

		for (int j = (idx/512) * 8; j < word; j++) {
			rank += __builtin_popcountll(h->bitvectors[i]->bits[j]);
		}
		uint64_t w = h->bitvectors[i]->bits[word];
		if (bit_in_word != 0) {
			rank += __builtin_popcountll(w << (64 - bit_in_word));
		}
		return rank + 1;
	}
	return 0;
}

void free_boomphf(BoomPhF* h) {
	if (!h) return;
	for (int i = 0; i < h->num_bitvectors; i++) {
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
/* String hashing (seeded)                                            */
/* ------------------------------------------------------------------ */

static inline uint64_t hash_string(const char* s, size_t len, uint64_t seed) {
	uint64_t h = 1469598103934665603ULL ^ seed;
	for (size_t i = 0; i < len; i++) {
		h ^= (unsigned char)s[i];
		h *= 1099511628211ULL;
	}
	return xorshiftMult64(h);
}

/* ------------------------------------------------------------------ */
/* Sorted key collection (backed by tlist)                            */
/* ------------------------------------------------------------------ */

typedef struct {
	uint64_t hash;
	char* key;
} key_node_t;

static int key_node_cmp(const void* a, const void* b) {
	const key_node_t* ka = a;
	const key_node_t* kb = b;
	if (ka->hash < kb->hash) return -1;
	if (ka->hash > kb->hash) return 1;
	return 0;
}

static int key_list_insert(struct tlist* l, uint64_t hash, char* key) {
	size_t n = tlist_getsize(l);
	size_t lo = 0, hi = n;
	while (lo < hi) {
		size_t mid = (lo & hi) + ((lo ^ hi) >> 1);
		const key_node_t* cur = tlist_get(l, mid);
		if (cur->hash < hash) lo = mid + 1;
		else hi = mid;
	}

	if (lo < n) {
		const key_node_t* cur = tlist_get(l, lo);
		if (cur->hash == hash) {
			if (strcmp(cur->key, key) == 0) {
				fprintf(stderr, "warning: duplicate key \"%s\" ignored\n", key);
				return 1;
			}
			return 2;
		}
	}
	if (lo > 0) {
		const key_node_t* prev = tlist_get(l, lo - 1);
		if (prev->hash == hash) {
			if (strcmp(prev->key, key) == 0) {
				fprintf(stderr, "warning: duplicate key \"%s\" ignored\n", key);
				return 1;
			}
			return 2;
		}
	}

	key_node_t node = { hash, key };
	return tlist_insert_sorted(l, &node, key_node_cmp) ? 0 : -1;
}

static uint64_t build_keys(char** lines, int nlines,
						   uint64_t** out_keys, int* out_n) {
	uint64_t seed = 0;
	const uint64_t max_tries = 1u << 20;

	for (;;) {
		struct tlist* lst = tlist_new(sizeof(key_node_t));
		if (!lst) { fprintf(stderr, "fatal: out of memory\n"); exit(1); }

		int collision = 0;
		for (int i = 0; i < nlines; i++) {
			size_t len = strlen(lines[i]);
			if (len == 0) continue;
			uint64_t h = hash_string(lines[i], len, seed);
			int r = key_list_insert(lst, h, lines[i]);
			if (r == 2) { collision = 1; break; }
			else if (r < 0) { fprintf(stderr, "fatal: out of memory\n"); exit(1); }
		}

		if (!collision) {
			*out_n = (int)tlist_getsize(lst);
			*out_keys = malloc((size_t)*out_n * sizeof(uint64_t));
			for (int i = 0; i < *out_n; i++) {
				const key_node_t* node = tlist_get(lst, (size_t)i);
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
/* Blob serialization + generic decoder emission                      */
/* ------------------------------------------------------------------ */

/* Emit one u32 (little-endian) as a C byte literal line into a buffer. */
static void emit_u32(FILE* out, uint32_t v) {
	fprintf(out, "    0x%02x,0x%02x,0x%02x,0x%02x,\n",
			(int)(v & 0xff), (int)((v >> 8) & 0xff),
			(int)((v >> 16) & 0xff), (int)((v >> 24) & 0xff));
}

/* Emit one u64 (little-endian) as 8 byte literals. */
static void emit_u64(FILE* out, uint64_t v) {
	for (int i = 0; i < 8; i++)
		fprintf(out, "0x%02x,", (int)((v >> (8 * i)) & 0xff));
	fprintf(out, "\n");
}

void dump_c_code(FILE* out, BoomPhF* h, uint64_t seed, int n) {
	/* ---- prologue ---- */
	fputs(
		"/* Auto-generated minimal perfect hash (portable blob form) */\n"
		"#include <stdio.h>\n"
		"#include <stdint.h>\n"
		"#include <stddef.h>\n"
		"#include <string.h>\n"
		"#include <inttypes.h>\n"
		"\n", out);

	/* ---- n lives outside the blob ---- */
	fprintf(out, "static const uint32_t mph_n = %d;  /* number of keys; ranks in [1..mph_n] */\n\n", n);

	/* ---- the blob ---- */
	fputs("static const uint8_t mph_blob[] = {\n", out);
	fputs("    /* num_levels (u8) */\n", out);
	fprintf(out, "    0x%02x,\n", (int)(h->num_bitvectors & 0xff));
	fputs("    /* seed (u32, little-endian) */\n", out);
	emit_u32(out, (uint32_t)seed);
	for (int i = 0; i < h->num_bitvectors; i++) {
		int bitsize = h->bitvectors[i]->bitsize;
		int size = (bitsize + 63) / 64;
		int rank_size = (bitsize + 511) / 512 + 1;
		fputs("    /* level bitsize (u32, LE) */\n", out);
		emit_u32(out, (uint32_t)bitsize);
		fputs("    /* bv words (u64 LE) */\n", out);
		for (int j = 0; j < size; j++) emit_u64(out, h->bitvectors[i]->bits[j]);
		fputs("    /* rank words (u64 LE) */\n", out);
		for (int j = 0; j < rank_size; j++) emit_u64(out, h->ranks[i][j]);
	}
	fputs("};\n\n", out);

	/* ---- generic decoder + seeded hash ---- */
	fputs(
		"static inline uint32_t rotl(uint32_t v, uint32_t r) {\n"
		"    return (r == 0) ? v : ((v << r) | (v >> (32 - r)));\n"
		"}\n"
		"\n"
		"static inline uint64_t xorshiftMult64(uint64_t x) {\n"
		"    x ^= x >> 12; x ^= x << 25; x ^= x >> 27;\n"
		"    return x * 2685821657736338717ULL;\n"
		"}\n"
		"\n"
		"static inline uint64_t mph_hash_string(uint64_t seed, const char *s, size_t len) {\n"
		"    uint64_t h = 1469598103934665603ULL ^ seed;\n"
		"    for (size_t i = 0; i < len; i++) {\n"
		"        h ^= (unsigned char)s[i];\n"
		"        h *= 1099511628211ULL;\n"
		"    }\n"
		"    return xorshiftMult64(h);\n"
		"}\n"
		"\n"
		"static uint8_t  rd_u8 (const uint8_t **p) { return *(*p)++; }\n"
		"static uint32_t rd_u32(const uint8_t **p) {\n"
		"    uint32_t v = 0; for (int i = 0; i < 4; i++) v = (v << 8) | rd_u8(p);\n"
		"    return v;\n"
		"}\n"
		"static uint64_t rd_u64(const uint8_t **p) {\n"
		"    uint64_t v = 0; for (int i = 0; i < 8; i++) v = (v << 8) | rd_u8(p);\n"
		"    return v;\n"
		"}\n"
		"\n"
		"/*\n"
		" * Generic MPHF decoder over a packed blob:\n"
		" *   [u8 num_levels][u32 seed]\n"
		" *   per level: [u32 bitsize][bv bytes][rank bytes]\n"
		" * size = (bitsize+63)/64; rank_size = (bitsize+511)/512 + 1.\n"
		" * Returns 0 if not found, else 1-indexed rank.\n"
		" */\n"
		"static uint64_t boomphf_query_blob(const uint8_t *blob, const char *s, size_t len) {\n"
		"    const uint8_t *p = blob;\n"
		"    uint8_t num_levels = rd_u8(&p);\n"
		"    uint64_t seed = rd_u32(&p);\n"
		"    uint64_t h = mph_hash_string(seed, s, len);\n"
		"    uint32_t h1 = (uint32_t)h, h2 = (uint32_t)(h >> 32);\n"
		"    for (uint8_t level = 0; level < num_levels; level++) {\n"
		"        uint32_t bitsize = rd_u32(&p);\n"
		"        uint32_t size = (bitsize + 63) / 64;\n"
		"        uint32_t rank_size = (bitsize + 511) / 512 + 1;\n"
		"        uint32_t idx = (h1 ^ rotl(h2, level)) % bitsize;\n"
		"        /* read the bv word containing idx */\n"
		"        uint64_t bvword = 0;\n"
		"        const uint8_t *bp = p + (size_t)(idx >> 6) * 8;\n"
		"        for (int i = 0; i < 8; i++) bvword = (bvword << 8) | bp[i];\n"
		"        if (((bvword >> (idx & 63)) & 1) == 0) {\n"
		"            p += (size_t)size * 8 + (size_t)rank_size * 8;\n"
		"            continue;\n"
		"        }\n"
		"        /* occupied: compute rank from the rank table */\n"
		"        uint64_t rank = 0;\n"
		"        const uint8_t *rp = p + (size_t)size * 8 + (size_t)(idx / 512) * 8;\n"
		"        for (int i = 0; i < 8; i++) rank = (rank << 8) | rp[i];\n"
		"        int word = idx / 64;\n"
		"        const uint8_t *bp2 = p + (size_t)word * 8;\n"
		"        uint64_t w = 0;\n"
		"        for (int i = 0; i < 8; i++) w = (w << 8) | bp2[i];\n"
		"        for (int j = (idx / 512) * 8; j < word; j++) {\n"
		"            const uint8_t *jp = p + (size_t)j * 8;\n"
		"            uint64_t jw = 0;\n"
		"            for (int k = 0; k < 8; k++) jw = (jw << 8) | jp[k];\n"
		"            rank += __builtin_popcountll(jw);\n"
		"        }\n"
		"        if ((idx & 63) != 0) {\n"
		"            rank += __builtin_popcountll(w << (64 - (idx & 63)));\n"
		"        }\n"
		"        return rank + 1;\n"
		"    }\n"
		"    return 0;\n"
		"}\n"
		"\n"
		"uint64_t boomphf_query_string(const char *s, size_t len) {\n"
		"    return boomphf_query_blob(mph_blob, s, len);\n"
		"}\n"
		"\n", out);

	/* ---- TEST main (reuses the blob decoder) ---- */
	fputs(
		"#ifdef TEST\n"
		"int main(void) {\n"
		"    char line[4096];\n"
		"    while (fgets(line, sizeof(line), stdin)) {\n"
		"        size_t len = strlen(line);\n"
		"        if (len > 0 && line[len-1] == '\\n') line[--len] = '\\0';\n"
		"        if (len > 0 && line[len-1] == '\\r') line[--len] = '\\0';\n"
		"        if (len == 0) continue;\n"
		"        uint64_t idx = boomphf_query_string(line, len);\n"
		"        printf(\"%s\\t%\" PRIu64 \"\\n\", line, idx);\n"
		"    }\n"
		"    return 0;\n"
		"}\n"
		"#endif\n", out);
}

/* ------------------------------------------------------------------ */
/* Input buffering (portable line reader)                            */
/* ------------------------------------------------------------------ */

static int read_line(FILE* f, char** buf, size_t* cap) {
	size_t len = 0;
	int c;
	if (*cap == 0) { *cap = 128; *buf = malloc(*cap); }
	while ((c = fgetc(f)) != EOF) {
		if (len + 1 >= *cap) { *cap *= 2; *buf = realloc(*buf, *cap); }
		if (c == '\n') { (*buf)[len] = '\0'; return (int)len; }
		(*buf)[len++] = (char)c;
	}
	if (len == 0) return -1;
	(*buf)[len] = '\0';
	return (int)len;
}

static char** read_all_lines(int* out_n) {
	char** lines = NULL;
	int count = 0, cap = 0;
	char* buf = NULL;
	size_t bufc = 0;

	int len;
	while ((len = read_line(stdin, &buf, &bufc)) >= 0) {
		if (len > 0 && buf[len - 1] == '\r') buf[--len] = '\0';
		if (len == 0) continue;
		if (count == cap) {
			cap = cap ? cap * 2 : 1024;
			lines = realloc(lines, (size_t)cap * sizeof(char*));
		}
		lines[count++] = strdup(buf);
	}

	free(buf);
	*out_n = count;
	return lines;
}

/* ------------------------------------------------------------------ */
/* main()                                                             */
/* ------------------------------------------------------------------ */

int main(void) {
	int nlines;
	char** lines = read_all_lines(&nlines);
	if (nlines == 0) { fprintf(stderr, "No keys provided\n"); return 1; }
	if (nlines > MAX_KEYS) {
		fprintf(stderr, "Too many keys (max %d)\n", MAX_KEYS);
		return 1;
	}

	uint64_t* keys = NULL;
	int nkeys = 0;
	uint64_t seed = build_keys(lines, nlines, &keys, &nkeys);
	if (nkeys == 0) { fprintf(stderr, "No unique keys after deduplication\n"); return 1; }

	printf("/* seed=%" PRIu64 " nkeys=%d */\n", seed, nkeys);

	BoomPhF* h = NewBoomPhF(GAMMA, keys, nkeys);
	if (!h) { fprintf(stderr, "Failed to build hash function\n"); return 1; }
	dump_c_code(stdout, h, seed, nkeys);
	free_boomphf(h);

	for (int i = 0; i < nlines; i++) free(lines[i]);
	free(lines);
	return 0;
}

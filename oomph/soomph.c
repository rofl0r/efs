/*
 * soomph.c - String-keyed minimal perfect hash function generator.
 *
 * Derived from boomphf.c (a C translation of boomphf.go, which implements the
 * BBHash / BoomPHF algorithm from arXiv:1702.03154). The core MPHF engine is
 * unchanged: it only ever sees a 64-bit value, so supporting string keys is
 * purely a matter of (1) hashing each string to 64 bits and (2) making sure
 * that hash is collision-free across the key set.
 *
 * Input is read as strings (one per line). Each string is hashed (seeded
 * FNV-1a + xorshift finalizer) into a 64-bit value. We keep a sorted
 * collection of {hash, key} entries using the tlist header (a sorted list);
 * new entries are inserted at the position found by binary search inside
 * tlist_insert_sorted(). While searching we detect:
 *     * duplicate key (same string)  -> warn and ignore,
 *     * hash collision (different strings, same hash) -> the whole set is
 *       rebuilt with a new seed (the old collection is dropped).
 *
 * Once a collision-free, sorted collection is built (EOF reached), the 64-bit
 * hashes are handed to the unchanged MPHF builder and C code is dumped. The
 * dumped C file also embeds the string hash (with the chosen seed baked in)
 * and a boomphf_query_string() helper, plus a #ifdef TEST main() that reads
 * strings from stdin and prints their MPHF index.
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
/* Core MPHF engine (verbatim from boomphf.c)                         */
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

		// First pass: detect collisions
		for (int i = 0; i < remaining; i++) {
			uint64_t v = current_keys[i];
			uint64_t hash = xorshiftMult64(v);
			uint32_t h1 = hash, h2 = hash >> 32;
			int idx = (h1 ^ rotl(h2, level)) % bitsize;

			if (bv_get(collide, idx)) continue;
			if (bv_get(A, idx)) { bv_set(collide, idx); continue; }
			bv_set(A, idx);
		}

		// Second pass: store to bv or redo
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

		// Store bitvector
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

	// Compute ranks - cumulative across all previous levels
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

/* Seeded FNV-1a 64-bit, finalized with the same xorshift mixer used  */
/* by the core engine. The seed lets us retry with a different hash   */
/* mapping if a collision is detected.                                */
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
/*                                                                     */
/* Each stored element is a key_node_t { uint64_t hash; char* key; }. */
/* tlist_insert_sorted() keeps the list ordered by hash using its     */
/* internal binary search. The collection does NOT take ownership of  */
/* the key strings (those are owned by the buffered `lines`).         */
/* ------------------------------------------------------------------ */

typedef struct {
	uint64_t hash;
	char* key;
} key_node_t;

/* Order by hash ascending; ties are indistinguishable from the list's
 * point of view, so the caller inspects neighbours for the duplicate /
 * collision check. */
static int key_node_cmp(const void* a, const void* b) {
	const key_node_t* ka = a;
	const key_node_t* kb = b;
	if (ka->hash < kb->hash) return -1;
	if (ka->hash > kb->hash) return 1;
	return 0;
}

/*
 * Insert (hash, key) into the sorted tlist. We first locate any existing
 * element with the same hash and inspect it (and its immediate predecessor)
 * for duplicates / collisions before inserting.
 *
 * Returns:
 *   0 = inserted,
 *   1 = duplicate key (same string already present) -- caller ignores it,
 *   2 = hash collision (different string, same hash) -- caller must reseed.
 */
static int key_list_insert(struct tlist* l, uint64_t hash, char* key) {
	size_t n = tlist_getsize(l);
	size_t lo = 0, hi = n;
	/* Binary search for the first element with hash >= `hash`. */
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
				return 1; /* duplicate */
			}
			return 2; /* hash collision with a different key */
		}
	}
	/* Also check the predecessor (equal-hash cluster could start at lo-1
	 * only if lo>0 and that element has the same hash). */
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

/* ------------------------------------------------------------------ */
/* Build a collision-free, sorted list from buffered lines.           */
/*                                                                     */
/* On a hash collision we drop the current list, bump the seed, and    */
/* rebuild from the buffered lines. Returns the seed that worked and   */
/* fills *out_keys / *out_n with the resulting 64-bit hashes.          */
/* ------------------------------------------------------------------ */

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
			if (len == 0) continue; /* skip empty lines */
			uint64_t h = hash_string(lines[i], len, seed);
			int r = key_list_insert(lst, h, lines[i]);
			if (r == 2) { collision = 1; break; }
			/* r == 0 inserted, r == 1 duplicate ignored, r == -1 OOM */
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
/* C code generation                                                  */
/* ------------------------------------------------------------------ */

/* Static, non-data parts of the generated file. Emitted as whole      */
/* blocks so we don't call printf/puts per line.                       */

static const char* GEN_PROLOGUE =
"/* Auto-generated perfect hash function for STRING keys */\n"
"#include <stdio.h>\n"
"#include <stdint.h>\n"
"#include <stddef.h>\n"
"#include <string.h>\n"
"#include <inttypes.h>\n"
"\n";

static const char* GEN_HELPERS =
"static inline uint32_t rotl(uint32_t v, uint32_t r) {\n"
"    return (v << r) | (v >> (32 - r));\n"
"}\n"
"\n"
"static inline uint64_t xorshiftMult64(uint64_t x) {\n"
"    x ^= x >> 12; x ^= x << 25; x ^= x >> 27;\n"
"    return x * 2685821657736338717ULL;\n"
"}\n"
"\n";

static const char* GEN_QUERY_OPEN =
"uint64_t boomphf_query(uint64_t key) {\n"
"    uint64_t h = xorshiftMult64(key);\n"
"    uint32_t h1 = (uint32_t)h, h2 = (uint32_t)(h >> 32);\n"
"\n";

static const char* GEN_QUERY_CLOSE =
"    return 0;\n"
"}\n"
"\n"
"uint64_t boomphf_query_string(const char *s, size_t len) {\n"
"    return boomphf_query(hash_string(s, len));\n"
"}\n"
"\n";

static const char* GEN_TEST_MAIN =
"#ifdef TEST\n"
"/* Reads strings from stdin (one per line), strips the trailing newline, */\n"
"/* and prints \"<string>\\t<hash>\" using the generated tables.         */\n"
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
"#endif\n";

void dump_c_code(FILE* out, BoomPhF* h, uint64_t seed) {
	fputs(GEN_PROLOGUE, out);

	// Bitvectors (data)
	for (int i = 0; i < h->num_bitvectors; i++) {
		fprintf(out, "static const uint64_t bv_%d[%d] = {\n", i, h->bitvectors[i]->size);
		for (int j = 0; j < h->bitvectors[i]->size; j++) {
			fprintf(out, "    0x%016" PRIx64 "ULL", h->bitvectors[i]->bits[j]);
			if (j < h->bitvectors[i]->size - 1) fprintf(out, ",");
			fprintf(out, "\n");
		}
		fputs("};\n\n", out);
	}

	// Ranks (data)
	for (int i = 0; i < h->num_bitvectors; i++) {
		int rank_size = (h->bitvectors[i]->bitsize + 511) / 512 + 1;
		fprintf(out, "static const uint64_t rank_%d[%d] = {\n", i, rank_size);
		for (int j = 0; j < rank_size; j++) {
			fprintf(out, "    %" PRIu64 "", h->ranks[i][j]);
			if (j < rank_size - 1) fprintf(out, ",");
			fprintf(out, "\n");
		}
		fputs("};\n\n", out);
	}

	fputs(GEN_HELPERS, out);

	// Seeded string hash with the chosen seed baked in (one substitution).
	fprintf(out,
		"/* string hash with baked-in seed %" PRIu64 " */\n"
		"static inline uint64_t hash_string(const char *s, size_t len) {\n"
		"    uint64_t h = 1469598103934665603ULL ^ %" PRIu64 "ULL;\n"
		"    for (size_t i = 0; i < len; i++) {\n"
		"        h ^= (unsigned char)s[i];\n"
		"        h *= 1099511628211ULL;\n"
		"    }\n"
		"    return xorshiftMult64(h);\n"
		"}\n"
		"\n",
		seed, seed);

	fputs(GEN_QUERY_OPEN, out);

	for (int i = 0; i < h->num_bitvectors; i++) {
		int bv_bitsize = h->bitvectors[i]->bitsize;
		fprintf(out,
			"    { int bitsize = %d, idx = (h1 ^ rotl(h2, %d)) %% bitsize;\n"
			"      if (idx < bitsize && (bv_%d[idx>>6] & (1ULL << (idx & 63)))) {\n"
			"          uint64_t rank = rank_%d[idx/512];\n"
			"          int word = idx >> 6;\n"
			"          for (int j = (idx/512) * 8; j < word; j++) rank += __builtin_popcountll(bv_%d[j]);\n"
			"          if ((idx & 63) != 0) rank += __builtin_popcountll(bv_%d[word] << (64 - (idx & 63)));\n"
			"          return rank + 1;\n"
			"      } }\n"
			"\n",
			bv_bitsize, i, i, i, i, i);
	}

	fputs(GEN_QUERY_CLOSE, out);
	fputs(GEN_TEST_MAIN, out);
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
	if (len == 0) return -1; /* EOF, nothing read */
	(*buf)[len] = '\0';
	return (int)len;
}

/* Read all non-empty lines from stdin into a freshly allocated array of
 * strdup'd strings (trailing \n and \r stripped). Returns the array and
 * count via the out parameters; caller frees. */
static char** read_all_lines(int* out_n) {
	char** lines = NULL;
	int count = 0, cap = 0;
	char* buf = NULL;
	size_t bufc = 0;

	int len;
	while ((len = read_line(stdin, &buf, &bufc)) >= 0) {
		/* strip a trailing \r (DOS line endings) */
		if (len > 0 && buf[len - 1] == '\r') buf[--len] = '\0';
		if (len == 0) continue; /* skip empty lines */

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
/* main() : read string keys, build a collision-free set, emit C.    */
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
	dump_c_code(stdout, h, seed);
	free_boomphf(h);
	/* NewBoomPhF took ownership of and freed `keys`. */

	for (int i = 0; i < nlines; i++) free(lines[i]);
	free(lines);
	return 0;
}

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>
#include <inttypes.h>

#define GAMMA 2.0
#define MAX_KEYS 10000000

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
		uint64_t* redo = malloc((size_t)remaining * sizeof(uint64_t));
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

void dump_c_code(FILE* out, BoomPhF* h) {
	fprintf(out, "/* Auto-generated perfect hash function */\n");
	fprintf(out, "#include <stdio.h>\n");
	fprintf(out, "#include <stdint.h>\n");
	fprintf(out, "#include <inttypes.h>\n\n");
	// Bitvectors
	for (int i = 0; i < h->num_bitvectors; i++) {
		fprintf(out, "static const uint64_t bv_%d[%d] = {\n", i, h->bitvectors[i]->size);
		for (int j = 0; j < h->bitvectors[i]->size; j++) {
			fprintf(out, "    0x%016" PRIx64 "ULL", h->bitvectors[i]->bits[j]);
			if (j < h->bitvectors[i]->size - 1) fprintf(out, ",");
			fprintf(out, "\n");
		}
		fprintf(out, "};\n\n");
	}
	// Ranks
	for (int i = 0; i < h->num_bitvectors; i++) {
		int rank_size = (h->bitvectors[i]->bitsize + 511) / 512 + 1;
		fprintf(out, "static const uint64_t rank_%d[%d] = {\n", i, rank_size);
		for (int j = 0; j < rank_size; j++) {
			fprintf(out, "    %" PRIu64 "", h->ranks[i][j]);
			if (j < rank_size - 1) fprintf(out, ",");
			fprintf(out, "\n");
		}
		fprintf(out, "};\n\n");
	}
	// Helper functions
	fprintf(out, "static inline uint32_t rotl(uint32_t v, uint32_t r) {\n");
	fprintf(out, "    return (v << r) | (v >> (32 - r));\n");
	fprintf(out, "}\n\n");
	fprintf(out, "static inline uint64_t hash_func(uint64_t key) {\n");
	fprintf(out, "    uint64_t h = key;\n");
	fprintf(out, "    h ^= h >> 12; h ^= h << 25; h ^= h >> 27;\n");
	fprintf(out, "    h *= 2685821657736338717ULL;\n");
	fprintf(out, "    return h;\n");
	fprintf(out, "}\n\n");
	// Query function
	fprintf(out, "uint64_t boomphf_query(uint64_t key) {\n");
	fprintf(out, "    uint64_t h = hash_func(key);\n");
	fprintf(out, "    uint32_t h1 = (uint32_t)h, h2 = (uint32_t)(h >> 32);\n\n");
	for (int i = 0; i < h->num_bitvectors; i++) {
		int bv_bitsize = h->bitvectors[i]->bitsize;
		fprintf(out, "    { int bitsize = %d, idx = (h1 ^ rotl(h2, %d)) %% bitsize;\n", bv_bitsize, i);
		fprintf(out, "      if (idx < bitsize && (bv_%d[idx>>6] & (1ULL << (idx & 63)))) {\n", i);
		fprintf(out, "          uint64_t rank = rank_%d[idx/512];\n", i);
		fprintf(out, "          int word = idx >> 6;\n");
		fprintf(out, "          for (int j = (idx/512) * 8; j < word; j++) rank += __builtin_popcountll(bv_%d[j]);\n", i);
		fprintf(out, "          if ((idx & 63) != 0) rank += __builtin_popcountll(bv_%d[word] << (64 - (idx & 63)));\n", i);
		fprintf(out, "          return rank + 1;\n");
		fprintf(out, "      } }\n\n");
	}
	fprintf(out, "    return 0;\n");
	fprintf(out, "}\n");
}

int main() {
	uint64_t* keys = malloc(MAX_KEYS * sizeof(uint64_t));
	int count = 0;
	char line[256];
	while (fgets(line, sizeof(line), stdin) && count < MAX_KEYS) {
		char* p = line;
		while (*p && isspace((unsigned char)*p)) p++;
		if (*p == '\0' || *p == '\n') continue;
		errno = 0; char* end;
		unsigned long val = strtoul(p, &end, 0);
		if (errno == 0 && end != p) keys[count++] = (uint64_t)val;
	}
	if (count == 0) { fprintf(stderr, "No keys provided\n"); return 1; }
	printf("/* Read %d keys */\n", count);
	BoomPhF* h = NewBoomPhF(GAMMA, keys, count);
	if (!h) { fprintf(stderr, "Failed to build hash function\n"); return 1; }
	dump_c_code(stdout, h);
	free_boomphf(h);
	return 0;
}

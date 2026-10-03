#include <stdio.h>
#include <stdint.h>
#include <inttypes.h>
#include <stdlib.h>

/* test generated hash func */
#include "hash.h"


int main() {
	uint64_t *keys = malloc(10000000 * sizeof(uint64_t));
	int n = 0;
	char line[256];

	while (fgets(line, sizeof(line), stdin) && n < 10000000) {
		char *p = line;
		while (*p && (*p == ' ' || *p == '\t')) p++;
		if (*p == '\n' || *p == '\0') continue;
		char *end;
		unsigned long val = strtoul(p, &end, 0);
		if (end != p) keys[n++] = (uint64_t)val;
	}

	if (n == 0) { fprintf(stderr, "No keys provided\n"); return 1; }

	printf("Testing %d keys...\n", n);
	int errors = 0;
	for (int i = 0; i < n; i++) {
		uint64_t rank = boomphf_query(keys[i]);
		printf("key %" PRIu64 " -> hash %" PRIu64 "\n", keys[i], rank);
		if (rank == 0) {
			fprintf(stderr, "FAIL: key %" PRIu64 " not found\n", keys[i]);
			errors++;
		}
	}

	// Check uniqueness of ranks
	for (int i = 0; i < n; i++) {
		uint64_t rank = boomphf_query(keys[i]);
		for (int j = i + 1; j < n; j++) {
			uint64_t rank2 = boomphf_query(keys[j]);
			if (rank == rank2) {
				fprintf(stderr, "FAIL: keys %" PRIu64 " and %" PRIu64 " collide (rank %" PRIu64 ")\n", keys[i], keys[j], rank);
				errors++;
			}
		}
	}

	if (errors == 0) {
		printf("OK: all %d keys have unique hashes\n", n);
	} else {
		printf("FAIL: %d errors found\n", errors);
	}
	return errors > 0 ? 1 : 0;
}

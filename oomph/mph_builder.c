/*
 * mph_builder.c - String-keyed minimal perfect hash generator using the
 * factored-out mph.h header.
 *
 * Reads string keys (one per line) from stdin and writes a compilable C
 * table to stdout: a `static const uint8_t mph_blob[]` literal plus an
 * #include "mph.h" and a -DTEST main() that exercises mph_lookup() (the
 * consumer path) so the result can be validated by the test harness.
 */

#define MPH_API static
#define MPH_IMPL
#include "mph.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <inttypes.h>

/* Read all non-empty lines from stdin, strdup'd and NUL-terminated (trailing
 * \n and \r stripped). Returns the array and count via out param; caller frees.
 * Lines longer than the initial buffer are read in chunks via repeated fgets. */
static char **read_all_lines(size_t *out_n) {
	char **lines = NULL;
	size_t count = 0, cap = 0;
	char *buf = NULL;
	size_t bufc = 128;
	buf = malloc(bufc);

	for (;;) {
		if (!fgets(buf, (int)bufc, stdin)) break;

		size_t len = strlen(buf);
		/* If the buffer filled without a newline, the line is longer than
		 * bufc-1; grow and keep reading the remainder into the same buffer
		 * (we only keep the final assembled line via strdup below). */
		while (len > 0 && buf[len - 1] != '\n' && !feof(stdin)) {
			bufc *= 2;
			buf = realloc(buf, bufc);
			if (!fgets(buf + len, (int)(bufc - len), stdin)) break;
			len += strlen(buf + len);
		}

		if (len > 0 && buf[len - 1] == '\n') buf[--len] = '\0';
		if (len > 0 && buf[len - 1] == '\r') buf[--len] = '\0';
		if (len == 0) continue;

		if (count == cap) {
			cap = cap ? cap * 2 : 1024;
			lines = realloc(lines, cap * sizeof(char *));
		}
		lines[count++] = strdup(buf);
	}

	free(buf);
	*out_n = count;
	return lines;
}

int main(void) {
	size_t nlines;
	char **lines = read_all_lines(&nlines);
	if (nlines == 0) { fprintf(stderr, "No keys provided\n"); return 1; }

	size_t out_len = 0;
	uint8_t *blob = mph_build((const char * const *)lines, nlines, &out_len);
	if (!blob) { fprintf(stderr, "Failed to build MPHF\n"); return 1; }

	printf("/* Auto-generated minimal perfect hash (mph.h, blob form) */\n");
	printf("#include <stdio.h>\n");
	printf("#include <stdint.h>\n");
	printf("#include <stddef.h>\n");
	printf("#include <string.h>\n");
	printf("#include <inttypes.h>\n");
	printf("\n");
	printf("#include \"mph.h\"\n");
	printf("\n");
	printf("static const uint32_t mph_n = %zu; /* number of keys; ranks in [1..mph_n] */\n",
		   nlines);
	printf("\n");
	printf("static const uint8_t mph_blob[] = {\n");

	/* Emit exactly out_len bytes, wrapped. */
	for (size_t i = 0; i < out_len; i++) {
		printf("0x%02x,", blob[i]);
		if ((i % 16) == 15) printf("\n");
	}
	printf("\n};\n\n");

	printf("#ifdef TEST\n");
	printf("int main(void) {\n");
	printf("    char line[4096];\n");
	printf("    while (fgets(line, sizeof(line), stdin)) {\n");
	printf("        size_t len = strlen(line);\n");
	printf("        if (len > 0 && line[len-1] == '\\n') line[--len] = '\\0';\n");
	printf("        if (len > 0 && line[len-1] == '\\r') line[--len] = '\\0';\n");
	printf("        if (len == 0) continue;\n");
	printf("        uint64_t idx = mph_lookup(mph_blob, sizeof(mph_blob), line, len);\n");
	printf("        printf(\"%%s\\t%%\" PRIu64 \"\\n\", line, idx);\n");
	printf("    }\n");
	printf("    return 0;\n");
	printf("}\n");
	printf("#endif\n");

	free(blob);
	for (size_t i = 0; i < nlines; i++) free(lines[i]);
	free(lines);
	return 0;
}

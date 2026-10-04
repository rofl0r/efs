/*
 * jmph_test.c - self-check driver for the jmph.h minimal-perfect-hash
 * builder. Reads one key per line from stdin, builds the MPH via
 * jmph_build(), then prints "<key>\t<rank>" for every key, where rank
 * is 1-based (jmph_index() + 1). The test_*.sh harness verifies the
 * output is a permutation of 1..N (i.e. a valid minimal perfect hash).
 */
/* jmph.h now names `union mph_params` (the shared per-algorithm parameter
 * union) in its prototypes; include efs.h first so the type is defined. The
 * header itself stays inert here -- we only want the MPH builder API. */
#include "efs.h"
#define MPH_IMPL
#include "jmph.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(void){
    char **keys = NULL;
    uint32_t *kl = NULL;
    size_t n = 0, cap = 0;
    char buf[65536];

    while(fgets(buf, sizeof buf, stdin)){
        size_t L = strlen(buf);
        if(L && buf[L-1] == '\n') buf[--L] = 0;
        if(L && buf[L-1] == '\r') buf[--L] = 0;
        if(L == 0) continue;
        if(n == cap){
            cap = cap ? cap*2 : 1024;
            keys = realloc(keys, cap * sizeof(*keys));
            kl   = realloc(kl,   cap * sizeof(*kl));
        }
        keys[n] = strdup(buf);
        kl[n] = (uint32_t)L;
        n++;
    }
    if(n == 0){ fprintf(stderr, "no keys\n"); return 1; }

    struct jmph_in mi = { (uint32_t)n, (const char **)keys, kl };
    struct jmph_out mo;
    if(!jmph_build(&mi, &mo)){
        fprintf(stderr, "jmph_build failed (duplicate keys?)\n");
        return 1;
    }

    for(uint32_t i = 0; i < n; i++){
        uint32_t idx = jmph_index(&mo, (const uint8_t*)keys[i], kl[i]);
        if(idx >= n){
            fprintf(stderr, "jmph_index out of range for key %s\n", keys[i]);
            return 1;
        }
        printf("%s\t%u\n", keys[i], idx + 1);
    }

    free(mo.data);
    for(uint32_t i = 0; i < n; i++) free(keys[i]);
    free(keys); free(kl);
    return 0;
}

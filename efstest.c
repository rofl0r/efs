/*
 * efstest.c - end-to-end correctness runner for the EFS builder/reader.
 *
 * Two modes:
 *   efstest <dir>        use an existing directory tree (never deleted).
 *   efstest -n N [-s S]  generate a temporary directory of N files with
 *                        Markov-chain filenames (seeded by S), then test it.
 *
 * For the chosen tree it:
 *   1. builds an EFS image with efs_build_path() (the builder library),
 *   2. mmaps the image and reads every entry back with efs_lookup(),
 *   3. compares each file's contents byte-for-byte against the source,
 *   4. checks the recursive directory-listing (names + kinds) matches,
 *   5. confirms an absent name is reported "not found".
 *
 * Optional benchmark flags:
 *   --bench-stats  print build time, total MPH bytes, and whole-image bytes.
 *   --cold-cache   advise the kernel to evict source/image file data.
 *
 * Cleanup: a generated temporary directory is deleted on success and left in
 * place (path printed) on failure, for inspection. A user-supplied directory
 * is never deleted. Exit status 0 = pass, 1 = fail.
 *
 * The MPH implementation under test is whichever one efsbuilder was compiled
 * with (see MPH= in the Makefile), so `make check` runs this for both.
 */

#define EFS_IMPL
#include "efs.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <time.h>

/* ---- deterministic PRNG (xorshift64*) --------------------------------- */

static uint64_t g_rng;
static uint64_t rnd(void){
    uint64_t x = g_rng;
    x ^= x >> 12; x ^= x << 25; x ^= x >> 27;
    g_rng = x;
    return x * 2685821657736338717ULL;
}
static uint32_t rnd_below(uint32_t n){ return (uint32_t)(rnd() % n); }

/* ---- Markov filename generation --------------------------------------- */
/* Order-2 Markov chain over a pronounceable bigram model: the next char is
 * sampled from a transition row keyed by the previous char, weighted toward
 * common English letter pairs, so names look like plausible file names. */

static const char *VOWELS = "aaaeeeiiouy";
static const char *CONS   = "bbbcccdddffgghjklmmnnnprrrssstttvwxz";

static char markov_next(char prev){
    /* After a vowel favour a consonant and vice versa, with some noise, so
     * the chain alternates like real words instead of producing garbage. */
    int prev_vowel = strchr("aeiouy", prev) != NULL;
    uint32_t r = rnd_below(100);
    const char *pool;
    size_t plen;
    if (prev_vowel)      { pool = (r < 82) ? CONS   : VOWELS; }
    else                 { pool = (r < 82) ? VOWELS : CONS;   }
    plen = strlen(pool);
    return pool[rnd_below((uint32_t)plen)];
}

static void markov_name(char *out, size_t cap){
    size_t len = 3 + rnd_below(9);          /* 3..11 chars */
    if (len >= cap) len = cap - 1;
    char prev = CONS[rnd_below((uint32_t)strlen(CONS))];
    size_t i = 0;
    /* occasionally prefix a short directory-ish syllable for variety */
    for (; i < len; i++){
        out[i] = prev;
        prev = markov_next(prev);
    }
    out[i] = 0;
}

/* ---- small dynamic string set (for unique names + expected listing) --- */

struct strvec { char **v; uint32_t n, cap; };
static void sv_push(struct strvec *s, const char *str){
    if (s->n == s->cap){ s->cap = s->cap ? s->cap*2 : 64; s->v = realloc(s->v, s->cap*sizeof(char*)); }
    s->v[s->n++] = strdup(str);
}
static int sv_contains(const struct strvec *s, const char *str){
    for (uint32_t i = 0; i < s->n; i++) if (!strcmp(s->v[i], str)) return 1;
    return 0;
}
static void sv_free(struct strvec *s){ for (uint32_t i=0;i<s->n;i++) free(s->v[i]); free(s->v); s->v=0;s->n=s->cap=0; }

/* ---- generate a temp tree of N files (flat + a couple of subdirs) ------ */

static char g_tmpdir[4096];
static int g_generated = 0;

static int gen_tree(long n, unsigned seed){
    strcpy(g_tmpdir, "/tmp/efstest.XXXXXX");
    if (!mkdtemp(g_tmpdir)){ perror("mkdtemp"); return -1; }
    g_rng = seed ? seed : 0x9e3779b97f4a7c15ULL;
    if (!g_rng) g_rng = 1;

    /* a few subdirectories (including an empty one) to exercise recursion */
    char sub1[4096], sub2[4096], emptyd[4096];
    snprintf(sub1, sizeof sub1, "%s/alpha", g_tmpdir);   mkdir(sub1, 0755);
    snprintf(sub2, sizeof sub2, "%s/beta",  g_tmpdir);   mkdir(sub2, 0755);
    snprintf(emptyd,sizeof emptyd,"%s/emptydir", g_tmpdir); mkdir(emptyd, 0755);

    struct strvec used = {0};
    for (long i = 0; i < n; i++){
        char name[64];
        do { markov_name(name, sizeof name); } while (sv_contains(&used, name));
        sv_push(&used, name);
        /* spread ~80% at top level, ~10% in each subdir */
        char path[4096];
        uint32_t r = rnd_below(100);
        const char *dir = (r < 80) ? g_tmpdir : (r < 90 ? sub1 : sub2);
        snprintf(path, sizeof path, "%s/%s", dir, name);
        FILE *f = fopen(path, "wb");
        if (!f){ perror("fopen"); sv_free(&used); return -1; }
        /* pseudo-random content of pseudo-random length, derived from the
         * name so it is reproducible and checkable */
        size_t len = 1 + rnd_below(200);
        for (size_t k = 0; k < len; k++) fputc((int)(rnd() & 0xff), f);
        fclose(f);
    }
    sv_free(&used);
    g_generated = 1;
    return 0;
}

/* ---- recursive verify: walk source tree, check every file in image ----- */

static const struct efs_dir *g_root;
static uint32_t g_checked, g_failed;
static uint64_t g_hash_table_bytes;
static int g_bench_stats;
static int g_cold_cache;
#ifdef EFS_BENCH
extern uint64_t efs_mph_build_ns;
#endif

static uint64_t monotonic_ns(void){
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) return 0;
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

static void evict_dir(int fd){
    int iterfd = dup(fd);
    if (iterfd < 0) return;
    DIR *d = fdopendir(iterfd);
    if (!d){ close(iterfd); return; }
    struct dirent *de;
    while ((de = readdir(d))){
        if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) continue;
        int childfd = openat(dirfd(d), de->d_name,
                             O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
        if (childfd < 0) continue;
        struct stat st;
        if (fstat(childfd, &st) == 0){
            if (S_ISDIR(st.st_mode)) evict_dir(childfd);
            else if (S_ISREG(st.st_mode))
                (void)posix_fadvise(childfd, 0, 0, POSIX_FADV_DONTNEED);
        }
        close(childfd);
    }
    closedir(d);
}

static void evict_tree(const char *path){
    int fd = open(path, O_RDONLY | O_CLOEXEC | O_DIRECTORY);
    if (fd < 0) return;
    evict_dir(fd);
    close(fd);
}

static void evict_file(const char *path){
    int fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
    if (fd >= 0){
        struct stat st;
        if (fstat(fd, &st) == 0 && S_ISREG(st.st_mode))
            (void)posix_fadvise(fd, 0, 0, POSIX_FADV_DONTNEED);
        close(fd);
    }
}

static int cmp_str(const void *a, const void *b){ return strcmp(*(char*const*)a, *(char*const*)b); }

/* Verify one directory: recurse the source, and separately compare the
 * EFS readdir() listing against the source's entry set. */
static void verify_dir(const char *srcpath, const char *efspath){
    /* collect source entries (relative names) */
    struct strvec src = {0};
    DIR *d = opendir(srcpath);
    if (!d){ fprintf(stderr, "opendir %s: %s\n", srcpath, strerror(errno)); g_failed++; return; }
    struct dirent *de;
    while ((de = readdir(d))){
        if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) continue;
        sv_push(&src, de->d_name);
    }
    closedir(d);

    /* 1) per-file content check via efs_lookup */
    for (uint32_t i = 0; i < src.n; i++){
        char child_src[4096], child_efs[4096];
        snprintf(child_src, sizeof child_src, "%s/%s", srcpath, src.v[i]);
        snprintf(child_efs, sizeof child_efs, "%s/%s", efspath, src.v[i]);
        struct stat st;
        if (stat(child_src, &st) != 0){ continue; }
        if (S_ISDIR(st.st_mode)){
            verify_dir(child_src, child_efs);
            continue;
        }
        /* regular file: read source, compare with image */
        FILE *f = fopen(child_src, "rb");
        if (!f){ fprintf(stderr,"open %s failed\n", child_src); g_failed++; continue; }
        fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
        uint8_t *buf = malloc(sz ? sz : 1);
        if (fread(buf, 1, sz, f) != (size_t)sz){ /* tolerate */ }
        fclose(f);

        uint32_t len = 0; int is_dir = 0;
        const uint8_t *p = efs_lookup(g_root, child_efs, &len, &is_dir);
        if (!p){
            printf("MISS: %s\n", child_efs); g_failed++;
        } else if (is_dir){
            printf("TYPE: %s reported as dir, expected file\n", child_efs); g_failed++;
        } else if ((long)len != sz || memcmp(p, buf, sz) != 0){
            printf("MISMATCH: %s (len %u vs %ld)\n", child_efs, len, sz); g_failed++;
        } else {
            g_checked++;
        }
        free(buf);
    }

    /* 2) listing check: EFS readdir of the dir must equal the source set */
    uint32_t dl = 0; int ddir = 0;
    const uint8_t *dp = efs_lookup(g_root, efspath, &dl, &ddir);
    if (!dp || !ddir){ printf("DIR NOT FOUND: %s\n", efspath); g_failed++; sv_free(&src); return; }
    const struct efs_dir *sub = (const struct efs_dir*)dp;
    g_hash_table_bytes += efs_mph_bytes(sub->blen, sub->mph_params);
    struct strvec got = {0};
    uint32_t cur = 0; const char *nm;
    while ((nm = efs_readdir(sub, &cur))){
        const char *c = nm;
        if (c[0] == '/') c++;           /* strip dir marker for comparison */
        sv_push(&got, c);
    }
    if (got.n != src.n){
        printf("LISTING COUNT: %s has %u entries in image, %u on disk\n",
               efspath, got.n, src.n); g_failed++;
    } else {
        /* compare as sorted sets */
        qsort(src.v, src.n, sizeof(char*), cmp_str);
        qsort(got.v, got.n, sizeof(char*), cmp_str);
        for (uint32_t i = 0; i < src.n; i++){
            if (strcmp(src.v[i], got.v[i])){
                printf("LISTING DIFF: %s: image has '%s', disk has '%s'\n",
                       efspath, got.v[i], src.v[i]); g_failed++;
            }
        }
    }
    sv_free(&got);
    sv_free(&src);
}

/* ---- recursive delete (only ever called on our generated temp dir) ----- */

static void rm_rf(const char *path){
    DIR *d = opendir(path);
    if (d){
        struct dirent *de;
        while ((de = readdir(d))){
            if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) continue;
            char child[4096]; snprintf(child, sizeof child, "%s/%s", path, de->d_name);
            struct stat st;
            if (lstat(child, &st) == 0 && S_ISDIR(st.st_mode)) rm_rf(child);
            else unlink(child);
        }
        closedir(d);
    }
    rmdir(path);
}

int main(int argc, char **argv){
    const char *target = NULL;
    long n = -1; unsigned seed = 1;
    for (int i = 1; i < argc; i++){
        if (!strcmp(argv[i], "-n") && i+1 < argc) n = atol(argv[++i]);
        else if (!strcmp(argv[i], "-s") && i+1 < argc) seed = (unsigned)strtoul(argv[++i],0,0);
        else if (!strcmp(argv[i], "--bench-stats")) g_bench_stats = 1;
        else if (!strcmp(argv[i], "--cold-cache")) g_cold_cache = 1;
        else target = argv[i];
    }
    uint64_t run_start = monotonic_ns();

    if (n >= 0){
        if (gen_tree(n, seed) != 0) return 1;
        target = g_tmpdir;
    } else if (!target){
        fprintf(stderr, "usage: %s <dir> | -n N [-s seed]\n", argv[0]);
        return 1;
    }

    if (g_cold_cache) evict_tree(target);

    /* build the image */
    char img[4096]; snprintf(img, sizeof img, "%s.efs", g_generated ? g_tmpdir : "/tmp/efstest_img");
    uint64_t build_start = monotonic_ns();
    if (efs_build_path(target, img) != 0){
        fprintf(stderr, "efs_build_path failed for %s\n", target);
        if (g_generated) printf("kept temp dir for inspection: %s\n", g_tmpdir);
        return 1;
    }
    uint64_t build_ns = monotonic_ns() - build_start;

    /* mmap the image */
    int fd = open(img, O_RDONLY);
    if (fd < 0){ perror("open image"); return 1; }
    struct stat st; fstat(fd, &st);
    if (g_cold_cache){
        evict_tree(target);
        (void)posix_fadvise(fd, 0, 0, POSIX_FADV_DONTNEED);
    }
    uint8_t *base = mmap(0, st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    if (base == MAP_FAILED){ perror("mmap"); close(fd); return 1; }
    if (memcmp(base, EFS_MAGIC, 4) != 0){ fprintf(stderr,"bad magic\n"); return 1; }
    g_root = (const struct efs_dir*)(base + 4);

    /* verify */
    verify_dir(target, "/");

    /* absent name must be reported not-found */
    uint32_t l; int id;
    if (efs_lookup(g_root, "/definitely-not-a-real-name-zzz", &l, &id) != NULL){
        printf("FALSE POSITIVE: absent name resolved\n"); g_failed++;
    }

    if (g_bench_stats){
        double mph_pct = st.st_size ? 100.0 * (double)g_hash_table_bytes / (double)st.st_size : 0.0;
        printf("BENCH files=%ld seed=%u total_ns=%llu build_ns=%llu mph_ns=%llu mph_bytes=%llu image_bytes=%lld mph_pct=%.3f\n",
               n, seed, (unsigned long long)(monotonic_ns() - run_start),
               (unsigned long long)build_ns,
#ifdef EFS_BENCH
               (unsigned long long)efs_mph_build_ns,
#else
               0ULL,
#endif
               (unsigned long long)g_hash_table_bytes, (long long)st.st_size, mph_pct);
    }

    munmap(base, st.st_size);
    close(fd);
    unlink(img);

    int ok = (g_failed == 0);
    if (ok){
        if (g_generated) rm_rf(g_tmpdir);
        printf("PASS: %s (%u files verified, %u failures)\n",
               g_generated ? "generated" : target, g_checked, g_failed);
        return 0;
    }
    if (g_generated) printf("FAIL: kept temp dir for inspection: %s\n", g_tmpdir);
    printf("FAIL: %u failures (%u files checked)\n", g_failed, g_checked);
    return 1;
}

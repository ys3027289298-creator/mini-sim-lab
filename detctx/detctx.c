/*
 * detctx: shared deterministic context implementation (pthreads only,
 * no third-party dependencies).
 */
#include "detctx.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <pthread.h>

void detctx_usage(const char* prog) {
    printf(
        "usage: %s [options]\n"
        "  --seed=N         64-bit RNG seed, 0..18446744073709551615 (default 42)\n"
        "  --threads=N      worker threads, 1..%d (default 1)\n"
        "  --shared=on|off  shared deterministic context (default on)\n"
        "  -h, --help       show this help\n",
        prog, DETCTX_MAX_THREADS);
}

static int parse_u64(const char* s, uint64_t* out) {
    if (s == NULL || *s == '\0' || s[0] == '-') return -1;
    errno = 0;
    char* end = NULL;
    unsigned long long v = strtoull(s, &end, 10);
    if (errno != 0 || end == s || *end != '\0') return -1;
    *out = (uint64_t)v;
    return 0;
}

static int parse_i32(const char* s, int* out) {
    if (s == NULL || *s == '\0') return -1;
    errno = 0;
    char* end = NULL;
    long v = strtol(s, &end, 10);
    if (errno != 0 || end == s || *end != '\0') return -1;
    *out = (int)v;
    return 0;
}

/* Match "--name=value" (value required). Returns 1 on match, 0 otherwise;
   on a malformed match prints an error and returns -1. */
static int match_opt(const char* arg, const char* name, const char** value) {
    size_t nlen = strlen(name);
    if (strncmp(arg, name, nlen) != 0) return 0;
    if (arg[nlen] != '=') {
        fprintf(stderr, "error: %s requires '=' value (got '%s')\n", name, arg);
        return -1;
    }
    *value = arg + nlen + 1;
    return 1;
}

int detctx_parse(DetCtx* ctx, int argc, char** argv) {
    ctx->seed = DETCTX_DEFAULT_SEED;
    ctx->threads = DETCTX_DEFAULT_THREADS;
    ctx->shared = 1;

    for (int i = 1; i < argc; i++) {
        const char* arg = argv[i];
        const char* val = NULL;
        int m;

        if (strcmp(arg, "-h") == 0 || strcmp(arg, "--help") == 0) {
            detctx_usage(argv[0]);
            return 1;
        }

        if ((m = match_opt(arg, "--seed", &val)) != 0) {
            if (m < 0) return -1;
            if (parse_u64(val, &ctx->seed) != 0) {
                fprintf(stderr, "error: invalid --seed value '%s'\n", val);
                return -1;
            }
            continue;
        }
        if ((m = match_opt(arg, "--threads", &val)) != 0) {
            if (m < 0) return -1;
            if (parse_i32(val, &ctx->threads) != 0 ||
                ctx->threads < 1 || ctx->threads > DETCTX_MAX_THREADS) {
                fprintf(stderr,
                        "error: invalid --threads value '%s' (want 1..%d)\n",
                        val, DETCTX_MAX_THREADS);
                return -1;
            }
            continue;
        }
        if ((m = match_opt(arg, "--shared", &val)) != 0) {
            if (m < 0) return -1;
            if (strcmp(val, "on") == 0) {
                ctx->shared = 1;
            } else if (strcmp(val, "off") == 0) {
                ctx->shared = 0;
            } else {
                fprintf(stderr, "error: invalid --shared value '%s' (want on|off)\n", val);
                return -1;
            }
            continue;
        }

        fprintf(stderr, "error: unknown argument '%s'\n", arg);
        detctx_usage(argv[0]);
        return -1;
    }
    return 0;
}

uint64_t detctx_splitmix64(uint64_t* state) {
    uint64_t z = (*state += 0x9E3779B97F4A7C15ULL);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31);
}

uint64_t detctx_item_seed(const DetCtx* ctx, int item) {
    uint64_t st = ctx->seed ^ ((uint64_t)(item + 1) * 0x9E3779B97F4A7C15ULL);
    return detctx_splitmix64(&st);
}

uint32_t detctx_range(uint64_t* state, uint32_t n) {
    if (n == 0) return 0;
    uint32_t limit = UINT32_MAX - (UINT32_MAX % n);
    uint32_t v;
    do {
        v = (uint32_t)detctx_splitmix64(state);
    } while (v >= limit);
    return v % n;
}

struct work {
    void (*fn)(int item, void* arg);
    void* arg;
    int lo;
    int hi;
};

static void* worker_main(void* p) {
    struct work* w = (struct work*)p;
    for (int i = w->lo; i < w->hi; i++) w->fn(i, w->arg);
    return NULL;
}

int detctx_parallel_for(const DetCtx* ctx, int count,
                        void (*fn)(int item, void* arg), void* arg) {
    if (count <= 0) return 0;

    int n = ctx->threads < count ? ctx->threads : count;
    pthread_t th[DETCTX_MAX_THREADS];
    struct work jobs[DETCTX_MAX_THREADS];

    for (int t = 0; t < n; t++) {
        jobs[t].fn = fn;
        jobs[t].arg = arg;
        jobs[t].lo = (int)((long long)count * t / n);
        jobs[t].hi = (int)((long long)count * (t + 1) / n);
        int rc = pthread_create(&th[t], NULL, worker_main, &jobs[t]);
        if (rc != 0) {
            /* join what we started, then fail loudly rather than run
               nondeterministically on a partial worker set */
            for (int k = 0; k < t; k++) pthread_join(th[k], NULL);
            fprintf(stderr, "error: pthread_create failed: %s\n", strerror(rc));
            return -1;
        }
    }
    for (int t = 0; t < n; t++) pthread_join(th[t], NULL);
    return 0;
}

uint64_t detctx_checksum(const void* data, size_t len) {
    const unsigned char* p = (const unsigned char*)data;
    uint64_t h = 0xcbf29ce484222325ULL;
    for (size_t i = 0; i < len; i++) {
        h ^= p[i];
        h *= 0x100000001b3ULL;
    }
    return h;
}

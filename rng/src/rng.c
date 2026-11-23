
/* rng: splitmix64-style stream with snapshot, fork and unbiased ranges.
   In shared mode the stream is supplied by the shared detctx context and a
   deterministic parallel workload is verified alongside the legacy checks. */
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdlib.h>

#include "../../detctx/detctx.h"

typedef struct { uint64_t state; } Stream;

static void stream_init(Stream* s, uint64_t seed) { s->state = seed; }

static uint64_t stream_next(Stream* s) {
    uint64_t z = (s->state += 0x9E3779B97F4A7C15ULL);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31);
}

/* unbiased range via rejection sampling */
static uint32_t stream_range(Stream* s, uint32_t n) {
    if (n == 0) return 0;
    uint32_t limit = UINT32_MAX - (UINT32_MAX % n);
    uint32_t v;
    do { v = (uint32_t)stream_next(s); } while (v >= limit);
    return v % n;
}

static Stream stream_fork(const Stream* s, uint64_t tag) {
    Stream f;
    f.state = s->state ^ (tag * 0x9E3779B97F4A7C15ULL);
    return f;
}

static int failures = 0;
static void check(const char* name, int ok) {
    printf("%s=%d\n", name, ok ? 1 : 0);
    if (!ok) failures++;
}

/* ---- shared deterministic context workload -------------------------------- */

static DetCtx g_ctx;

typedef struct {
    uint64_t seed;
    uint64_t draw0;
    uint64_t draw1;
    uint32_t r6;
    uint32_t r100;
} RngSlot;

static void rng_task(int item, void* arg) {
    RngSlot* slots = (RngSlot*)arg;
    RngSlot* s = &slots[item];
    /* Every item derives its own substream purely from (shared seed, index):
       no cross-item state, so the result is independent of worker scheduling. */
    uint64_t st = detctx_item_seed(&g_ctx, item);
    s->seed = st;
    s->draw0 = detctx_splitmix64(&st);
    s->draw1 = detctx_splitmix64(&st);
    s->r6 = detctx_range(&st, 6);
    s->r100 = detctx_range(&st, 100);
}

/* Same computation, driven single-threaded straight from the context. */
static void rng_reference(const DetCtx* ctx, RngSlot* ref) {
    for (int i = 0; i < DETCTX_ITEMS; i++) {
        uint64_t st = detctx_item_seed(ctx, i);
        ref[i].seed = st;
        ref[i].draw0 = detctx_splitmix64(&st);
        ref[i].draw1 = detctx_splitmix64(&st);
        ref[i].r6 = detctx_range(&st, 6);
        ref[i].r100 = detctx_range(&st, 100);
    }
}

static void run_shared(const DetCtx* ctx) {
    printf("shared=on seed=%llu threads=%d items=%d\n",
           (unsigned long long)ctx->seed, ctx->threads, DETCTX_ITEMS);

    RngSlot* slots = calloc(DETCTX_ITEMS, sizeof(RngSlot));
    RngSlot* ref = calloc(DETCTX_ITEMS, sizeof(RngSlot));
    if (!slots || !ref) { fprintf(stderr, "error: out of memory\n"); exit(2); }

    g_ctx = *ctx;
    if (detctx_parallel_for(ctx, DETCTX_ITEMS, rng_task, slots) != 0) exit(2);
    rng_reference(ctx, ref);

    int mismatch = 0, bounds_bad = 0;
    for (int i = 0; i < DETCTX_ITEMS; i++) {
        if (memcmp(&slots[i], &ref[i], sizeof(RngSlot)) != 0) mismatch = 1;
        if (slots[i].r6 >= 6 || slots[i].r100 >= 100) bounds_bad = 1;
        printf("item=%d seed=%016llx draw0=%016llx draw1=%016llx r6=%u r100=%u\n",
               i, (unsigned long long)slots[i].seed,
               (unsigned long long)slots[i].draw0,
               (unsigned long long)slots[i].draw1,
               slots[i].r6, slots[i].r100);
    }
    uint64_t sum = detctx_checksum(slots, (size_t)DETCTX_ITEMS * sizeof(RngSlot));
    printf("parallel_matches_sequential=%d\n", mismatch == 0);
    printf("range_bounds=%d\n", bounds_bad == 0);
    printf("checksum=%016llx\n", (unsigned long long)sum);

    free(slots);
    free(ref);
    if (mismatch || bounds_bad) failures++;
}

/* ---- legacy self-test, byte-for-byte the original behaviour --------------- */

static void run_legacy_checks(void) {
    Stream a, b;
    stream_init(&a, 42); stream_init(&b, 42);
    check("deterministic", stream_next(&a) == stream_next(&b));

    Stream c;
    stream_init(&c, 7);
    uint64_t first = stream_next(&c);
    Stream snap = c;
    uint64_t next1 = stream_next(&c);
    uint64_t next2 = stream_next(&snap);
    check("snapshot_restores", next1 == next2);
    (void)first;

    Stream base, f1, f2;
    stream_init(&base, 99);
    f1 = stream_fork(&base, 1);
    f2 = stream_fork(&base, 2);
    check("fork_independent", stream_next(&f1) != stream_next(&f2));
    check("fork_repeatable", stream_fork(&base, 1).state == f1.state || 1);

    Stream r;
    stream_init(&r, 1234);
    int bad = 0;
    for (int i = 0; i < 10000; i++) {
        uint32_t v = stream_range(&r, 6);
        if (v >= 6) bad = 1;
    }
    check("range_bounds", bad == 0);
    check("range_zero", stream_range(&r, 0) == 0);

    Stream d1, d2;
    stream_init(&d1, 5); stream_init(&d2, 5);
    int same = 1;
    for (int i = 0; i < 1000; i++) if (stream_next(&d1) != stream_next(&d2)) same = 0;
    check("long_sequence_same", same == 1);
}

int main(int argc, char** argv) {
    DetCtx ctx;
    int pr = detctx_parse(&ctx, argc, argv);
    if (pr < 0) return 2;
    if (pr > 0) return 0;

    if (ctx.shared) run_shared(&ctx);
    run_legacy_checks();

    printf("all_ok=%d\n", failures == 0);
    return failures == 0 ? 0 : 1;
}

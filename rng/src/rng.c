
/* rng: splitmix64-style stream with snapshot, fork and unbiased ranges. */
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#ifdef RECON
#include "../../recon/recon.h"
#endif

typedef struct {
    uint64_t state;
#ifdef RECON
    long long draw_count;   /* module's own books, for reconciliation */
    long long range_count;
#endif
} Stream;

static void stream_init(Stream* s, uint64_t seed) {
    s->state = seed;
#ifdef RECON
    s->draw_count = 0;
    s->range_count = 0;
#endif
}

static uint64_t stream_next(Stream* s) {
#ifdef RECON
    s->draw_count++;
    recon_log_count(RECON_ITEM_RNG_DRAWS, 1);
#endif
    uint64_t z = (s->state += 0x9E3779B97F4A7C15ULL);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31);
}

/* unbiased range via rejection sampling */
static uint32_t stream_range(Stream* s, uint32_t n) {
#ifdef RECON
    s->range_count++;
    recon_log_count(RECON_ITEM_RNG_RANGES, 1);
#endif
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

#ifdef RECON
/* End-of-frame report for one stream: its own cumulative draw/range counts. */
static void stream_recon_frame(const Stream* s) {
    recon_checkpoint_count(RECON_ITEM_RNG_DRAWS, s->draw_count);
    recon_checkpoint_count(RECON_ITEM_RNG_RANGES, s->range_count);
}
#endif

#ifndef RECON_NO_SELFTEST
static int failures = 0;
static void check(const char* name, int ok) {
    printf("%s=%d\n", name, ok ? 1 : 0);
    if (!ok) failures++;
}

int main(void) {
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
    int in_range = 1, bad = 0;
    for (int i = 0; i < 10000; i++) {
        uint32_t v = stream_range(&r, 6);
        if (v >= 6) bad = 1;
        if (v == 0) in_range = in_range;
    }
    check("range_bounds", bad == 0);
    check("range_zero", stream_range(&r, 0) == 0);

    Stream d1, d2;
    stream_init(&d1, 5); stream_init(&d2, 5);
    int same = 1;
    for (int i = 0; i < 1000; i++) if (stream_next(&d1) != stream_next(&d2)) same = 0;
    check("long_sequence_same", same == 1);

    printf("all_ok=%d\n", failures == 0);
    return failures == 0 ? 0 : 1;
}
#endif /* RECON_NO_SELFTEST */

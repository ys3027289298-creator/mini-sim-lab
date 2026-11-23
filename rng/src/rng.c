
/* rng: splitmix64-style stream with snapshot, fork and unbiased ranges. */
#include "rng.h"
#include <string.h>

void stream_init(Stream* s, uint64_t seed) { s->state = seed; }

uint64_t stream_next(Stream* s) {
    uint64_t z = (s->state += 0x9E3779B97F4A7C15ULL);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31);
}

/* unbiased range via rejection sampling */
uint32_t stream_range(Stream* s, uint32_t n) {
    if (n == 0) return 0;
    uint32_t limit = UINT32_MAX - (UINT32_MAX % n);
    uint32_t v;
    do { v = (uint32_t)stream_next(s); } while (v >= limit);
    return v % n;
}

Stream stream_fork(const Stream* s, uint64_t tag) {
    Stream f;
    f.state = s->state ^ (tag * 0x9E3779B97F4A7C15ULL);
    return f;
}

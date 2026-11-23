/* rng: splitmix64-style stream with snapshot, fork and unbiased ranges. */
#ifndef RNG_H
#define RNG_H

#include <stdint.h>

typedef struct { uint64_t state; } Stream;

void stream_init(Stream* s, uint64_t seed);
uint64_t stream_next(Stream* s);

/* Uniform in [0, n) by rejection, never modulo-biased.  n == 0 returns 0. */
uint32_t stream_range(Stream* s, uint32_t n);

/* Derives an independent stream from the parent at its current position. */
Stream stream_fork(const Stream* s, uint64_t tag);

#endif /* RNG_H */

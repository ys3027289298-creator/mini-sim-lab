
/* rng: splitmix64-style stream with snapshot, fork and unbiased ranges. */
#ifndef RNG_H
#define RNG_H

#include <stdint.h>

typedef struct { uint64_t state; } Stream;

void stream_init(Stream* s, uint64_t seed);
uint64_t stream_next(Stream* s);

/* unbiased range via rejection sampling */
uint32_t stream_range(Stream* s, uint32_t n);

Stream stream_fork(const Stream* s, uint64_t tag);

#endif /* RNG_H */

#ifndef RNG_H
#define RNG_H

/*
 * rng: splitmix64-style stream with snapshot, fork and unbiased ranges.
 *
 * Determinism contract: every operation is a pure function of the
 * stream state, so identical seeds produce identical sequences on every
 * platform and thread. Snapshot/fork let a simulation give every entity
 * a private substream keyed by a stable id; no stream is ever shared
 * across threads, which keeps replay output independent of thread count.
 */

#include <stdint.h>

typedef struct { uint64_t state; } Stream;

void stream_init(Stream* s, uint64_t seed);
uint64_t stream_next(Stream* s);

/* unbiased range via rejection sampling; n == 0 returns 0 */
uint32_t stream_range(Stream* s, uint32_t n);
Stream stream_fork(const Stream* s, uint64_t tag);

#endif /* RNG_H */

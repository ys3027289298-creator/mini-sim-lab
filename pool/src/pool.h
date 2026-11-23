#ifndef POOL_H
#define POOL_H

/*
 * pool: fixed-capacity object pool with generation handles, so dangling
 * handles are detected instead of silently reused.
 *
 * Determinism contract:
 *   - allocation always returns the lowest free slot, free just marks a
 *     slot free and bumps its generation; both are pure index logic with
 *     no randomness or pointer dependence,
 *   - a handle therefore says nothing about entity identity across
 *     reuse. Deterministic simulations must bind per-entity state (e.g.
 *     RNG substreams) to a stable spawn id, never to handle.index,
 *   - when pools are touched from worker threads the caller is
 *     responsible for making the logical operation order independent of
 *     scheduling (stable slot iteration, staged free list). The pool
 *     itself adds no nondeterminism.
 */

#define POOL_CAP 16

typedef struct {
    unsigned int index;
    unsigned int gen;
} Handle;

typedef struct {
    int in_use[POOL_CAP];
    unsigned int gen[POOL_CAP];
    int value[POOL_CAP];
    int used;
    int high_water;
} Pool;

void pool_init(Pool* p);
Handle pool_alloc(Pool* p, int v);
int pool_valid(const Pool* p, Handle h);
int pool_get(const Pool* p, Handle h, int* out);
int pool_free(Pool* p, Handle h);

#endif /* POOL_H */

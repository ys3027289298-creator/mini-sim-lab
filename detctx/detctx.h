/*
 * detctx: deterministic execution context shared by fixedstep / pool / rng.
 *
 * One context carries the whole reproducible input:
 *   - seed    : 64-bit splitmix64 seed
 *   - threads : worker count for the deterministic parallel-for
 *   - shared  : whether shared-context mode is on
 *
 * Determinism contract:
 *   - The RNG is a pure function of (seed, draw index); it never reads
 *     clocks, addresses or thread state.
 *   - detctx_parallel_for splits [0,count) into contiguous ranges and runs
 *     them on pthreads. Each item's result depends only on (ctx, item index),
 *     never on scheduling: workers hold private state, share no mutable data,
 *     and the caller reads the slots in index order. Therefore the output is
 *     identical for any thread count (including 1 and oversubscription).
 */
#ifndef DETCTX_H
#define DETCTX_H

#include <stdint.h>
#include <stddef.h>

#define DETCTX_DEFAULT_SEED    42ULL
#define DETCTX_DEFAULT_THREADS 1
#define DETCTX_MAX_THREADS     256
#define DETCTX_ITEMS           128

typedef struct {
    uint64_t seed;
    int      threads;
    int      shared;
} DetCtx;

/* Parse argv into *ctx. Defaults: seed=42, threads=1, shared=1.
   Recognised: --seed=N (0..2^64-1), --threads=N (1..256),
               --shared=on|off, -h/--help.
   On success returns 0. On invalid input prints "error: ..." to stderr and
   returns -1; --help prints usage to stdout and returns 1. */
int detctx_parse(DetCtx* ctx, int argc, char** argv);
void detctx_usage(const char* prog);

/* splitmix64: same constants used by rng/, so all three modules consume the
   exact same deterministic stream. */
uint64_t detctx_splitmix64(uint64_t* state);

/* Per-item deterministic substream: forking by index means every item draws
   from the same sequence regardless of which worker happens to run it. */
uint64_t detctx_item_seed(const DetCtx* ctx, int item);

/* Unbiased uint32 in [0,n); n==0 returns 0. */
uint32_t detctx_range(uint64_t* state, uint32_t n);

/* Deterministic parallel-for. fn(item, arg) is invoked once per item in
   [0,count), spread over ctx->threads pthreads with contiguous ranges; the
   main thread joins all workers before returning. Returns 0 on success,
   -1 if a pthread could not be created. */
int detctx_parallel_for(const DetCtx* ctx, int count,
                        void (*fn)(int item, void* arg), void* arg);

/* FNV-1a 64-bit checksum over the slot bytes; every module folds its output
   slots through this so acceptance can compare one hash per run. */
uint64_t detctx_checksum(const void* data, size_t len);

#endif

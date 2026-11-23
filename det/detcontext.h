/*
 * detcontext: shared deterministic context for mini-sim-lab.
 *
 * Owns the seed every module derives its streams from, a deterministic
 * bump allocator for module state, a bit-exact 64-bit hash used by the
 * reproducibility evidence, an instance snapshot format, and a small
 * parallel-for helper used by the thread-independence tables.
 *
 * Nothing here reads the wall clock, the environment, or the address of an
 * allocation.  Modules keep their own state layout; this library only
 * carries identity (name, seed) and bytes.
 *
 * Errors are reported through (err, errcap) buffers.  Every module-facing
 * helper takes the module name so the message starts with it, e.g.
 * "rng: draws: expected an unsigned integer, got 'x'" -- callers can always
 * tell which module rejected the input.
 */
#ifndef DETCONTEXT_H
#define DETCONTEXT_H

#include <stddef.h>
#include <stdint.h>

#define DET_MAX_UNITS 4
#define DET_NAME_MAX 32
#define DET_MAX_THREADS 8
#define DET_INSTANCE_MAX (64u * 1024u)

typedef enum {
    DET_OK = 0,
    DET_ERR_ARG = 2,    /* bad caller input, message is module-located */
    DET_ERR_IO = 3,
    DET_ERR_FORMAT = 4,
    DET_ERR_STATE = 5,
    DET_ERR_NOMEM = 6
} DetStatus;

typedef struct DetContext DetContext;

/* Lifecycle.  The context itself is heap allocated; module state inside it
 * comes from its internal deterministic arena. */
DetContext* det_context_create(uint64_t seed);
void det_context_destroy(DetContext* ctx);
uint64_t det_context_seed(const DetContext* ctx);

/* Open (or fetch) a named unit.  Returns a zeroed, 16-byte-aligned state
 * block of `size` bytes from the context arena, or NULL with `err` set.
 * Opening the same name twice with the same size returns the same block. */
void* det_unit_open(DetContext* ctx, const char* unit, size_t size,
                    char* err, size_t errcap);
void* det_unit_state(DetContext* ctx, const char* unit);
/* Size of a live unit's state block, or 0 if the unit is not open. */
size_t det_unit_size(DetContext* ctx, const char* unit);
uint64_t det_unit_seed(DetContext* ctx, const char* unit);

/* Deterministic arena.  Allocation order and zeroing are fixed; no address
 * is ever fed into a digest. */
void* det_alloc(DetContext* ctx, size_t bytes);
void det_alloc_reset(DetContext* ctx);

/* Error helper: always prefixes "<unit>: ".  Returns DET_ERR_ARG so callers
 * can `return det_errf(...)`. */
int det_errf(char* err, size_t cap, const char* unit, const char* fmt, ...);

/* Deterministic primitives. */
uint64_t det_mix64(uint64_t x);                  /* splitmix64 finalizer */
uint64_t det_unit_seed_raw(uint64_t seed, const char* unit);

typedef struct { uint64_t h; } DetHash;          /* FNV-1a 64 */
void det_hash_init(DetHash* h);
void det_hash_u32(DetHash* h, uint32_t v);       /* canonical little-endian */
void det_hash_u64(DetHash* h, uint64_t v);       /* canonical little-endian */
void det_hash_i64(DetHash* h, int64_t v);
void det_hash_double(DetHash* h, double v);      /* exact bit pattern */
void det_hash_bytes(DetHash* h, const void* p, size_t n);
void det_hash_str(DetHash* h, const char* s);
uint64_t det_hash_value(const DetHash* h);

/* Canonical little-endian codec for module state images.  Never store a
 * struct image raw: padding bytes are not defined and would make two runs
 * with equal fields hash differently. */
void det_store_u32(unsigned char* p, uint32_t v);
void det_store_u64(unsigned char* p, uint64_t v);
void det_store_double(unsigned char* p, double v);
uint32_t det_load_u32(const unsigned char* p);
uint64_t det_load_u64(const unsigned char* p);
double det_load_double(const unsigned char* p);
uint64_t det_hash_image(const unsigned char* p, size_t n);

/* Argument parsing with module-located error messages.  All reject trailing
 * garbage and out-of-range values. */
int det_parse_u64(const char* unit, const char* field, const char* s,
                  uint64_t* out, char* err, size_t cap);
int det_parse_long(const char* unit, const char* field, const char* s,
                   long lo, long hi, long* out, char* err, size_t cap);
int det_parse_double(const char* unit, const char* field, const char* s,
                     double* out, char* err, size_t cap);
int det_parse_hex64(const char* unit, const char* field, const char* s,
                    uint64_t* out, char* err, size_t cap);
/* "1,2,4,8" -> strictly ascending thread counts, each in [1, DET_MAX_THREADS]. */
int det_parse_threads(const char* unit, const char* spec, int* out, int max,
                      int* count, char* err, size_t cap);

/* Run njobs jobs across nthreads, writing only job-indexed slots.  The caller
 * folds slots in index order, so output does not depend on which thread ran
 * which job.  Returns 0, or -1 if a thread could not be started. */
typedef void (*DetJobFn)(void* arg, int job);
int det_parallel_for(int nthreads, int njobs, DetJobFn fn, void* arg,
                     const char* unit, char* err, size_t cap);

/* Instance snapshot: little-endian, records name/size/seed/state per unit,
 * with a trailing FNV-1a checksum over all preceding bytes.  `load` refuses
 * files whose expected unit is absent and reports the unit names in the
 * error, so a wrong pile of state is caught by module, not by luck. */
int det_context_save(DetContext* ctx, const char* unit, const char* path,
                     char* err, size_t cap);
int det_context_load(DetContext* ctx, const char* path, const char* expected_unit,
                     char* err, size_t cap);

#endif /* DETCONTEXT_H */

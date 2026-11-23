/* detcontext implementation: see detcontext.h for the contract. */
#include "detcontext.h"

#include <errno.h>
#include <limits.h>
#include <math.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DET_ARENA_MAX (256u * 1024u)
#define DET_ALIGN 16u

typedef struct {
    char name[DET_NAME_MAX];
    unsigned char* state;
    size_t size;
    int used;
} Unit;

struct DetContext {
    uint64_t seed;
    unsigned char* arena;
    size_t arena_used;
    Unit units[DET_MAX_UNITS];
    int nunits;
};

/* ---- lifecycle ---------------------------------------------------------- */

DetContext* det_context_create(uint64_t seed) {
    DetContext* ctx = (DetContext*)calloc(1, sizeof(*ctx));
    if (!ctx) return NULL;
    ctx->arena = (unsigned char*)calloc(1, DET_ARENA_MAX);
    if (!ctx->arena) { free(ctx); return NULL; }
    ctx->seed = seed;
    return ctx;
}

void det_context_destroy(DetContext* ctx) {
    if (!ctx) return;
    free(ctx->arena);
    free(ctx);
}

uint64_t det_context_seed(const DetContext* ctx) { return ctx->seed; }

/* ---- arena -------------------------------------------------------------- */

void det_alloc_reset(DetContext* ctx) {
    if (!ctx) return;
    ctx->arena_used = 0;
    ctx->nunits = 0;
    memset(ctx->arena, 0, DET_ARENA_MAX);
}

void* det_alloc(DetContext* ctx, size_t bytes) {
    if (!ctx) return NULL;
    size_t off = (ctx->arena_used + (DET_ALIGN - 1)) & ~(size_t)(DET_ALIGN - 1);
    if (bytes > DET_ARENA_MAX - off) return NULL;
    void* p = ctx->arena + off;
    ctx->arena_used = off + bytes;
    memset(p, 0, bytes);   /* deterministic zeroing, never stale bytes */
    return p;
}

/* ---- units -------------------------------------------------------------- */

static Unit* find_unit(DetContext* ctx, const char* name) {
    for (int i = 0; i < ctx->nunits; i++)
        if (strcmp(ctx->units[i].name, name) == 0) return &ctx->units[i];
    return NULL;
}

void* det_unit_open(DetContext* ctx, const char* unit, size_t size,
                    char* err, size_t cap) {
    if (!ctx || !unit || size == 0) {
        det_errf(err, cap, unit ? unit : "detcontext", "invalid unit open request");
        return NULL;
    }
    if (strlen(unit) >= DET_NAME_MAX) {
        det_errf(err, cap, "detcontext", "unit name '%.24s...' exceeds %d bytes",
                 unit, DET_NAME_MAX - 1);
        return NULL;
    }
    Unit* u = find_unit(ctx, unit);
    if (u) {
        if (u->size != size) {
            det_errf(err, cap, unit, "reopened with size %zu, already live with %zu",
                     size, u->size);
            return NULL;
        }
        return u->state;
    }
    if (ctx->nunits == DET_MAX_UNITS) {
        det_errf(err, cap, "detcontext", "unit capacity %d exhausted opening '%s'",
                 DET_MAX_UNITS, unit);
        return NULL;
    }
    void* p = det_alloc(ctx, size);
    if (!p) {
        det_errf(err, cap, unit, "state block of %zu bytes exceeds arena", size);
        return NULL;
    }
    u = &ctx->units[ctx->nunits++];
    snprintf(u->name, sizeof(u->name), "%s", unit);
    u->state = (unsigned char*)p;
    u->size = size;
    u->used = 1;
    return p;
}

void* det_unit_state(DetContext* ctx, const char* unit) {
    if (!ctx || !unit) return NULL;
    Unit* u = find_unit(ctx, unit);
    return u ? u->state : NULL;
}

size_t det_unit_size(DetContext* ctx, const char* unit) {
    if (!ctx || !unit) return 0;
    Unit* u = find_unit(ctx, unit);
    return u ? u->size : 0;
}

uint64_t det_unit_seed(DetContext* ctx, const char* unit) {
    if (!ctx || !unit) return 0;
    return det_unit_seed_raw(ctx->seed, unit);
}

/* ---- errors ------------------------------------------------------------- */

int det_errf(char* err, size_t cap, const char* unit, const char* fmt, ...) {
    if (!err || cap == 0) return DET_ERR_ARG;
    int n = snprintf(err, cap, "%s: ", unit ? unit : "detcontext");
    if (n < 0 || (size_t)n >= cap) return DET_ERR_ARG;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(err + n, cap - (size_t)n, fmt, ap);
    va_end(ap);
    return DET_ERR_ARG;
}

/* ---- hashing ------------------------------------------------------------ */

uint64_t det_mix64(uint64_t x) {
    x += 0x9E3779B97F4A7C15ULL;
    x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ULL;
    x = (x ^ (x >> 27)) * 0x94D049BB133111EBULL;
    return x ^ (x >> 31);
}

uint64_t det_unit_seed_raw(uint64_t seed, const char* unit) {
    DetHash h;
    det_hash_init(&h);
    det_hash_u64(&h, seed);
    det_hash_str(&h, unit);
    return det_mix64(det_hash_value(&h));
}

void det_hash_init(DetHash* h) { h->h = 0xCBF29CE484222325ULL; }

void det_hash_bytes(DetHash* h, const void* p, size_t n) {
    const unsigned char* b = (const unsigned char*)p;
    uint64_t x = h->h;
    for (size_t i = 0; i < n; i++) {
        x ^= b[i];
        x *= 0x100000001B3ULL;
    }
    h->h = x;
}

/* Canonical byte order: little-endian regardless of host, so digests from
 * different architectures can be compared directly. */
void det_hash_u32(DetHash* h, uint32_t v) {
    unsigned char b[4];
    for (int i = 0; i < 4; i++) b[i] = (unsigned char)(v >> (8 * i));
    det_hash_bytes(h, b, 4);
}

void det_hash_u64(DetHash* h, uint64_t v) {
    unsigned char b[8];
    for (int i = 0; i < 8; i++) b[i] = (unsigned char)(v >> (8 * i));
    det_hash_bytes(h, b, 8);
}

void det_hash_i64(DetHash* h, int64_t v) { det_hash_u64(h, (uint64_t)v); }

void det_hash_double(DetHash* h, double v) {
    uint64_t bits;
    memcpy(&bits, &v, sizeof(bits));   /* exact pattern; NaN/-0 preserved */
    det_hash_u64(h, bits);
}

void det_hash_str(DetHash* h, const char* s) {
    det_hash_bytes(h, s, strlen(s));
    det_hash_u64(h, 0x00u);            /* length-independent separator */
}

uint64_t det_hash_value(const DetHash* h) { return h->h; }

void det_store_u32(unsigned char* p, uint32_t v) {
    for (int i = 0; i < 4; i++) p[i] = (unsigned char)(v >> (8 * i));
}
void det_store_u64(unsigned char* p, uint64_t v) {
    for (int i = 0; i < 8; i++) p[i] = (unsigned char)(v >> (8 * i));
}
void det_store_double(unsigned char* p, double v) {
    uint64_t bits;
    memcpy(&bits, &v, sizeof(bits));
    det_store_u64(p, bits);
}
uint32_t det_load_u32(const unsigned char* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}
uint64_t det_load_u64(const unsigned char* p) {
    uint64_t v = 0;
    for (int i = 7; i >= 0; i--) v = (v << 8) | p[i];
    return v;
}
double det_load_double(const unsigned char* p) {
    uint64_t bits = det_load_u64(p);
    double v;
    memcpy(&v, &bits, sizeof(v));
    return v;
}
uint64_t det_hash_image(const unsigned char* p, size_t n) {
    DetHash h;
    det_hash_init(&h);
    det_hash_bytes(&h, p, n);
    return det_hash_value(&h);
}

/* ---- parsing ------------------------------------------------------------ */

int det_parse_u64(const char* unit, const char* field, const char* s,
                  uint64_t* out, char* err, size_t cap) {
    if (!s || !*s)
        return det_errf(err, cap, unit, "%s: expected an unsigned integer, got empty", field);
    if (*s == '-' || *s == '+')
        return det_errf(err, cap, unit, "%s: expected an unsigned integer, got '%s'", field, s);
    errno = 0;
    char* end = NULL;
    unsigned long long v = strtoull(s, &end, 0);
    if (end == s || *end != '\0' || errno == ERANGE)
        return det_errf(err, cap, unit, "%s: expected an unsigned integer, got '%s'", field, s);
    *out = (uint64_t)v;
    return DET_OK;
}

int det_parse_long(const char* unit, const char* field, const char* s,
                   long lo, long hi, long* out, char* err, size_t cap) {
    if (!s || !*s)
        return det_errf(err, cap, unit, "%s: expected an integer, got empty", field);
    errno = 0;
    char* end = NULL;
    long v = strtol(s, &end, 0);
    if (end == s || *end != '\0' || errno == ERANGE)
        return det_errf(err, cap, unit, "%s: expected an integer, got '%s'", field, s);
    if (v < lo || v > hi)
        return det_errf(err, cap, unit, "%s: %ld out of range [%ld, %ld]", field, v, lo, hi);
    *out = v;
    return DET_OK;
}

int det_parse_double(const char* unit, const char* field, const char* s,
                     double* out, char* err, size_t cap) {
    if (!s || !*s)
        return det_errf(err, cap, unit, "%s: expected a number, got empty", field);
    errno = 0;
    char* end = NULL;
    double v = strtod(s, &end);
    if (end == s || *end != '\0')
        return det_errf(err, cap, unit, "%s: expected a number, got '%s'", field, s);
    if (!isfinite(v))
        return det_errf(err, cap, unit, "%s: expected a finite number, got '%s'", field, s);
    *out = v;
    return DET_OK;
}

static int hexval(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

int det_parse_hex64(const char* unit, const char* field, const char* s,
                    uint64_t* out, char* err, size_t cap) {
    if (!s) return det_errf(err, cap, unit, "%s: expected 16 hex digits, got null", field);
    size_t n = strlen(s);
    if (n == 0 || n > 16)
        return det_errf(err, cap, unit, "%s: expected 1..16 hex digits, got '%s'", field, s);
    uint64_t v = 0;
    for (size_t i = 0; i < n; i++) {
        int d = hexval(s[i]);
        if (d < 0)
            return det_errf(err, cap, unit, "%s: expected hex digits, got '%s'", field, s);
        v = (v << 4) | (uint64_t)d;
    }
    *out = v;
    return DET_OK;
}

int det_parse_threads(const char* unit, const char* spec, int* out, int max,
                      int* count, char* err, size_t cap) {
    if (!spec || !*spec)
        return det_errf(err, cap, unit, "threads: expected a list like 1,2,4, got empty");
    int n = 0;
    const char* p = spec;
    while (*p) {
        if (n == max)
            return det_errf(err, cap, unit, "threads: too many entries in '%s'", spec);
        char* end = NULL;
        errno = 0;
        long v = strtol(p, &end, 10);
        if (end == p || errno == ERANGE)
            return det_errf(err, cap, unit, "threads: expected a list like 1,2,4, got '%s'", spec);
        if (v < 1 || v > DET_MAX_THREADS)
            return det_errf(err, cap, unit, "threads: %ld out of range [1, %d]", v, DET_MAX_THREADS);
        if (n > 0 && v <= out[n - 1])
            return det_errf(err, cap, unit, "threads: %s is not strictly ascending", spec);
        out[n++] = (int)v;
        if (*end == ',') { p = end + 1; if (!*p)
            return det_errf(err, cap, unit, "threads: trailing comma in '%s'", spec); }
        else if (*end == '\0') p = end;
        else return det_errf(err, cap, unit, "threads: expected a list like 1,2,4, got '%s'", spec);
    }
    *count = n;
    return DET_OK;
}

/* ---- parallel-for ------------------------------------------------------- */

typedef struct {
    DetJobFn fn;
    void* arg;
    int njobs;
    int next;
    pthread_mutex_t lock;
} ParallelJob;

static void* parallel_worker(void* p) {
    ParallelJob* pj = (ParallelJob*)p;
    for (;;) {
        pthread_mutex_lock(&pj->lock);
        int job = pj->next < pj->njobs ? pj->next++ : -1;
        pthread_mutex_unlock(&pj->lock);
        if (job < 0) break;
        pj->fn(pj->arg, job);
    }
    return NULL;
}

int det_parallel_for(int nthreads, int njobs, DetJobFn fn, void* arg,
                     const char* unit, char* err, size_t cap) {
    if (nthreads < 1 || nthreads > DET_MAX_THREADS)
        return det_errf(err, cap, unit, "parallel: nthreads %d out of range [1, %d]",
                        nthreads, DET_MAX_THREADS);
    if (njobs < 0)
        return det_errf(err, cap, unit, "parallel: negative job count %d", njobs);
    if (njobs == 0) return DET_OK;
    if (nthreads == 1) {
        for (int i = 0; i < njobs; i++) fn(arg, i);
        return DET_OK;
    }
    ParallelJob pj = { fn, arg, njobs, 0, PTHREAD_MUTEX_INITIALIZER };
    pthread_t tids[DET_MAX_THREADS];
    int started = 0;
    for (int i = 0; i < nthreads; i++) {
        int rc = pthread_create(&tids[started], NULL, parallel_worker, &pj);
        if (rc != 0) {
            pthread_mutex_destroy(&pj.lock);
            return det_errf(err, cap, unit, "parallel: pthread_create failed (%d)", rc);
        }
        started++;
    }
    for (int i = 0; i < started; i++) pthread_join(tids[i], NULL);
    pthread_mutex_destroy(&pj.lock);
    return DET_OK;
}

/* ---- instance snapshot -------------------------------------------------- */

static void put_u32(unsigned char* b, uint32_t v) {
    for (int i = 0; i < 4; i++) b[i] = (unsigned char)(v >> (8 * i));
}
static void put_u64(unsigned char* b, uint64_t v) {
    for (int i = 0; i < 8; i++) b[i] = (unsigned char)(v >> (8 * i));
}
static uint32_t get_u32(const unsigned char* b) {
    return (uint32_t)b[0] | ((uint32_t)b[1] << 8) | ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 24);
}
static uint64_t get_u64(const unsigned char* b) {
    uint64_t v = 0;
    for (int i = 7; i >= 0; i--) v = (v << 8) | b[i];
    return v;
}

#define DET_SNAP_MAGIC 0x4E4F4354u  /* "TCON" little-endian */

static int write_bytes(FILE* f, const void* p, size_t n) {
    return fwrite(p, 1, n, f) == n ? 0 : -1;
}

/* 4 magic + 8 context seed + 4 unit count + per unit (4 nlen + DET_NAME_MAX
 * name + 8 size + 8 unit seed + state) + 8 trailing checksum. */
#define DET_HEADER_BYTES 16u
#define DET_UNIT_RECORD_OVERHEAD (4u + DET_NAME_MAX + 8u + 8u)

int det_context_save(DetContext* ctx, const char* unit, const char* path,
                     char* err, size_t cap) {
    if (!ctx || !path)
        return det_errf(err, cap, unit, "save: internal null context or path");
    if (ctx->nunits == 0)
        return det_errf(err, cap, unit, "save: no units open");

    size_t need = DET_HEADER_BYTES +
                  (size_t)ctx->nunits * DET_UNIT_RECORD_OVERHEAD + 8;
    for (int i = 0; i < ctx->nunits; i++) need += ctx->units[i].size;
    if (need > DET_INSTANCE_MAX)
        return det_errf(err, cap, unit, "save: snapshot would be %zu bytes", need);

    unsigned char* buf = (unsigned char*)malloc(need);
    if (!buf) return det_errf(err, cap, unit, "save: out of memory");

    size_t off = 0;
    put_u32(buf + off, DET_SNAP_MAGIC); off += 4;
    put_u64(buf + off, ctx->seed); off += 8;
    put_u32(buf + off, (uint32_t)ctx->nunits); off += 4;
    for (int i = 0; i < ctx->nunits; i++) {
        Unit* u = &ctx->units[i];
        size_t nlen = strlen(u->name) + 1;
        put_u32(buf + off, (uint32_t)nlen); off += 4;
        memset(buf + off, 0, DET_NAME_MAX);
        memcpy(buf + off, u->name, nlen);
        off += DET_NAME_MAX;
        put_u64(buf + off, (uint64_t)u->size); off += 8;
        put_u64(buf + off, det_unit_seed_raw(ctx->seed, u->name)); off += 8;
        memcpy(buf + off, u->state, u->size); off += u->size;
    }
    DetHash h;
    det_hash_init(&h);
    det_hash_bytes(&h, buf, off);
    put_u64(buf + off, det_hash_value(&h)); off += 8;

    FILE* f = fopen(path, "wb");
    if (!f) {
        free(buf);
        return det_errf(err, cap, unit, "save: cannot open '%s' for writing", path);
    }
    int rc = write_bytes(f, buf, off);
    if (fclose(f) != 0 || rc != 0) {
        free(buf);
        return det_errf(err, cap, unit, "save: write to '%s' failed", path);
    }
    free(buf);
    return DET_OK;
}

int det_context_load(DetContext* ctx, const char* path, const char* expected_unit,
                     char* err, size_t cap) {
    if (!ctx || !path)
        return det_errf(err, cap, expected_unit, "load: internal null context or path");
    FILE* f = fopen(path, "rb");
    if (!f)
        return det_errf(err, cap, expected_unit, "load: cannot open '%s'", path);
    unsigned char* buf = (unsigned char*)malloc(DET_INSTANCE_MAX);
    if (!buf) { fclose(f); return det_errf(err, cap, expected_unit, "load: out of memory"); }
    size_t len = fread(buf, 1, DET_INSTANCE_MAX, f);
    int ferr = ferror(f);
    fclose(f);
    if (ferr) { free(buf); return det_errf(err, cap, expected_unit, "load: read from '%s' failed", path); }
    if (len < DET_HEADER_BYTES + 8) { free(buf); return det_errf(err, cap, expected_unit, "load: '%s' is too short", path); }

    uint64_t want = get_u64(buf + len - 8);
    DetHash h;
    det_hash_init(&h);
    det_hash_bytes(&h, buf, len - 8);
    if (det_hash_value(&h) != want) {
        free(buf);
        return det_errf(err, cap, expected_unit, "load: '%s' checksum mismatch", path);
    }
    if (get_u32(buf) != DET_SNAP_MAGIC) {
        free(buf);
        return det_errf(err, cap, expected_unit, "load: '%s' is not an instance snapshot", path);
    }
    /* Adopt the saved seed so restored state continues from the exact stream
     * that produced it; per-unit seeds are then verified against it below. */
    ctx->seed = get_u64(buf + 4);
    uint32_t n = get_u32(buf + 12);
    if (n > DET_MAX_UNITS) {
        free(buf);
        return det_errf(err, cap, expected_unit, "load: '%s' declares %u units", path, n);
    }
    size_t off = DET_HEADER_BYTES;
    int found = 0;
    char seen[128] = "";
    for (uint32_t i = 0; i < n; i++) {
        /* nlen(4) + fixed-width name + size(8) + seed(8) must fit before the
         * trailing checksum. */
        if (off + 4 + DET_NAME_MAX + 16 > len - 8) {
            free(buf);
            return det_errf(err, cap, expected_unit, "load: '%s' truncated record", path);
        }
        uint32_t nlen = get_u32(buf + off); off += 4;
        char name[DET_NAME_MAX];
        if (nlen == 0 || nlen > DET_NAME_MAX) {
            free(buf);
            return det_errf(err, cap, expected_unit, "load: '%s' bad name length %u", path, nlen);
        }
        if (buf[off + nlen - 1] != '\0') {
            free(buf);
            return det_errf(err, cap, expected_unit, "load: '%s' unit name not terminated", path);
        }
        memset(name, 0, sizeof(name));
        memcpy(name, buf + off, nlen - 1);
        off += DET_NAME_MAX;
        uint64_t size = get_u64(buf + off); off += 8;
        uint64_t seed = get_u64(buf + off); off += 8;
        if (size == 0 || size > DET_ARENA_MAX || off + size > len - 8) {
            free(buf);
            return det_errf(err, cap, expected_unit, "load: '%s' bad size for unit '%s'", path, name);
        }
        void* dst = det_unit_open(ctx, name, (size_t)size, err, cap);
        if (!dst) { free(buf); return DET_ERR_STATE; }
        memcpy(dst, buf + off, (size_t)size); off += (size_t)size;
        if (det_unit_seed_raw(ctx->seed, name) != seed) {
            free(buf);
            return det_errf(err, cap, expected_unit,
                            "load: '%s' seeded %llu does not match context seed for unit '%s'",
                            path, (unsigned long long)seed, name);
        }
        if (strcmp(name, expected_unit) == 0) found = 1;
        if (strlen(seen) + strlen(name) + 2 < sizeof(seen)) {
            if (seen[0]) strcat(seen, ",");
            strcat(seen, name);
        }
    }
    if (!found) {
        free(buf);
        return det_errf(err, cap, expected_unit,
                        "load: '%s' contains units [%s], expected '%s'",
                        path, seen[0] ? seen : "", expected_unit);
    }
    free(buf);
    return DET_OK;
}

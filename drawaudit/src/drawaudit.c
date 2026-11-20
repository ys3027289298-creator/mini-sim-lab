
/* drawaudit: draw-batch statistics audit.

   The renderer keeps cheap per-(material,state) batch counters, which can
   drift from what was actually submitted (e.g. an optimization that skips
   a stat update on flush). The audit trail records every real submission,
   so the two can be reconciled per key and any mismatch traced back to
   the objects inside the implicated batches. The report emits exactly one
   entry per actual submission.

   Build with -DAUDIT_ENABLED=0 and every audit hook compiles away, so the
   disabled build carries zero audit overhead. */
#define _POSIX_C_SOURCE 199309L
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <time.h>

#ifndef AUDIT_ENABLED
#define AUDIT_ENABLED 0
#endif

#define MATS 8
#define STATES 4
#define NKEYS (MATS * STATES)
#define MAX_BATCH_OBJS 256
#define MAX_BATCHES (1 << 18)
#define MAX_OBJS (1 << 20)

typedef struct {
    uint32_t obj;
    uint16_t mat;
    uint16_t state;
} Draw;

static uint32_t key_of(uint16_t mat, uint16_t state) {
    return ((uint32_t)mat << 16) | (uint32_t)state;
}

static int kidx_of(uint16_t mat, uint16_t state) {
    return (int)mat * STATES + (int)state;
}

/* ---- audit trail ---- */

typedef struct {
    uint32_t key;
    uint32_t obj_start;     /* into Audit.objs */
    uint32_t obj_count;
} BatchRec;

typedef struct {
#if AUDIT_ENABLED
    BatchRec batches[MAX_BATCHES];
    uint32_t objs[MAX_OBJS];
    uint32_t per_key[NKEYS]; /* actual batches per (material,state) */
    uint64_t submits;        /* actual submissions seen */
    int nbatches;
    int nobjs;
    int overflow;
#else
    char unused;             /* no state, no cost */
#endif
} Audit;

#if AUDIT_ENABLED
static void audit_reset(Audit* a) { memset(a, 0, sizeof(*a)); }

static void audit_submit(Audit* a, uint32_t key, int kidx,
                         const uint32_t* objs, uint32_t n) {
    a->submits++;
    a->per_key[kidx]++;
    if (a->nbatches < MAX_BATCHES && a->nobjs + (int)n <= MAX_OBJS) {
        BatchRec* r = &a->batches[a->nbatches++];
        r->key = key;
        r->obj_start = (uint32_t)a->nobjs;
        r->obj_count = n;
        memcpy(&a->objs[a->nobjs], objs, n * sizeof(uint32_t));
        a->nobjs += (int)n;
    } else {
        a->overflow = 1;
    }
}
#define AUDIT_SUBMIT(a, key, kidx, objs, n) audit_submit(a, key, kidx, objs, n)
#define AUDIT_APPEND(r, o) \
    do { if ((r)->cur_count < MAX_BATCH_OBJS) (r)->cur_objs[(r)->cur_count] = (o); } while (0)
#else
#define AUDIT_SUBMIT(a, key, kidx, objs, n) ((void)0)
#define AUDIT_APPEND(r, o) ((void)0)
#endif

/* ---- renderer under audit ---- */

typedef struct {
    uint32_t stat[NKEYS];   /* renderer's own batch counters (may drift) */
    uint64_t submitted;     /* ground-truth submission count */
    int drop_flush_stat;    /* planted bug: flush submits but forgets to count */
    int open;
    uint32_t cur_key;
    int cur_kidx;
    uint32_t cur_count;
#if AUDIT_ENABLED
    uint32_t cur_objs[MAX_BATCH_OBJS];
    Audit* audit;
#endif
} Renderer;

static volatile uint64_t gpu_sink;

/* stand-in for real per-batch render cost */
static void gpu_work(uint32_t n) {
    uint64_t x = gpu_sink + n;
    for (int i = 0; i < 4000; i++)
        x = x * 6364136223846793005ULL + 1442695040888963407ULL;
    gpu_sink = x;
}

static void submit_batch(Renderer* r, int is_flush) {
    if (!r->open) return;
    r->submitted++;
    if (!(is_flush && r->drop_flush_stat))
        r->stat[r->cur_kidx]++;
    AUDIT_SUBMIT(r->audit, r->cur_key, r->cur_kidx, r->cur_objs,
                 r->cur_count < MAX_BATCH_OBJS ? r->cur_count : MAX_BATCH_OBJS);
    gpu_work(r->cur_count);
    r->open = 0;
}

static void renderer_draw(Renderer* r, const Draw* d) {
    uint32_t key = key_of(d->mat, d->state);
    if (r->open && key != r->cur_key) submit_batch(r, 0);
    if (!r->open) {
        r->open = 1;
        r->cur_key = key;
        r->cur_kidx = kidx_of(d->mat, d->state);
        r->cur_count = 0;
    }
    AUDIT_APPEND(r, d->obj);
    r->cur_count++;
}

static void renderer_flush(Renderer* r) { submit_batch(r, 1); }

/* ---- reconciliation ---- */

#if AUDIT_ENABLED
typedef struct {
    int kidx;
    uint32_t counted;   /* renderer stat */
    uint32_t actual;    /* audit record */
} Mismatch;

static int audit_verify(const Audit* a, const uint32_t stat[NKEYS],
                        Mismatch* out, int max) {
    int n = 0;
    for (int k = 0; k < NKEYS; k++) {
        if (stat[k] != a->per_key[k]) {
            if (out && n < max) {
                out[n].kidx = k;
                out[n].counted = stat[k];
                out[n].actual = a->per_key[k];
            }
            n++;
        }
    }
    return n;
}

/* collect object ids of every recorded batch for one key, so a mismatch
   can be traced back to the objects involved */
static int audit_objects_for_key(const Audit* a, int kidx, uint32_t* out, int max) {
    uint32_t want = ((uint32_t)(kidx / STATES) << 16) | (uint32_t)(kidx % STATES);
    int n = 0;
    for (int i = 0; i < a->nbatches; i++) {
        const BatchRec* r = &a->batches[i];
        if (r->key != want) continue;
        for (uint32_t j = 0; j < r->obj_count && n < max; j++)
            out[n++] = a->objs[r->obj_start + j];
    }
    return n;
}

/* one report entry per recorded submission; returns the entry count */
static uint64_t audit_report(const Audit* a, int verbose) {
    uint64_t entries = 0;
    for (int i = 0; i < a->nbatches; i++) {
        const BatchRec* r = &a->batches[i];
        if (verbose)
            printf("entry=%llu mat=%u state=%u objs=%u\n",
                   (unsigned long long)entries,
                   (unsigned)(r->key >> 16), (unsigned)(r->key & 0xffff),
                   (unsigned)r->obj_count);
        entries++;
    }
    return entries;
}
#endif

static double now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1e9 + (double)ts.tv_nsec;
}

static int failures = 0;
static void check(const char* name, int ok) {
    printf("%s=%d\n", name, ok ? 1 : 0);
    if (!ok) failures++;
}

int main(void) {
#if AUDIT_ENABLED
    /* scenario A: stats and submissions agree */
    {
        Audit a;
        audit_reset(&a);
        Renderer r;
        memset(&r, 0, sizeof r);
        r.audit = &a;
        Draw draws[] = {
            {1,0,0},{2,0,0},{3,0,0},{4,1,2},
            {5,1,2},{6,0,0},{7,0,0},{8,0,0},
        };
        for (int i = 0; i < 4; i++) renderer_draw(&r, &draws[i]);
        renderer_flush(&r);
        for (int i = 4; i < 8; i++) renderer_draw(&r, &draws[i]);
        renderer_flush(&r);

        check("submitted", r.submitted == 4);
        check("stat_a", r.stat[kidx_of(0,0)] == 2);
        check("stat_b", r.stat[kidx_of(1,2)] == 2);
        check("no_overflow", a.overflow == 0);
        Mismatch mm[NKEYS];
        check("verify_clean", audit_verify(&a, r.stat, mm, NKEYS) == 0);
        check("report_matches", audit_report(&a, 0) == r.submitted);
    }
    /* scenario B: flush forgets to count -> audit pinpoints the objects */
    {
        Audit a;
        audit_reset(&a);
        Renderer r;
        memset(&r, 0, sizeof r);
        r.audit = &a;
        r.drop_flush_stat = 1;
        Draw draws[] = {
            {1,0,0},{2,0,0},{3,0,0},{4,1,2},
            {5,1,2},{6,0,0},{7,0,0},{8,0,0},
        };
        for (int i = 0; i < 4; i++) renderer_draw(&r, &draws[i]);
        renderer_flush(&r);
        for (int i = 4; i < 8; i++) renderer_draw(&r, &draws[i]);
        renderer_flush(&r);

        check("drift_exists", r.stat[kidx_of(0,0)] != a.per_key[kidx_of(0,0)]);
        Mismatch mm[NKEYS];
        int n = audit_verify(&a, r.stat, mm, NKEYS);
        check("two_keys_off", n == 2);
        /* keyA batches: {1,2,3} counted, {6,7,8} dropped by the bug */
        uint32_t objs[64];
        int na = audit_objects_for_key(&a, kidx_of(0,0), objs, 64);
        int found = 0;
        for (int i = 0; i < na; i++)
            if (objs[i] == 6 || objs[i] == 7 || objs[i] == 8) found++;
        check("locate_key_a", found == 3);
        /* keyB batches: {4} dropped, {5} counted */
        int nb = audit_objects_for_key(&a, kidx_of(1,2), objs, 64);
        found = 0;
        for (int i = 0; i < nb; i++)
            if (objs[i] == 4) found++;
        check("locate_key_b", found == 1);
        check("report_still_matches", audit_report(&a, 0) == r.submitted);
    }
#else
    check("audit_struct_empty", sizeof(Audit) == 1);
#endif

    /* perf workload: identical loop in both builds; the Makefile compares
       work_ns across them and fails if audit overhead exceeds 2% */
    Renderer pr;
    memset(&pr, 0, sizeof pr);
#if AUDIT_ENABLED
    Audit pa;
    audit_reset(&pa);
    pr.audit = &pa;
#endif
    uint64_t s = 0x123456789abcdefULL;
    double t0 = now_ns();
    for (int f = 0; f < 128; f++) {
        for (int i = 0; i < 1024; i++) {
            s += 0x9E3779B97F4A7C15ULL;
            uint64_t z = s;
            z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
            z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
            z ^= z >> 31;
            Draw d;
            d.obj = (uint32_t)(f * 1024 + i);
            d.mat = (uint16_t)(z % MATS);
            d.state = (uint16_t)((z >> 8) % STATES);
            renderer_draw(&pr, &d);
        }
        renderer_flush(&pr);
    }
    double t1 = now_ns();
#if AUDIT_ENABLED
    check("perf_no_overflow", pa.overflow == 0);
    check("perf_report_matches", audit_report(&pa, 0) == pr.submitted);
#endif
    printf("work_ns=%lld\n", (long long)(t1 - t0));

    printf("all_ok=%d\n", failures == 0);
    return failures == 0 ? 0 : 1;
}

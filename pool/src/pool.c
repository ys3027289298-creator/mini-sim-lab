
/* pool: fixed-capacity object pool with generation handles, so dangling
   handles are detected instead of silently reused.
   In shared mode each parallel item runs its own private pool driven by the
   shared detctx RNG; item results are pure functions of (seed, index). */
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdlib.h>

#include "../../detctx/detctx.h"

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

static void pool_init(Pool* p) {
    memset(p, 0, sizeof(*p));
    for (int i = 0; i < POOL_CAP; i++) p->gen[i] = 1;
}

static Handle pool_alloc(Pool* p, int v) {
    for (int i = 0; i < POOL_CAP; i++) {
        if (!p->in_use[i]) {
            p->in_use[i] = 1;
            p->value[i] = v;
            p->used++;
            if (p->used > p->high_water) p->high_water = p->used;
            Handle h = { (unsigned)i, p->gen[i] };
            return h;
        }
    }
    Handle bad = { (unsigned)-1, 0 };
    return bad;
}

static int pool_valid(const Pool* p, Handle h) {
    if (h.index >= POOL_CAP) return 0;
    if (!p->in_use[h.index]) return 0;
    return p->gen[h.index] == h.gen;
}

static int pool_get(const Pool* p, Handle h, int* out) {
    if (!pool_valid(p, h)) return -1;
    *out = p->value[h.index];
    return 0;
}

static int pool_free(Pool* p, Handle h) {
    if (!pool_valid(p, h)) return -1;
    p->in_use[h.index] = 0;
    p->gen[h.index]++;          /* bump so stale handles fail */
    p->used--;
    return 0;
}

static int failures = 0;
static void check(const char* name, int ok) {
    printf("%s=%d\n", name, ok ? 1 : 0);
    if (!ok) failures++;
}

/* ---- shared deterministic context workload -------------------------------- */

static DetCtx g_ctx;

#define POOL_OPS 64

typedef struct {
    int allocs;
    int frees;
    int rejected;
    int dangling_ok;    /* every freed handle later read as stale: expect 1 */
    int final_used;
    int high_water;
    uint64_t trace;
} PoolSlot;

static uint64_t fold_event(uint64_t h, uint32_t kind, uint64_t payload) {
    h ^= (uint64_t)kind + 0x9e3779b97f4a7c15ULL;
    h *= 0x100000001b3ULL;
    h ^= payload;
    h *= 0x100000001b3ULL;
    return h;
}

/* Pure function of (ctx, item): no shared mutable state, safe on any thread. */
static void pool_compute(const DetCtx* ctx, int item, PoolSlot* s) {
    memset(s, 0, sizeof(*s));
    Pool p;
    pool_init(&p);

    uint64_t st = detctx_item_seed(ctx, item);
    Handle live[POOL_CAP];
    int nlive = 0;
    int stale_checks = 0, stale_pass = 0;
    s->trace = 0xcbf29ce484222325ULL;

    for (int op = 0; op < POOL_OPS; op++) {
        uint32_t kind = detctx_range(&st, 3);
        if (kind <= 1) {                                  /* allocate */
            int v = (int)detctx_range(&st, 1000000);
            Handle h = pool_alloc(&p, v);
            if (h.index == (unsigned)-1) {
                s->rejected++;
                s->trace = fold_event(s->trace, 0, 0xffffffffULL);
            } else {
                s->allocs++;
                live[nlive++] = h;
                s->trace = fold_event(s->trace, 1,
                                      ((uint64_t)h.gen << 32) | h.index);
            }
        } else if (nlive > 0) {                           /* free oldest */
            Handle h = live[0];
            memmove(&live[0], &live[1], (size_t)(nlive - 1) * sizeof(Handle));
            nlive--;
            int rc = pool_free(&p, h);
            if (rc == 0) {
                s->frees++;
                s->trace = fold_event(s->trace, 2,
                                      ((uint64_t)h.gen << 32) | h.index);
            }
            /* the freed handle must now read as stale (generation guard) */
            int dummy;
            stale_checks++;
            if (pool_get(&p, h, &dummy) == -1) stale_pass++;
        } else {                                          /* free with nothing live */
            Handle bogus = { (unsigned)detctx_range(&st, POOL_CAP), 0 };
            stale_checks++;
            if (pool_free(&p, bogus) == -1) stale_pass++;
            s->trace = fold_event(s->trace, 3, bogus.index);
        }
    }

    /* drain: freeing every surviving handle must invalidate it too, so the
       dangling-detection guarantee is exercised on every item */
    while (nlive > 0) {
        Handle h = live[--nlive];
        if (pool_free(&p, h) == 0) {
            s->frees++;
            s->trace = fold_event(s->trace, 2,
                                  ((uint64_t)h.gen << 32) | h.index);
        }
        int dummy;
        stale_checks++;
        if (pool_get(&p, h, &dummy) == -1) stale_pass++;
    }

    s->dangling_ok = (stale_checks > 0 && stale_pass == stale_checks);
    s->final_used = p.used;
    s->high_water = p.high_water;
}

static void pool_task(int item, void* arg) {
    PoolSlot* slots = (PoolSlot*)arg;
    pool_compute(&g_ctx, item, &slots[item]);
}

static void run_shared(const DetCtx* ctx) {
    printf("shared=on seed=%llu threads=%d items=%d ops=%d\n",
           (unsigned long long)ctx->seed, ctx->threads,
           DETCTX_ITEMS, POOL_OPS);

    PoolSlot* slots = calloc(DETCTX_ITEMS, sizeof(PoolSlot));
    PoolSlot* ref = calloc(DETCTX_ITEMS, sizeof(PoolSlot));
    if (!slots || !ref) { fprintf(stderr, "error: out of memory\n"); exit(2); }

    g_ctx = *ctx;
    if (detctx_parallel_for(ctx, DETCTX_ITEMS, pool_task, slots) != 0) exit(2);
    for (int i = 0; i < DETCTX_ITEMS; i++) pool_compute(ctx, i, &ref[i]);

    int mismatch = 0, integrity_bad = 0;
    for (int i = 0; i < DETCTX_ITEMS; i++) {
        if (memcmp(&slots[i], &ref[i], sizeof(PoolSlot)) != 0) mismatch = 1;
        /* live count bookkeeping: rejected allocs never change used, so
           allocs minus frees equals the pool's own used counter; no leaks */
        if (slots[i].dangling_ok != 1 ||
            slots[i].allocs - slots[i].frees != slots[i].final_used ||
            slots[i].final_used != 0) {
            integrity_bad = 1;
        }
        printf("item=%d allocs=%d frees=%d rejected=%d used=%d hw=%d dangling=%d trace=%016llx\n",
               i, slots[i].allocs, slots[i].frees, slots[i].rejected,
               slots[i].final_used, slots[i].high_water, slots[i].dangling_ok,
               (unsigned long long)slots[i].trace);
    }
    uint64_t sum = detctx_checksum(slots, (size_t)DETCTX_ITEMS * sizeof(PoolSlot));
    printf("parallel_matches_sequential=%d\n", mismatch == 0);
    printf("pool_integrity=%d\n", integrity_bad == 0);
    printf("checksum=%016llx\n", (unsigned long long)sum);

    free(slots);
    free(ref);
    if (mismatch || integrity_bad) failures++;
}

/* ---- legacy self-test, byte-for-byte the original behaviour --------------- */

static void run_legacy_checks(void) {
    Pool p;
    pool_init(&p);

    Handle a = pool_alloc(&p, 10);
    Handle b = pool_alloc(&p, 20);
    int v = 0;
    check("get_a", pool_get(&p, a, &v) == 0 && v == 10);
    check("get_b", pool_get(&p, b, &v) == 0 && v == 20);
    check("count", p.used == 2);

    check("free_a", pool_free(&p, a) == 0 && p.used == 1);
    check("stale_a", pool_get(&p, a, &v) == -1);

    Handle c = pool_alloc(&p, 30);
    check("reuse_slot", c.index == a.index);
    check("new_gen", c.gen != a.gen);
    check("stale_still_bad", pool_get(&p, a, &v) == -1);
    check("c_ok", pool_get(&p, c, &v) == 0 && v == 30);

    check("double_free", pool_free(&p, c) == 0 && pool_free(&p, c) == -1);

    Pool q;
    pool_init(&q);
    Handle hs[POOL_CAP];
    for (int i = 0; i < POOL_CAP; i++) hs[i] = pool_alloc(&q, i);
    Handle full = pool_alloc(&q, 99);
    check("exhausted", full.index == (unsigned)-1);
    check("high_water", q.high_water == POOL_CAP);
    for (int i = 0; i < POOL_CAP; i++) pool_free(&q, hs[i]);
    check("all_free", q.used == 0);
    check("handles_stale", pool_get(&q, hs[0], &v) == -1);
}

int main(int argc, char** argv) {
    DetCtx ctx;
    int pr = detctx_parse(&ctx, argc, argv);
    if (pr < 0) return 2;
    if (pr > 0) return 0;

    if (ctx.shared) run_shared(&ctx);
    run_legacy_checks();

    printf("all_ok=%d\n", failures == 0);
    return failures == 0 ? 0 : 1;
}

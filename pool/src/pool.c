
/* pool: object pool with generation handles and threshold-driven
   generational growth.
   - growth triggers on utilization (used >= cap * 3/4), capacity doubles
   - handles carry (index, generation); growth copies the arrays, so old
     handles stay valid and queries return the same value as before
   - capacity, threshold and grow count are queryable; every grow is
     recorded for reporting
   - growth is all-or-nothing: if the new arrays cannot be allocated,
     the old pool is left completely intact (rollback) and alloc falls
     back to whatever free slots remain in the current capacity */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define POOL_INIT_CAP   4u
#define POOL_GROW_NUM   3u
#define POOL_GROW_DEN   4u    /* grow threshold: 75% utilization */
#define POOL_MAX_CAP    64u
#define POOL_MAX_GROWS  24
#define POOL_BAD_INDEX  ((unsigned int)-1)

typedef struct {
    unsigned int index;
    unsigned int gen;
} Handle;

typedef struct {
    unsigned int from_cap;
    unsigned int to_cap;
    unsigned int used_before;
    unsigned int threshold;
} GrowEvent;

typedef struct {
    unsigned char* in_use;
    unsigned int*  gen;
    int*           value;
    unsigned int   cap;
    unsigned int   used;
    unsigned int   high_water;
    unsigned int   grows;
    GrowEvent      history[POOL_MAX_GROWS];
    int            alloc_fails_left;  /* failure injection: >0 => next N internal allocs fail */
} Pool;

/* malloc wrapper so tests can force the real allocation-failure path */
static void* pool_xmalloc(Pool* p, size_t bytes) {
    if (p->alloc_fails_left > 0) {
        p->alloc_fails_left--;
        return NULL;
    }
    return malloc(bytes);
}

static void pool_fail_next_grow(Pool* p, int internal_allocs) {
    p->alloc_fails_left = internal_allocs;
}

static int pool_init(Pool* p) {
    memset(p, 0, sizeof(*p));
    p->in_use = malloc(POOL_INIT_CAP * sizeof(*p->in_use));
    p->gen    = malloc(POOL_INIT_CAP * sizeof(*p->gen));
    p->value  = malloc(POOL_INIT_CAP * sizeof(*p->value));
    if (!p->in_use || !p->gen || !p->value) {
        free(p->in_use);
        free(p->gen);
        free(p->value);
        memset(p, 0, sizeof(*p));
        return -1;
    }
    p->cap = POOL_INIT_CAP;
    for (unsigned int i = 0; i < p->cap; i++) {
        p->in_use[i] = 0;
        p->gen[i] = 1;
        p->value[i] = 0;
    }
    return 0;
}

static void pool_destroy(Pool* p) {
    free(p->in_use);
    free(p->gen);
    free(p->value);
    memset(p, 0, sizeof(*p));
}

/* ---- queryable capacity / threshold / growth stats ---- */

static unsigned int pool_capacity(const Pool* p) { return p->cap; }
static unsigned int pool_used(const Pool* p) { return p->used; }
static unsigned int pool_threshold(const Pool* p) {
    return p->cap * POOL_GROW_NUM / POOL_GROW_DEN;
}
static unsigned int pool_grow_count(const Pool* p) { return p->grows; }

static int pool_grow(Pool* p) {
    unsigned int old_cap = p->cap;
    unsigned int new_cap = old_cap * 2u;
    if (new_cap == 0 || new_cap > POOL_MAX_CAP) return -1;

    /* allocate all new arrays first; until the swap below, the old pool
       is untouched, so any failure here is a clean rollback */
    unsigned char* ni = pool_xmalloc(p, new_cap * sizeof(*ni));
    unsigned int*  ng = pool_xmalloc(p, new_cap * sizeof(*ng));
    int*           nv = pool_xmalloc(p, new_cap * sizeof(*nv));
    if (!ni || !ng || !nv) {
        free(ni);
        free(ng);
        free(nv);
        return -1;
    }

    memcpy(ni, p->in_use, old_cap * sizeof(*ni));
    memcpy(ng, p->gen,    old_cap * sizeof(*ng));
    memcpy(nv, p->value,  old_cap * sizeof(*nv));
    for (unsigned int i = old_cap; i < new_cap; i++) {
        ni[i] = 0;
        ng[i] = 1;       /* fresh slots start at generation 1 */
        nv[i] = 0;
    }

    if (p->grows < POOL_MAX_GROWS) {
        p->history[p->grows] = (GrowEvent){
            .from_cap = old_cap,
            .to_cap = new_cap,
            .used_before = p->used,
            .threshold = old_cap * POOL_GROW_NUM / POOL_GROW_DEN,
        };
    }

    free(p->in_use);
    free(p->gen);
    free(p->value);
    p->in_use = ni;
    p->gen = ng;
    p->value = nv;
    p->cap = new_cap;
    p->grows++;
    return 0;
}

static Handle pool_alloc(Pool* p, int v) {
    if (p->used >= pool_threshold(p))
        (void)pool_grow(p);   /* failure is non-fatal: try current cap */

    for (unsigned int i = 0; i < p->cap; i++) {
        if (!p->in_use[i]) {
            p->in_use[i] = 1;
            p->value[i] = v;
            p->used++;
            if (p->used > p->high_water) p->high_water = p->used;
            Handle h = { i, p->gen[i] };
            return h;
        }
    }
    Handle bad = { POOL_BAD_INDEX, 0 };
    return bad;
}

static int pool_valid(const Pool* p, Handle h) {
    if (h.index >= p->cap) return 0;
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

static void pool_print_grows(const Pool* p) {
    printf("grow_report: grows=%u cap %u -> %u (%.1fx)\n",
           p->grows, POOL_INIT_CAP, p->cap, (double)p->cap / POOL_INIT_CAP);
    printf("  %-4s %-11s %-9s %s\n", "grow", "used_before", "threshold", "cap: old -> new");
    for (unsigned int i = 0; i < p->grows; i++) {
        const GrowEvent* e = &p->history[i];
        printf("  %-4u %-11u %-9u %u -> %u\n",
               i + 1, e->used_before, e->threshold, e->from_cap, e->to_cap);
    }
}

static int failures = 0;
static void check(const char* name, int ok) {
    printf("%s=%d\n", name, ok ? 1 : 0);
    if (!ok) failures++;
}

int main(void) {
    Pool p;
    check("init", pool_init(&p) == 0);
    check("init_cap", pool_capacity(&p) == POOL_INIT_CAP);
    check("init_threshold", pool_threshold(&p) == 3);
    check("init_grows", pool_grow_count(&p) == 0);

    /* ---- utilization-driven growth; old handles stay valid ---- */
    enum { N = 20 };
    Handle hs[N];
    for (int i = 0; i < N; i++) {
        hs[i] = pool_alloc(&p, 100 + i);
        if (hs[i].index == POOL_BAD_INDEX) check("alloc_succeeded", 0);
    }
    check("alloc_succeeded", 1);
    check("grew_3_times", pool_grow_count(&p) == 3);       /* 4->8->16->32 */
    check("cap_32", pool_capacity(&p) == 32);
    check("threshold_24", pool_threshold(&p) == 24);
    check("used_20", pool_used(&p) == 20);
    check("high_water_20", p.high_water == 20);

    /* every handle acquired before the grows still resolves identically */
    int consistent = 1;
    for (int i = 0; i < N; i++) {
        int v = 0;
        if (pool_get(&p, hs[i], &v) != 0 || v != 100 + i) consistent = 0;
    }
    check("handles_consistent_across_grows", consistent);

    check("event0_4_to_8",
          p.history[0].from_cap == 4 && p.history[0].to_cap == 8 &&
          p.history[0].used_before == 3 && p.history[0].threshold == 3);
    check("event1_8_to_16",
          p.history[1].from_cap == 8 && p.history[1].to_cap == 16 &&
          p.history[1].used_before == 6 && p.history[1].threshold == 6);
    check("event2_16_to_32",
          p.history[2].from_cap == 16 && p.history[2].to_cap == 32 &&
          p.history[2].used_before == 12 && p.history[2].threshold == 12);
    pool_print_grows(&p);

    /* ---- drive to the hard ceiling ---- */
    Handle more[POOL_MAX_CAP];
    unsigned int more_n = 0;
    while (p.used < POOL_MAX_CAP) {
        Handle h = pool_alloc(&p, 1000 + (int)more_n);
        if (h.index == POOL_BAD_INDEX) break;
        more[more_n++] = h;
    }
    check("filled_to_max", pool_used(&p) == POOL_MAX_CAP);
    check("grew_4_times", pool_grow_count(&p) == 4);       /* 32->64 */
    check("cap_max", pool_capacity(&p) == POOL_MAX_CAP);
    Handle full = pool_alloc(&p, 99);
    check("exhausted_at_max", full.index == POOL_BAD_INDEX);
    check("grows_still_4_after_refusal", pool_grow_count(&p) == 4);
    check("cap_still_64_after_refusal", pool_capacity(&p) == POOL_MAX_CAP);

    /* ---- generation handles still behave across growth ---- */
    Handle x0 = hs[0];
    int v = 0;
    check("free_x0", pool_free(&p, x0) == 0 && pool_used(&p) == 63);
    check("stale_x0", pool_get(&p, x0, &v) == -1);
    /* grow is attempted (63 >= threshold 48), fails at max cap, slot is reused anyway */
    Handle y = pool_alloc(&p, 777);
    check("reuse_slot", y.index == x0.index);
    check("new_gen", y.gen != x0.gen);
    check("stale_still_bad", pool_get(&p, x0, &v) == -1);
    check("y_value", pool_get(&p, y, &v) == 0 && v == 777);
    check("double_free", pool_free(&p, y) == 0 && pool_free(&p, y) == -1);

    for (int i = 1; i < N; i++) pool_free(&p, hs[i]);
    for (unsigned int i = 0; i < more_n; i++) pool_free(&p, more[i]);
    check("all_free", pool_used(&p) == 0);
    check("handles_stale", pool_get(&p, hs[1], &v) == -1);
    pool_destroy(&p);

    /* ---- failed growth rolls back atomically (each internal malloc in turn) ---- */
    for (int fail_at = 1; fail_at <= 3; fail_at++) {
        Pool r;
        check("r_init", pool_init(&r) == 0);
        Handle rh[POOL_INIT_CAP];
        for (unsigned int i = 0; i < pool_threshold(&r); i++)
            rh[i] = pool_alloc(&r, (int)i + 1);     /* used reaches 3/4, no grow yet */
        check("r_pre_cap_4", pool_capacity(&r) == 4);

        pool_fail_next_grow(&r, fail_at);
        Handle extra = pool_alloc(&r, 42);           /* triggers grow; it fails */
        char name[64];
        snprintf(name, sizeof(name), "rollback_alloc_ok_%d", fail_at);
        check(name, extra.index != POOL_BAD_INDEX);  /* fallback: free slot still there */
        snprintf(name, sizeof(name), "rollback_cap_unchanged_%d", fail_at);
        check(name, pool_capacity(&r) == 4);
        snprintf(name, sizeof(name), "rollback_no_grow_%d", fail_at);
        check(name, pool_grow_count(&r) == 0);
        snprintf(name, sizeof(name), "rollback_threshold_unchanged_%d", fail_at);
        check(name, pool_threshold(&r) == 3);

        int intact = 1;
        for (unsigned int i = 0; i < pool_threshold(&r); i++) {
            int got = 0;
            if (pool_get(&r, rh[i], &got) != 0 || got != (int)i + 1) intact = 0;
        }
        if (pool_get(&r, extra, &v) != 0 || v != 42) intact = 0;
        snprintf(name, sizeof(name), "rollback_data_intact_%d", fail_at);
        check(name, intact);

        /* injection cleared: next grow succeeds and old handles survive it */
        Handle grown = pool_alloc(&r, 99);
        snprintf(name, sizeof(name), "recover_grew_to_8_%d", fail_at);
        check(name, pool_capacity(&r) == 8 && pool_grow_count(&r) == 1 &&
                    grown.index != POOL_BAD_INDEX);
        int survived = 1;
        for (unsigned int i = 0; i < 3; i++) {
            int got = 0;
            if (pool_get(&r, rh[i], &got) != 0 || got != (int)i + 1) survived = 0;
        }
        if (pool_get(&r, extra, &v) != 0 || v != 42) survived = 0;
        snprintf(name, sizeof(name), "recover_old_handles_valid_%d", fail_at);
        check(name, survived);
        pool_destroy(&r);
    }

    /* ---- full pool + failed grow: refusal, state untouched, then recovery ---- */
    Pool q;
    check("q_init", pool_init(&q) == 0);
    pool_fail_next_grow(&q, 1);
    Handle qh[POOL_INIT_CAP];
    for (unsigned int i = 0; i < POOL_INIT_CAP; i++)
        qh[i] = pool_alloc(&q, (int)i);              /* 4th alloc: grow fails, last slot used */
    check("q_still_cap_4", pool_capacity(&q) == 4 && pool_grow_count(&q) == 0);
    check("q_full", pool_used(&q) == 4);

    pool_fail_next_grow(&q, 1);
    Handle refused = pool_alloc(&q, 5);              /* grow fails again, no free slot */
    check("full_pool_refused", refused.index == POOL_BAD_INDEX);
    check("full_pool_state_unchanged",
          pool_capacity(&q) == 4 && pool_grow_count(&q) == 0 && pool_used(&q) == 4);
    int q_intact = 1;
    for (unsigned int i = 0; i < POOL_INIT_CAP; i++) {
        int got = -1;
        if (pool_get(&q, qh[i], &got) != 0 || got != (int)i) q_intact = 0;
    }
    check("full_pool_data_intact", q_intact);

    Handle rec = pool_alloc(&q, 5);                  /* no injection now */
    check("recovered_grow", rec.index != POOL_BAD_INDEX &&
          pool_capacity(&q) == 8 && pool_grow_count(&q) == 1);
    int q_survived = 1;
    for (unsigned int i = 0; i < POOL_INIT_CAP; i++) {
        int got = -1;
        if (pool_get(&q, qh[i], &got) != 0 || got != (int)i) q_survived = 0;
    }
    check("recovered_old_handles_valid", q_survived);
    pool_destroy(&q);

    printf("all_ok=%d\n", failures == 0);
    return failures == 0 ? 0 : 1;
}

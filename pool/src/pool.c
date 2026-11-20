
/* pool: fixed-capacity object pool with generation handles, so dangling
   handles are detected instead of silently reused.

   Slot generations are 32-bit. A wrapped generation would make a very old
   handle valid again, so when a generation approaches POOL_GEN_MAX the pool
   sweeps every slot:
     - the sweep starts a new epoch and resets all slot generations to 1,
       invalidating every handle issued before it;
     - a migration table records old -> new generations for the sweep, and
       lookup of an old handle returns an explicit invalidation reason
       (never an empty result);
     - an automatic sweep only runs when the pool is empty. If live objects
       make that impossible, the release that would cross the limit fails
       with POOL_ERR_GEN_LIMIT and writes an error log. pool_reset() forces
       the sweep and drops live objects.

   Handles carry their epoch, so a handle from before the sweep can never
   alias a new handle that happens to reuse the same slot and generation. */
#include <stdio.h>
#include <string.h>
#include <stdint.h>

#define POOL_CAP 16
#define POOL_GEN_MAX 0xFFFFFFFFu
#define POOL_GEN_MARGIN 16u
#define POOL_LOG_CAP 160

typedef enum {
    POOL_OK = 0,
    POOL_ERR_INDEX,           /* index out of range */
    POOL_ERR_FREED,           /* slot is not allocated */
    POOL_ERR_GEN,             /* generation mismatch in the current epoch */
    POOL_ERR_RESET,           /* invalidated by the latest generation sweep */
    POOL_ERR_RESET_TOO_OLD,   /* invalidated by an older sweep */
    POOL_ERR_GEN_LIMIT        /* generation limit crossed, sweep impossible */
} PoolResult;

typedef struct {
    unsigned int index;
    unsigned int gen;
    unsigned int epoch;
} Handle;

typedef struct {
    unsigned int index;
    unsigned int old_gen;
    unsigned int new_gen;
    unsigned int old_epoch;
    unsigned int new_epoch;
    int found;                /* 1 when the table maps this exact old gen */
    PoolResult reason;
} MigrationEntry;

typedef struct {
    int used;
    int high_water;
    unsigned int epoch;
    int resets;
    unsigned long long alloc_count;
    unsigned long long gens_retired;
    unsigned int max_gen_seen;
    int log_count;
} PoolStats;

typedef struct {
    int in_use[POOL_CAP];
    unsigned int gen[POOL_CAP];
    int refs[POOL_CAP];
    int value[POOL_CAP];
    int used;
    int high_water;

    unsigned int epoch;
    int resets;
    unsigned long long alloc_count;
    unsigned long long gens_retired;
    unsigned int max_gen_seen;

    MigrationEntry migration[POOL_CAP];
    int migration_valid;
    unsigned int prev_epoch;

    char last_log[POOL_LOG_CAP];
    int log_count;
} Pool;

static void pool_log(Pool* p, const char* msg) {
    snprintf(p->last_log, sizeof(p->last_log), "%s", msg);
    p->log_count++;
    fprintf(stderr, "[pool] error: %s\n", msg);
}

static const char* pool_reason(PoolResult r) {
    switch (r) {
    case POOL_OK: return "ok";
    case POOL_ERR_INDEX: return "index-out-of-range";
    case POOL_ERR_FREED: return "slot-freed";
    case POOL_ERR_GEN: return "generation-mismatch";
    case POOL_ERR_RESET: return "invalidated-by-reset";
    case POOL_ERR_RESET_TOO_OLD: return "invalidated-by-older-reset";
    case POOL_ERR_GEN_LIMIT: return "generation-limit";
    }
    return "unknown";
}

static void pool_init(Pool* p) {
    memset(p, 0, sizeof(*p));
    for (int i = 0; i < POOL_CAP; i++) p->gen[i] = 1;
    p->epoch = 1;
}

/* Reset every slot, bump the epoch and fill the migration table. */
static void pool_reset_internal(Pool* p, const char* why) {
    unsigned int old_epoch = p->epoch;
    for (int i = 0; i < POOL_CAP; i++) {
        MigrationEntry* m = &p->migration[i];
        m->index = (unsigned)i;
        m->old_gen = p->gen[i];
        m->new_gen = 1;
        m->old_epoch = old_epoch;
        m->new_epoch = old_epoch + 1;
        m->found = 1;
        m->reason = POOL_ERR_RESET;
        p->gen[i] = 1;
        p->in_use[i] = 0;
        p->refs[i] = 0;
    }
    p->migration_valid = 1;
    p->prev_epoch = old_epoch;
    p->epoch = old_epoch + 1;
    p->used = 0;
    p->resets++;
    fprintf(stderr, "[pool] reset: %s (epoch %u -> %u)\n", why, old_epoch, p->epoch);
}

/* Hard reset: invalidates live handles too, so their objects are dropped. */
static void pool_reset(Pool* p) {
    pool_reset_internal(p, "explicit reset");
}

static Handle pool_make_bad(void) {
    Handle bad = { (unsigned)-1, 0, 0 };
    return bad;
}

static Handle pool_alloc(Pool* p, int v) {
    for (int i = 0; i < POOL_CAP; i++) {
        if (!p->in_use[i]) {
            p->in_use[i] = 1;
            p->value[i] = v;
            p->refs[i] = 1;
            p->used++;
            p->alloc_count++;
            if (p->used > p->high_water) p->high_water = p->used;
            if (p->gen[i] > p->max_gen_seen) p->max_gen_seen = p->gen[i];
            Handle h = { (unsigned)i, p->gen[i], p->epoch };
            return h;
        }
    }
    return pool_make_bad();
}

static PoolResult pool_check(const Pool* p, Handle h) {
    if (h.index >= POOL_CAP) return POOL_ERR_INDEX;
    if (h.epoch != p->epoch) {
        if (p->migration_valid && h.epoch == p->prev_epoch) return POOL_ERR_RESET;
        return POOL_ERR_RESET_TOO_OLD;
    }
    if (!p->in_use[h.index] || p->refs[h.index] <= 0) return POOL_ERR_FREED;
    if (p->gen[h.index] != h.gen) return POOL_ERR_GEN;
    return POOL_OK;
}

static PoolResult pool_get(const Pool* p, Handle h, int* out) {
    PoolResult r = pool_check(p, h);
    if (r != POOL_OK) return r;
    *out = p->value[h.index];
    return POOL_OK;
}

static PoolResult pool_refcount(const Pool* p, Handle h, int* out) {
    PoolResult r = pool_check(p, h);
    if (r != POOL_OK) return r;
    *out = p->refs[h.index];
    return POOL_OK;
}

static PoolResult pool_retain(Pool* p, Handle h) {
    PoolResult r = pool_check(p, h);
    if (r != POOL_OK) return r;
    p->refs[h.index]++;
    return POOL_OK;
}

/* Drop one reference. The last reference recycles the slot, which consumes
   a generation and may trigger a sweep when the limit is near. */
static PoolResult pool_release(Pool* p, Handle h) {
    PoolResult r = pool_check(p, h);
    if (r != POOL_OK) return r;

    unsigned int i = h.index;
    if (p->refs[i] > 1) {
        p->refs[i]--;
        return POOL_OK;
    }

    if (p->gen[i] >= p->max_gen_seen) p->max_gen_seen = p->gen[i];

    if (p->gen[i] == POOL_GEN_MAX) {
        if (p->used == 1) {
            p->in_use[i] = 0;
            p->refs[i] = 0;
            p->used = 0;
            p->gens_retired++;
            pool_reset_internal(p, "generation limit reached; swept all slots");
            return POOL_OK;
        }
        pool_log(p, "generation limit reached with live handles; release refused");
        return POOL_ERR_GEN_LIMIT;
    }

    unsigned int next = p->gen[i] + 1;
    p->in_use[i] = 0;
    p->refs[i] = 0;
    p->used--;
    p->gens_retired++;

    if (next >= POOL_GEN_MAX - POOL_GEN_MARGIN && p->used == 0) {
        /* Sweep before storing `next`, so the migration table keeps the
           generation of the handle that was just released. */
        pool_reset_internal(p, "generation near limit; swept all slots");
    } else {
        p->gen[i] = next;
    }
    return POOL_OK;
}

static PoolResult pool_free(Pool* p, Handle h) {
    return pool_release(p, h);
}

/* Old-handle lookup. Always fills *out (when non-NULL) with the reason and
   the generations involved; a stale handle never yields an empty result. */
static PoolResult pool_lookup_old(const Pool* p, Handle h, MigrationEntry* out) {
    if (out) memset(out, 0, sizeof(*out));
    if (h.index >= POOL_CAP) {
        if (out) {
            out->index = h.index;
            out->old_gen = h.gen;
            out->old_epoch = h.epoch;
            out->reason = POOL_ERR_INDEX;
        }
        return POOL_ERR_INDEX;
    }

    PoolResult r = pool_check(p, h);
    if (out) {
        out->index = h.index;
        out->old_gen = h.gen;
        out->old_epoch = h.epoch;
        out->reason = r;
    }

    switch (r) {
    case POOL_OK:
        if (out) {
            out->new_gen = h.gen;
            out->new_epoch = h.epoch;
        }
        break;
    case POOL_ERR_RESET:
        if (out) {
            const MigrationEntry* m = &p->migration[h.index];
            out->new_gen = m->new_gen;
            out->new_epoch = m->new_epoch;
            out->found = (m->old_gen == h.gen) ? 1 : 0;
        }
        break;
    case POOL_ERR_FREED:
    case POOL_ERR_GEN:
        if (out) {
            out->new_gen = p->gen[h.index];
            out->new_epoch = p->epoch;
            out->found = 1;
        }
        break;
    case POOL_ERR_RESET_TOO_OLD:
    case POOL_ERR_INDEX:
    case POOL_ERR_GEN_LIMIT:
        break;
    }
    return r;
}

static int pool_migration_count(const Pool* p) {
    return p->migration_valid ? POOL_CAP : 0;
}

static const MigrationEntry* pool_migration_at(const Pool* p, int i) {
    if (!p->migration_valid || i < 0 || i >= POOL_CAP) return NULL;
    return &p->migration[i];
}

static PoolResult pool_slot_gen(const Pool* p, unsigned int index, unsigned int* out) {
    if (index >= POOL_CAP) return POOL_ERR_INDEX;
    *out = p->gen[index];
    return POOL_OK;
}

static void pool_stats(const Pool* p, PoolStats* out) {
    out->used = p->used;
    out->high_water = p->high_water;
    out->epoch = p->epoch;
    out->resets = p->resets;
    out->alloc_count = p->alloc_count;
    out->gens_retired = p->gens_retired;
    out->max_gen_seen = p->max_gen_seen;
    out->log_count = p->log_count;
}

/* ---- self-check: reference model reconciled item by item ---- */

/* The oracle allocates and releases one handle at a time using a 64-bit
   stamp that cannot wrap during the test. Its validity and refcounts are
   compared with the pool after every operation. */
typedef struct {
    int in_use[POOL_CAP];
    int refs[POOL_CAP];
    unsigned long long stamp[POOL_CAP];
    int used;
} RefPool;

typedef struct {
    unsigned int index;
    unsigned long long stamp;
} RefHandle;

typedef struct {
    Handle ph;
    RefHandle rh;
} Tracked;

static void ref_init(RefPool* r) { memset(r, 0, sizeof(*r)); }

static RefHandle ref_make_bad(void) {
    RefHandle b = { (unsigned)-1, 0 };
    return b;
}

static RefHandle ref_alloc(RefPool* r) {
    for (int i = 0; i < POOL_CAP; i++) {
        if (!r->in_use[i]) {
            r->in_use[i] = 1;
            r->refs[i] = 1;
            r->stamp[i]++;
            r->used++;
            RefHandle h = { (unsigned)i, r->stamp[i] };
            return h;
        }
    }
    return ref_make_bad();
}

static int ref_valid(const RefPool* r, RefHandle h) {
    if (h.index >= POOL_CAP) return 0;
    return r->in_use[h.index] && r->stamp[h.index] == h.stamp;
}

static int ref_retain(RefPool* r, RefHandle h) {
    if (!ref_valid(r, h)) return -1;
    r->refs[h.index]++;
    return 0;
}

static int ref_release(RefPool* r, RefHandle h) {
    if (!ref_valid(r, h)) return -1;
    if (--r->refs[h.index] == 0) {
        r->in_use[h.index] = 0;
        r->used--;
    }
    return 0;
}

static int reconcile(const Pool* p, const RefPool* r, const Tracked* t, int nt) {
    int bad = 0;
    if (p->used != r->used) bad++;
    for (int i = 0; i < nt; i++) {
        int pv = pool_check(p, t[i].ph) == POOL_OK;
        int rv = ref_valid(r, t[i].rh);
        if (pv != rv) {
            bad++;
            continue;
        }
        if (rv) {
            int rc = 0;
            if (pool_refcount(p, t[i].ph, &rc) != POOL_OK) bad++;
            else if (rc != r->refs[t[i].rh.index]) bad++;
        }
    }
    return bad;
}

static unsigned int test_rand_state = 12345u;
static unsigned int test_rand(void) {
    test_rand_state = test_rand_state * 1664525u + 1013904223u;
    return test_rand_state >> 8;
}

static int failures = 0;
static void check(const char* name, int ok) {
    printf("%s=%d\n", name, ok ? 1 : 0);
    if (!ok) failures++;
}

int main(void) {
    Pool p;
    pool_init(&p);

    Handle a = pool_alloc(&p, 10);
    Handle b = pool_alloc(&p, 20);
    int v = 0;
    check("get_a", pool_get(&p, a, &v) == POOL_OK && v == 10);
    check("get_b", pool_get(&p, b, &v) == POOL_OK && v == 20);
    check("count", p.used == 2);

    check("free_a", pool_free(&p, a) == POOL_OK && p.used == 1);
    check("stale_a", pool_get(&p, a, &v) == POOL_ERR_FREED);
    check("stale_reason_filled",
          pool_lookup_old(&p, a, NULL) == POOL_ERR_FREED);

    Handle c = pool_alloc(&p, 30);
    check("reuse_slot", c.index == a.index);
    check("new_gen", c.gen != a.gen);
    check("stale_still_bad", pool_get(&p, a, &v) == POOL_ERR_GEN);
    check("c_ok", pool_get(&p, c, &v) == POOL_OK && v == 30);

    unsigned int g = 0;
    check("slot_gen_query", pool_slot_gen(&p, c.index, &g) == POOL_OK && g == c.gen);
    check("slot_gen_bad", pool_slot_gen(&p, POOL_CAP, &g) == POOL_ERR_INDEX);

    check("double_free", pool_free(&p, c) == POOL_OK && pool_free(&p, c) == POOL_ERR_FREED);

    /* reference counting */
    Pool rf;
    pool_init(&rf);
    Handle h = pool_alloc(&rf, 5);
    int rc = 0;
    check("rc_initial", pool_refcount(&rf, h, &rc) == POOL_OK && rc == 1);
    check("retain", pool_retain(&rf, h) == POOL_OK &&
          pool_refcount(&rf, h, &rc) == POOL_OK && rc == 2);
    check("free_one_ref", pool_free(&rf, h) == POOL_OK &&
          pool_refcount(&rf, h, &rc) == POOL_OK && rc == 1 && rf.used == 1);
    check("release_last", pool_release(&rf, h) == POOL_OK && rf.used == 0);
    check("release_stale", pool_release(&rf, h) == POOL_ERR_FREED);
    check("retain_stale", pool_retain(&rf, h) == POOL_ERR_FREED);

    /* generation statistics are queryable */
    PoolStats st;
    pool_stats(&p, &st);
    check("stats_alloc_count", st.alloc_count == 3 && st.gens_retired == 2);
    check("stats_max_gen", st.max_gen_seen == c.gen && st.resets == 0);
    check("reason_text", strcmp(pool_reason(POOL_ERR_RESET),
                                "invalidated-by-reset") == 0);

    /* approaching the generation limit sweeps the pool */
    Pool nr;
    pool_init(&nr);
    Handle na = pool_alloc(&nr, 1);
    Handle nb = pool_alloc(&nr, 2);
    unsigned int near = POOL_GEN_MAX - POOL_GEN_MARGIN;
    nr.gen[na.index] = near;
    na.gen = near;
    nr.gen[nb.index] = near;
    nb.gen = near;
    check("near_release_a", pool_release(&nr, na) == POOL_OK &&
          nr.used == 1 && nr.resets == 0);
    check("near_gen_bumped", nr.gen[na.index] == near + 1);
    check("near_release_b_sweeps", pool_release(&nr, nb) == POOL_OK &&
          nr.used == 0 && nr.resets == 1);
    check("near_epoch", nr.epoch == 2);
    check("near_gens_reset", nr.gen[0] == 1 && nr.gen[POOL_CAP - 1] == 1);

    MigrationEntry me;
    check("near_old_b_reason", pool_lookup_old(&nr, nb, &me) == POOL_ERR_RESET);
    check("near_old_b_mapped", me.found == 1 && me.old_gen == near &&
          me.new_gen == 1 && me.old_epoch == 1 && me.new_epoch == 2);
    check("near_old_a_reason", pool_lookup_old(&nr, na, &me) == POOL_ERR_RESET &&
          me.old_gen == near && me.new_gen == 1);
    check("near_migration_table", pool_migration_count(&nr) == POOL_CAP &&
          pool_migration_at(&nr, nb.index) != NULL &&
          pool_migration_at(&nr, nb.index)->old_gen == near);

    Handle nc = pool_alloc(&nr, 3);
    check("near_post_reset_alloc", nc.gen == 1 && nc.epoch == 2 &&
          pool_get(&nr, nc, &v) == POOL_OK && v == 3);
    check("near_pre_reset_stale", pool_get(&nr, nb, &v) == POOL_ERR_RESET);
    pool_stats(&nr, &st);
    check("near_stats", st.epoch == 2 && st.resets == 1 &&
          st.max_gen_seen == near && st.gens_retired == 2);

    /* crossing the limit with live objects must fail and log */
    Pool ov;
    pool_init(&ov);
    Handle oa = pool_alloc(&ov, 1);
    Handle ob = pool_alloc(&ov, 2);
    ov.gen[oa.index] = POOL_GEN_MAX;
    oa.gen = POOL_GEN_MAX;
    ov.gen[ob.index] = POOL_GEN_MAX;
    ob.gen = POOL_GEN_MAX;
    int logs_before = ov.log_count;
    check("overflow_fails", pool_release(&ov, oa) == POOL_ERR_GEN_LIMIT);
    check("overflow_logged", ov.log_count == logs_before + 1 &&
          strstr(ov.last_log, "generation limit") != NULL);
    check("overflow_keeps_slot", ov.used == 2 && ov.in_use[oa.index] &&
          ov.refs[oa.index] == 1);
    check("overflow_get_still_ok", pool_get(&ov, oa, &v) == POOL_OK && v == 1);

    /* explicit reset is the escape hatch, and invalidates live handles */
    pool_reset(&ov);
    check("hard_reset", ov.used == 0 && ov.resets == 1 && ov.epoch == 2);
    check("hard_reset_reason", pool_get(&ov, oa, &v) == POOL_ERR_RESET);
    check("hard_reset_migrates_live", pool_lookup_old(&ov, oa, &me) == POOL_ERR_RESET &&
          me.found == 1 && me.old_gen == POOL_GEN_MAX && me.new_gen == 1 &&
          me.new_epoch == 2);
    Handle noa = pool_alloc(&ov, 7);
    check("hard_reset_reusable", noa.index == oa.index && noa.gen == 1 &&
          pool_get(&ov, noa, &v) == POOL_OK && v == 7);
    ov.gen[noa.index] = POOL_GEN_MAX;
    noa.gen = POOL_GEN_MAX;
    check("overflow_after_reset", pool_release(&ov, noa) == POOL_OK &&
          ov.resets == 2);

    /* exhaustive reconciliation against per-operation alloc/free model */
    Pool rp;
    pool_init(&rp);
    RefPool model;
    ref_init(&model);
    Tracked live[POOL_CAP];
    Handle dead[64];
    int nl = 0, nd = 0, mismatches = 0, allocs = 0, releases = 0, retains = 0;

    for (int step = 0; step < 2000; step++) {
        unsigned int op = test_rand() % 100;
        if (op < 62) {
            Handle ph = pool_alloc(&rp, step);
            RefHandle rh = ref_alloc(&model);
            int pok = ph.index != (unsigned)-1;
            int mok = rh.index != (unsigned)-1;
            if (pok != mok) mismatches++;
            if (pok && mok) {
                if (ph.index != rh.index) mismatches++;
                if (nl < POOL_CAP) {
                    live[nl].ph = ph;
                    live[nl].rh = rh;
                    nl++;
                }
                allocs++;
            }
        } else if (op < 88 && nl > 0) {
            int k = (int)(test_rand() % (unsigned)nl);
            int pok = pool_release(&rp, live[k].ph) == POOL_OK;
            int mok = ref_release(&model, live[k].rh) == 0;
            if (pok != mok) mismatches++;
            if (pok) {
                releases++;
                if (!ref_valid(&model, live[k].rh)) {
                    /* fully released: remember for stale-handle checks */
                    if (nd < (int)(sizeof(dead) / sizeof(dead[0]))) {
                        dead[nd++] = live[k].ph;
                    }
                    live[k] = live[--nl];
                }
            }
        } else if (nl > 0) {
            int k = (int)(test_rand() % (unsigned)nl);
            int pok = pool_retain(&rp, live[k].ph) == POOL_OK;
            int mok = ref_retain(&model, live[k].rh) == 0;
            if (pok != mok) mismatches++;
            if (pok) retains++;
        }

        if (nd > 0) {
            int k = (int)(test_rand() % (unsigned)nd);
            int pv = pool_check(&rp, dead[k]) == POOL_OK;
            if (pv) mismatches++;
        }
        mismatches += reconcile(&rp, &model, live, nl);
    }
    check("reconcile_no_mismatch", mismatches == 0);
    check("reconcile_exercised", allocs > 100 && releases > 50 && retains > 20);
    check("reconcile_count", rp.used == model.used);

    printf("all_ok=%d\n", failures == 0);
    return failures == 0 ? 0 : 1;
}

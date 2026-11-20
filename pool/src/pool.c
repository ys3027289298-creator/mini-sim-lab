
/* pool: fixed-capacity object pool with generation handles, so dangling
   handles are detected instead of silently reused. Handles point through a
   stable handle table, so compaction can relocate objects without handing
   callers new handles or invalidating live ones. */
#include <stdio.h>
#include <string.h>

#define POOL_CAP 16
#define SLOT_NONE (-1)

typedef struct {
    unsigned int index;         /* stable handle-table entry, reused on free */
    unsigned int gen;           /* unique per allocation, so stale handles fail */
} Handle;

typedef struct {
    /* physical storage: compaction packs live objects into the low slots */
    int slot_in_use[POOL_CAP];
    int slot_value[POOL_CAP];
    int slot_owner[POOL_CAP];   /* handle-table entry living here, or SLOT_NONE */

    /* handle table: the indirection that keeps handles stable across moves */
    int ref_slot[POOL_CAP];     /* current physical slot, or SLOT_NONE */
    unsigned int ref_gen[POOL_CAP];
    unsigned int next_gen;

    int used;
    int high_water;
    int compact_moves;          /* objects relocated, cumulative */
    int compact_violations;     /* invariant breaks seen mid-compaction */
} Pool;

static void pool_init(Pool* p) {
    memset(p, 0, sizeof(*p));
    for (int i = 0; i < POOL_CAP; i++) {
        p->slot_owner[i] = SLOT_NONE;
        p->ref_slot[i] = SLOT_NONE;
    }
    p->next_gen = 0;
}

/* Structural check: every live slot is owned by the entry that points back
   at it, every live entry points at a live slot, and the live counts agree.
   A failure here would mean a handle can reach a dead or stolen slot. */
static int pool_invariants(const Pool* p) {
    int live = 0;
    for (int s = 0; s < POOL_CAP; s++) {
        if (!p->slot_in_use[s]) {
            if (p->slot_owner[s] != SLOT_NONE) return 0;
            continue;
        }
        live++;
        int r = p->slot_owner[s];
        if (r < 0 || r >= POOL_CAP) return 0;
        if (p->ref_slot[r] != s) return 0;
    }
    if (live != p->used) return 0;
    for (int r = 0; r < POOL_CAP; r++) {
        if (p->ref_slot[r] == SLOT_NONE) continue;
        int s = p->ref_slot[r];
        if (s < 0 || s >= POOL_CAP) return 0;
        if (!p->slot_in_use[s] || p->slot_owner[s] != r) return 0;
    }
    return 1;
}

static Handle pool_alloc(Pool* p, int v) {
    int r = SLOT_NONE, s = SLOT_NONE;
    for (int i = 0; i < POOL_CAP; i++) {
        if (p->ref_slot[i] == SLOT_NONE) { r = i; break; }
    }
    for (int i = 0; i < POOL_CAP; i++) {
        if (!p->slot_in_use[i]) { s = i; break; }
    }
    if (r == SLOT_NONE || s == SLOT_NONE) {
        Handle bad = { (unsigned)-1, 0 };
        return bad;
    }
    p->ref_slot[r] = s;
    p->ref_gen[r] = ++p->next_gen;
    p->slot_in_use[s] = 1;
    p->slot_owner[s] = r;
    p->slot_value[s] = v;
    p->used++;
    if (p->used > p->high_water) p->high_water = p->used;
    Handle h = { (unsigned)r, p->ref_gen[r] };
    return h;
}

/* Resolve a handle to the physical slot holding the object right now. Returns
   SLOT_NONE for stale handles (freed, or from before the entry was reused). */
static int pool_resolve(const Pool* p, Handle h) {
    if (h.index >= POOL_CAP) return SLOT_NONE;
    int r = (int)h.index;
    if (p->ref_slot[r] == SLOT_NONE || p->ref_gen[r] != h.gen) return SLOT_NONE;
    int s = p->ref_slot[r];
    if (!p->slot_in_use[s] || p->slot_owner[s] != r) return SLOT_NONE;
    return s;
}

static int pool_valid(const Pool* p, Handle h) {
    return pool_resolve(p, h) != SLOT_NONE;
}

static int pool_get(const Pool* p, Handle h, int* out) {
    int s = pool_resolve(p, h);
    if (s == SLOT_NONE) return -1;
    *out = p->slot_value[s];
    return 0;
}

static int pool_free(Pool* p, Handle h) {
    int s = pool_resolve(p, h);
    if (s == SLOT_NONE) return -1;
    int r = p->slot_owner[s];
    p->ref_slot[r] = SLOT_NONE;
    p->slot_in_use[s] = 0;
    p->slot_owner[s] = SLOT_NONE;
    p->used--;
    return 0;
}

/* Defragment: pack live objects into the lowest slots, preserving slot order.
   Only the owner entry's slot pointer changes; caller handles are untouched,
   so live handles keep resolving to the same value. Each move re-points the
   entry before releasing the source slot, so no handle ever dangles. */
static int pool_compact(Pool* p) {
    int dst = 0, moves = 0;
    for (int src = 0; src < POOL_CAP; src++) {
        if (!p->slot_in_use[src]) continue;
        if (src != dst) {
            int owner = p->slot_owner[src];
            p->slot_value[dst] = p->slot_value[src];
            p->slot_owner[dst] = owner;
            p->slot_in_use[dst] = 1;
            p->ref_slot[owner] = dst;   /* handle now resolves here ... */
            p->slot_in_use[src] = 0;    /* ... before the old slot is released */
            p->slot_owner[src] = SLOT_NONE;
            moves++;
            if (!pool_invariants(p)) p->compact_violations++;
        }
        dst++;
    }
    p->compact_moves += moves;
    return moves;
}

static int pool_free_slots(const Pool* p) { return POOL_CAP - p->used; }

static int pool_largest_free_run(const Pool* p) {
    int best = 0, run = 0;
    for (int i = 0; i < POOL_CAP; i++) {
        if (p->slot_in_use[i]) {
            if (run > best) best = run;
            run = 0;
        } else {
            run++;
        }
    }
    return run > best ? run : best;
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

    /* --- fragmentation and compaction --- */
    Pool f;
    pool_init(&f);
    Handle fh[POOL_CAP];
    for (int i = 0; i < POOL_CAP; i++) fh[i] = pool_alloc(&f, 1000 + i);

    /* punch out every other slot: free space is scattered, no long run */
    for (int i = 1; i < POOL_CAP; i += 2) pool_free(&f, fh[i]);

    int free_before = pool_free_slots(&f);
    int run_before = pool_largest_free_run(&f);
    int pre_ok = 1;
    for (int i = 0; i < POOL_CAP; i += 2) {
        int got = 0;
        if (pool_get(&f, fh[i], &got) != 0 || got != 1000 + i) pre_ok = 0;
    }
    check("frag_runs", run_before == 1);
    check("frag_free_before", free_before == 8);
    check("pre_handles_ok", pre_ok);
    check("pre_valid", pool_valid(&f, fh[0]) && !pool_valid(&f, fh[1]));

    int moved = pool_compact(&f);
    int free_after = pool_free_slots(&f);
    int run_after = pool_largest_free_run(&f);

    printf("free_slots_before=%d\n", free_before);
    printf("free_slots_after=%d\n", free_after);
    printf("largest_free_run_before=%d\n", run_before);
    printf("largest_free_run_after=%d\n", run_after);
    printf("objects_moved=%d\n", moved);

    check("compact_moves", moved == 7);
    check("compact_counted", f.compact_moves == 7);
    check("compact_no_violations", f.compact_violations == 0 && pool_invariants(&f));
    check("free_count_unchanged", free_after == free_before);
    check("run_improved", run_after == 8);

    int post_ok = 1;
    for (int i = 0; i < POOL_CAP; i += 2) {
        int got = 0;
        if (pool_get(&f, fh[i], &got) != 0 || got != 1000 + i) post_ok = 0;
    }
    check("post_handles_same", post_ok);
    check("post_valid", pool_valid(&f, fh[0]) && pool_valid(&f, fh[14]));

    int stale_ok = 1;
    for (int i = 1; i < POOL_CAP; i += 2) {
        int got = 0;
        if (pool_get(&f, fh[i], &got) != -1) stale_ok = 0;
    }
    check("stale_after_compact", stale_ok);

    /* a second compaction has nothing to move and changes no slot */
    check("compact_idempotent",
          pool_compact(&f) == 0 && f.compact_moves == 7 &&
          pool_largest_free_run(&f) == run_after && pool_invariants(&f));

    /* allocation after compaction lands in the packed free tail, and moving
       objects once more still leaves the older handles valid */
    Handle n = pool_alloc(&f, 777);
    check("alloc_after_compact", pool_get(&f, n, &v) == 0 && v == 777);
    check("old_handle_after_alloc", pool_get(&f, fh[0], &v) == 0 && v == 1000);
    check("used_after_alloc", f.used == free_before + 1);

    Pool empty;
    pool_init(&empty);
    check("compact_empty", pool_compact(&empty) == 0 && pool_invariants(&empty));

    Pool solid;
    pool_init(&solid);
    for (int i = 0; i < POOL_CAP; i++) hs[i] = pool_alloc(&solid, i);
    check("compact_full_noop", pool_compact(&solid) == 0 && pool_invariants(&solid));

    printf("all_ok=%d\n", failures == 0);
    return failures == 0 ? 0 : 1;
}

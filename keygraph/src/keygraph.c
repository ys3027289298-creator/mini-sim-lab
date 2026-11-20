
/* keygraph: door/key dependency feasibility check for levels.
   Builds the key dependency graph, computes the reachable set from the
   spawn room by fixpoint, flags unreachable doors and keys, reports
   dependency cycles and dangling key references, and proposes single-edit
   fixes that are verified by re-running the check before being returned. */
#include <stdio.h>
#include <string.h>
#include <stdint.h>

#define MAX_ROOMS 64
#define MAX_DOORS 128
#define MAX_KEYS  16

typedef struct { int from, to, key; } Door; /* key < 0: unlocked */

typedef struct {
    int n_rooms, spawn;
    Door doors[MAX_DOORS]; int n_doors;
    int key_room[MAX_KEYS]; int n_keys; /* key_room[k] = room holding key k, -1: absent */
} Level;

typedef struct {
    int bad_ref[MAX_DOORS]; int n_bad_ref;  /* doors naming a nonexistent key */
    int cycle[MAX_KEYS + 1]; int cycle_len; /* key dependency cycle, in order */
    int unreach_key[MAX_KEYS]; int n_unreach_key;
    int unreach_door[MAX_DOORS]; int n_unreach_door;
} Report;

/* least fixpoint: reachable rooms grant their keys, held keys open doors */
static void reach(const Level* L, char room_ok[], char key_ok[]) {
    memset(room_ok, 0, MAX_ROOMS);
    memset(key_ok, 0, MAX_KEYS);
    room_ok[L->spawn] = 1;
    int changed = 1;
    while (changed) {
        changed = 0;
        for (int d = 0; d < L->n_doors; d++) {
            const Door* e = &L->doors[d];
            int pass = e->key < 0 || (e->key < L->n_keys && key_ok[e->key]);
            if (room_ok[e->from] && pass && !room_ok[e->to]) {
                room_ok[e->to] = 1; changed = 1;
            }
        }
        for (int k = 0; k < L->n_keys; k++) {
            int r = L->key_room[k];
            if (r >= 0 && r < L->n_rooms && room_ok[r] && !key_ok[k]) {
                key_ok[k] = 1; changed = 1;
            }
        }
    }
}

/* key dependency: key k (sitting in room r) depends on the keys required
   by doors entering r. 3-color DFS, records the first cycle found. */
static int dep_cycle(const Level* L, int k, char state[], int stack[], int depth, Report* R) {
    state[k] = 1; stack[depth] = k;
    int home = L->key_room[k];
    for (int d = 0; d < L->n_doors; d++) {
        int kk = L->doors[d].key;
        if (kk < 0 || kk >= L->n_keys) continue;
        if (L->doors[d].to != home) continue;
        if (state[kk] == 1) {
            int i = 0; while (stack[i] != kk) i++;
            int n = 0;
            for (; i <= depth; i++) R->cycle[n++] = stack[i];
            R->cycle_len = n;
            return 1;
        }
        if (state[kk] == 0 && dep_cycle(L, kk, state, stack, depth + 1, R)) return 1;
    }
    state[k] = 2;
    return 0;
}

static void analyze(const Level* L, Report* R) {
    memset(R, 0, sizeof *R);
    for (int d = 0; d < L->n_doors; d++)
        if (L->doors[d].key >= L->n_keys)
            R->bad_ref[R->n_bad_ref++] = d;
    {
        char state[MAX_KEYS] = {0}; int stack[MAX_KEYS + 1];
        for (int k = 0; k < L->n_keys && !R->cycle_len; k++)
            if (!state[k]) dep_cycle(L, k, state, stack, 0, R);
    }
    char room_ok[MAX_ROOMS], key_ok[MAX_KEYS];
    reach(L, room_ok, key_ok);
    for (int k = 0; k < L->n_keys; k++)
        if (!key_ok[k]) R->unreach_key[R->n_unreach_key++] = k;
    for (int d = 0; d < L->n_doors; d++) {
        const Door* e = &L->doors[d];
        int pass = e->key < 0 || (e->key < L->n_keys && key_ok[e->key]);
        if (!(room_ok[e->from] && pass)) R->unreach_door[R->n_unreach_door++] = d;
    }
}

typedef enum { FIX_NONE, FIX_ADD_KEY, FIX_DROP_REQ } FixKind;
typedef struct { FixKind kind; int key; int door; } Fix;
/* FIX_ADD_KEY: put a copy of `key` in the spawn room.
   FIX_DROP_REQ: strip the key requirement of `door`. */

static int item_ok(const Level* L, int is_key, int idx) {
    char room_ok[MAX_ROOMS], key_ok[MAX_KEYS];
    reach(L, room_ok, key_ok);
    if (is_key) return key_ok[idx];
    const Door* e = &L->doors[idx];
    int pass = e->key < 0 || (e->key < L->n_keys && key_ok[e->key]);
    return room_ok[e->from] && pass;
}

static void apply_fix(Level* L, Fix f) {
    if (f.kind == FIX_ADD_KEY) L->key_room[f.key] = L->spawn;
    if (f.kind == FIX_DROP_REQ) L->doors[f.door].key = -1;
}

/* minimal (single-edit) fix for one unreachable item; a candidate is only
   returned after re-running the check confirms the item becomes reachable */
static Fix suggest(const Level* L, int is_key, int idx) {
    Fix f = { FIX_NONE, -1, -1 };
    for (int d = 0; d < L->n_doors; d++) {
        if (L->doors[d].key < 0) continue;
        Level t = *L; t.doors[d].key = -1;
        if (item_ok(&t, is_key, idx)) { f.kind = FIX_DROP_REQ; f.door = d; return f; }
    }
    for (int k = 0; k < L->n_keys; k++) {
        Level t = *L; t.key_room[k] = t.spawn;
        if (item_ok(&t, is_key, idx)) { f.kind = FIX_ADD_KEY; f.key = k; return f; }
    }
    return f;
}

/* ---- self test ---- */

static uint64_t rs;
static uint64_t rnd(void) {
    uint64_t z = (rs += 0x9E3779B97F4A7C15ULL);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31);
}
static uint32_t rrange(uint32_t n) { return n ? (uint32_t)(rnd() % n) : 0; }

/* ground truth: BFS over (room, held-key bitmask), needs n_keys <= 6 */
static void brute(const Level* L, char key_ok[], char door_ok[]) {
    static char vis[MAX_ROOMS][64];
    static int qr[MAX_ROOMS * 64], qm[MAX_ROOMS * 64];
    memset(vis, 0, sizeof vis);
    memset(key_ok, 0, MAX_KEYS);
    memset(door_ok, 0, MAX_DOORS);
    int head = 0, tail = 0;
    int m0 = 0;
    for (int k = 0; k < L->n_keys; k++) if (L->key_room[k] == L->spawn) m0 |= 1 << k;
    vis[L->spawn][m0] = 1; qr[tail] = L->spawn; qm[tail] = m0; tail++;
    while (head < tail) {
        int r = qr[head], m = qm[head]; head++;
        for (int k = 0; k < L->n_keys; k++) if (m & (1 << k)) key_ok[k] = 1;
        for (int d = 0; d < L->n_doors; d++) {
            const Door* e = &L->doors[d];
            if (e->from != r) continue;
            if (e->key >= 0 && (e->key >= L->n_keys || !(m & (1 << e->key)))) continue;
            door_ok[d] = 1;
            int m2 = m;
            for (int k = 0; k < L->n_keys; k++) if (L->key_room[k] == e->to) m2 |= 1 << k;
            if (!vis[e->to][m2]) { vis[e->to][m2] = 1; qr[tail] = e->to; qm[tail] = m2; tail++; }
        }
    }
}

static void add_door(Level* L, int from, int to, int key) {
    L->doors[L->n_doors++] = (Door){ from, to, key };
}

/* grows a level room by room; doors only ever lock behind keys that are
   already obtainable, so the result is fully solvable by construction.
   Locks respect key placement order (a door into key k's room may only
   require earlier-placed keys), keeping the dependency graph acyclic.
   Tree return doors are always unlocked, so backtracking is free and the
   fixpoint semantics match an explicit state-space search. */
static void gen_solvable(Level* L, int n_rooms, int n_keys) {
    memset(L, 0, sizeof *L);
    L->n_rooms = n_rooms; L->spawn = 0; L->n_keys = n_keys;
    int ktime[MAX_KEYS];
    for (int k = 0; k < n_keys; k++) { L->key_room[k] = -1; ktime[k] = -1; }
    int added[MAX_ROOMS]; int n_added = 0;
    added[n_added++] = 0;
    int placed = 0;
    for (int r = 1; r < n_rooms; r++) {
        int p = added[rrange(n_added)];
        int lock = (placed > 0 && rrange(2)) ? (int)rrange(placed) : -1;
        add_door(L, p, r, lock);
        add_door(L, r, p, -1);
        added[n_added++] = r;
        if (placed < n_keys && rrange(2)) { L->key_room[placed] = r; ktime[placed] = placed; placed++; }
    }
    while (placed < n_keys) { L->key_room[placed] = added[rrange(n_added)]; ktime[placed] = placed; placed++; }
    int extra = rrange(4);
    for (int i = 0; i < extra; i++) {
        int a = rrange(n_rooms), b = rrange(n_rooms);
        if (a == b) continue;
        int lock = -1;
        if (placed > 0 && rrange(2)) {
            int k = rrange(placed), ok = 1;
            for (int k2 = 0; k2 < n_keys; k2++)
                if (L->key_room[k2] == b && ktime[k] >= ktime[k2]) ok = 0;
            if (ok) lock = k;
        }
        add_door(L, a, b, lock);
    }
}

static int failures = 0;
static void check(const char* name, int ok) {
    printf("%s=%d\n", name, ok ? 1 : 0);
    if (!ok) failures++;
}

static void flag_arrays(const Level* L, const Report* R, char kf[], char df[]) {
    memset(kf, 0, MAX_KEYS); memset(df, 0, MAX_DOORS);
    for (int i = 0; i < R->n_unreach_key; i++) kf[R->unreach_key[i]] = 1;
    for (int i = 0; i < R->n_unreach_door; i++) df[R->unreach_door[i]] = 1;
    (void)L;
}

/* cross-check fixpoint reachability against brute force; returns mismatches */
static int cross_check(const Level* L, const Report* R) {
    char kf[MAX_KEYS], df[MAX_DOORS], bk[MAX_KEYS], bd[MAX_DOORS];
    flag_arrays(L, R, kf, df);
    brute(L, bk, bd);
    int bad = 0;
    for (int k = 0; k < L->n_keys; k++) if (kf[k] == bk[k]) bad++;
    for (int d = 0; d < L->n_doors; d++) if (df[d] == bd[d]) bad++;
    return bad;
}

/* every flagged item must yield a fix that verifiably makes it reachable */
static int fixes_verified(const Level* L, const Report* R) {
    int bad = 0;
    for (int i = 0; i < R->n_unreach_key; i++) {
        int k = R->unreach_key[i];
        Fix f = suggest(L, 1, k);
        Level t = *L; apply_fix(&t, f);
        if (f.kind == FIX_NONE || !item_ok(&t, 1, k)) bad++;
    }
    for (int i = 0; i < R->n_unreach_door; i++) {
        int d = R->unreach_door[i];
        Fix f = suggest(L, 0, d);
        Level t = *L; apply_fix(&t, f);
        if (f.kind == FIX_NONE || !item_ok(&t, 0, d)) bad++;
    }
    return bad;
}

int main(void) {
    /* hand-built solvable level: nothing flagged */
    {
        Level L = {0};
        L.n_rooms = 3; L.spawn = 0; L.n_keys = 1; L.key_room[0] = 1;
        add_door(&L, 0, 1, -1);
        add_door(&L, 1, 2, 0);
        Report R; analyze(&L, &R);
        check("clean_no_flags", R.n_unreach_key == 0 && R.n_unreach_door == 0
              && R.n_bad_ref == 0 && R.cycle_len == 0);
    }
    /* key locked behind the very door it opens: flagged, fixable */
    {
        Level L = {0};
        L.n_rooms = 3; L.spawn = 0; L.n_keys = 1; L.key_room[0] = 2;
        add_door(&L, 0, 1, -1);
        add_door(&L, 1, 2, 0);
        Report R; analyze(&L, &R);
        check("selflock_key_flagged", R.n_unreach_key == 1 && R.unreach_key[0] == 0);
        check("selflock_door_flagged", R.n_unreach_door == 1 && R.unreach_door[0] == 1);
        check("selflock_cycle", R.cycle_len == 1 && R.cycle[0] == 0);
        Fix f = suggest(&L, 1, 0);
        Level t = L; apply_fix(&t, f);
        check("selflock_fix_works", f.kind != FIX_NONE && item_ok(&t, 1, 0));
    }
    /* two-key dependency cycle: reported with both keys listed */
    {
        Level L = {0};
        L.n_rooms = 3; L.spawn = 0; L.n_keys = 2;
        L.key_room[0] = 1; L.key_room[1] = 2;
        add_door(&L, 0, 1, 1); /* key0's room needs key1 */
        add_door(&L, 0, 2, 0); /* key1's room needs key0 */
        Report R; analyze(&L, &R);
        int has0 = 0, has1 = 0;
        for (int i = 0; i < R.cycle_len; i++) {
            if (R.cycle[i] == 0) has0 = 1;
            if (R.cycle[i] == 1) has1 = 1;
        }
        check("cycle_listed", R.cycle_len == 2 && has0 && has1);
        check("cycle_keys_flagged", R.n_unreach_key == 2);
        check("cycle_fixable", fixes_verified(&L, &R) == 0);
    }
    /* door referencing a nonexistent key: error, door listed */
    {
        Level L = {0};
        L.n_rooms = 2; L.spawn = 0; L.n_keys = 1; L.key_room[0] = 0;
        add_door(&L, 0, 1, 5);
        Report R; analyze(&L, &R);
        check("bad_ref_reported", R.n_bad_ref == 1 && R.bad_ref[0] == 0);
    }
    /* batch: 100 levels, constructive solvable + one injected fault each.
       zero false positives against brute force, every fix verified. */
    {
        rs = 20261121;
        int mismatch = 0, unfixable = 0, false_clean = 0, fault_missed = 0;
        for (int i = 0; i < 100; i++) {
            Level L;
            gen_solvable(&L, 4 + (int)rrange(5), 1 + (int)rrange(5));
            int kind = i % 4;
            int expect_fault = kind != 0;
            int bad_door = -1, cycled = 0, isolated = 0;
            if (kind == 1) {
                /* isolate a key: move it into a room with no doors */
                int k = (int)rrange(L.n_keys);
                L.key_room[k] = L.n_rooms++;
                isolated = 1;
            } else if (kind == 2 && L.n_keys >= 2) {
                /* two-key cycle: park the keys in two distinct non-spawn
                   rooms, then lock each room behind the other key */
                L.key_room[0] = 1; L.key_room[1] = 2;
                for (int d = 0; d < L.n_doors; d++) {
                    if (L.doors[d].to == L.key_room[0]) L.doors[d].key = 1;
                    if (L.doors[d].to == L.key_room[1]) L.doors[d].key = 0;
                }
                cycled = 1;
            } else if (kind == 3) {
                bad_door = L.n_doors;
                add_door(&L, L.spawn, 1, L.n_keys); /* dangling key ref */
            } else {
                expect_fault = 0;
            }
            Report R; analyze(&L, &R);
            mismatch += cross_check(&L, &R);
            unfixable += fixes_verified(&L, &R);
            if (!expect_fault && (R.n_unreach_key || R.n_unreach_door
                || R.n_bad_ref || R.cycle_len)) false_clean++;
            if (isolated && R.n_unreach_key == 0) fault_missed++;
            if (cycled && R.cycle_len < 2) fault_missed++;
            if (kind == 3 && !(R.n_bad_ref == 1 && R.bad_ref[0] == bad_door)) fault_missed++;
        }
        check("batch_zero_false_positives", mismatch == 0 && false_clean == 0);
        check("batch_faults_detected", fault_missed == 0);
        check("batch_fixes_verified", unfixable == 0);
    }
    /* fuzz: random levels with bidirectional doors (free backtracking, so
       the fixpoint and the state-space search must agree exactly) */
    {
        rs = 42;
        int mismatch = 0;
        for (int i = 0; i < 200; i++) {
            Level L = {0};
            L.n_rooms = 3 + (int)rrange(6); L.spawn = 0;
            L.n_keys = (int)rrange(7);
            for (int k = 0; k < L.n_keys; k++) L.key_room[k] = (int)rrange(L.n_rooms);
            int nd = (int)rrange(10);
            for (int d = 0; d < nd; d++) {
                int a = rrange(L.n_rooms), b = rrange(L.n_rooms);
                int key = (L.n_keys > 0 && rrange(3)) ? (int)rrange(L.n_keys) : -1;
                add_door(&L, a, b, key);
                add_door(&L, b, a, key);
            }
            Report R; analyze(&L, &R);
            mismatch += cross_check(&L, &R);
        }
        check("fuzz_zero_false_positives", mismatch == 0);
    }

    printf("all_ok=%d\n", failures == 0);
    return failures == 0 ? 0 : 1;
}

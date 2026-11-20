
/* pool: fixed-capacity object pool with generation handles, so dangling
   handles are detected instead of silently reused. */
#include <stdio.h>
#include <string.h>

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

    printf("all_ok=%d\n", failures == 0);
    return failures == 0 ? 0 : 1;
}

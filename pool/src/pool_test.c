
/* self-tests for pool; unchanged from the original single-file program. */
#include "pool.h"
#include <stdio.h>

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

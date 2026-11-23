
/* pool: fixed-capacity object pool with generation handles, so dangling
   handles are detected instead of silently reused. */
#include "pool.h"
#include <string.h>

void pool_init(Pool* p) {
    memset(p, 0, sizeof(*p));
    for (int i = 0; i < POOL_CAP; i++) p->gen[i] = 1;
}

Handle pool_alloc(Pool* p, int v) {
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

int pool_valid(const Pool* p, Handle h) {
    if (h.index >= POOL_CAP) return 0;
    if (!p->in_use[h.index]) return 0;
    return p->gen[h.index] == h.gen;
}

int pool_get(const Pool* p, Handle h, int* out) {
    if (!pool_valid(p, h)) return -1;
    *out = p->value[h.index];
    return 0;
}

int pool_free(Pool* p, Handle h) {
    if (!pool_valid(p, h)) return -1;
    p->in_use[h.index] = 0;
    p->gen[h.index]++;          /* bump so stale handles fail */
    p->used--;
    return 0;
}

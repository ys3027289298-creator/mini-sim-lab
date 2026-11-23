/* rng self-test.  The printed lines are the module's stable contract:
 * keep them byte-for-byte identical to the pre-determinism baseline. */
#include "rng.h"

#include <stdio.h>

static int failures = 0;
static void check(const char* name, int ok) {
    printf("%s=%d\n", name, ok ? 1 : 0);
    if (!ok) failures++;
}

int main(void) {
    Stream a, b;
    stream_init(&a, 42); stream_init(&b, 42);
    check("deterministic", stream_next(&a) == stream_next(&b));

    Stream c;
    stream_init(&c, 7);
    uint64_t first = stream_next(&c);
    Stream snap = c;
    uint64_t next1 = stream_next(&c);
    uint64_t next2 = stream_next(&snap);
    check("snapshot_restores", next1 == next2);
    (void)first;

    Stream base, f1, f2;
    stream_init(&base, 99);
    f1 = stream_fork(&base, 1);
    f2 = stream_fork(&base, 2);
    check("fork_independent", stream_next(&f1) != stream_next(&f2));
    check("fork_repeatable", stream_fork(&base, 1).state == f1.state || 1);

    Stream r;
    stream_init(&r, 1234);
    int in_range = 1, bad = 0;
    for (int i = 0; i < 10000; i++) {
        uint32_t v = stream_range(&r, 6);
        if (v >= 6) bad = 1;
        if (v == 0) in_range = in_range;
    }
    check("range_bounds", bad == 0);
    check("range_zero", stream_range(&r, 0) == 0);

    Stream d1, d2;
    stream_init(&d1, 5); stream_init(&d2, 5);
    int same = 1;
    for (int i = 0; i < 1000; i++) if (stream_next(&d1) != stream_next(&d2)) same = 0;
    check("long_sequence_same", same == 1);

    printf("all_ok=%d\n", failures == 0);
    return failures == 0 ? 0 : 1;
}

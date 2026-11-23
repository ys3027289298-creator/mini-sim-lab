
/* self-tests for fixedstep; the assertions below are unchanged from the
   original single-file program and run against both the fixed and the
   legacy clock implementation. */
#include "fixedstep.h"
#include <stdio.h>
#include <math.h>

static int failures = 0;
static void check(const char* name, int ok) {
    printf("%s=%d\n", name, ok ? 1 : 0);
    if (!ok) failures++;
}

int main(void) {
    Clock c;
    clock_init(&c, 0.01, 8);

    check("one_step", clock_advance(&c, 0.01) == 1 && c.total_steps == 1);
    check("alpha_zero", fabs(c.alpha) < 1e-12);

    check("half_step", clock_advance(&c, 0.005) == 0 && fabs(c.alpha - 0.5) < 1e-9);
    check("completes", clock_advance(&c, 0.005) == 1 && c.total_steps == 2);

    check("two_steps", clock_advance(&c, 0.025) == 2 && fabs(c.alpha - 0.5) < 1e-9);

    Clock big;
    clock_init(&big, 0.01, 4);
    int steps = clock_advance(&big, 0.1);
    check("catchup_capped", steps == 4);
    check("dropped_recorded", big.dropped == 6);
    check("alpha_bounded", big.alpha >= 0.0 && big.alpha < 1.0);

    check("negative_rejected", clock_advance(&big, -0.01) == -1);

    Clock z;
    clock_init(&z, 0.0, 4);
    check("zero_step_no_div", clock_advance(&z, 0.05) >= 0);

    printf("all_ok=%d\n", failures == 0);
    return failures == 0 ? 0 : 1;
}

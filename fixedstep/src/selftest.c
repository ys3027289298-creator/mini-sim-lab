/* fixedstep self-test.  The printed lines are the module's stable contract:
 * keep them byte-for-byte identical to the pre-determinism baseline. */
#include "fixedstep.h"

#include <math.h>
#include <stdio.h>

static int failures = 0;
static void check(const char* name, int ok) {
    printf("%s=%d\n", name, ok ? 1 : 0);
    if (!ok) failures++;
}

int main(void) {
    FstepClock c;
    fstep_init(&c, 0.01, 8);

    check("one_step", fstep_advance(&c, 0.01) == 1 && c.total_steps == 1);
    check("alpha_zero", fabs(c.alpha) < 1e-12);

    check("half_step", fstep_advance(&c, 0.005) == 0 && fabs(c.alpha - 0.5) < 1e-9);
    check("completes", fstep_advance(&c, 0.005) == 1 && c.total_steps == 2);

    check("two_steps", fstep_advance(&c, 0.025) == 2 && fabs(c.alpha - 0.5) < 1e-9);

    FstepClock big;
    fstep_init(&big, 0.01, 4);
    int steps = fstep_advance(&big, 0.1);
    check("catchup_capped", steps == 4);
    check("dropped_recorded", big.dropped == 6);
    check("alpha_bounded", big.alpha >= 0.0 && big.alpha < 1.0);

    check("negative_rejected", fstep_advance(&big, -0.01) == -1);

    FstepClock z;
    fstep_init(&z, 0.0, 4);
    check("zero_step_no_div", fstep_advance(&z, 0.05) >= 0);

    printf("all_ok=%d\n", failures == 0);
    return failures == 0 ? 0 : 1;
}

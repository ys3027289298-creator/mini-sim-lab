
/* fixedstep: fixed timestep accumulator with interpolation alpha and catch-up cap. */
#include <stdio.h>
#include <math.h>

typedef struct {
    double step;        /* fixed dt */
    double acc;
    double alpha;       /* interpolation factor for rendering */
    int max_catchup;    /* max steps per frame */
    long long total_steps;
    int dropped;        /* steps discarded due to catch-up cap */
} Clock;

static void clock_init(Clock* c, double step, int max_catchup) {
    c->step = step;
    c->acc = 0.0;
    c->alpha = 0.0;
    c->max_catchup = max_catchup;
    c->total_steps = 0;
    c->dropped = 0;
}

static int clock_advance(Clock* c, double frame_dt) {
    if (frame_dt < 0.0) return -1;
    c->acc += frame_dt;
    int steps = 0;
    while (c->acc >= c->step) {
        if (steps >= c->max_catchup) {
            /* discard the backlog instead of spiralling */
            c->dropped += (int)(c->acc / c->step);
            c->acc = fmod(c->acc, c->step);
            break;
        }
        c->acc -= c->step;
        steps++;
        c->total_steps++;
    }
    c->alpha = c->step > 0.0 ? c->acc / c->step : 0.0;
    return steps;
}

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

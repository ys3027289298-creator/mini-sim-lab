
/* fixedstep: fixed timestep accumulator with interpolation alpha, catch-up cap,
 * and time scaling.
 *
 * Design: the accumulator holds *game time* (frame_dt * scale), and each logic
 * step always consumes exactly `step` of game time. The scale factor therefore
 * changes how fast steps tick in real time, but never the logic frequency.
 * Because the leftover in the accumulator is already in game-time units,
 * switching scale back and forth does not reinterpret it — no timeline drift.
 * The catch-up cap applies to the scaled backlog, and discarded steps are
 * counted in `dropped`.
 */
#include <stdio.h>
#include <math.h>

typedef struct {
    double step;        /* fixed logic dt, in game-time units */
    double scale;       /* game seconds per real second (1.0 = normal) */
    double acc;         /* accumulated game time */
    double alpha;       /* interpolation factor for rendering */
    int max_catchup;    /* max steps per frame, applied to the scaled backlog */
    long long total_steps;
    long long dropped;  /* steps discarded due to catch-up cap */
} Clock;

static void clock_init(Clock* c, double step, int max_catchup) {
    c->step = step;
    c->scale = 1.0;
    c->acc = 0.0;
    c->alpha = 0.0;
    c->max_catchup = max_catchup;
    c->total_steps = 0;
    c->dropped = 0;
}

/* Set time scale. Rejects negative or non-finite values. The accumulator is
 * in game-time units, so no conversion is needed here and the timeline does
 * not drift across switches. */
static int clock_set_scale(Clock* c, double scale) {
    if (!(scale >= 0.0) || !isfinite(scale)) return -1;
    c->scale = scale;
    return 0;
}

static int clock_advance(Clock* c, double frame_dt) {
    if (!(frame_dt >= 0.0) || !isfinite(frame_dt)) return -1;
    if (c->step <= 0.0) { c->alpha = 0.0; return 0; }

    double scaled = frame_dt * c->scale;
    if (!isfinite(scaled)) return -1;   /* scale * frame_dt overflowed */
    c->acc += scaled;

    /* Snap values within a hair of a step boundary onto the boundary, so
     * float residue (e.g. 0.015 - 0.01 - 0.005) cannot lose or invent a
     * step versus hand computation. tol is ~1e-9 of a step: far below any
     * real frame quantum, far above representation error. */
    const double tol = c->step * 1e-9;

    int steps = 0;
    while (c->acc + tol >= c->step) {
        if (steps >= c->max_catchup) {
            /* discard the backlog instead of spiralling; count it exactly */
            long long d = (long long)((c->acc + tol) / c->step);
            double rem = c->acc - (double)d * c->step;
            if (rem < 0.0) rem = 0.0;
            c->dropped += d;
            c->acc = rem;
            break;
        }
        c->acc -= c->step;
        if (c->acc < 0.0) c->acc = 0.0;
        steps++;
        c->total_steps++;
    }
    c->alpha = c->acc / c->step;
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

    /* --- time scaling --- */

    /* slow motion: 0.5x halves game-time inflow, logic dt unchanged */
    Clock slow;
    clock_init(&slow, 0.01, 8);
    clock_set_scale(&slow, 0.5);
    check("slow_no_step", clock_advance(&slow, 0.01) == 0 && fabs(slow.alpha - 0.5) < 1e-9);
    check("slow_step", clock_advance(&slow, 0.01) == 1 && slow.total_steps == 1);

    /* fast forward: 2x doubles inflow */
    Clock fast;
    clock_init(&fast, 0.01, 8);
    clock_set_scale(&fast, 2.0);
    check("fast_two_steps", clock_advance(&fast, 0.01) == 2 && fast.total_steps == 2);

    /* pause: scale 0 freezes the timeline but keeps the leftover */
    Clock pause;
    clock_init(&pause, 0.01, 8);
    clock_advance(&pause, 0.015);              /* 1 step, 0.005 left */
    clock_set_scale(&pause, 0.0);
    check("paused_no_step", clock_advance(&pause, 1.0) == 0 && fabs(pause.alpha - 0.5) < 1e-9);
    clock_set_scale(&pause, 1.0);
    check("resume_keeps_leftover", clock_advance(&pause, 0.005) == 1 && pause.total_steps == 2);

    /* catch-up cap is computed on the scaled backlog: 10x over 0.1s of real
     * time is 1.0s of game time = 100 steps; cap 4 runs 4 and drops 96 */
    Clock cap;
    clock_init(&cap, 0.01, 4);
    clock_set_scale(&cap, 10.0);
    check("scaled_catchup_capped", clock_advance(&cap, 0.1) == 4);
    check("scaled_dropped_counted", cap.dropped == 96);
    check("scaled_alpha_bounded", cap.alpha >= 0.0 && cap.alpha < 1.0);

    /* extreme scale: bounded work, exact drop count, no spiral */
    Clock ext;
    clock_init(&ext, 0.01, 8);
    clock_set_scale(&ext, 1e6);
    check("extreme_capped", clock_advance(&ext, 0.1) == 8);
    check("extreme_dropped", ext.dropped == 10000000 - 8);
    check("extreme_recovers", clock_set_scale(&ext, 1.0) == 0
          && clock_advance(&ext, 0.01) == 1);

    /* invalid scales rejected, timeline untouched */
    check("scale_negative_rejected", clock_set_scale(&ext, -1.0) == -1);
    check("scale_nan_rejected", clock_set_scale(&ext, NAN) == -1);
    check("scale_inf_rejected", clock_set_scale(&ext, INFINITY) == -1);
    check("scale_kept_after_reject", fabs(ext.scale - 1.0) < 1e-12);

    /* no drift across scale toggles: all dts and scales are powers of two,
     * so the hand-computed total in quarter-step units (0.0025s) is exact */
    Clock drift;
    clock_init(&drift, 0.01, 16);
    static const struct { double dt; double scale; } seq[] = {
        {0.01,   1.0},   /*  4 units */
        {0.01,   0.5},   /*  2 */
        {0.02,   2.0},   /* 16 */
        {0.005,  4.0},   /*  8 */
        {0.01,   0.25},  /*  1 */
        {0.0025, 1.0},   /*  1 */
        {0.01,   2.0},   /*  8 */
        {0.04,   0.5},   /*  8 */
        {0.01,   1.0},   /*  4 */
        {0.0075, 2.0},   /*  6 */
    };
    long long units = 0;               /* total game time in 0.0025s units */
    for (unsigned i = 0; i < sizeof(seq) / sizeof(seq[0]); i++) {
        clock_set_scale(&drift, seq[i].scale);
        clock_advance(&drift, seq[i].dt);
        units += (long long)(seq[i].dt * seq[i].scale / 0.0025 + 0.5);
    }
    /* 58 units total = 14.5 steps: 14 steps, alpha 0.5, zero drops */
    check("drift_units_sane", units == 58);
    check("no_drift_steps", drift.total_steps == units / 4);
    check("no_drift_alpha", fabs(drift.alpha - (units % 4) / 4.0) < 1e-9);
    check("no_drift_no_drops", drift.dropped == 0);

    /* toggling scale mid-frame never loses or invents time: same total game
     * time fed at constant 1x must land on the same step count */
    Clock ref;
    clock_init(&ref, 0.01, 16);
    for (unsigned i = 0; i < sizeof(seq) / sizeof(seq[0]); i++)
        clock_advance(&ref, seq[i].dt * seq[i].scale);
    check("matches_reference", drift.total_steps == ref.total_steps
          && fabs(drift.alpha - ref.alpha) < 1e-9);

    printf("all_ok=%d\n", failures == 0);
    return failures == 0 ? 0 : 1;
}

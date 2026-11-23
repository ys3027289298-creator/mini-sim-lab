#ifndef FIXEDSTEP_H
#define FIXEDSTEP_H

/*
 * fixedstep: fixed-timestep accumulator with interpolation alpha and
 * catch-up cap.
 *
 * Determinism contract (default build):
 *   Frame dt values are quantised to 1ns fixed point on entry, so the
 *   sequence of emitted steps depends only on the input values, not on
 *   how callers group or sum them, nor on thread count. Replays are
 *   bit-identical.
 *
 * Build with -DFIXEDSTEP_LEGACY to restore the pre-fix double
 * accumulator/fmod implementation. The legacy build exists only so the
 * replay-drift reproduction can show the old behaviour.
 */

#define CLOCK_FP_SCALE 1000000000LL /* fixed-point units per second */

typedef struct {
    double step;        /* fixed dt (same field/meaning as the old API) */
    double acc;         /* residual seconds, mirror of acc_fp */
    double alpha;       /* interpolation factor for rendering, [0,1) */
    int max_catchup;    /* max steps per frame */
    long long total_steps;
    int dropped;        /* steps discarded due to catch-up cap */
    /* new internal fields appended at the end; old fields keep layout */
    long long step_fp;  /* step in fixed-point units */
    long long acc_fp;   /* accumulated residual in fixed-point units */
} Clock;

void clock_init(Clock* c, double step, int max_catchup);

/* Returns number of steps emitted this frame, -1 on negative dt. */
int clock_advance(Clock* c, double frame_dt);

/*
 * Integer-nanosecond entry point. Because quantisation happens before
 * any summation, feeding quantise(a)+quantise(b) gives exactly the same
 * step sequence as feeding them in separate calls: frame grouping
 * cannot change the trajectory. The double clock_advance() is retained
 * for source compatibility and simply rounds then delegates here.
 *
 * clock_advance_ns applies the per-frame catch-up cap (matching the
 * original library behaviour); clock_drain_ns instead emits every step
 * the accumulator contains and never drops. A driver that wants the cap
 * counted at original-frame boundaries (so merged frames cannot lose
 * work) should use clock_drain_ns and do its own accounting.
 */
int clock_advance_ns(Clock* c, long long frame_dt_ns);
int clock_drain_ns(Clock* c, long long frame_dt_ns);

/*
 * Re-insert n already-drained steps into the accumulator and subtract
 * them from total_steps. Used by drivers that drain first and enforce
 * their own catch-up cap afterwards. Returns -1 if n exceeds the
 * drained backlog, 0 on success.
 */
int clock_unstep(Clock* c, int n);

#endif /* FIXEDSTEP_H */


/* clocksync: multi-clock synchronization over one integer master timeline.
 *
 * Physics runs on a low-frequency fixed grid, rendering on a high-frequency
 * fixed grid. Instead of two floating-point accumulators that have to be
 * re-aligned (and drift apart on long runs), both grids are derived from the
 * same int64 nanosecond master counter:
 *
 *   grid time of tick k   = origin + k * period
 *   current tick index    = (master - origin) / period      (exact integer)
 *   phase inside a tick   = (master - origin) % period      (exact integer)
 *   interpolation alpha   = phase / period
 *
 * Consequences that the self-test suite checks:
 *   - ratio conversion is integer math, no round-off in the hot path;
 *   - each clock lags the master by less than its own period, so the
 *     deviation between the two clocks is bounded for any run length;
 *   - the rendered (interpolated) time equals master time by construction,
 *     so changing either period re-anchors the grid without a time jump;
 *   - an integer accumulator advanced on its own produces exactly the same
 *     indices and phases as the derived grid (equivalence test).
 */
#include <stdio.h>
#include <math.h>

typedef struct {
    long long t;              /* master timeline, nanoseconds */
    long long phys_origin;    /* physics grid origin (a tick boundary), ns */
    long long render_origin;  /* render grid origin (a frame boundary), ns */
    long long phys_period;    /* physics tick period P, ns (low frequency) */
    long long render_period;  /* render frame period R, ns (high frequency) */
    long long phys_idx;       /* last completed physics tick index */
    long long render_idx;     /* current render frame index */
    long long phys_total;     /* cumulative physics ticks executed */
    long long dropped;        /* physics ticks discarded by the catch-up cap */
    int max_catchup;          /* max physics ticks executed per update */
} MultiClock;

/* origins are grid boundaries at or before t=0; the initial phase is -origin,
   which must be strictly below the corresponding period. */
static int mc_init(MultiClock* mc, long long phys_period, long long render_period,
                   int max_catchup, long long phys_origin, long long render_origin) {
    if (phys_period <= 0 || render_period <= 0 || max_catchup < 0) return -1;
    if (phys_origin > 0 || render_origin > 0) return -1;
    if (-phys_origin >= phys_period || -render_origin >= render_period) return -1;
    mc->t = 0;
    mc->phys_origin = phys_origin;
    mc->render_origin = render_origin;
    mc->phys_period = phys_period;
    mc->render_period = render_period;
    mc->phys_idx = 0;
    mc->render_idx = 0;
    mc->phys_total = 0;
    mc->dropped = 0;
    mc->max_catchup = max_catchup;
    return 0;
}

static long long mc_phys_phase_ns(const MultiClock* mc) {
    return mc->t - (mc->phys_origin + mc->phys_idx * mc->phys_period);
}

static long long mc_render_phase_ns(const MultiClock* mc) {
    return mc->t - (mc->render_origin + mc->render_idx * mc->render_period);
}

static double mc_alpha(const MultiClock* mc) {
    return (double)mc_phys_phase_ns(mc) / (double)mc->phys_period;
}

/* rendered/interpolated simulation time: last boundary + alpha * P, in ns.
   By construction this is always exactly the master time. */
static long long mc_vis_time_ns(const MultiClock* mc) {
    return mc->phys_origin + mc->phys_idx * mc->phys_period + mc_phys_phase_ns(mc);
}

/* Advance the master by frame_dt nanoseconds, execute the physics ticks that
   fall due (catch-up cap matches fixedstep semantics), and return the number
   of physics ticks executed. -1 on negative input, state untouched. */
static int mc_update(MultiClock* mc, long long frame_dt_ns) {
    if (frame_dt_ns < 0) return -1;
    mc->t += frame_dt_ns;

    long long wanted = (mc->t - mc->phys_origin) / mc->phys_period - mc->phys_idx;
    long long steps = wanted;
    if (steps > mc->max_catchup) {
        steps = mc->max_catchup;
        mc->dropped += wanted - steps;
    }
    mc->phys_idx += steps;
    mc->phys_total += steps;
    if (wanted > mc->max_catchup) {
        /* Re-anchor the grid: the executed tick boundary moves to master
           time. Simulated state in between is skipped (teleport), but the
           rendered time coordinate stays exactly on the master timeline. */
        mc->phys_origin = mc->t - mc->phys_idx * mc->phys_period;
    }

    mc->render_idx = (mc->t - mc->render_origin) / mc->render_period;
    return (int)steps;
}

/* Change the physics tick rate. Whole new-period ticks inside the current
   phase are drained immediately so alpha stays in [0,1). The rendered
   instant is unchanged: new boundary + new phase still add up to master t.
   Returns the number of ticks drained, -1 on invalid input. */
static long long mc_set_phys_period(MultiClock* mc, long long period_ns) {
    if (period_ns <= 0) return -1;
    long long boundary = mc->phys_origin + mc->phys_idx * mc->phys_period;
    long long phase = mc->t - boundary;                 /* [0, old P) */
    long long extra = phase / period_ns;
    long long new_boundary = boundary + extra * period_ns;
    mc->phys_idx += extra;
    mc->phys_total += extra;
    mc->phys_period = period_ns;
    mc->phys_origin = new_boundary - mc->phys_idx * period_ns;
    return extra;
}

/* Change the render frame rate. The current frame index is kept and its
   boundary is anchored at master time, so nothing the renderer consumes can
   jump. Returns -1 on invalid input. */
static int mc_set_render_period(MultiClock* mc, long long period_ns) {
    if (period_ns <= 0) return -1;
    mc->render_origin = mc->t - mc->render_idx * period_ns;
    mc->render_period = period_ns;
    return 0;
}

/* Exact ratio helpers: physics ticks fully covered by n render frames. */
static long long mc_phys_ticks_for_frames(const MultiClock* mc, long long frames) {
    if (frames < 0) return -1;
    return frames * mc->render_period / mc->phys_period;
}

static long long mc_frames_for_phys_ticks(const MultiClock* mc, long long ticks) {
    if (ticks < 0) return -1;
    return ticks * mc->phys_period / mc->render_period;
}

/* physics ticks per render frame, as a fraction */
static double mc_ticks_per_frame(const MultiClock* mc) {
    return (double)mc->render_period / (double)mc->phys_period;
}

/* ------------------------------------------------------------------ */
/* self-test                                                           */
/* ------------------------------------------------------------------ */

static int failures = 0;
static void check(const char* name, int ok) {
    printf("%s=%d\n", name, ok ? 1 : 0);
    if (!ok) failures++;
}

static int mc_invariants(const MultiClock* mc) {
    long long pp = mc_phys_phase_ns(mc);
    long long rp = mc_render_phase_ns(mc);
    double a = mc_alpha(mc);
    return pp >= 0 && pp < mc->phys_period &&
           rp >= 0 && rp < mc->render_period &&
           a >= 0.0 && a < 1.0 &&
           mc_vis_time_ns(mc) == mc->t;
}

/* independent reference: its own accumulator, advanced on its own, then
   read out for comparison with the master-derived grid. */
typedef struct {
    long long period;
    long long acc;
    long long idx;
} RefClock;

static void ref_init(RefClock* r, long long origin, long long period) {
    r->period = period;
    r->acc = -origin;   /* elapsed grid time before master t=0 */
    r->idx = 0;
}

static void ref_advance(RefClock* r, long long dt) {
    r->acc += dt;
    while (r->acc >= r->period) {
        r->acc -= r->period;
        r->idx++;
    }
}

/* independent rate change: drain whole ticks out of the phase, same
   observable result as mc_set_phys_period's grid re-anchoring. */
static void ref_set_period(RefClock* r, long long period) {
    r->period = period;
    while (r->acc >= r->period) {
        r->acc -= r->period;
        r->idx++;
    }
}

static unsigned long long rng_state = 0x123456789abcdef0ULL;
static unsigned long long xr_next(void) {
    unsigned long long x = rng_state;
    x ^= x << 13;
    x ^= x >> 7;
    x ^= x << 17;
    rng_state = x;
    return x;
}

/* frame dt around base_ns, varying by +/- pct percent (simulates jitter) */
static long long jitter_dt(long long base_ns, int pct) {
    long long span = 2 * base_ns * pct / 100;
    long long low = base_ns - base_ns * pct / 100;
    return low + (long long)(xr_next() % (unsigned long long)(span + 1));
}

static int equiv_with_refs(long long phys_origin, long long render_origin,
                           int rate_changes) {
    MultiClock mc;
    if (mc_init(&mc, 50000000, 10000000, 8, phys_origin, render_origin) != 0)
        return 0;
    RefClock rp, rr;
    ref_init(&rp, phys_origin, mc.phys_period);
    ref_init(&rr, render_origin, mc.render_period);

    static const long long p_set[] = { 50000000, 40000000, 25000000 };
    static const long long r_set[] = { 10000000, 5000000, 8000000 };
    long long prev_vis = mc_vis_time_ns(&mc);

    for (int frame = 0; frame < 20000; frame++) {
        long long dt = jitter_dt(mc.render_period, 30);
        mc_update(&mc, dt);
        ref_advance(&rp, dt);
        ref_advance(&rr, dt);

        if (mc.phys_idx != rp.idx || mc_phys_phase_ns(&mc) != rp.acc ||
            mc.render_idx != rr.idx || mc_render_phase_ns(&mc) != rr.acc ||
            !mc_invariants(&mc) || mc_vis_time_ns(&mc) < prev_vis)
            return 0;
        prev_vis = mc_vis_time_ns(&mc);

        if (rate_changes && frame % 977 == 976) {
            long long new_p = p_set[xr_next() % 3];
            long long new_r = r_set[xr_next() % 3];
            long long vis_before = mc_vis_time_ns(&mc);
            long long extra = mc_set_phys_period(&mc, new_p);
            if (extra < 0) return 0;
            ref_set_period(&rp, new_p);
            if (mc_set_render_period(&mc, new_r) != 0) return 0;
            /* render re-anchor keeps idx and pins the frame boundary at t */
            rr.period = new_r;
            rr.acc = 0;

            if (mc_vis_time_ns(&mc) != vis_before ||   /* no time jump */
                mc.phys_idx != rp.idx || mc_phys_phase_ns(&mc) != rp.acc ||
                mc.render_idx != rr.idx || mc_render_phase_ns(&mc) != 0 ||
                !mc_invariants(&mc))
                return 0;
        }
    }
    return 1;
}

int main(void) {
    /* ---- basics and exact ratio conversion ---- */
    MultiClock mc;
    check("init_ok", mc_init(&mc, 50000000, 10000000, 8, 0, 0) == 0); /* 20/100 Hz */
    check("init_zero_state", mc.phys_idx == 0 && mc.render_idx == 0 &&
                             mc.phys_total == 0 && mc.dropped == 0 &&
                             mc_alpha(&mc) == 0.0);
    check("ratio_fifth", fabs(mc_ticks_per_frame(&mc) - 0.2) < 1e-15);
    check("ticks_for_5_frames", mc_phys_ticks_for_frames(&mc, 5) == 1);
    check("ticks_for_4_frames_floor", mc_phys_ticks_for_frames(&mc, 4) == 0);
    check("frames_per_tick", mc_frames_for_phys_ticks(&mc, 1) == 5);
    check("ratio_negative_rejected", mc_phys_ticks_for_frames(&mc, -1) == -1);

    /* ---- fixed-step behavior mirrors fixedstep/ ---- */
    check("one_tick", mc_update(&mc, 50000000) == 1 &&
                      mc.phys_total == 1 && mc_alpha(&mc) == 0.0);
    check("half_alpha", mc_update(&mc, 25000000) == 0 &&
                        mc.phys_idx == 1 && fabs(mc_alpha(&mc) - 0.5) < 1e-12);
    check("tick_completes", mc_update(&mc, 25000000) == 1 && mc.phys_idx == 2);
    check("two_ticks", mc_update(&mc, 100000000) == 2 &&
                       fabs(mc_alpha(&mc) - 0.0) < 1e-12);

    /* ---- alpha stays bounded and the unwrapped coordinate is smooth
            across jittery frame times ---- */
    int alpha_ok = 1, monotone_ok = 1, vis_ok = 1;
    double prev_unwrap = (double)mc.phys_idx + mc_alpha(&mc);
    long long prev_vis = mc_vis_time_ns(&mc);
    for (int i = 0; i < 5000; i++) {
        mc_update(&mc, jitter_dt(10000000, 30));
        double a = mc_alpha(&mc);
        double unwrap = (double)mc.phys_idx + a;
        if (!(a >= 0.0 && a < 1.0)) alpha_ok = 0;
        /* frame dt <= 13 ms < P = 50 ms: at most one tick, so the
           unwrapped coordinate can move by less than 2 per frame */
        if (!(unwrap >= prev_unwrap - 1e-12 && unwrap - prev_unwrap < 2.0))
            monotone_ok = 0;
        if (!(mc_vis_time_ns(&mc) == mc.t && mc_vis_time_ns(&mc) >= prev_vis))
            vis_ok = 0;
        prev_unwrap = unwrap;
        prev_vis = mc_vis_time_ns(&mc);
    }
    check("alpha_bounded_jitter", alpha_ok);
    check("unwrapped_smooth", monotone_ok);
    check("vis_on_master", vis_ok);

    /* ---- derived grid == independently advanced integer clocks ---- */
    check("equiv_independent_zero_origin", equiv_with_refs(0, 0, 0));
    check("equiv_independent_offset_origin",
          equiv_with_refs(-3000000, -7000000, 0));
    check("equiv_through_rate_changes", equiv_with_refs(0, 0, 1));

    /* ---- one simulated hour: clock deviation stays bounded ---- */
    MultiClock hr;
    mc_init(&hr, 50000000, 10000000, 8, 0, 0);
    const long long ONE_HOUR_NS = 3600LL * 1000000000LL;
    while (hr.t < ONE_HOUR_NS)
        mc_update(&hr, jitter_dt(10000000, 25));
    long long pp = mc_phys_phase_ns(&hr);
    long long rp = mc_render_phase_ns(&hr);
    long long phys_boundary = hr.phys_origin + hr.phys_idx * hr.phys_period;
    long long render_boundary = hr.render_origin + hr.render_idx * hr.render_period;
    long long grid_gap = phys_boundary - render_boundary;
    if (grid_gap < 0) grid_gap = -grid_gap;
    check("hour_phys_lag_bounded", pp >= 0 && pp < hr.phys_period);
    check("hour_render_lag_bounded", rp >= 0 && rp < hr.render_period);
    check("hour_grid_gap_bounded", grid_gap < hr.phys_period + hr.render_period);
    check("hour_vis_exact", mc_vis_time_ns(&hr) == hr.t);
    printf("hour_master_ns=%lld phys_ticks=%lld render_frames=%lld grid_gap_ns=%lld\n",
           hr.t, hr.phys_idx, hr.render_idx, grid_gap);

    /* ---- control: per-clock float accumulators drift, the integer master
       does not. Same one-hour stream (360k frames of 10 ms, 50 ms grid):
       a float32 accumulator loses a whole physics tick; the integer grid is
       exact. The same stream in double over 1e8 frames (~115 days at
       100 Hz) loses a tick as well — precision only delays the divergence. ---- */
    float f32acc = 0.0f;
    long f32ticks = 0;
    for (long i = 0; i < 360000; i++) {
        f32acc += 0.01f;
        while (f32acc >= 0.05f) {
            f32acc -= 0.05f;
            f32ticks++;
        }
    }
    RefClock exact;
    ref_init(&exact, 0, 50000000);
    for (long i = 0; i < 360000; i++)
        ref_advance(&exact, 10000000);
    double f32_phase_ns = (double)f32acc * 1e9;
    check("float32_loses_tick", f32ticks == 71999);
    check("integer_exact_tick", exact.idx == 72000 && exact.acc == 0);
    check("float32_phase_drifts", f32_phase_ns > 40000000.0);
    printf("float32_phase_error_ns=%.3f integer_phase_ns=%lld\n",
           f32_phase_ns, exact.acc);

    double dacc = 0.0;
    long dticks = 0;
    for (long i = 0; i < 100000000L; i++) {
        dacc += 0.1;
        while (dacc >= 1.0) {
            dacc -= 1.0;
            dticks++;
        }
    }
    check("double_longrun_loses_tick", dticks == 9999999);
    printf("double_1e8_phase_error_ns=%.3f\n", dacc * 1e9);

    /* ---- rate changes must not move the rendered time coordinate ---- */
    MultiClock rc;
    mc_init(&rc, 50000000, 10000000, 8, 0, 0);
    mc_update(&rc, 32000000);                       /* off-boundary, phase 32 ms */
    long long vis_before = mc_vis_time_ns(&rc);
    double a_before = mc_alpha(&rc);
    long long extra = mc_set_phys_period(&rc, 20000000);  /* 20 -> 50 Hz */
    check("rate_change_drains_ticks", extra == 1);
    check("rate_change_no_jump", mc_vis_time_ns(&rc) == vis_before);
    check("rate_change_alpha_bounded", mc_alpha(&rc) >= 0.0 && mc_alpha(&rc) < 1.0);
    check("rate_change_invariants", mc_invariants(&rc));
    check("rate_change_alpha_shifted",
          fabs(mc_alpha(&rc) - (12000000.0 / 20000000.0)) < 1e-12 &&
          fabs(a_before - (32000000.0 / 50000000.0)) < 1e-12);

    /* slower tick rate: no tick fits in the old phase, vis time still fixed */
    vis_before = mc_vis_time_ns(&rc);
    extra = mc_set_phys_period(&rc, 80000000);
    check("rate_change_slower_no_drain", extra == 0);
    check("rate_change_slower_no_jump", mc_vis_time_ns(&rc) == vis_before);
    check("rate_change_slower_invariants", mc_invariants(&rc));

    /* change exactly on a boundary: alpha stays zero */
    MultiClock rb;
    mc_init(&rb, 50000000, 10000000, 8, 0, 0);
    mc_update(&rb, 50000000);
    check("rate_change_at_boundary", mc_set_phys_period(&rb, 25000000) == 0 &&
                                     mc_alpha(&rb) == 0.0 &&
                                     mc_vis_time_ns(&rb) == rb.t);

    /* rapid back-to-back changes: rendered time never leaves master time */
    MultiClock rq;
    mc_init(&rq, 50000000, 10000000, 8, 0, 0);
    int rapid_ok = 1;
    static const long long rapid_periods[] =
        { 50000000, 20000000, 80000000, 33333333, 25000000 };
    for (int i = 0; i < 100; i++) {
        mc_update(&rq, jitter_dt(10000000, 40));
        long long vb = mc_vis_time_ns(&rq);
        if (mc_set_phys_period(&rq, rapid_periods[i % 5]) < 0 ||
            mc_set_render_period(&rq, rapid_periods[(i + 2) % 5]) < 0 ||
            mc_vis_time_ns(&rq) != vb || !mc_invariants(&rq))
            rapid_ok = 0;
    }
    check("rate_change_rapid_no_jump", rapid_ok);

    /* render-only rate change: physics state and vis time untouched */
    MultiClock rr2;
    mc_init(&rr2, 50000000, 10000000, 8, 0, 0);
    mc_update(&rr2, 37000000);
    long long phys_idx_before = rr2.phys_idx;
    vis_before = mc_vis_time_ns(&rr2);
    check("render_rate_change_ok", mc_set_render_period(&rr2, 6250000) == 0);
    check("render_rate_change_phys_untouched",
          rr2.phys_idx == phys_idx_before && mc_vis_time_ns(&rr2) == vis_before);
    check("render_rate_change_invariants", mc_invariants(&rr2));
    mc_update(&rr2, 6250000);
    check("render_rate_change_ticks_on", mc_invariants(&rr2));

    /* ---- catch-up cap, same semantics as fixedstep/ ---- */
    MultiClock cap;
    mc_init(&cap, 10000000, 10000000, 4, 0, 0);
    int steps = mc_update(&cap, 100000000);           /* 10 ticks due */
    check("catchup_capped", steps == 4);
    check("catchup_dropped", cap.dropped == 6);
    check("catchup_invariants", mc_invariants(&cap));
    check("catchup_vis_on_master", mc_vis_time_ns(&cap) == cap.t);
    check("catchup_resumes", mc_update(&cap, 10000000) == 1 && cap.dropped == 6);

    /* ---- input validation ---- */
    MultiClock bad;
    check("negative_dt_rejected", mc_update(&mc, -1) == -1);
    check("zero_period_rejected", mc_set_phys_period(&mc, 0) == -1);
    check("neg_period_rejected", mc_set_render_period(&mc, -5) == -1);
    check("bad_init_period", mc_init(&bad, 0, 10000000, 8, 0, 0) == -1);
    check("bad_init_catchup", mc_init(&bad, 50000000, 10000000, -1, 0, 0) == -1);
    check("bad_init_future_origin", mc_init(&bad, 50000000, 10000000, 8, 1, 0) == -1);
    check("bad_init_origin_past_period",
          mc_init(&bad, 50000000, 10000000, 8, -50000000, 0) == -1);

    printf("all_ok=%d\n", failures == 0);
    return failures == 0 ? 0 : 1;
}

/* lagcomp: server-side hit validation with rewind (client-prediction style).

   The server keeps a ring of tick snapshots. When a hit request arrives it
   rewinds to the tick the client reported, resolves the shot against the
   target pose recorded at that tick, and rejects the request outright when
   the reported tick fell outside the rewind window. With compensation
   disabled the shot is validated against the current tick, which is what the
   server did before rewind existed.

   Ticks are milliseconds since server start, so the rewind window is a plain
   millisecond budget.
*/
#include <stdio.h>
#include <math.h>
#include <string.h>

#define LC_RING 64          /* snapshot ring capacity */

typedef struct {
    int tick;
    double x, y;
    double radius;
} Snap;

typedef struct {
    Snap ring[LC_RING];
    int head;               /* next write slot */
    int count;
    int cur_tick;
    double cur_x, cur_y, cur_radius; /* live target pose */
} World;

typedef struct {
    int enabled;
    int window_ms;          /* rewind budget */
    int max_window_ms;      /* largest window the caller may configure */
} Rewind;

typedef enum {
    LC_OK = 0,
    LC_MISS = 1,
    LC_OUT_OF_WINDOW = 2,
    LC_BAD_TIMESTAMP = 3,
    LC_BAD_WINDOW = 4,
    LC_BAD_ARG = 5
} LCStatus;

typedef struct {
    int applied;            /* compensation actually used for this shot */
    int rewound;            /* resolved at a tick older than the current one */
    int age_ms;             /* how far back the resolved tick was */
    int hit_tick;           /* tick the shot was resolved against */
} LCResult;

static void world_init(World* w) {
    memset(w, 0, sizeof(*w));
}

/* Advance the live pose by dt_ms and record a snapshot at the new tick. */
static void world_step(World* w, double vx, double vy, int dt_ms) {
    w->cur_tick += dt_ms;
    w->cur_x += vx * (double)dt_ms;
    w->cur_y += vy * (double)dt_ms;
}

static void world_record(World* w) {
    Snap* s = &w->ring[w->head];
    s->tick = w->cur_tick;
    s->x = w->cur_x;
    s->y = w->cur_y;
    s->radius = w->cur_radius;
    w->head = (w->head + 1) % LC_RING;
    if (w->count < LC_RING) w->count++;
}

/* Swept point shot: walk the ray from the muzzle out to `travel` and hit if
   the target circle is ever within radius of it. At zero travel this is
   exactly the plain point-in-circle test. */
static int shot_hits(double tx, double ty, double radius,
                     double sx, double sy, double dir_x, double dir_y,
                     double travel) {
    double len2 = dir_x * dir_x + dir_y * dir_y;
    if (len2 <= 0.0) return 0;
    double t = (tx - sx) * dir_x + (ty - sy) * dir_y;
    if (t < 0.0) t = 0.0;
    if (t > travel) t = travel;
    double px = sx + dir_x * t;
    double py = sy + dir_y * t;
    double dx = tx - px;
    double dy = ty - py;
    return dx * dx + dy * dy <= radius * radius;
}

static int rewind_init(Rewind* r, int enabled, int window_ms) {
    if (!r) return LC_BAD_ARG;
    r->enabled = enabled;
    r->window_ms = window_ms;
    r->max_window_ms = 1000;
    return LC_OK;
}

static int rewind_set_window(Rewind* r, int window_ms) {
    if (!r) return LC_BAD_ARG;
    if (window_ms < 0) return LC_BAD_WINDOW;      /* negative: refuse */
    if (window_ms > r->max_window_ms) return LC_BAD_WINDOW;
    r->window_ms = window_ms;
    return LC_OK;
}

/* Find the snapshot at the requested tick. Returns a ring slot, or -1 when
   the tick is not covered by anything we kept. */
static int rewind_slot(const World* w, int client_tick) {
    int best = -1;
    int best_tick = -1;
    for (int i = 0; i < w->count; i++) {
        const Snap* s = &w->ring[i];
        if (s->tick <= client_tick && s->tick > best_tick) {
            best_tick = s->tick;
            best = i;
        }
    }
    return best;
}

static LCStatus resolve_shot(const World* w, const Rewind* r,
                             int client_tick, double sx, double sy,
                             double dir_x, double dir_y, double travel,
                             LCResult* out) {
    if (!w || !r || !out) return LC_BAD_ARG;
    /* A timestamp that runs backwards (before the server started) or jumps
       into the future is a protocol error, not a miss. */
    if (client_tick < 0 || client_tick > w->cur_tick) return LC_BAD_TIMESTAMP;
    if (r->window_ms < 0) return LC_BAD_WINDOW;

    if (!r->enabled) {
        out->applied = 0;
        out->rewound = 0;
        out->age_ms = w->cur_tick - client_tick;
        out->hit_tick = w->cur_tick;
        return shot_hits(w->cur_x, w->cur_y, w->cur_radius,
                         sx, sy, dir_x, dir_y, travel) ? LC_OK : LC_MISS;
    }

    int age = w->cur_tick - client_tick;
    if (age > r->window_ms) return LC_OUT_OF_WINDOW;
    int slot = rewind_slot(w, client_tick);
    if (slot < 0) return LC_OUT_OF_WINDOW; /* older than anything retained */
    const Snap* s = &w->ring[slot];
    out->applied = 1;
    out->rewound = s->tick != w->cur_tick;
    out->age_ms = w->cur_tick - s->tick;
    out->hit_tick = s->tick;
    return shot_hits(s->x, s->y, s->radius, sx, sy, dir_x, dir_y, travel)
               ? LC_OK : LC_MISS;
}

/* ---------- tests ---------- */

static int failures = 0;
static void check(const char* name, int ok) {
    printf("%s=%d\n", name, ok ? 1 : 0);
    if (!ok) failures++;
}

/* Target sits at x = 30, radius 2, and sweeps laterally in y on a triangle
   wave: 0 -> +10 over 10ms, +10 -> -10 over 20ms, -10 -> 0 over 10ms. The
   muzzle is the origin firing along +x with 40mm of bullet travel, so a shot
   hits iff |y| <= 2 at the tick the shot is resolved against. */
#define TARGET_X 30.0
#define TARGET_R 2.0
#define SHOT_TRAVEL 40.0
#define PERIOD 40

static double tri_vy(int t) {
    int p = ((t % PERIOD) + PERIOD) % PERIOD;
    if (p < 10) return 1.0;
    if (p < 30) return -1.0;
    return 1.0;
}

typedef struct {
    World w;
    Rewind r;
} Ctx;

static void ctx_init(Ctx* c, int window_ms, double radius) {
    world_init(&c->w);
    c->w.cur_x = TARGET_X;
    c->w.cur_y = 0.0;
    c->w.cur_radius = radius;
    world_record(&c->w);
    rewind_init(&c->r, 1, window_ms);
}

/* One millisecond of target motion, snapshotted. */
static void tick(Ctx* c) {
    world_step(&c->w, 0.0, tri_vy(c->w.cur_tick), 1);
    world_record(&c->w);
}

static void run_to(Ctx* c, int t) {
    while (c->w.cur_tick < t) tick(c);
}

static LCStatus fire(const Ctx* c, int client_tick, LCResult* res) {
    return resolve_shot(&c->w, &c->r, client_tick, 0.0, 0.0, 1.0, 0.0,
                        SHOT_TRAVEL, res);
}

static int theoretical_hit(const Ctx* c, int client_tick) {
    int slot = rewind_slot(&c->w, client_tick);
    if (slot < 0) return 0;
    const Snap* s = &c->w.ring[slot];
    return shot_hits(s->x, s->y, s->radius, 0.0, 0.0, 1.0, 0.0, SHOT_TRAVEL);
}

static void test_exact_rewind(void) {
    /* tick 100 -> y = 0 (hit), tick 104 -> y = -4 (miss). */
    Ctx c;
    ctx_init(&c, 120, TARGET_R);
    run_to(&c, 104);

    LCResult at_now, old, lagged;
    check("current_tick_hit", fire(&c, 104, &at_now) == LC_MISS && at_now.hit_tick == 104);

    /* zero lag at the tick that is truly a hit */
    Ctx z;
    ctx_init(&z, 120, TARGET_R);
    run_to(&z, 100);
    check("zero_lag_hit", fire(&z, 100, &at_now) == LC_OK &&
                              at_now.hit_tick == 100 && at_now.rewound == 0 &&
                              at_now.applied == 1);

    /* 44ms of lag: rewind to tick 60 (y = 0, hit), while the current pose
       at tick 104 (y = -4) is a miss. */
    check("rewind_hit_after_lag", fire(&c, 60, &lagged) == LC_OK);
    check("rewind_used_old_tick", lagged.hit_tick == 60 && lagged.rewound == 1 &&
                                      lagged.applied == 1);
    check("rewind_age_reported", lagged.age_ms == 44);

    /* the same request validated without compensation must miss */
    Ctx d;
    ctx_init(&d, 120, TARGET_R);
    rewind_init(&d.r, 0, 120);
    run_to(&d, 104);
    check("disabled_misses_lead_shot", fire(&d, 60, &old) == LC_MISS);
    check("disabled_age_is_reported", old.age_ms == 44 && old.hit_tick == 104);
}

static void test_window_exclusion(void) {
    Ctx c;
    ctx_init(&c, 120, TARGET_R);
    run_to(&c, 104);   /* request at tick 60 has age 44 */

    rewind_set_window(&c.r, 43);
    LCResult r;
    check("beyond_window_rejected", fire(&c, 60, &r) == LC_OUT_OF_WINDOW);
    rewind_set_window(&c.r, 44);
    check("window_boundary_inclusive", fire(&c, 60, &r) == LC_OK && r.hit_tick == 60);
    rewind_set_window(&c.r, 45);
    check("window_one_further_still_hit", fire(&c, 60, &r) == LC_OK);

    /* a request inside the window but older than the ring is also refused */
    Ctx big;
    ctx_init(&big, 1000, TARGET_R);
    run_to(&big, 500);
    check("ring_evicted_rejected", fire(&big, 10, &r) == LC_OUT_OF_WINDOW);
    check("ring_covered_resolves", fire(&big, 490, &r) != LC_OUT_OF_WINDOW &&
                                       r.hit_tick == 490);
}

static void test_invalid_input(void) {
    Ctx c;
    ctx_init(&c, 120, TARGET_R);
    run_to(&c, 100);
    LCResult r;
    check("future_timestamp_rejected", fire(&c, 101, &r) == LC_BAD_TIMESTAMP);
    check("negative_timestamp_rejected", fire(&c, -1, &r) == LC_BAD_TIMESTAMP);
    check("negative_window_rejected", rewind_set_window(&c.r, -5) == LC_BAD_WINDOW);
    check("window_unchanged_after_reject", c.r.window_ms == 120);
    check("zero_window_accepted", rewind_set_window(&c.r, 0) == LC_OK);
    check("zero_window_current_ok", fire(&c, 100, &r) == LC_OK);
    check("zero_window_past_rejected", fire(&c, 99, &r) == LC_OUT_OF_WINDOW);
}

static void test_no_compensation_equivalence(void) {
    Ctx on, off;
    ctx_init(&on, 120, TARGET_R);
    ctx_init(&off, 120, TARGET_R);
    rewind_init(&off.r, 0, 120);
    run_to(&on, 200);
    run_to(&off, 200);

    int uses_current = 1, matches_current = 1;
    int hits_on = 0, hits_off = 0;
    for (int ct = 0; ct <= off.w.cur_tick; ct += 7) {
        LCResult ro, rr;
        LCStatus so = fire(&off, ct, &ro);
        LCStatus sr = fire(&on, ct, &rr);
        /* compensation off always scores against the live pose ... */
        if (ro.applied != 0 || ro.hit_tick != off.w.cur_tick) uses_current = 0;
        /* ... and the answer is exactly the plain current-pose hit test */
        int want = shot_hits(off.w.cur_x, off.w.cur_y, off.w.cur_radius,
                             0.0, 0.0, 1.0, 0.0, SHOT_TRAVEL);
        if ((so == LC_OK) != want) matches_current = 0;
        if (so != LC_OK && so != LC_MISS) matches_current = 0; /* never dropped by age */
        if (so == LC_OK) hits_off++;
        if (sr == LC_OK) hits_on++;
    }
    check("disabled_scores_current_tick", uses_current == 1);
    check("disabled_equals_plain_test", matches_current == 1);
    check("disabled_differs_from_rewound", hits_off != hits_on);
    check("disabled_hits_positive", hits_off > 0);
    check("rewound_hits_positive", hits_on > 0);

    /* With compensation off an ancient request is not dropped for age. */
    LCResult stale;
    LCStatus st = fire(&off, 0, &stale);
    check("disabled_no_age_drop", st == LC_OK || st == LC_MISS);
}

/* Simulate a client whose reports arrive with network jitter. At each server
   tick a request is validated against the tick the client reported, one lag
   behind, exactly as an online server would see it (so the reported tick is
   always inside the ring). The window is set from `window_ms` first.

   Alongside each request we also compute the theoretical answer: whether a
   shot resolved at the reported tick would hit the pose recorded there. That
   is the ground truth the compensation is trying to reproduce, and it is
   window-independent, so the caller can reuse `theory_hits`. */
#define LAG_MIN 20
#define LAG_SPAN 41          /* observed lags fall in [LAG_MIN, LAG_MIN+LAG_SPAN-1] */
static int lag_at(int t) { return LAG_MIN + (t * 13) % LAG_SPAN; }

typedef struct {
    int hits;            /* compensated hits */
    int theory_hits;     /* hits if resolved exactly at the reported tick */
    int resolved;        /* requests that did not fall outside the window */
    int shots;
} SimRate;

static SimRate simulate(const Ctx* template, int window_ms,
                        int first, int last) {
    Ctx c = *template;               /* copy the pre-rolled history */
    rewind_set_window(&c.r, window_ms);
    SimRate s = {0, 0, 0, 0};
    for (int t = first; t <= last; t++) {
        while (c.w.cur_tick < t) tick(&c);
        int ct = t - lag_at(t);
        if (ct < 0) continue;
        LCResult r;
        LCStatus st = fire(&c, ct, &r);
        if (st != LC_OUT_OF_WINDOW) s.resolved++;
        if (st == LC_OK) s.hits++;
        if (theoretical_hit(&c, ct)) s.theory_hits++;
        s.shots++;
    }
    return s;
}

static double rate(SimRate s, int theory) {
    if (!s.shots) return 0.0;
    return (double)(theory ? s.theory_hits : s.hits) / (double)s.shots;
}

static void test_hit_rate(void) {
    /* Generous ring so the whole simulated span stays covered. Small target
       radius relative to the lateral sweep keeps the rate away from 0 and 1,
       which makes the comparison meaningful. */
    Ctx base;
    ctx_init(&base, 1000, 1.0);
    run_to(&base, 400);

    const int first = 400;           /* start late enough that even the oldest
                                        reported tick is still in the ring */
    const int last = 520;

    /* Theory is window-independent; read it off a full-window run. */
    SimRate full = simulate(&base, 1000, first, last);
    double th = rate(full, 1);
    printf("theory_rate=%.6f\n", th);
    check("theory_rate_meaningful", th > 0.05 && th < 0.95);

    /* Full window: every request resolves at its reported tick, so the
       observed rate equals theory and the deviation is exactly zero. */
    printf("observed_rate_full_window=%.6f\n", rate(full, 0));
    printf("deviation_full_window=%.6f\n", rate(full, 0) - th);
    check("deviation_zero_on_full_window", fabs(rate(full, 0) - th) < 1e-12);

    /* Window shorter than the minimum lag: every late request is dropped, so
       the observed rate is zero and the deviation is strictly negative. */
    SimRate shortw = simulate(&base, LAG_MIN - 1, first, last);
    printf("observed_rate_short_window=%.6f\n", rate(shortw, 0));
    printf("deviation_short_window=%.6f\n", rate(shortw, 0) - th);
    check("short_window_resolves_nothing",
          shortw.resolved == 0 && shortw.shots > 0);
    check("deviation_negative_on_short_window", rate(shortw, 0) - th < 0.0);

    /* Walk the window from 0 past the maximum lag. Widening the window can
       only turn drops into resolutions, so the curve must be non-decreasing;
       with jitter it climbs in steps rather than jumping once. */
    printf("rewind_window_curve:\n");
    double prev = -1.0;
    int monotone = 1, distinct_steps = 0;
    int max_lag = LAG_MIN + LAG_SPAN - 1;
    for (int w = 0; w <= max_lag + 4; w++) {
        double hr = rate(simulate(&base, w, first, last), 0);
        if (w % 7 == 0 || w == max_lag) {
            printf("  window=%d observed=%.6f\n", w, hr);
        }
        if (prev >= 0.0 && hr < prev - 1e-12) {
            printf("  monotone_violation_at_window=%d prev=%.6f now=%.6f\n",
                   w, prev, hr);
            monotone = 0;
        }
        if (hr > prev + 1e-12) distinct_steps++;
        prev = hr;
    }
    check("hit_rate_monotone", monotone == 1);
    check("curve_rises_in_steps", distinct_steps >= 3);

    /* Once the window covers the maximum lag the curve is flat at theory. */
    check("curve_reaches_theory_at_max_lag",
          fabs(rate(simulate(&base, max_lag, first, last), 0) - th) < 1e-12);
    check("curve_flat_past_max_lag",
          fabs(rate(simulate(&base, max_lag + 4, first, last), 0) - th) < 1e-12);
}

int main(void) {
    test_exact_rewind();
    test_window_exclusion();
    test_invalid_input();
    test_no_compensation_equivalence();
    test_hit_rate();

    printf("all_ok=%d\n", failures == 0);
    return failures == 0 ? 0 : 1;
}

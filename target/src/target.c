/* target: target selection by ballistic hit probability, utility composition
   (distance / threat / time-window payoff), plus a brute-force future-frame
   oracle used to measure agreement. */
#include <stdio.h>
#include <math.h>

#define MAX_TARGETS 8
#define MAX_FRAMES 8

typedef enum { ERR_OK = 0, ERR_PARAM = -1, ERR_NO_TARGET = -2, ERR_CAP = -3 } Err;

typedef struct {
    double x, y;
    double vx, vy;
} Body;

typedef struct {
    /* utility weights: each >= 0, sum > 0 */
    double w_dist;
    double w_threat;
    double w_window;
    /* engagement / ballistic params */
    double projectile_speed;   /* world units per second */
    double target_radius;      /* hit radius */
    double aim_spread;         /* angular dispersion, radians (>= 0) */
    int    window_frames;      /* evaluation window, 1..MAX_FRAMES */
    double frame_dt;           /* seconds per frame */
    double decay;              /* payoff decay per frame, [0, 1] */
} DecisionParams;

typedef struct {
    int    index;
    double dist;
    double hit_prob;
    double threat;
    double window_payoff;
    double utility;
    int    feasible;           /* intercept exists inside the time window */
} TargetEval;

typedef struct {
    int    best;               /* chosen target index, -1 if none */
    int    n_evaluated;
    TargetEval eval[MAX_TARGETS];
} Decision;

static int params_valid(const DecisionParams* p) {
    if (!p) return 0;
    if (p->w_dist < 0.0 || p->w_threat < 0.0 || p->w_window < 0.0) return 0;
    if (p->w_dist + p->w_threat + p->w_window <= 0.0) return 0;
    if (!(p->projectile_speed > 0.0)) return 0;
    if (!(p->target_radius > 0.0)) return 0;
    if (!(p->aim_spread >= 0.0) || p->aim_spread > 1.0) return 0;
    if (p->window_frames <= 0 || p->window_frames > MAX_FRAMES) return 0;
    if (!(p->frame_dt > 0.0)) return 0;
    if (!(p->decay >= 0.0) || p->decay > 1.0) return 0;
    return 1;
}

/* Earliest positive intercept time for a projectile of the given speed, or -1
   when the target cannot be led to. Solves |r + v*t| = speed*t. */
static double time_to_hit(const Body* shooter, const Body* tgt, double speed) {
    double rx = tgt->x - shooter->x;
    double ry = tgt->y - shooter->y;
    double vx = tgt->vx - shooter->vx;
    double vy = tgt->vy - shooter->vy;
    double a = vx * vx + vy * vy - speed * speed;
    double b = 2.0 * (rx * vx + ry * vy);
    double c = rx * rx + ry * ry;
    double best = -1.0;
    if (fabs(a) < 1e-12) {
        if (fabs(b) > 1e-12) {
            double t = -c / b;
            if (t > 0.0) best = t;
        }
        return best;
    }
    double disc = b * b - 4.0 * a * c;
    if (disc < 0.0) return -1.0;
    double sq = sqrt(disc);
    double t1 = (-b - sq) / (2.0 * a);
    double t2 = (-b + sq) / (2.0 * a);
    if (t1 > 0.0) best = t1;
    if (t2 > 0.0 && (best < 0.0 || t2 < best)) best = t2;
    return best;
}

/* Hit probability: circular-error model. Perpendicular miss from the lead
   solution and angular dispersion both widen the miss radius; probability is
   radius^2 / (radius^2 + miss^2). */
static double hit_probability(const Body* shooter, const Body* tgt,
                              double speed, double radius, double aim_spread) {
    double t = time_to_hit(shooter, tgt, speed);
    if (t < 0.0) return 0.0;
    double rx = tgt->x - shooter->x;
    double ry = tgt->y - shooter->y;
    double vx = tgt->vx - shooter->vx;
    double vy = tgt->vy - shooter->vy;
    double los = sqrt(rx * rx + ry * ry);
    double cross = fabs(rx * vy - ry * vx);
    double perp = los > 1e-12 ? cross / los * t : 0.0;
    double miss = perp + los * aim_spread;
    return (radius * radius) / (radius * radius + miss * miss);
}

/* Threat: closing speed toward the shooter, squashed into [0, 1). */
static double threat_of(const Body* shooter, const Body* tgt) {
    double rx = shooter->x - tgt->x;
    double ry = shooter->y - tgt->y;
    double d = sqrt(rx * rx + ry * ry);
    if (d < 1e-9) return 1.0;
    double closing = (tgt->vx * rx + tgt->vy * ry) / d;
    if (closing < 0.0) closing = 0.0;
    return closing / (closing + 1.0);
}

/* Expected damage over the window: average lead-model hit probability of the
   target's future positions, weighted by a per-frame decay. */
static double window_payoff(const Body* shooter, const Body* tgt,
                            const DecisionParams* p) {
    double payoff = 0.0;
    double w = 1.0;
    for (int f = 1; f <= p->window_frames; f++) {
        Body sim = *tgt;
        double dt = p->frame_dt * (double)f;
        sim.x += tgt->vx * dt;
        sim.y += tgt->vy * dt;
        payoff += hit_probability(shooter, &sim, p->projectile_speed,
                                  p->target_radius, p->aim_spread) * w;
        w *= p->decay;
    }
    return payoff / (double)p->window_frames;
}

static void evaluate(const Body* shooter, const Body* tgt, int index,
                     const DecisionParams* p, TargetEval* e) {
    double dx = tgt->x - shooter->x;
    double dy = tgt->y - shooter->y;
    e->index = index;
    e->dist = sqrt(dx * dx + dy * dy);

    double t = time_to_hit(shooter, tgt, p->projectile_speed);
    double horizon = p->frame_dt * (double)p->window_frames;
    e->feasible = (t > 0.0 && t <= horizon);

    e->hit_prob = hit_probability(shooter, tgt, p->projectile_speed,
                                  p->target_radius, p->aim_spread);
    e->threat = threat_of(shooter, tgt);
    e->window_payoff = window_payoff(shooter, tgt, p);

    double prox = 1.0 / (1.0 + e->dist);
    e->utility = p->w_dist * prox
               + p->w_threat * e->threat
               + p->w_window * e->window_payoff;
}

/* The previous behaviour: shoot whatever is nearest. */
static int legacy_nearest(const Body* shooter, const Body* targets, int n) {
    int best = -1;
    double bd = 1e300;
    for (int i = 0; i < n; i++) {
        double dx = targets[i].x - shooter->x;
        double dy = targets[i].y - shooter->y;
        double d = dx * dx + dy * dy;
        if (d < bd) { bd = d; best = i; }
    }
    return best;
}

static Err decide(const Body* shooter, const Body* targets, int n,
                  const DecisionParams* p, int enabled, Decision* out) {
    if (!shooter || !targets || !out) return ERR_PARAM;
    if (n < 0 || n > MAX_TARGETS) return ERR_CAP;

    out->n_evaluated = 0;
    if (!enabled) {
        /* legacy path: never validates the decision params */
        out->best = legacy_nearest(shooter, targets, n);
        return ERR_OK;
    }

    if (!params_valid(p)) return ERR_PARAM;
    if (n == 0) return ERR_NO_TARGET;

    int best = -1;
    double best_u = -1.0;
    int any_feasible = 0;
    for (int i = 0; i < n; i++) {
        evaluate(shooter, &targets[i], i, p, &out->eval[i]);
        if (out->eval[i].feasible) {
            any_feasible = 1;
            if (best < 0 || out->eval[i].utility > best_u) {
                best = i;
                best_u = out->eval[i].utility;
            }
        }
    }
    out->n_evaluated = n;
    if (!any_feasible) return ERR_NO_TARGET;
    out->best = best;
    return ERR_OK;
}

/* Brute force over the next `horizon` frames: for each target take its best
   utility across those frames, then pick the target with the highest value. */
static int oracle_pick(const Body* shooter, const Body* targets, int n,
                       const DecisionParams* p, int horizon) {
    int best = -1;
    double best_u = -1.0;
    for (int i = 0; i < n; i++) {
        double peak = -1.0;
        for (int k = 0; k <= horizon; k++) {
            Body sim = targets[i];
            double dt = p->frame_dt * (double)k;
            sim.x += sim.vx * dt;
            sim.y += sim.vy * dt;
            TargetEval e;
            evaluate(shooter, &sim, i, p, &e);
            if (e.feasible && e.utility > peak) peak = e.utility;
        }
        if (peak > best_u) { best_u = peak; best = i; }
    }
    return best;
}

static int failures = 0;
static void check(const char* name, int ok) {
    printf("%s=%d\n", name, ok ? 1 : 0);
    if (!ok) failures++;
}

static DecisionParams default_params(void) {
    DecisionParams p;
    p.w_dist = 1.0;
    p.w_threat = 1.0;
    p.w_window = 1.0;
    p.projectile_speed = 100.0;
    p.target_radius = 1.0;
    p.aim_spread = 0.02;
    p.window_frames = 3;
    p.frame_dt = 0.05;
    p.decay = 0.8;
    return p;
}

int main(void) {
    Body shooter = { 0.0, 0.0, 0.0, 0.0 };
    /* t0: far, idle. t1: charging at the shooter (high threat). t2: nearest
       but sliding laterally (hardest to hit) -- the "shoot whoever is in
       front" legacy pick. */
    Body targets[3] = {
        { 10.0, 0.0, 0.0, 0.0 },
        { 6.0, 0.5, -2.0, 0.0 },
        { 4.0, 2.0, 0.0, 3.0 },
    };
    DecisionParams p = default_params();
    Decision d;

    check("decide_ok", decide(&shooter, targets, 3, &p, 1, &d) == ERR_OK);
    check("has_best", d.best >= 0 && d.best < 3);
    check("evaluated_all", d.n_evaluated == 3);
    check("all_feasible", d.eval[0].feasible && d.eval[1].feasible &&
                          d.eval[2].feasible);

    /* Hit probability sanity. */
    Body static_far = { 40.0, 0.0, 0.0, 0.0 };
    Body static_near = { 4.0, 0.0, 0.0, 0.0 };
    double hp_near = hit_probability(&shooter, &static_near, 100.0, 1.0, 0.02);
    double hp_far = hit_probability(&shooter, &static_far, 100.0, 1.0, 0.02);
    check("hp_in_range", hp_near >= 0.0 && hp_near <= 1.0);
    check("hp_decreases_with_dist", hp_near > hp_far);
    check("hp_static_hit", hp_near > 0.95);

    /* On-axis stationary target has zero perpendicular miss. */
    Body axis = { 20.0, 0.0, 0.0, 0.0 };
    check("hp_axis_no_perp",
          hit_probability(&shooter, &axis, 100.0, 1.0, 0.0) > 0.999);

    /* Threat ordering: charger above idle, receding target is zero. */
    Body receding = { 5.0, 0.0, 2.0, 0.0 };
    check("threat_charger", threat_of(&shooter, &targets[1]) > 0.0);
    check("threat_idle_zero", threat_of(&shooter, &targets[0]) == 0.0);
    check("threat_receding_zero", threat_of(&shooter, &receding) == 0.0);

    /* Agreement with the brute-force future-frame oracle. */
    int agree = 0, trials = 0;
    const double ws[] = { 0.0, 1.0, 2.0, 3.0, 5.0 };
    for (int a = 0; a < 5; a++)
        for (int b = 0; b < 5; b++)
            for (int c = 0; c < 5; c++) {
                if (a == 0 && b == 0 && c == 0) continue;
                DecisionParams q = default_params();
                q.w_dist = ws[a];
                q.w_threat = ws[b];
                q.w_window = ws[c];
                Decision dd;
                if (decide(&shooter, targets, 3, &q, 1, &dd) != ERR_OK) continue;
                int oracle = oracle_pick(&shooter, targets, 3, &q, 4);
                trials++;
                if (dd.best == oracle) agree++;
            }
    printf("agree_rate=%.4f\n", trials ? (double)agree / (double)trials : 0.0);
    check("agreement_reported", trials > 0);
    check("agreement_high", agree * 10 >= trials * 9);

    /* Monotonicity: with the other weights fixed, raising w_dist must not move
       the pick to a farther target, and raising w_threat must not move it to a
       less threatening one (linear scalarisation property). */
    int mono_dist = 1, prev_seen = 0;
    double prev_prox = 0.0;
    for (int w = 0; w <= 20; w++) {
        DecisionParams q = default_params();
        q.w_dist = (double)w;
        Decision dd;
        if (decide(&shooter, targets, 3, &q, 1, &dd) != ERR_OK) { mono_dist = 0; break; }
        double prox = 1.0 / (1.0 + dd.eval[dd.best].dist);
        if (prev_seen && prox < prev_prox - 1e-9) mono_dist = 0;
        prev_prox = prox;
        prev_seen = 1;
    }
    check("monotonic_distance", mono_dist);

    int mono_threat = 1;
    prev_seen = 0;
    double prev_th = 0.0;
    for (int w = 0; w <= 20; w++) {
        DecisionParams q = default_params();
        q.w_threat = (double)w;
        Decision dd;
        if (decide(&shooter, targets, 3, &q, 1, &dd) != ERR_OK) { mono_threat = 0; break; }
        double th = dd.eval[dd.best].threat;
        if (prev_seen && th < prev_th - 1e-9) mono_threat = 0;
        prev_th = th;
        prev_seen = 1;
    }
    check("monotonic_threat", mono_threat);

    int mono_window = 1;
    prev_seen = 0;
    double prev_win = 0.0;
    for (int w = 0; w <= 20; w++) {
        DecisionParams q = default_params();
        q.w_window = (double)w;
        Decision dd;
        if (decide(&shooter, targets, 3, &q, 1, &dd) != ERR_OK) { mono_window = 0; break; }
        double win = dd.eval[dd.best].window_payoff;
        if (prev_seen && win < prev_win - 1e-9) mono_window = 0;
        prev_win = win;
        prev_seen = 1;
    }
    check("monotonic_window", mono_window);

    /* Enabled decision actually differs from the legacy nearest pick. */
    Decision on;
    decide(&shooter, targets, 3, &p, 1, &on);
    printf("chosen=%d legacy=%d\n", on.best, legacy_nearest(&shooter, targets, 3));
    check("enabled_differs_from_legacy",
          on.best != legacy_nearest(&shooter, targets, 3));

    /* Param validation errors. */
    DecisionParams bad = default_params();
    bad.w_dist = -1.0;
    check("err_neg_weight", decide(&shooter, targets, 3, &bad, 1, &d) == ERR_PARAM);
    bad = default_params();
    bad.w_dist = bad.w_threat = bad.w_window = 0.0;
    check("err_zero_weights", decide(&shooter, targets, 3, &bad, 1, &d) == ERR_PARAM);
    bad = default_params();
    bad.projectile_speed = 0.0;
    check("err_zero_speed", decide(&shooter, targets, 3, &bad, 1, &d) == ERR_PARAM);
    bad = default_params();
    bad.target_radius = 0.0;
    check("err_zero_radius", decide(&shooter, targets, 3, &bad, 1, &d) == ERR_PARAM);
    bad = default_params();
    bad.aim_spread = -0.1;
    check("err_neg_spread", decide(&shooter, targets, 3, &bad, 1, &d) == ERR_PARAM);
    bad = default_params();
    bad.window_frames = MAX_FRAMES + 1;
    check("err_window_range", decide(&shooter, targets, 3, &bad, 1, &d) == ERR_PARAM);
    bad = default_params();
    bad.frame_dt = 0.0;
    check("err_zero_dt", decide(&shooter, targets, 3, &bad, 1, &d) == ERR_PARAM);
    bad = default_params();
    bad.decay = 1.5;
    check("err_decay_range", decide(&shooter, targets, 3, &bad, 1, &d) == ERR_PARAM);

    /* No feasible target: nothing is reachable inside the time window. */
    Body unreachable[2] = {
        { 1000.0, 0.0, 0.0, 0.0 },
        { 0.0, 900.0, 0.0, 0.0 },
    };
    check("err_no_feasible", decide(&shooter, unreachable, 2, &p, 1, &d) == ERR_NO_TARGET);
    check("err_empty", decide(&shooter, targets, 0, &p, 1, &d) == ERR_NO_TARGET);
    check("err_null", decide(NULL, targets, 3, &p, 1, &d) == ERR_PARAM);
    check("err_cap", decide(&shooter, targets, MAX_TARGETS + 1, &p, 1, &d) == ERR_CAP);

    /* Disabled: identical to legacy nearest, and invalid params are ignored
       because disabled callers never configured them. */
    DecisionParams junk = default_params();
    junk.w_dist = -5.0;
    junk.window_frames = 999;
    Decision off;
    Err e = decide(&shooter, targets, 3, &junk, 0, &off);
    check("disabled_ok", e == ERR_OK);
    check("legacy_same", off.best == legacy_nearest(&shooter, targets, 3));
    check("disabled_skips_validation", decide(NULL, targets, 3, &junk, 0, &off) == ERR_PARAM);
    Decision off_empty;
    check("disabled_empty", decide(&shooter, targets, 0, &junk, 0, &off_empty) == ERR_OK &&
                            off_empty.best == -1);

    printf("all_ok=%d\n", failures == 0);
    return failures == 0 ? 0 : 1;
}

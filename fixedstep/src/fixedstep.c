
/* fixedstep: fixed timestep accumulator with interpolation alpha and catch-up cap.

   Pause contract:
   - While paused, clock_advance() freezes acc: frames delivered during the
     pause add nothing to acc, total_steps or dropped, and the paused wall
     time is only recorded in paused_time (diagnostics).  Resuming therefore
     cannot replay, drop, or double-count the paused interval.
   - The frame that contains the pause/resume event is split by the caller:
     pass only its live prefix to clock_pause_at() (which advances it, then
     freezes) and only its live tail to clock_resume_at() (which unfreezes,
     then advances it).  Time that is live is advanced once, time that is
     paused is never advanced.
   - Repeated pause or resume calls are idempotent in accounting: a second
     pause discards its dt, a resume on a running clock just advances live dt.
*/
#include <stdio.h>
#include <math.h>
#include <string.h>

typedef struct {
    double step;        /* fixed dt */
    double acc;
    double alpha;       /* interpolation factor for rendering */
    int max_catchup;    /* max steps per frame */
    long long total_steps;
    int dropped;        /* steps discarded due to catch-up cap */
    int paused;
    double paused_time; /* wall time observed while paused (diagnostics) */
} Clock;

static void clock_init(Clock* c, double step, int max_catchup) {
    c->step = step;
    c->acc = 0.0;
    c->alpha = 0.0;
    c->max_catchup = max_catchup;
    c->total_steps = 0;
    c->dropped = 0;
    c->paused = 0;
    c->paused_time = 0.0;
}

static int clock_advance(Clock* c, double frame_dt) {
    if (frame_dt < 0.0) return -1;
    if (c->paused) {
        /* Frozen: a paused frame must not touch the accumulator or the
           catch-up cap, otherwise resuming would replay paused time. */
        c->paused_time += frame_dt;
        return 0;
    }
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

/* Freeze / unfreeze without consuming any frame time.  Use these when the
   pause event lands exactly on a frame boundary; otherwise use the _at
   variants so the live part of the boundary frame is not lost. */
static void clock_pause(Clock* c) {
    c->paused = 1;
}

static void clock_resume(Clock* c) {
    c->paused = 0;
}

/* Advance the live part of the frame in which the pause lands, then freeze.
   Returns the steps taken by that live prefix, or -1 for a negative dt. */
static int clock_pause_at(Clock* c, double live_dt) {
    if (live_dt < 0.0) return -1;
    if (c->paused) return 0;        /* already frozen; this dt is not live */
    int steps = clock_advance(c, live_dt);
    if (steps < 0) return -1;
    clock_pause(c);
    return steps;
}

/* Unfreeze, then advance the live part of the frame in which the resume
   lands.  Never adds the paused interval back into acc. */
static int clock_resume_at(Clock* c, double live_dt) {
    if (live_dt < 0.0) return -1;
    clock_resume(c);
    return clock_advance(c, live_dt);
}

static int failures = 0;
static void check(const char* name, int ok) {
    printf("%s=%d\n", name, ok ? 1 : 0);
    if (!ok) failures++;
}

static int near(double a, double b) {
    return fabs(a - b) <= 1e-15;
}

typedef enum { OP_ADV, OP_PAUSE, OP_RESUME, OP_PAUSE_NOW, OP_RESUME_NOW } Op;

/* One row of the hand-computed table: the operation to feed, the steps the
   operation must return, and the clock state expected right after it. */
typedef struct {
    Op op;
    double dt;
    int steps;
    long long total;
    double acc;
    double alpha;
    int dropped;
} Row;

static int run_case(const char* name, double step, int cap, const Row* rows,
                    int n, double expect_paused_time, int verbose) {
    Clock c;
    clock_init(&c, step, cap);
    int started = failures;
    for (int i = 0; i < n; i++) {
        const Row* r = &rows[i];
        int steps;
        switch (r->op) {
        case OP_ADV:        steps = clock_advance(&c, r->dt); break;
        case OP_PAUSE:      steps = clock_pause_at(&c, r->dt); break;
        case OP_RESUME:     steps = clock_resume_at(&c, r->dt); break;
        case OP_PAUSE_NOW:  clock_pause(&c); steps = 0; break;
        default:            clock_resume(&c); steps = 0; break;
        }
        int ok = steps == r->steps
              && c.total_steps == r->total
              && near(c.acc, r->acc)
              && near(c.alpha, r->alpha)
              && c.dropped == r->dropped;
        if (verbose || !ok) {
            printf("%s[%02d] op=%d dt=%.6f got={steps=%d total=%lld acc=%.6f alpha=%.6f dropped=%d}"
                   " want={steps=%d total=%lld acc=%.6f alpha=%.6f dropped=%d} %s\n",
                   name, i, (int)r->op, r->dt,
                   steps, c.total_steps, c.acc, c.alpha, c.dropped,
                   r->steps, r->total, r->acc, r->alpha, r->dropped,
                   ok ? "ok" : "MISMATCH");
        }
        char label[64];
        snprintf(label, sizeof label, "%s_%02d", name, i);
        check(label, ok);
    }
    int frozen = near(c.paused_time, expect_paused_time);
    if (verbose || !frozen)
        printf("%s paused_time=%.6f want=%.6f %s\n",
               name, c.paused_time, expect_paused_time, frozen ? "ok" : "MISMATCH");
    char label[64];
    snprintf(label, sizeof label, "%s_paused_time", name);
    check(label, frozen);
    return failures - started;
}

/* Replay only live rows: paused frames are dropped and the live boundary
   parts are fed exactly as in real time.  Sim state must match real time
   bit for bit. */
static int run_replay(const Row* rows, int n, double step, int cap, Clock* out) {
    clock_init(out, step, cap);
    int paused = 0;
    for (int i = 0; i < n; i++) {
        switch (rows[i].op) {
        case OP_ADV:
            if (paused) continue;               /* wall time of the pause */
            clock_advance(out, rows[i].dt);
            break;
        case OP_PAUSE:
            clock_pause_at(out, rows[i].dt);
            paused = 1;
            break;
        case OP_PAUSE_NOW:
            clock_pause(out);
            paused = 1;
            break;
        case OP_RESUME_NOW:
            clock_resume(out);
            paused = 0;
            break;
        default:
            clock_resume_at(out, rows[i].dt);
            paused = 0;
            break;
        }
    }
    return 0;
}

/* Serialize a run's per-frame results byte for byte, optionally inserting
   back-to-back pause/resume pairs between frames.  With no frame time owned
   by the pause, the two transcripts must be identical down to the byte. */
static size_t transcript(char* buf, size_t cap, int with_noop_pause) {
    Clock c;
    clock_init(&c, 0.01, 4);
    size_t n = 0;
    for (int i = 0; i < 200; i++) {
        double dt = 0.003 * (i % 9) + 0.00017 * (i % 5);
        int steps = clock_advance(&c, dt);
        if (with_noop_pause) {
            clock_pause(&c);
            clock_resume(&c);
        }
        n += (size_t)snprintf(buf + n, cap - n, "%d %lld %.17g %.17g %d\n",
                              steps, c.total_steps, c.acc, c.alpha, c.dropped);
    }
    return n;
}

int main(int argc, char** argv) {
    int verbose = argc > 1 && argv[1][0] == '-' && argv[1][1] == '-';
    (void)verbose;

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

    /* Pause/resume must leave no trace when there is no frame time to own:
       state after P/R is identical to never having paused, bit for bit. */
    static char a[65536], b[65536];
    size_t na = transcript(a, sizeof a, 0);
    size_t nb = transcript(b, sizeof b, 1);
    check("noop_pause_byte_identical", na == nb && memcmp(a, b, na) == 0);

    /* Same guard for the wide, non-binary-fraction timings a real loop sees. */
    Clock np, pr;
    clock_init(&np, 0.01, 4);
    clock_init(&pr, 0.01, 4);
    for (int i = 0; i < 6; i++) {
        clock_advance(&np, 0.003 * (i + 1));
        clock_advance(&pr, 0.003 * (i + 1));
        clock_pause(&pr);
        clock_resume(&pr);
    }
    check("noop_pause_bitwise", np.acc == pr.acc && np.alpha == pr.alpha
          && np.total_steps == pr.total_steps && np.dropped == pr.dropped);
    check("noop_pause_no_paused_time", pr.paused_time == 0.0);

    /* Values below are exact binary fractions (step = 1/8 s), so the table
       can be checked by hand: 0.0625 = 1/16, 0.125 = 1/8, 0.25 = 1/4. */
    static const Row single[] = {
        /* ADV   dt       steps total  acc     alpha dropped */
        { OP_ADV,     0.25,   2,    2, 0.0,     0.0, 0 },
        { OP_ADV,     0.0625, 0,    2, 0.0625,  0.5, 0 },
        { OP_PAUSE,   0.0625, 1,    3, 0.0,     0.0, 0 }, /* prefix makes 1 step */
        { OP_ADV,     0.5,    0,    3, 0.0,     0.0, 0 }, /* frozen */
        { OP_ADV,     0.125,  0,    3, 0.0,     0.0, 0 }, /* frozen */
        { OP_RESUME,  0.0625, 0,    3, 0.0625,  0.5, 0 }, /* live tail only */
        { OP_ADV,     0.25,   2,    5, 0.0625,  0.5, 0 },
    };
    run_case("single", 0.125, 8, single, (int)(sizeof single / sizeof single[0]),
             0.625, verbose);

    static const Row consecutive[] = {
        { OP_ADV,     0.125,  1,    1, 0.0,     0.0, 0 },
        { OP_PAUSE,   0.0625, 0,    1, 0.0625,  0.5, 0 },
        { OP_PAUSE,   0.0,    0,    1, 0.0625,  0.5, 0 }, /* already paused */
        { OP_ADV,     0.375,  0,    1, 0.0625,  0.5, 0 },
        { OP_ADV,     0.125,  0,    1, 0.0625,  0.5, 0 },
        { OP_PAUSE,   0.0,    0,    1, 0.0625,  0.5, 0 },
        { OP_RESUME,  0.0625, 1,    2, 0.0,     0.0, 0 },
        { OP_RESUME,  0.0,    0,    2, 0.0,     0.0, 0 }, /* already running */
        { OP_ADV,     0.125,  1,    3, 0.0,     0.0, 0 },
        { OP_PAUSE,   0.125,  1,    4, 0.0,     0.0, 0 },
        { OP_ADV,     1.0,    0,    4, 0.0,     0.0, 0 },
        { OP_RESUME,  0.0,    0,    4, 0.0,     0.0, 0 },
        { OP_ADV,     0.25,   2,    6, 0.0,     0.0, 0 },
    };
    run_case("consecutive", 0.125, 8, consecutive,
             (int)(sizeof consecutive / sizeof consecutive[0]), 1.5, verbose);

    static const Row boundary[] = {
        { OP_ADV,     0.25,   2,    2, 0.0,     0.0, 0 },
        { OP_PAUSE,   0.0625, 0,    2, 0.0625,  0.5, 0 }, /* frame split here */
        { OP_ADV,     0.1875, 0,    2, 0.0625,  0.5, 0 }, /* paused tail */
        { OP_ADV,     0.0625, 0,    2, 0.0625,  0.5, 0 }, /* whole paused frame */
        { OP_RESUME,  0.1875, 2,    4, 0.0,     0.0, 0 }, /* live tail 0.0625+0.1875 */
        { OP_ADV,     0.0625, 0,    4, 0.0625,  0.5, 0 },
        { OP_PAUSE,   0.0625, 1,    5, 0.0,     0.0, 0 }, /* boundary again */
        { OP_RESUME,  0.0,    0,    5, 0.0,     0.0, 0 },
        { OP_ADV,     0.375,  3,    8, 0.0,     0.0, 0 },
    };
    run_case("boundary", 0.125, 8, boundary,
             (int)(sizeof boundary / sizeof boundary[0]), 0.25, verbose);

    static const Row long_pause[] = {
        { OP_ADV,       0.25,   2,    2, 0.0, 0.0, 0 },
        { OP_PAUSE_NOW, 0.0,    0,    2, 0.0, 0.0, 0 },
        { OP_ADV,       10.0,   0,    2, 0.0, 0.0, 0 }, /* no catch-up, no drops */
        { OP_ADV,       0.125,  0,    2, 0.0, 0.0, 0 },
        { OP_RESUME_NOW, 0.0,   0,    2, 0.0, 0.0, 0 },
        { OP_ADV,       0.125,  1,    3, 0.0, 0.0, 0 },
    };
    run_case("long_pause", 0.125, 8, long_pause,
             (int)(sizeof long_pause / sizeof long_pause[0]), 10.125, verbose);

    /* Replay vs real time: real time sees every paused frame, replay only
       the live ones; sim state must agree exactly. */
    static const Row realtime[] = {
        { OP_ADV,     0.25,   0, 0, 0.0, 0.0, 0 },
        { OP_ADV,     0.0625, 0, 0, 0.0, 0.0, 0 },
        { OP_PAUSE,   0.0625, 0, 0, 0.0, 0.0, 0 },
        { OP_ADV,     0.1875, 0, 0, 0.0, 0.0, 0 },
        { OP_ADV,     0.0625, 0, 0, 0.0, 0.0, 0 },
        { OP_RESUME,  0.1875, 0, 0, 0.0, 0.0, 0 },
        { OP_ADV,     0.0625, 0, 0, 0.0, 0.0, 0 },
        { OP_PAUSE,   0.125,  0, 0, 0.0, 0.0, 0 },
        { OP_ADV,     1.0,    0, 0, 0.0, 0.0, 0 },
        { OP_RESUME,  0.0,    0, 0, 0.0, 0.0, 0 },
        { OP_ADV,     0.375,  0, 0, 0.0, 0.0, 0 },
    };
    Clock rt, rp;
    clock_init(&rt, 0.125, 8);
    for (unsigned i = 0; i < sizeof realtime / sizeof realtime[0]; i++) {
        switch (realtime[i].op) {
        case OP_ADV:   clock_advance(&rt, realtime[i].dt); break;
        case OP_PAUSE: clock_pause_at(&rt, realtime[i].dt); break;
        default:       clock_resume_at(&rt, realtime[i].dt); break;
        }
    }
    run_replay(realtime, (int)(sizeof realtime / sizeof realtime[0]), 0.125, 8, &rp);
    check("replay_realtime_steps", rt.total_steps == rp.total_steps && rt.total_steps == 9);
    check("replay_realtime_acc", rt.acc == rp.acc);
    check("replay_realtime_alpha", rt.alpha == rp.alpha);
    check("replay_realtime_dropped", rt.dropped == rp.dropped);
    check("realtime_paused_time", near(rt.paused_time, 1.25));

    printf("all_ok=%d\n", failures == 0);
    return failures == 0 ? 0 : 1;
}

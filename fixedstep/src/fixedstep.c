/* fixedstep: deterministic fixed-timestep timeline.
 *
 * Time is counted in integer ticks (microseconds). Frame time is quantized
 * once against a cumulative clock, so float frame times never accumulate:
 *
 *     frame_tick[i] = round(sum(frame_seconds[0..i]) * 1e6)
 *     delta[i]      = frame_tick[i] - frame_tick[i-1]
 *
 * The accumulator, executed step count and dropped step count are integers,
 * so the ledger balances exactly:
 *
 *     (executed + dropped) * step_ticks + acc == total_ticks
 *
 * The simulation advances only in fixed steps. Rendering alpha = acc/step
 * interpolates between the previous and current step and never mutates state.
 * Input is quantized to the first step boundary at or after its tick, so it is
 * independent of frame pacing. A recording stores the integer deltas plus the
 * step-aligned input; replay consumes exactly those integers and reproduces
 * every frame bit for bit.
 */
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <math.h>

#define TICKS_PER_SEC   1000000LL   /* one tick = 1 us */
#define STEP_TICKS      10000LL     /* 10 ms -> 100 Hz */
#define MAX_CATCHUP     8
#define MAX_EVENTS      256
#define MAX_FRAMES      4096

#define FNV_OFFSET      1469598103934665603ULL
#define FNV_PRIME       1099511628211ULL

/* ---------------------------------------------------------------- clock */

typedef struct {
    int64_t step_ticks;
    int64_t acc;            /* ticks not yet consumed by a step */
    int64_t total_ticks;    /* all frame ticks ever submitted */
    long long total_steps;  /* steps actually executed */
    long long dropped;      /* steps skipped by the catch-up cap */
    int max_catchup;
    double alpha;           /* acc/step, render-only */
} Clock;

static int clock_init(Clock* c, int64_t step_ticks, int max_catchup) {
    if (!c || step_ticks <= 0 || max_catchup <= 0) return -1;
    c->step_ticks = step_ticks;
    c->acc = 0;
    c->total_ticks = 0;
    c->total_steps = 0;
    c->dropped = 0;
    c->max_catchup = max_catchup;
    c->alpha = 0.0;
    return 0;
}

/* Advance by an integer number of ticks. Returns steps executed this frame,
 * or -1 for an invalid delta. No float ever influences the step count. */
static int clock_advance_ticks(Clock* c, int64_t delta_ticks) {
    if (!c || delta_ticks < 0) return -1;
    if (c->total_ticks > INT64_MAX - delta_ticks) return -1;

    c->total_ticks += delta_ticks;
    c->acc += delta_ticks;

    int64_t due = c->acc / c->step_ticks;
    int64_t run = due;
    if (run > c->max_catchup) {
        c->dropped += run - c->max_catchup;
        run = c->max_catchup;
    }
    c->acc -= due * c->step_ticks;  /* dropped steps leave the backlog too */
    c->total_steps += run;
    c->alpha = (double)c->acc / (double)c->step_ticks;
    return (int)run;
}

/* Seconds -> ticks, rounding to nearest, saturating. Called on cumulative
 * time so per-frame rounding cannot accumulate. */
static int64_t ticks_from_seconds(double seconds) {
    if (!(seconds > 0.0)) return 0;
    if (seconds >= (double)INT64_MAX / (double)TICKS_PER_SEC) return INT64_MAX;
    return llround(seconds * (double)TICKS_PER_SEC);
}

/* ------------------------------------------------------------ simulation */

typedef struct {
    int64_t at_tick;    /* when the input happened, absolute ticks */
    int64_t step_index; /* first step boundary at or after at_tick */
    int applied;
} InputEvent;

typedef struct {
    uint64_t frame;
    long long step;
    int64_t acc;
    int64_t x;          /* step state, replicated */
    int64_t y;
    uint64_t h;         /* running hash after the frame's last step */
} FrameState;

typedef struct {
    Clock clock;
    InputEvent events[MAX_EVENTS];
    int event_count;
    int64_t x, y;       /* current step state */
    int64_t px, py;     /* state at the previous step, render only */
    long long step;     /* completed steps = clock.total_steps */
    uint64_t h;
    FrameState frames[MAX_FRAMES];
    int frame_count;
} Sim;

static uint64_t hash_bytes(uint64_t h, const void* p, size_t n) {
    const unsigned char* b = (const unsigned char*)p;
    for (size_t i = 0; i < n; i++) {
        h ^= b[i];
        h *= FNV_PRIME;
    }
    return h;
}

/* Quantize an input tick to a step boundary: ceil(at_tick / step_ticks). */
static int64_t align_input(int64_t at_tick) {
    if (at_tick <= 0) return 0;
    return (at_tick + STEP_TICKS - 1) / STEP_TICKS;
}

static int sim_init(Sim* s) {
    memset(s, 0, sizeof(*s));
    if (clock_init(&s->clock, STEP_TICKS, MAX_CATCHUP) != 0) return -1;
    s->h = FNV_OFFSET;
    return 0;
}

/* Load events (given in absolute ticks) and align them to step boundaries. */
static int sim_load_input(Sim* s, const int64_t* ticks, int n) {
    if (n > MAX_EVENTS) return -1;
    for (int i = 0; i < n; i++) {
        s->events[i].at_tick = ticks[i];
        s->events[i].step_index = align_input(ticks[i]);
        s->events[i].applied = 0;
    }
    s->event_count = n;
    return 0;
}

/* Input applies at its step boundary. If that step was dropped by the
 * catch-up cap, it applies at the next step that actually runs. */
static void apply_input(Sim* s) {
    for (int i = 0; i < s->event_count; i++) {
        InputEvent* e = &s->events[i];
        if (!e->applied && e->step_index <= s->step) {
            s->x += 1;
            e->applied = 1;
        }
    }
}

static void sim_step(Sim* s) {
    int64_t t = (int64_t)s->step;
    apply_input(s);
    s->y += s->x;
    s->x += (t & 1) ? -1 : 1;
    s->step++;
    s->h = hash_bytes(s->h, &s->step, sizeof s->step);
    s->h = hash_bytes(s->h, &s->x, sizeof s->x);
    s->h = hash_bytes(s->h, &s->y, sizeof s->y);
}

/* Run one frame worth of integer ticks and record a snapshot. */
static void sim_frame(Sim* s, int64_t delta_ticks) {
    if (s->frame_count >= MAX_FRAMES) return;
    s->px = s->x;
    s->py = s->y;
    int steps = clock_advance_ticks(&s->clock, delta_ticks);
    for (int i = 0; i < steps; i++) sim_step(s);

    FrameState* f = &s->frames[s->frame_count++];
    f->frame = (uint64_t)s->frame_count;
    f->step = s->step;
    f->acc = s->clock.acc;
    f->x = s->x;
    f->y = s->y;
    f->h = s->h;
}

/* alpha in [0,1] blends previous and current step state for rendering. */
static void render_sample(const Sim* s, double* rx, double* ry) {
    double a = s->clock.alpha;
    *rx = (double)s->px + ((double)s->x - (double)s->px) * a;
    *ry = (double)s->py + ((double)s->y - (double)s->py) * a;
}

static int ledger_balanced(const Sim* s) {
    return (s->clock.total_steps + s->clock.dropped) * s->clock.step_ticks +
           s->clock.acc == s->clock.total_ticks;
}

/* ------------------------------------------------------------- recording */

typedef struct {
    int64_t delta_ticks;
    int64_t frame_tick; /* cumulative absolute tick of this frame */
} RecordedFrame;

typedef struct {
    RecordedFrame frames[MAX_FRAMES];
    int frame_count;
    int64_t event_ticks[MAX_EVENTS];
    int event_count;
} Recording;

/* Record a live run at an arbitrary, jittery frame rate. Frame times are
 * doubles from wall clock; they are quantized against cumulative time once. */
static void record_run(Recording* r, const double* frame_seconds, int n,
                       const int64_t* event_ticks, int event_count, Sim* out) {
    memset(r, 0, sizeof(*r));
    sim_init(out);
    /* Input is aligned by its absolute tick, never by the frame that happened
     * to observe it, so frame pacing cannot change the input stream. */
    sim_load_input(out, event_ticks, event_count);

    double t = 0.0;
    int64_t prev_tick = 0;
    for (int i = 0; i < n && i < MAX_FRAMES; i++) {
        t += frame_seconds[i];
        int64_t abs_tick = ticks_from_seconds(t);
        int64_t delta = abs_tick - prev_tick;
        if (delta < 0) delta = 0; /* clock went backwards: clamp, no drift */

        r->frames[r->frame_count].delta_ticks = delta;
        r->frames[r->frame_count].frame_tick = abs_tick;
        r->frame_count++;
        prev_tick = abs_tick;

        sim_frame(out, delta);
    }
    for (int i = 0; i < event_count && i < MAX_EVENTS; i++)
        r->event_ticks[i] = event_ticks[i];
    r->event_count = event_count;
}

/* Replay consumes exactly the recorded integers and the recorded input.
 * No wall clock is read, no float frame time is involved. */
static void replay_run(const Recording* r, Sim* out) {
    sim_init(out);
    sim_load_input(out, r->event_ticks, r->event_count);
    for (int i = 0; i < r->frame_count; i++)
        sim_frame(out, r->frames[i].delta_ticks);
}

/* Same total ticks, different frame split: step count and final state must
 * match. Per-frame snapshots are allowed to differ. */
static void retimed_run(const Recording* r, const int64_t* deltas, int n,
                        Sim* out) {
    sim_init(out);
    sim_load_input(out, r->event_ticks, r->event_count);
    for (int i = 0; i < n; i++) sim_frame(out, deltas[i]);
}

/* ------------------------------------------------------------- self-test */

static int failures = 0;
static void check(const char* name, int ok) {
    printf("%s=%d\n", name, ok ? 1 : 0);
    if (!ok) failures++;
}

static int frames_equal(const Sim* a, const Sim* b) {
    if (a->frame_count != b->frame_count || a->step != b->step) return 0;
    for (int i = 0; i < a->frame_count; i++) {
        const FrameState* fa = &a->frames[i];
        const FrameState* fb = &b->frames[i];
        if (fa->frame != fb->frame || fa->step != fb->step ||
            fa->acc != fb->acc || fa->x != fb->x || fa->y != fb->y ||
            fa->h != fb->h) return 0;
    }
    return 1;
}

int main(void) {
    Clock c;

    /* ---------- clock: integer semantics ---------- */
    check("init", clock_init(&c, STEP_TICKS, MAX_CATCHUP) == 0);
    check("init_rejects", clock_init(&c, 0, 4) == -1);

    clock_init(&c, STEP_TICKS, MAX_CATCHUP);
    check("one_step", clock_advance_ticks(&c, STEP_TICKS) == 1 && c.total_steps == 1);
    check("alpha_zero", fabs(c.alpha) < 1e-12);

    check("half_step", clock_advance_ticks(&c, STEP_TICKS / 2) == 0 &&
                       fabs(c.alpha - 0.5) < 1e-12);
    check("completes", clock_advance_ticks(&c, STEP_TICKS / 2) == 1 &&
                       c.total_steps == 2);

    check("two_steps", clock_advance_ticks(&c, 2 * STEP_TICKS + STEP_TICKS / 2) == 2 &&
                       fabs(c.alpha - 0.5) < 1e-12);

    check("negative_rejected", clock_advance_ticks(&c, -1) == -1);
    check("zero_delta", clock_advance_ticks(&c, 0) == 0);

    /* catch-up cap: 10 steps due, 4 allowed, 6 dropped, ledger intact */
    clock_init(&c, STEP_TICKS, 4);
    check("catchup_capped", clock_advance_ticks(&c, 10 * STEP_TICKS) == 4);
    check("dropped_recorded", c.dropped == 6);
    check("alpha_bounded", c.alpha >= 0.0 && c.alpha < 1.0);
    check("ledger_capped",
          (c.total_steps + c.dropped) * STEP_TICKS + c.acc == c.total_ticks);

    /* input alignment is by absolute tick, not by frame */
    check("align_input_floor", align_input(0) == 0 && align_input(1) == 1);
    check("align_input_exact", align_input(STEP_TICKS) == 1);
    check("align_input_inside", align_input(STEP_TICKS + 1) == 2);

    /* ---------- 10 s at deliberately bad pacing ---------- */
    static double jitter[600];
    for (int i = 0; i < 600; i++) {
        static const double pattern[] = { 0.005, 0.0273, 0.0167, 0.0334, 0.011 };
        jitter[i] = pattern[i % 5] + (double)((i * 37) % 11) * 0.0009;
    }

    static int64_t event_ticks[MAX_EVENTS];
    int event_count = 12;
    for (int i = 0; i < event_count; i++)
        event_ticks[i] = (int64_t)(i * 833333) + 4242; /* some mid-step */

    static Recording rec;
    static Sim live, played;

    /* 600 frames sum to ~10.02 s; the live run must stay exact */
    record_run(&rec, jitter, 600, event_ticks, event_count, &live);

    double live_seconds = 0.0;
    for (int i = 0; i < 600; i++) live_seconds += jitter[i];
    int64_t expected_ticks = ticks_from_seconds(live_seconds);
    check("live_total_ticks", live.clock.total_ticks == expected_ticks);
    check("live_ledger", ledger_balanced(&live));
    check("live_steps_match", live.clock.total_steps == live.step);
    check("live_input_applied", live.events[0].applied == 1 &&
                                live.events[event_count - 1].applied == 1);
    check("live_input_aligned", live.events[0].step_index ==
                                (4242 + STEP_TICKS - 1) / STEP_TICKS);

    /* replay of the exact recording: identical per frame */
    replay_run(&rec, &played);
    check("replay_frames_identical", frames_equal(&live, &played));
    check("replay_state_equal", live.x == played.x && live.y == played.y &&
                                live.h == played.h && live.step == played.step);
    check("replay_drift_zero", live.x - played.x == 0 && live.y - played.y == 0);
    check("replay_total_ticks_equal", played.clock.total_ticks ==
                                      live.clock.total_ticks);

    /* run the same recording twice: bit-identical */
    static Sim played2;
    replay_run(&rec, &played2);
    check("replay_twice_identical", frames_equal(&played, &played2) &&
                                    played.h == played2.h);

    /* ---------- retimed replay: same total, same step count ---------- */
    int64_t total = live.clock.total_ticks;
    int64_t full = total / STEP_TICKS;
    int64_t rem = total % STEP_TICKS;
    static int64_t deltas[MAX_FRAMES];
    int nd = 0;
    for (int64_t i = 0; i < full && nd < MAX_FRAMES - 1; i++) deltas[nd++] = STEP_TICKS;
    if (rem > 0) deltas[nd++] = rem;

    static Sim retimed;
    retimed_run(&rec, deltas, nd, &retimed);
    check("retimed_total_ticks", retimed.clock.total_ticks == total);
    check("retimed_state_equal", retimed.x == live.x && retimed.y == live.y &&
                                 retimed.h == live.h);
    check("retimed_step_count", retimed.step == live.step);
    check("retimed_ledger", ledger_balanced(&retimed));

    /* ---------- float frame times do not accumulate drift ---------- */
    /* Both runs total 10 s and stay under the catch-up cap, so the only
     * difference is the frame split and the float rounding in the clock. */
    static Recording f1, f2;
    static Sim s1, s2;
    double fine[600], coarse[200];
    for (int i = 0; i < 600; i++) fine[i] = 1.0 / 60.0;
    for (int i = 0; i < 200; i++) coarse[i] = 1.0 / 20.0;
    record_run(&f1, fine, 600, NULL, 0, &s1);
    record_run(&f2, coarse, 200, NULL, 0, &s2);
    check("pacing_total_equal", s1.clock.total_ticks == s2.clock.total_ticks);
    check("pacing_total_exact", s1.clock.total_ticks == 10 * TICKS_PER_SEC);
    check("pacing_steps_equal", s1.step == s2.step);
    check("pacing_state_equal", s1.x == s2.x && s1.y == s2.y && s1.h == s2.h);
    check("pacing_ledger", ledger_balanced(&s1) && ledger_balanced(&s2));

    /* ---------- interpolation is read-only ---------- */
    int64_t before_x = live.x, before_y = live.y;
    double rx, ry;
    render_sample(&live, &rx, &ry);
    check("render_readonly", live.x == before_x && live.y == before_y);
    check("render_bounded", rx >= (double)(live.px < live.x ? live.px : live.x) &&
                            rx <= (double)(live.px > live.x ? live.px : live.x));

    /* ---------- replay vs live at the end of the run: zero difference ---- */
    const FrameState* fa = &live.frames[live.frame_count - 1];
    const FrameState* fb = &played.frames[played.frame_count - 1];
    check("live_vs_replay_final_zero",
          fa->x == fb->x && fa->y == fb->y && fa->h == fb->h &&
          fa->step == fb->step && fa->acc == fb->acc);

    printf("all_ok=%d\n", failures == 0);
    return failures == 0 ? 0 : 1;
}


/* inputbuf: combo input buffer with configurable pre/post tolerance windows,
   same-frame press merging, a strict frame-exact reference matcher (the
   hand-written per-frame behavior) and a zero-to-max window hit curve. */
#include <stdio.h>
#include <stdint.h>
#include <string.h>

#define BTN_MAX     16      /* buttons fit in a uint16_t mask */
#define SEQ_MAX     16
#define INPUT_MAX   64
#define CURVE_MAX   16

enum { OK = 0, E_ARG = -1, E_NEG = -2, E_ORDER = -3, E_OVERLAP = -4, E_FULL = -5 };

typedef struct {
    int action;     /* button id, 0..BTN_MAX-1 */
    int frame;      /* frame the action is expected on */
    int pre;        /* frames allowed early (input buffer) */
    int post;       /* frames allowed late */
} SeqStep;

typedef struct {
    int frame;
    uint16_t mask;  /* buttons pressed on this frame, same-frame presses merged */
} FrameInput;

typedef struct {
    FrameInput ev[INPUT_MAX];
    int raw;        /* pushes before merging */
    int count;      /* events after finalize */
    int merges;     /* raw - count: presses folded into an earlier event */
    int finalized;
} InputLog;

typedef struct {
    int matched[SEQ_MAX];   /* frame of the event that hit each step, else -1 */
    int hits;
    int full;               /* every step matched */
} MatchResult;

typedef struct { int enabled; } BufferCfg;

typedef struct {
    int w;
    int hits;
    int full;
    double hit_rate;
    double full_rate;
} CurvePoint;

/* ---- sequence definition / validation ---- */

static int seq_validate(const SeqStep* s, int n) {
    if (!s || n <= 0 || n > SEQ_MAX) return E_ARG;
    for (int i = 0; i < n; i++) {
        if (s[i].action < 0 || s[i].action >= BTN_MAX) return E_ARG;
        if (s[i].pre < 0 || s[i].post < 0) return E_NEG;
        if (i > 0) {
            if (s[i].frame <= s[i - 1].frame) return E_ORDER;
            int prev_end = s[i - 1].frame + s[i - 1].post;
            int cur_start = s[i].frame - s[i].pre;
            if (prev_end >= cur_start) return E_OVERLAP;   /* shared frame is ambiguous */
        }
    }
    return OK;
}

/* build a uniform-window sequence; per-step pre/post can be edited afterwards */
static int seq_build(SeqStep* out, int n, const int* actions, const int* frames,
                     int pre, int post) {
    if (!out || !actions || !frames || n > SEQ_MAX) return E_ARG;
    if (pre < 0 || post < 0) return E_NEG;
    for (int i = 0; i < n; i++) {
        out[i].action = actions[i];
        out[i].frame = frames[i];
        out[i].pre = pre;
        out[i].post = post;
    }
    return seq_validate(out, n);
}

/* ---- input log: collect presses, sort and merge same-frame ones ---- */

static void log_init(InputLog* l) { memset(l, 0, sizeof(*l)); }

static int log_push(InputLog* l, int frame, int button) {
    if (!l || frame < 0 || button < 0 || button >= BTN_MAX) return E_ARG;
    if (l->raw >= INPUT_MAX) return E_FULL;
    l->ev[l->raw].frame = frame;
    l->ev[l->raw].mask = (uint16_t)(1u << button);
    l->raw++;
    l->finalized = 0;
    return OK;
}

static void log_finalize(InputLog* l) {
    if (l->finalized) return;               /* idempotent: never re-fold leftovers */
    for (int i = 1; i < l->raw; i++) {          /* insertion sort by frame */
        FrameInput k = l->ev[i];
        int j = i - 1;
        while (j >= 0 && l->ev[j].frame > k.frame) {
            l->ev[j + 1] = l->ev[j];
            j--;
        }
        l->ev[j + 1] = k;
    }
    int out = 0;
    for (int i = 0; i < l->raw; ) {             /* fold same-frame events */
        FrameInput m = l->ev[i];
        int k = i + 1;
        while (k < l->raw && l->ev[k].frame == m.frame) {
            m.mask |= l->ev[k].mask;
            k++;
        }
        l->ev[out++] = m;
        i = k;
    }
    l->count = out;
    l->merges = l->raw - l->count;
    l->finalized = 1;
}

/* ---- matchers ---- */

static void result_finish(MatchResult* r, int n) {
    r->hits = 0;
    for (int i = 0; i < n; i++) if (r->matched[i] >= 0) r->hits++;
    r->full = (r->hits == n);
}

/* reference: the hand-written frame-exact judge, no tolerance at all */
static void match_strict(const SeqStep* s, int n, InputLog* l, MatchResult* out) {
    memset(out, 0, sizeof(*out));          /* full-defined so results can be memcmp'd */
    log_finalize(l);
    int cur = 0;
    for (int i = 0; i < n; i++) {
        out->matched[i] = -1;
        uint16_t want = (uint16_t)(1u << s[i].action);
        int j = cur;
        while (j < l->count && l->ev[j].frame < s[i].frame) j++;
        if (j < l->count && l->ev[j].frame == s[i].frame && (l->ev[j].mask & want)) {
            out->matched[i] = l->ev[j].frame;
            cur = j + 1;
        } else {
            cur = j;
        }
    }
    result_finish(out, n);
}

/* buffered: hit if an event carrying the button lies in [F-pre, F+post];
   events are consumed in order, earliest qualifying event wins */
static void match_buffered(const SeqStep* s, int n, InputLog* l, MatchResult* out) {
    memset(out, 0, sizeof(*out));
    log_finalize(l);
    int cur = 0;
    for (int i = 0; i < n; i++) {
        out->matched[i] = -1;
        int lo = s[i].frame - s[i].pre;
        int hi = s[i].frame + s[i].post;
        uint16_t want = (uint16_t)(1u << s[i].action);
        int j = cur;
        while (j < l->count && l->ev[j].frame < lo) j++;   /* expired events */
        int k = j;
        while (k < l->count && l->ev[k].frame <= hi) {
            if (l->ev[k].mask & want) {
                out->matched[i] = l->ev[k].frame;
                cur = k + 1;
                break;
            }
            k++;
        }
        if (out->matched[i] < 0) cur = j;
    }
    result_finish(out, n);
}

/* disabled tolerance routes to the strict path, regardless of window values */
static void match_cfg(const BufferCfg* cfg, const SeqStep* s, int n,
                      InputLog* l, MatchResult* r) {
    if (cfg && cfg->enabled) match_buffered(s, n, l, r);
    else match_strict(s, n, l, r);
}

/* ---- hit curve: sweep a symmetric window from 0 up to max_w ---- */

static int hit_curve(const SeqStep* tmpl, int n, InputLog* trials, int ntrials,
                     int max_w, CurvePoint* out) {
    if (!tmpl || !trials || !out || n <= 0 || ntrials <= 0 || max_w < 0)
        return E_ARG;
    SeqStep work[SEQ_MAX];
    int pts = 0;
    for (int w = 0; w <= max_w && w <= CURVE_MAX; w++) {
        for (int i = 0; i < n; i++) {
            work[i] = tmpl[i];
            work[i].pre = w;
            work[i].post = w;
        }
        if (seq_validate(work, n) != OK) break;       /* bigger w only overlaps more */
        int hits = 0, full = 0;
        for (int t = 0; t < ntrials; t++) {
            MatchResult r;
            match_buffered(work, n, &trials[t], &r);
            hits += r.hits;
            full += r.full;
        }
        memset(&out[pts], 0, sizeof(CurvePoint));   /* define padding too, memcmp-safe */
        out[pts].w = w;
        out[pts].hits = hits;
        out[pts].full = full;
        out[pts].hit_rate = (double)hits / (double)(ntrials * n);
        out[pts].full_rate = (double)full / (double)ntrials;
        pts++;
    }
    return pts;
}

/* ---- tiny local rng so the self-test is deterministic and self-contained ---- */

static uint64_t rng_s;
static uint64_t rnd64(void) {
    uint64_t z = (rng_s += 0x9E3779B97F4A7C15ULL);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31);
}
static int rnd_range(int lo, int hi) {
    return lo + (int)(rnd64() % (unsigned)(hi - lo + 1));
}

static int failures = 0;
static void check(const char* name, int ok) {
    printf("%s=%d\n", name, ok ? 1 : 0);
    if (!ok) failures++;
}

#define NTRIAL 300

static void build_trials(InputLog* trials, const int* frames, int n, uint64_t seed) {
    rng_s = seed;
    for (int t = 0; t < NTRIAL; t++) {
        log_init(&trials[t]);
        for (int i = 0; i < n; i++)
            log_push(&trials[t], frames[i] + rnd_range(-3, 3), i);
        for (int k = 0; k < 3; k++)                          /* noise presses */
            log_push(&trials[t], rnd_range(0, 40), rnd_range(0, BTN_MAX - 1));
    }
}

int main(int argc, char** argv) {
    static const int act[4] = {0, 1, 2, 3};
    static const int fr[4]  = {10, 18, 26, 34};   /* gap 8: w up to 3 is overlap-free */

    /* same-frame presses merge into one masked event; duplicate key is idempotent */
    InputLog m;
    log_init(&m);
    check("merge_push_ok", log_push(&m, 20, 2) == OK
          && log_push(&m, 20, 5) == OK && log_push(&m, 20, 2) == OK);
    log_finalize(&m);
    check("merge_one_event", m.count == 1 && m.merges == 2);
    check("merge_mask", m.ev[0].mask == ((1u << 2) | (1u << 5)));
    check("merge_bad_arg", log_push(&m, -1, 0) == E_ARG
          && log_push(&m, 5, BTN_MAX) == E_ARG);

    /* hand-built case: every press is 1-2 frames off, strict judge drops the combo */
    SeqStep seq0[4], seq2[4];
    check("build_zero", seq_build(seq0, 4, act, fr, 0, 0) == OK);
    check("build_w2", seq_build(seq2, 4, act, fr, 2, 2) == OK);
    InputLog demo;
    log_init(&demo);
    log_push(&demo, 9, 0); log_push(&demo, 20, 1);
    log_push(&demo, 26, 2); log_push(&demo, 36, 3);
    MatchResult rs, rb, rd, re;
    match_strict(seq0, 4, &demo, &rs);
    check("strict_only_exact", rs.hits == 1 && rs.matched[0] == -1
          && rs.matched[1] == -1 && rs.matched[2] == 26 && rs.matched[3] == -1);
    match_buffered(seq2, 4, &demo, &rb);
    check("buffered_saves_combo", rb.full == 1
          && rb.matched[0] == 9 && rb.matched[1] == 20
          && rb.matched[2] == 26 && rb.matched[3] == 36);

    /* disabled tolerance is the strict path even when windows are configured */
    BufferCfg off = {0}, on = {1};
    match_cfg(&off, seq2, 4, &demo, &rd);
    match_cfg(&on, seq2, 4, &demo, &re);
    check("cfg_off_is_strict", memcmp(&rd, &rs, sizeof(rs)) == 0);
    check("cfg_on_is_buffered", memcmp(&re, &rb, sizeof(rb)) == 0);
    match_cfg(0, seq2, 4, &demo, &rd);
    check("cfg_null_is_strict", memcmp(&rd, &rs, sizeof(rs)) == 0);

    /* earliest qualifying event wins; events before the window expire */
    SeqStep one[1];
    int a1 = 1, f1 = 15;
    seq_build(one, 1, &a1, &f1, 1, 0);
    InputLog e1;
    log_init(&e1); log_push(&e1, 12, 1); log_push(&e1, 15, 1);
    MatchResult r1;
    match_buffered(one, 1, &e1, &r1);
    check("early_event_expires", r1.matched[0] == 15);
    seq_build(one, 1, &a1, &f1, 1, 1);
    InputLog e2;
    log_init(&e2); log_push(&e2, 14, 1); log_push(&e2, 16, 1);
    MatchResult r2;
    match_buffered(one, 1, &e2, &r2);
    check("earliest_in_window", r2.matched[0] == 14);

    /* merged event satisfies a step when its mask contains the button */
    SeqStep two[1];
    int a2 = 2, f2 = 20;
    seq_build(two, 1, &a2, &f2, 0, 0);
    MatchResult rm;
    match_strict(two, 1, &m, &rm);
    check("merged_mask_matches", rm.matched[0] == 20);

    /* zero-window buffered matching must equal strict on every fuzz trial */
    static InputLog trials[NTRIAL], trials2[NTRIAL];
    build_trials(trials, fr, 4, 42);
    int equiv = 1, strict_hits = 0;
    for (int t = 0; t < NTRIAL; t++) {
        MatchResult a, b;
        match_strict(seq0, 4, &trials[t], &a);
        match_buffered(seq0, 4, &trials[t], &b);
        if (memcmp(&a, &b, sizeof(a)) != 0) equiv = 0;
        strict_hits += a.hits;
    }
    check("zero_window_equals_strict", equiv == 1);

    /* hit curve: monotone, starts at strict total; w=3 covers the +/-3 jitter,
       w=4 overlaps adjacent windows (gap 8) and truncates the sweep */
    CurvePoint curve[CURVE_MAX + 1];
    int pts = hit_curve(seq0, 4, trials, NTRIAL, 3, curve);
    check("curve_points", pts == 4);
    check("curve_starts_strict", pts > 0 && curve[0].hits == strict_hits);
    int mono = 1;
    for (int i = 1; i < pts; i++)
        if (curve[i].hits < curve[i - 1].hits) mono = 0;
    check("curve_monotone", mono == 1);
    check("curve_reaches_full", curve[pts - 1].full == NTRIAL
          && curve[pts - 1].hit_rate == 1.0);
    check("curve_overlap_truncates",
          hit_curve(seq0, 4, trials, NTRIAL, 6, curve) == 4);

    printf("hit_curve: trials=%d steps=%d\n", NTRIAL, 4);
    printf(" w   hits  hit_rate  full  full_rate\n");
    for (int i = 0; i < pts; i++) {
        printf("%2d  %5d  %.4f   %4d  %.4f  ", curve[i].w, curve[i].hits,
               curve[i].hit_rate, curve[i].full, curve[i].full_rate);
        for (int b = 0; b < (int)(curve[i].hit_rate * 20.0); b++) putchar('#');
        putchar('\n');
    }

    /* same seed reproduces the same curve */
    build_trials(trials2, fr, 4, 42);
    CurvePoint curve2[CURVE_MAX + 1];
    int pts2 = hit_curve(seq0, 4, trials2, NTRIAL, 3, curve2);
    check("curve_deterministic", pts2 == pts
          && memcmp(curve, curve2, (size_t)pts * sizeof(CurvePoint)) == 0);

    /* optional csv artifact: argv[1] is the output path */
    if (argc > 1) {
        FILE* f = fopen(argv[1], "w");
        if (f) {
            fprintf(f, "w,hits,total,hit_rate,full,trials,full_rate\n");
            for (int i = 0; i < pts; i++)
                fprintf(f, "%d,%d,%d,%.6f,%d,%d,%.6f\n", curve[i].w, curve[i].hits,
                        NTRIAL * 4, curve[i].hit_rate, curve[i].full,
                        NTRIAL, curve[i].full_rate);
            fclose(f);
            printf("csv_written=%s\n", argv[1]);
        }
    }

    /* rejection: negative windows */
    SeqStep neg[2];
    check("reject_pre_neg", seq_build(neg, 2, act, fr, -1, 0) == E_NEG);
    check("reject_post_neg", seq_build(neg, 2, act, fr, 0, -2) == E_NEG);
    neg[0] = (SeqStep){0, 10, -1, 0};
    neg[1] = (SeqStep){1, 20, 0, 0};
    check("reject_validate_neg", seq_validate(neg, 2) == E_NEG);

    /* rejection: frame order */
    SeqStep ord[2];
    int bad_act[2] = {0, 1}, dup_fr[2] = {10, 10}, back_fr[2] = {12, 10};
    check("reject_dup_frame", seq_build(ord, 2, bad_act, dup_fr, 0, 0) == E_ORDER);
    check("reject_backwards", seq_build(ord, 2, bad_act, back_fr, 0, 0) == E_ORDER);
    check("reject_bad_button", seq_build(ord, 2, (int[]){0, BTN_MAX}, fr, 0, 0) == E_ARG);
    check("reject_bad_n", seq_validate(ord, 0) == E_ARG);

    /* rejection: overlapping tolerance windows (gap 2 only supports w=0) */
    int gap_fr[2] = {10, 12};
    check("tight_gap_w0_ok", seq_build(ord, 2, bad_act, gap_fr, 0, 0) == OK);
    check("reject_overlap", seq_build(ord, 2, bad_act, gap_fr, 1, 1) == E_OVERLAP);
    int gap3[2] = {10, 13};
    check("gap3_w1_ok", seq_build(ord, 2, bad_act, gap3, 1, 1) == OK);

    /* curve sweep truncates at the largest non-overlapping window */
    SeqStep tight[2];
    seq_build(tight, 2, bad_act, gap_fr, 0, 0);
    static InputLog tiny[8];
    for (int t = 0; t < 8; t++) {
        log_init(&tiny[t]);
        log_push(&tiny[t], 10, 0);
        log_push(&tiny[t], 12, 1);
    }
    CurvePoint cut[CURVE_MAX + 1];
    check("curve_truncates", hit_curve(tight, 2, tiny, 8, 3, cut) == 1
          && cut[0].w == 0);

    /* capacity limits report errors instead of overflowing */
    InputLog full;
    log_init(&full);
    int cap_ok = 1;
    for (int i = 0; i < INPUT_MAX; i++)
        if (log_push(&full, i, 0) != OK) cap_ok = 0;
    check("log_capacity", cap_ok == 1 && log_push(&full, INPUT_MAX, 0) == E_FULL);
    SeqStep big[SEQ_MAX + 1];
    check("seq_capacity", seq_build(big, SEQ_MAX + 1, act, fr, 0, 0) == E_ARG);

    printf("all_ok=%d\n", failures == 0);
    return failures == 0 ? 0 : 1;
}

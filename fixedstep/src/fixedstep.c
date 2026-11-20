
/* fixedstep: fixed timestep accumulator with interpolation alpha and catch-up cap.
   Includes a fixed-size, checksummed save blob so a clock can resume exactly
   where it was checkpointed. */
#include <stdio.h>
#include <math.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <limits.h>

typedef struct {
    double step;        /* fixed dt */
    double acc;         /* leftover time carried across frames */
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

/* ---- save format ---------------------------------------------------------
   Fixed 56-byte little-endian blob; bytes [0,48) are covered by an FNV-1a
   checksum stored at [48,56). Every double is stored as its raw 64-bit
   pattern, so a restore is bit-exact rather than a rounded decimal
   approximation. The layout is explicit (no struct padding). */

#define FIXEDSTEP_SAVE_MAGIC 0x3154534550495846ULL /* "FIXSTEP1" */
#define FIXEDSTEP_SAVE_VERSION 1u

#define FS_OFF_MAGIC      0
#define FS_OFF_VERSION    8
#define FS_OFF_MAXCATCHUP 12
#define FS_OFF_STEP       16
#define FS_OFF_ACC        24
#define FS_OFF_STEPS      32
#define FS_OFF_DROPPED    40
#define FS_OFF_RESERVED   44
#define FS_OFF_CHECKSUM   48
#define FIXEDSTEP_SAVE_SIZE 56

static void put_u32(unsigned char* p, uint32_t v) {
    p[0] = (unsigned char)(v & 0xFFu);
    p[1] = (unsigned char)((v >> 8) & 0xFFu);
    p[2] = (unsigned char)((v >> 16) & 0xFFu);
    p[3] = (unsigned char)((v >> 24) & 0xFFu);
}

static uint32_t get_u32(const unsigned char* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void put_u64(unsigned char* p, uint64_t v) {
    for (int i = 0; i < 8; i++) p[i] = (unsigned char)((v >> (8 * i)) & 0xFFu);
}

static uint64_t get_u64(const unsigned char* p) {
    uint64_t v = 0;
    for (int i = 0; i < 8; i++) v |= (uint64_t)p[i] << (8 * i);
    return v;
}

static uint64_t double_bits(double d) {
    uint64_t b;
    memcpy(&b, &d, sizeof b);
    return b;
}

static double bits_double(uint64_t b) {
    double d;
    memcpy(&d, &b, sizeof d);
    return d;
}

static uint64_t fs_checksum(const unsigned char* p, size_t n) {
    uint64_t h = 0xCBF29CE484222325ULL;
    for (size_t i = 0; i < n; i++) {
        h ^= p[i];
        h *= 0x100000001B3ULL;
    }
    return h;
}

/* A clock is serialisable only if its live state is well formed. */
static int clock_state_valid(const Clock* c) {
    if (!isfinite(c->step) || c->step <= 0.0) return 0;
    if (!isfinite(c->acc) || c->acc < 0.0 || c->acc >= c->step) return 0;
    if (c->max_catchup < 1) return 0;
    if (c->total_steps < 0) return 0;
    if (c->dropped < 0) return 0;
    return 1;
}

/* Write a checkpoint. Returns FIXEDSTEP_SAVE_SIZE, or -1 if the buffer is too
   small or the clock state is not serialisable. Output size never varies. */
static int clock_save(const Clock* c, unsigned char* buf, size_t cap) {
    if (buf == NULL || cap < FIXEDSTEP_SAVE_SIZE) return -1;
    if (!clock_state_valid(c)) return -1;

    put_u64(buf + FS_OFF_MAGIC, FIXEDSTEP_SAVE_MAGIC);
    put_u32(buf + FS_OFF_VERSION, FIXEDSTEP_SAVE_VERSION);
    put_u32(buf + FS_OFF_MAXCATCHUP, (uint32_t)c->max_catchup);
    put_u64(buf + FS_OFF_STEP, double_bits(c->step));
    put_u64(buf + FS_OFF_ACC, double_bits(c->acc));
    put_u64(buf + FS_OFF_STEPS, (uint64_t)c->total_steps);
    put_u32(buf + FS_OFF_DROPPED, (uint32_t)c->dropped);
    put_u32(buf + FS_OFF_RESERVED, 0u);
    put_u64(buf + FS_OFF_CHECKSUM, fs_checksum(buf, FS_OFF_CHECKSUM));
    return FIXEDSTEP_SAVE_SIZE;
}

/* Read a checkpoint. Returns 0 and fills *c on success; returns -1 and leaves
   *c untouched for any malformed, truncated, mismatched or corrupt blob. */
static int clock_load(Clock* c, const unsigned char* buf, size_t len) {
    if (c == NULL || buf == NULL) return -1;
    if (len != FIXEDSTEP_SAVE_SIZE) return -1;
    if (get_u64(buf + FS_OFF_MAGIC) != FIXEDSTEP_SAVE_MAGIC) return -1;
    if (get_u32(buf + FS_OFF_VERSION) != FIXEDSTEP_SAVE_VERSION) return -1;
    if (get_u64(buf + FS_OFF_CHECKSUM) != fs_checksum(buf, FS_OFF_CHECKSUM)) return -1;
    if (get_u32(buf + FS_OFF_RESERVED) != 0u) return -1;

    uint32_t max_catchup_u = get_u32(buf + FS_OFF_MAXCATCHUP);
    uint64_t steps_u = get_u64(buf + FS_OFF_STEPS);
    uint32_t dropped_u = get_u32(buf + FS_OFF_DROPPED);
    double step = bits_double(get_u64(buf + FS_OFF_STEP));
    double acc = bits_double(get_u64(buf + FS_OFF_ACC));

    /* Range checks stop crafted-but-checksummed blobs from installing state
       that would divide by zero or violate the accumulator invariant. */
    if (max_catchup_u < 1u || max_catchup_u > (uint32_t)INT_MAX) return -1;
    if (steps_u > (uint64_t)LLONG_MAX) return -1;
    if (dropped_u > (uint32_t)INT_MAX) return -1;
    if (!isfinite(step) || step <= 0.0) return -1;
    if (!isfinite(acc) || acc < 0.0 || acc >= step) return -1;

    Clock tmp;
    tmp.step = step;
    tmp.acc = acc;
    tmp.alpha = acc / step;
    tmp.max_catchup = (int)max_catchup_u;
    tmp.total_steps = (long long)steps_u;
    tmp.dropped = (int)dropped_u;
    *c = tmp;
    return 0;
}

static int failures = 0;
static void check(const char* name, int ok) {
    printf("%s=%d\n", name, ok ? 1 : 0);
    if (!ok) failures++;
}

static int same_double(double a, double b) {
    return double_bits(a) == double_bits(b);
}

/* Recompute the checksum after a deliberate semantic mutation, so the test
   exercises the range validators rather than only the checksum. */
static void reseal(unsigned char* buf) {
    put_u64(buf + FS_OFF_CHECKSUM, fs_checksum(buf, FS_OFF_CHECKSUM));
}

static void put_double_bits(unsigned char* buf, size_t off, uint64_t bits) {
    put_u64(buf + off, bits);
}

/* Replay the same 10 seconds of frame deltas through two clocks and require
   frame-by-frame bit-identical state. */
static int replay_ten_seconds(Clock* a, Clock* b) {
    static const double pattern[] = {
        1.0 / 60.0, 1.0 / 144.0, 1.0 / 30.0, 0.004, 0.05, 1.0 / 60.0
    };
    const int n = (int)(sizeof pattern / sizeof pattern[0]);
    double t = 0.0;
    int i = 0;
    while (t < 10.0) {
        double dt = pattern[i % n];
        if (t + dt > 10.0) dt = 10.0 - t;

        int sa = clock_advance(a, dt);
        int sb = clock_advance(b, dt);
        if (sa != sb) return 0;
        if (a->total_steps != b->total_steps) return 0;
        if (!same_double(a->acc, b->acc)) return 0;
        if (!same_double(a->alpha, b->alpha)) return 0;
        if (a->dropped != b->dropped) return 0;

        t += dt;
        i++;
    }
    return 1;
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

    /* ---- save / restore ------------------------------------------------ */

    unsigned char buf[FIXEDSTEP_SAVE_SIZE + 16];
    Clock src;
    clock_init(&src, 1.0 / 240.0, 8);
    for (int i = 0; i < 137; i++) clock_advance(&src, 0.016);
    check("save_size_fixed", clock_save(&src, buf, sizeof buf) == FIXEDSTEP_SAVE_SIZE);

    unsigned char buf2[FIXEDSTEP_SAVE_SIZE];

    Clock dropping;
    clock_init(&dropping, 0.02, 3);
    clock_advance(&dropping, 0.5); /* drops a large backlog */
    check("save_size_fixed_after_drops",
          clock_save(&dropping, buf2, sizeof buf2) == FIXEDSTEP_SAVE_SIZE);

    Clock other;
    clock_init(&other, 0.02, 3);
    check("save_size_fixed_other_state",
          clock_save(&other, buf2, sizeof buf2) == FIXEDSTEP_SAVE_SIZE);
    check("save_rejects_small_buffer", clock_save(&src, buf, FIXEDSTEP_SAVE_SIZE - 1) == -1);

    Clock zero_step_clock;
    clock_init(&zero_step_clock, 0.0, 4);
    (void)clock_advance(&zero_step_clock, 0.05);
    check("save_rejects_bad_state", clock_save(&zero_step_clock, buf, sizeof buf) == -1);

    Clock loaded;
    memset(&loaded, 0, sizeof loaded);
    check("load_ok", clock_load(&loaded, buf, FIXEDSTEP_SAVE_SIZE) == 0);
    check("restored_timeline_continues", loaded.total_steps == src.total_steps &&
                                          src.total_steps > 0);
    check("restored_step_exact", same_double(loaded.step, src.step));
    check("restored_acc_exact", same_double(loaded.acc, src.acc));
    check("restored_alpha_exact", same_double(loaded.alpha, src.alpha));
    check("restored_dropped", loaded.dropped == src.dropped);
    check("restored_config", loaded.max_catchup == src.max_catchup);

    /* Save at many points; each resumed clock must match the uninterrupted
       clock bit-for-bit over the next 10 seconds. */
    Clock cont;
    clock_init(&cont, 1.0 / 240.0, 8);
    int points_ok = 1;
    for (int seg = 0; seg < 4; seg++) {
        for (int i = 0; i < 50 + seg * 13; i++) clock_advance(&cont, 0.016);
        if (clock_save(&cont, buf, sizeof buf) != FIXEDSTEP_SAVE_SIZE) {
            points_ok = 0;
            break;
        }
        Clock resumed;
        memset(&resumed, 0, sizeof resumed);
        if (clock_load(&resumed, buf, FIXEDSTEP_SAVE_SIZE) != 0) {
            points_ok = 0;
            break;
        }
        if (resumed.total_steps != cont.total_steps) points_ok = 0;
        if (!same_double(resumed.step, cont.step)) points_ok = 0;
        if (!same_double(resumed.acc, cont.acc)) points_ok = 0;
        if (!same_double(resumed.alpha, cont.alpha)) points_ok = 0;
        if (!replay_ten_seconds(&cont, &resumed)) points_ok = 0;
    }
    check("resume_matches_uninterrupted_10s", points_ok);

    /* A checkpoint taken after catch-up drops must also resume exactly. */
    Clock loaded_cont;
    clock_init(&loaded_cont, 0.01, 4);
    (void)clock_advance(&loaded_cont, 0.1); /* drops 6 steps */
    check("save_capture_drops", clock_save(&loaded_cont, buf, sizeof buf) == FIXEDSTEP_SAVE_SIZE);
    Clock loaded_resumed;
    memset(&loaded_resumed, 0, sizeof loaded_resumed);
    check("load_after_drops", clock_load(&loaded_resumed, buf, FIXEDSTEP_SAVE_SIZE) == 0 &&
                              loaded_resumed.dropped == 6 &&
                              loaded_resumed.total_steps == 4);
    check("resume_after_drops_matches", replay_ten_seconds(&loaded_cont, &loaded_resumed));

    /* ---- corrupt saves are rejected ----------------------------------- */

    unsigned char bad[FIXEDSTEP_SAVE_SIZE];
    Clock victim;
    clock_init(&victim, 0.01, 8);
    clock_advance(&victim, 0.05);
    clock_save(&victim, bad, sizeof bad);

    Clock keep;
    memcpy(&keep, &victim, sizeof keep);

    check("reject_short", clock_load(&victim, bad, FIXEDSTEP_SAVE_SIZE - 1) == -1);
    check("reject_long", clock_load(&victim, bad, FIXEDSTEP_SAVE_SIZE + 1) == -1);
    check("reject_null", clock_load(&victim, NULL, FIXEDSTEP_SAVE_SIZE) == -1);

    clock_save(&victim, bad, sizeof bad);
    bad[FS_OFF_ACC] ^= 0x01u; /* payload bit flip, stale checksum */
    check("reject_bitflip", clock_load(&victim, bad, FIXEDSTEP_SAVE_SIZE) == -1);

    clock_save(&victim, bad, sizeof bad);
    bad[FS_OFF_MAGIC] ^= 0xFFu;
    reseal(bad);
    check("reject_magic", clock_load(&victim, bad, FIXEDSTEP_SAVE_SIZE) == -1);

    clock_save(&victim, bad, sizeof bad);
    put_u32(bad + FS_OFF_VERSION, FIXEDSTEP_SAVE_VERSION + 1u);
    reseal(bad);
    check("reject_version", clock_load(&victim, bad, FIXEDSTEP_SAVE_SIZE) == -1);

    clock_save(&victim, bad, sizeof bad);
    put_u32(bad + FS_OFF_RESERVED, 1u);
    reseal(bad);
    check("reject_reserved", clock_load(&victim, bad, FIXEDSTEP_SAVE_SIZE) == -1);

    clock_save(&victim, bad, sizeof bad);
    put_u32(bad + FS_OFF_MAXCATCHUP, 0u);
    reseal(bad);
    check("reject_zero_catchup", clock_load(&victim, bad, FIXEDSTEP_SAVE_SIZE) == -1);

    clock_save(&victim, bad, sizeof bad);
    put_double_bits(bad, FS_OFF_STEP, 0x7FF8000000000000ULL); /* NaN */
    reseal(bad);
    check("reject_nan_step", clock_load(&victim, bad, FIXEDSTEP_SAVE_SIZE) == -1);

    clock_save(&victim, bad, sizeof bad);
    put_double_bits(bad, FS_OFF_STEP, 0x7FF0000000000000ULL); /* +inf */
    reseal(bad);
    check("reject_inf_step", clock_load(&victim, bad, FIXEDSTEP_SAVE_SIZE) == -1);

    clock_save(&victim, bad, sizeof bad);
    put_double_bits(bad, FS_OFF_STEP, double_bits(0.0));
    reseal(bad);
    check("reject_zero_step", clock_load(&victim, bad, FIXEDSTEP_SAVE_SIZE) == -1);

    clock_save(&victim, bad, sizeof bad);
    put_double_bits(bad, FS_OFF_ACC, double_bits(-0.5));
    reseal(bad);
    check("reject_negative_acc", clock_load(&victim, bad, FIXEDSTEP_SAVE_SIZE) == -1);

    clock_save(&victim, bad, sizeof bad);
    put_double_bits(bad, FS_OFF_ACC, double_bits(victim.step)); /* acc == step */
    reseal(bad);
    check("reject_acc_at_step", clock_load(&victim, bad, FIXEDSTEP_SAVE_SIZE) == -1);

    clock_save(&victim, bad, sizeof bad);
    put_double_bits(bad, FS_OFF_ACC, 0x7FF8000000000000ULL); /* NaN acc */
    reseal(bad);
    check("reject_nan_acc", clock_load(&victim, bad, FIXEDSTEP_SAVE_SIZE) == -1);

    clock_save(&victim, bad, sizeof bad);
    put_u64(bad + FS_OFF_STEPS, 0x8000000000000000ULL); /* negative as i64 */
    reseal(bad);
    check("reject_negative_steps", clock_load(&victim, bad, FIXEDSTEP_SAVE_SIZE) == -1);

    clock_save(&victim, bad, sizeof bad);
    put_u32(bad + FS_OFF_DROPPED, 0x80000000u); /* overflows int */
    reseal(bad);
    check("reject_dropped_overflow", clock_load(&victim, bad, FIXEDSTEP_SAVE_SIZE) == -1);

    check("rejected_load_leaves_state_untouched",
          victim.total_steps == keep.total_steps &&
          same_double(victim.acc, keep.acc) &&
          same_double(victim.alpha, keep.alpha) &&
          victim.dropped == keep.dropped &&
          same_double(victim.step, keep.step) &&
          victim.max_catchup == keep.max_catchup);

    printf("all_ok=%d\n", failures == 0);
    return failures == 0 ? 0 : 1;
}

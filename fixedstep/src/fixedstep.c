
/* fixedstep: fixed timestep accumulator with interpolation alpha and
   catch-up cap.
   In shared mode each parallel item feeds a private Clock a deterministic
   frame-dt stream derived from the shared detctx RNG; every output bit is a
   pure function of (seed, index). */
#include <stdio.h>
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "../../detctx/detctx.h"

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

/* ---- shared deterministic context workload -------------------------------- */

#define FS_FRAMES 32

typedef struct {
    long long total_steps;
    int dropped;
    int frame_steps_sum;
    uint64_t alpha_bits;
    uint64_t trace;
} FsSlot;

static uint64_t fold_u32(uint64_t h, uint32_t v) {
    h ^= v;
    h *= 0x100000001b3ULL;
    return h;
}

/* Pure function of (ctx, item). Frame dts are integer-derived doubles, so the
   arithmetic is bit-reproducible on every IEEE-754 implementation. */
static void fs_compute(const DetCtx* ctx, int item, FsSlot* s) {
    memset(s, 0, sizeof(*s));
    Clock c;
    clock_init(&c, 0.01, 8);

    uint64_t st = detctx_item_seed(ctx, item);
    s->trace = 0xcbf29ce484222325ULL;
    int frame_sum = 0;

    for (int f = 0; f < FS_FRAMES; f++) {
        uint32_t milli = detctx_range(&st, 30001);   /* 0..30000 */
        double frame_dt = (double)milli / 100000.0;  /* 0.0 .. 0.30000 */
        int n = clock_advance(&c, frame_dt);
        frame_sum += n;
        s->trace = fold_u32(s->trace, (uint32_t)n);
    }

    s->total_steps = c.total_steps;
    s->dropped = c.dropped;
    s->frame_steps_sum = frame_sum;
    memcpy(&s->alpha_bits, &c.alpha, sizeof(double));
}

static DetCtx g_ctx;

static void fs_task(int item, void* arg) {
    FsSlot* slots = (FsSlot*)arg;
    fs_compute(&g_ctx, item, &slots[item]);
}

static void run_shared(const DetCtx* ctx) {
    printf("shared=on seed=%llu threads=%d items=%d frames=%d\n",
           (unsigned long long)ctx->seed, ctx->threads,
           DETCTX_ITEMS, FS_FRAMES);

    FsSlot* slots = calloc(DETCTX_ITEMS, sizeof(FsSlot));
    FsSlot* ref = calloc(DETCTX_ITEMS, sizeof(FsSlot));
    if (!slots || !ref) { fprintf(stderr, "error: out of memory\n"); exit(2); }

    g_ctx = *ctx;
    if (detctx_parallel_for(ctx, DETCTX_ITEMS, fs_task, slots) != 0) exit(2);
    for (int i = 0; i < DETCTX_ITEMS; i++) fs_compute(ctx, i, &ref[i]);

    int mismatch = 0, accounting_bad = 0;
    for (int i = 0; i < DETCTX_ITEMS; i++) {
        if (memcmp(&slots[i], &ref[i], sizeof(FsSlot)) != 0) mismatch = 1;
        /* clock_advance's per-frame return values sum to total_steps;
           dropped backlog never appears as an executed step */
        if (slots[i].frame_steps_sum != (int)slots[i].total_steps ||
            slots[i].total_steps < 0) {
            accounting_bad = 1;
        }
        double alpha;
        memcpy(&alpha, &slots[i].alpha_bits, sizeof(double));
        printf("item=%d total_steps=%lld dropped=%d frame_sum=%d alpha=%a\n",
               i, slots[i].total_steps, slots[i].dropped,
               slots[i].frame_steps_sum, alpha);
    }
    uint64_t sum = detctx_checksum(slots, (size_t)DETCTX_ITEMS * sizeof(FsSlot));
    printf("parallel_matches_sequential=%d\n", mismatch == 0);
    printf("step_accounting=%d\n", accounting_bad == 0);
    printf("checksum=%016llx\n", (unsigned long long)sum);

    free(slots);
    free(ref);
    if (mismatch || accounting_bad) failures++;
}

/* ---- legacy self-test, byte-for-byte the original behaviour --------------- */

static void run_legacy_checks(void) {
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
}

int main(int argc, char** argv) {
    DetCtx ctx;
    int pr = detctx_parse(&ctx, argc, argv);
    if (pr < 0) return 2;
    if (pr > 0) return 0;

    if (ctx.shared) run_shared(&ctx);
    run_legacy_checks();

    printf("all_ok=%d\n", failures == 0);
    return failures == 0 ? 0 : 1;
}

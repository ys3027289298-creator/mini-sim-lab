/* timeslice: cross-frame time slicing for a three-stage render pipeline.
 *
 * Culling, shadowing and post-processing are split into tile-sized work
 * units.  A frame is given a cost budget; whole units run until the next
 * one would exceed the budget, then the pipeline yields.  The resume
 * point is (stage, index) plus the intermediate buffers, all of which
 * live in Pipeline, so an interrupted run continues where it stopped.
 * Units are atomic: a unit is never torn apart, which keeps every
 * interrupt point a consistent state.  The final image is bit-identical
 * to an unbudgeted single-frame run regardless of how it was sliced.
 */
#include <stdio.h>
#include <string.h>
#include <stdint.h>

#define IMG_W 48
#define IMG_H 32
#define TILE 4
#define TILES_X (IMG_W / TILE)
#define TILES_Y (IMG_H / TILE)
#define NTILES (TILES_X * TILES_Y)
#define NPIX (IMG_W * IMG_H)
#define PRIMS_PER_TILE 8
#define NSTAGES 3

enum { STAGE_CULL = 0, STAGE_SHADOW = 1, STAGE_POST = 2 };

typedef struct {
    uint32_t depth;
    uint32_t flags;         /* bit 0: back-facing */
    uint32_t intensity;
} Prim;

typedef struct {
    Prim prims[NTILES][PRIMS_PER_TILE];
} Scene;

typedef struct {
    Scene scene;            /* immutable input */
    uint8_t vis[NTILES];    /* cull output: visible primitive mask per tile */
    uint32_t lit[NPIX];     /* shadow output: lit colour per pixel */
    uint32_t out[NPIX];     /* post output: final image */
    int stage;              /* next stage to run */
    int index;              /* next tile within that stage */
    long long units_done;
    long long slices;       /* frames this pipeline has been stepped */
    long long cost_total;   /* simulated frame time, summed over slices */
} Pipeline;

typedef struct {
    int spent;              /* cost consumed in this frame */
    int units;              /* work units completed in this frame */
    int yielded_over;       /* stopped because the next unit did not fit */
    int completed;          /* pipeline finished inside this frame */
} FrameStats;

/* ---- deterministic scene and per-unit work ---------------------------- */

static uint32_t hash32(uint32_t x) {
    x ^= x >> 16; x *= 0x7feb352du;
    x ^= x >> 15; x *= 0x846ca68bu;
    x ^= x >> 16;
    return x;
}

static void scene_init(Scene* s, uint32_t seed) {
    for (int t = 0; t < NTILES; t++) {
        for (int k = 0; k < PRIMS_PER_TILE; k++) {
            uint32_t h = hash32(seed ^ ((uint32_t)t * 0x9E3779B9u)
                                     ^ ((uint32_t)k * 0x85EBCA6Bu));
            Prim* pr = &s->prims[t][k];
            pr->depth = h & 255u;
            pr->flags = (h >> 8) & 1u;
            pr->intensity = 32u + ((h >> 9) & 223u);
        }
    }
}

static void do_cull(Pipeline* p, int tile) {
    unsigned mask = 0;
    for (int k = 0; k < PRIMS_PER_TILE; k++) {
        const Prim* pr = &p->scene.prims[tile][k];
        if (!(pr->flags & 1u) && pr->depth < 240u) mask |= 1u << k;
    }
    p->vis[tile] = (uint8_t)mask;
}

static int clamp8(int v) {
    if (v < 0) return 0;
    if (v > 255) return 255;
    return v;
}

static uint32_t pack_rgb(int r, int g, int b) {
    return ((uint32_t)clamp8(r) << 16) | ((uint32_t)clamp8(g) << 8) |
           (uint32_t)clamp8(b);
}

static void do_shadow(Pipeline* p, int tile) {
    int tx = tile % TILES_X, ty = tile / TILES_X;
    unsigned mask = p->vis[tile];
    for (int yy = 0; yy < TILE; yy++) {
        for (int xx = 0; xx < TILE; xx++) {
            int x = tx * TILE + xx, y = ty * TILE + yy;
            int r = 16 + ((x * 7 + y * 13) & 15);
            int g = 16 + ((x * 11 + y * 5) & 15);
            int b = 16 + ((x * 3 + y * 17) & 15);
            for (int k = 0; k < PRIMS_PER_TILE; k++) {
                if (!(mask & (1u << k))) continue;
                const Prim* pr = &p->scene.prims[tile][k];
                int i = (int)pr->intensity;
                r += (i * (32 + ((pr->depth + (unsigned)x * 3u) & 63u))) >> 8;
                g += (i * (24 + ((pr->depth + (unsigned)y * 5u) & 63u))) >> 8;
                b += (i * (16 + ((pr->depth + (unsigned)(x + y) * 7u) & 63u))) >> 8;
            }
            p->lit[y * IMG_W + x] = pack_rgb(r, g, b);
        }
    }
}

static uint32_t lit_at(const Pipeline* p, int x, int y) {
    if (x < 0) x = 0;
    if (x >= IMG_W) x = IMG_W - 1;
    if (y < 0) y = 0;
    if (y >= IMG_H) y = IMG_H - 1;
    return p->lit[y * IMG_W + x];
}

static void do_post(Pipeline* p, int tile) {
    int tx = tile % TILES_X, ty = tile / TILES_X;
    for (int yy = 0; yy < TILE; yy++) {
        for (int xx = 0; xx < TILE; xx++) {
            int x = tx * TILE + xx, y = ty * TILE + yy;
            int sr = 0, sg = 0, sb = 0;
            for (int dy = -1; dy <= 1; dy++) {
                for (int dx = -1; dx <= 1; dx++) {
                    uint32_t c = lit_at(p, x + dx, y + dy);
                    sr += (int)((c >> 16) & 255u);
                    sg += (int)((c >> 8) & 255u);
                    sb += (int)(c & 255u);
                }
            }
            sr /= 9; sg /= 9; sb /= 9;
            /* tone map: keeps the post stage a genuinely separate pass */
            sr = sr * 255 / (sr + 64);
            sg = sg * 255 / (sg + 64);
            sb = sb * 255 / (sb + 64);
            p->out[y * IMG_W + x] = pack_rgb(sr, sg, sb);
        }
    }
}

static int popcount8(unsigned v) {
    int n = 0;
    while (v) { n += (int)(v & 1u); v >>= 1; }
    return n;
}

/* Simulated frame cost of one unit.  Deterministic, and known before the
 * unit starts so the slicer can stop without tearing it. */
static int unit_cost(const Pipeline* p, int stage, int index) {
    switch (stage) {
        case STAGE_CULL:   return 3;
        case STAGE_SHADOW: return 3 + 5 * popcount8(p->vis[index]);
        case STAGE_POST:   return 8 + (index & 3);
    }
    return 0;
}

static void run_unit(Pipeline* p) {
    int t = p->index;
    switch (p->stage) {
        case STAGE_CULL:   do_cull(p, t); break;
        case STAGE_SHADOW: do_shadow(p, t); break;
        case STAGE_POST:   do_post(p, t); break;
        default: return;
    }
    p->units_done++;
    if (++p->index >= NTILES) {
        p->index = 0;
        p->stage++;         /* stage barrier: next stage starts clean */
    }
}

/* ---- slicing scheduler ------------------------------------------------ */

static void pipeline_init(Pipeline* p, uint32_t seed) {
    memset(p, 0, sizeof(*p));
    scene_init(&p->scene, seed);
    p->stage = STAGE_CULL;
}

/* Run one budgeted slice.  The first unit of a frame always runs even if
 * it alone exceeds the budget, so a tiny budget still makes progress. */
static FrameStats pipeline_step(Pipeline* p, int budget) {
    FrameStats fs = { 0, 0, 0, 0 };
    while (p->stage < NSTAGES) {
        int c = unit_cost(p, p->stage, p->index);
        if (fs.units > 0 && fs.spent + c > budget) {
            fs.yielded_over = 1;
            break;
        }
        run_unit(p);
        fs.spent += c;
        fs.units++;
    }
    fs.completed = (p->stage >= NSTAGES);
    p->slices++;
    p->cost_total += fs.spent;
    return fs;
}

/* One unbudgeted frame: the reference path. */
static void pipeline_run_all(Pipeline* p) {
    while (p->stage < NSTAGES) run_unit(p);
}

typedef struct {
    int frames;
    long long total_cost;
    int max_frame;
    int over_budget_frames;
    int yield_frames;
    int stage_crossings;    /* frames that crossed a stage barrier */
} RunReport;

static RunReport run_budgeted(Pipeline* p, int budget, int verbose) {
    RunReport r = { 0, 0, 0, 0, 0, 0 };
    while (p->stage < NSTAGES) {
        int stage_before = p->stage;
        FrameStats fs = pipeline_step(p, budget);
        r.frames++;
        r.total_cost += fs.spent;
        if (fs.spent > r.max_frame) r.max_frame = fs.spent;
        if (fs.spent > budget) r.over_budget_frames++;
        if (fs.yielded_over) r.yield_frames++;
        if (p->stage != stage_before) r.stage_crossings++;
        if (verbose) {
            printf("frame=%02d spent=%3d/%3d units=%2d stage=%d next=%2d%s\n",
                   r.frames, fs.spent, budget, fs.units, p->stage, p->index,
                   fs.completed ? " done" : "");
        }
    }
    return r;
}

static int pixel_diff(const uint32_t* a, const uint32_t* b, int n) {
    int d = 0;
    for (int i = 0; i < n; i++) if (a[i] != b[i]) d++;
    return d;
}

/* ---- self-test -------------------------------------------------------- */

static int failures = 0;
static void check(const char* name, int ok) {
    printf("%s=%d\n", name, ok ? 1 : 0);
    if (!ok) failures++;
}

#define SEED 0xC0FFEEu

int main(void) {
    /* reference: everything in one frame */
    Pipeline ref;
    pipeline_init(&ref, SEED);
    pipeline_run_all(&ref);
    check("reference_complete",
          ref.stage == NSTAGES && ref.units_done == (long long)NSTAGES * NTILES);

    int lit_written = 1, out_written = 1;
    for (int i = 0; i < NPIX; i++) {
        if (ref.lit[i] == 0) lit_written = 0;
        if (ref.out[i] == 0) out_written = 0;
    }
    check("barrier_lit_fully_written", lit_written);
    check("output_fully_written", out_written);

    /* budgeted cross-frame run */
    const int BUDGET = 160;
    Pipeline sl;
    pipeline_init(&sl, SEED);
    printf("-- budgeted run, budget=%d cost units --\n", BUDGET);
    RunReport rep = run_budgeted(&sl, BUDGET, 1);
    printf("slices=%d total_cost=%lld avg_cost=%lld max_frame=%d over_budget=%d "
           "yields=%d stage_crossings=%d\n",
           rep.frames, rep.total_cost,
           rep.frames ? rep.total_cost / rep.frames : 0,
           rep.max_frame, rep.over_budget_frames, rep.yield_frames,
           rep.stage_crossings);
    printf("pixel_diff=%d\n", pixel_diff(ref.out, sl.out, NPIX));

    check("sliced_complete",
          sl.stage == NSTAGES && sl.units_done == (long long)NSTAGES * NTILES);
    check("sliced_multi_frame", rep.frames > 1);
    check("pixels_match_single_frame", memcmp(ref.out, sl.out, sizeof ref.out) == 0);
    check("lit_matches_single_frame", memcmp(ref.lit, sl.lit, sizeof ref.lit) == 0);
    check("cull_matches_single_frame", memcmp(ref.vis, sl.vis, sizeof ref.vis) == 0);
    check("no_pixel_diff", pixel_diff(ref.out, sl.out, NPIX) == 0);
    check("budget_cap_respected", rep.over_budget_frames == 0);
    check("yielded_before_overrun", rep.yield_frames > 0);
    check("stage_barrier_crossed", rep.stage_crossings > 0);
    check("slice_count_reported", sl.slices == (long long)rep.frames);
    check("cost_accounting", sl.cost_total == rep.total_cost);

    /* any budget must yield the same pixels */
    const int budgets[] = { 1, 7, 64, 1000 };
    int invariant = 1;
    long long frames_at_budget[4];
    for (int i = 0; i < 4; i++) {
        Pipeline v;
        pipeline_init(&v, SEED);
        RunReport r = run_budgeted(&v, budgets[i], 0);
        frames_at_budget[i] = r.frames;
        if (v.stage != NSTAGES) invariant = 0;
        if (memcmp(v.out, ref.out, sizeof ref.out) != 0) invariant = 0;
        if (memcmp(v.lit, ref.lit, sizeof ref.lit) != 0) invariant = 0;
    }
    check("any_budget_same_pixels", invariant);
    check("smaller_budget_more_slices",
          frames_at_budget[0] > frames_at_budget[1] &&
          frames_at_budget[1] > frames_at_budget[2] &&
          frames_at_budget[2] > frames_at_budget[3]);

    /* interrupt mid-run, snapshot the whole pipeline, resume both copies */
    Pipeline a;
    pipeline_init(&a, SEED);
    for (int i = 0; i < 5; i++) (void)pipeline_step(&a, 91);
    check("interrupted_mid_run",
          a.units_done > 0 && a.units_done < (long long)NSTAGES * NTILES &&
          a.stage < NSTAGES);
    int saved_stage = a.stage, saved_index = a.index;
    long long saved_units = a.units_done, saved_slices = a.slices;
    Pipeline b = a;     /* resume point is plain state: copy it */
    check("snapshot_keeps_position",
          b.stage == saved_stage && b.index == saved_index &&
          b.units_done == saved_units && b.slices == saved_slices);
    RunReport ra = run_budgeted(&a, 91, 0);
    RunReport rb = run_budgeted(&b, 91, 0);
    check("resume_completes",
          a.stage == NSTAGES && b.stage == NSTAGES && ra.frames == rb.frames);
    check("resume_matches_reference",
          memcmp(a.out, ref.out, sizeof ref.out) == 0 &&
          memcmp(b.out, ref.out, sizeof ref.out) == 0);
    check("resume_keeps_total_work",
          a.units_done == (long long)NSTAGES * NTILES &&
          a.cost_total == sl.cost_total);
    check("resume_continues_not_restarts",
          a.slices == saved_slices + (long long)ra.frames);

    /* a budget smaller than one unit still advances and yields at once */
    Pipeline tiny;
    pipeline_init(&tiny, SEED);
    FrameStats f0 = pipeline_step(&tiny, 1);
    check("tiny_budget_one_unit", f0.units == 1 && tiny.units_done == 1);
    check("tiny_budget_atomic_overrun", f0.spent == 3 && f0.spent > 1);
    check("tiny_budget_yields_immediately", f0.yielded_over == 1 && !f0.completed);
    FrameStats f1 = pipeline_step(&tiny, 1);
    check("tiny_budget_next_slice_progresses",
          f1.units == 1 && tiny.units_done == 2 && tiny.index == 2);
    Pipeline tiny_done = tiny;
    RunReport rt = run_budgeted(&tiny_done, 1, 0);
    check("tiny_budget_finishes",
          tiny_done.stage == NSTAGES &&
          memcmp(tiny_done.out, ref.out, sizeof ref.out) == 0 && rt.frames > 0);

    /* zero budget: liveness is guaranteed by one unit per slice */
    Pipeline z;
    pipeline_init(&z, SEED);
    FrameStats z0 = pipeline_step(&z, 0);
    check("zero_budget_still_progresses",
          z0.units == 1 && z0.spent == 3 && z0.yielded_over == 1);

    printf("all_ok=%d\n", failures == 0);
    return failures == 0 ? 0 : 1;
}

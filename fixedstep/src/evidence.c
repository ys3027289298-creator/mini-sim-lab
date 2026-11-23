/* fixedstep evidence driver.
 *
 * Emits a per-family digest of the clock state after a deterministic frame
 * schedule, a thread-independence table, and the parameters needed to replay
 * the run.  Same inputs => identical stdout, byte for byte.
 *
 * Canonical state image (40 bytes, little-endian): step, acc, alpha, dropped,
 * max_catchup, total_steps.  The image, not the struct, is hashed, so padding
 * bytes can never make two runs differ.
 */
#include "cli.h"
#include "detcontext.h"
#include "evidence.h"
#include "fixedstep.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define UNIT "fixedstep"
#define IMAGE_BYTES 40u

typedef struct {
    FstepClock clk;
    double schedule_scale;
} Sim;

static void encode(const Sim* s, unsigned char* out) {
    det_store_double(out + 0, s->clk.step);
    det_store_double(out + 8, s->clk.acc);
    det_store_double(out + 16, s->clk.alpha);
    det_store_u32(out + 24, (uint32_t)s->clk.dropped);
    det_store_u32(out + 28, (uint32_t)s->clk.max_catchup);
    det_store_u64(out + 32, (uint64_t)s->clk.total_steps);
}

static void decode(const unsigned char* in, Sim* s) {
    s->clk.step = det_load_double(in + 0);
    s->clk.acc = det_load_double(in + 8);
    s->clk.alpha = det_load_double(in + 16);
    s->clk.dropped = (int)(int32_t)det_load_u32(in + 24);
    s->clk.max_catchup = (int)(int32_t)det_load_u32(in + 28);
    s->clk.total_steps = (long long)det_load_u64(in + 32);
}

static uint64_t state_digest(const unsigned char* image) {
    return det_hash_image(image, IMAGE_BYTES);
}

/* Periodic deltas plus a backlog spike every 37th frame.  Pure integer index
 * arithmetic, so the schedule is identical in every run. */
static double frame_dt(int frame, double scale) {
    if (frame > 0 && frame % 37 == 0) return 0.120 * scale;
    return (0.004 + (double)(frame % 7) * 0.001) * scale;
}

static void run_frames(Sim* s, int frames) {
    for (int f = 0; f < frames; f++) {
        int steps = fstep_advance(&s->clk, frame_dt(f, s->schedule_scale));
        if (steps < 0) det_cli_die(UNIT, "schedule", "frame %d rejected", f);
    }
}

/* ---- thread independence ------------------------------------------------- */

typedef struct {
    long long total_steps;
    long long dropped;
    uint64_t hash;
} TileResult;

typedef struct {
    double step;
    int catchup;
    int frames_per_tile;
    int ntiles;
    double scale;
    TileResult* out;
} TileJob;

/* Each tile owns its clock; only out[job] is written, by the job that owns
 * it, so the folded digest is independent of thread scheduling. */
static void tile_fn(void* arg, int job) {
    const TileJob* tj = (const TileJob*)arg;
    FstepClock c;
    fstep_init(&c, tj->step, tj->catchup);
    DetHash h;
    det_hash_init(&h);
    det_hash_u64(&h, (uint64_t)job);
    for (int f = 0; f < tj->frames_per_tile; f++) {
        int idx = (job * 131 + f * 7) % 29;
        double dt = (0.002 + (double)idx * 0.001) * tj->scale;
        int steps = fstep_advance(&c, dt);
        det_hash_i64(&h, steps);
        det_hash_u64(&h, (uint64_t)(c.dropped));
        det_hash_double(&h, c.acc);
        det_hash_double(&h, c.alpha);
    }
    tj->out[job].total_steps = c.total_steps;
    tj->out[job].dropped = c.dropped;
    tj->out[job].hash = det_hash_value(&h);
}

static uint64_t tile_table(int ntiles, double step, int catchup,
                           int frames_per_tile, double scale, int nthreads) {
    TileJob tj;
    tj.step = step;
    tj.catchup = catchup;
    tj.frames_per_tile = frames_per_tile;
    tj.ntiles = ntiles;
    tj.scale = scale;
    tj.out = (TileResult*)calloc((size_t)ntiles, sizeof(TileResult));
    if (!tj.out) det_cli_die(UNIT, "tiles", "out of memory");

    char err[256];
    err[0] = '\0';
    if (det_parallel_for(nthreads, ntiles, tile_fn, &tj, UNIT, err, sizeof(err)) != 0) {
        free(tj.out);
        det_cli_fail(err[0] ? err : UNIT ": parallel: failed");
    }

    DetHash h;
    det_hash_init(&h);
    det_hash_u64(&h, (uint64_t)ntiles);
    for (int i = 0; i < ntiles; i++) {   /* fold in index order */
        det_hash_i64(&h, tj.out[i].total_steps);
        det_hash_i64(&h, tj.out[i].dropped);
        det_hash_u64(&h, tj.out[i].hash);
    }
    free(tj.out);
    return det_hash_value(&h);
}

/* ---- main ---------------------------------------------------------------- */

static const char* const MODOPTS[] = {
    "--frames", "--warmup", "--advance", "--step", "--catchup", "--tiles",
    "--tile-frames", "--threads", NULL
};

int main(int argc, char** argv) {
    DetCli cli = det_cli_common(UNIT, argc, argv, MODOPTS);

    long frames = 240;
    long warmup = 0;
    long advance = 0;
    int advance_given = 0;
    long catchup = 4;
    long tiles = 64;
    long tile_frames = 24;
    double step = 0.01;
    int counts[DET_MAX_THREADS];
    int ncounts = 0;
    const char* thread_spec = "1,2,4,8";

    for (int i = 1; i < argc; i++) {
        const char* a = argv[i];
        if (strcmp(a, "--frames") == 0)
            frames = det_cli_long(UNIT, a, det_cli_value(UNIT, a, argc, argv, &i), 1, 1000000);
        else if (strcmp(a, "--warmup") == 0)
            warmup = det_cli_long(UNIT, a, det_cli_value(UNIT, a, argc, argv, &i), 0, 1000000);
        else if (strcmp(a, "--advance") == 0) {
            advance = det_cli_long(UNIT, a, det_cli_value(UNIT, a, argc, argv, &i), 1, 1000000);
            advance_given = 1;
        }
        else if (strcmp(a, "--step") == 0) {
            step = det_cli_double(UNIT, a, det_cli_value(UNIT, a, argc, argv, &i));
            if (!(step > 0.0) || step > 10.0)
                det_cli_die(UNIT, a, "must be in (0, 10], got %g", step);
        } else if (strcmp(a, "--catchup") == 0)
            catchup = det_cli_long(UNIT, a, det_cli_value(UNIT, a, argc, argv, &i), 1, 10000);
        else if (strcmp(a, "--tiles") == 0)
            tiles = det_cli_long(UNIT, a, det_cli_value(UNIT, a, argc, argv, &i), 1, 100000);
        else if (strcmp(a, "--tile-frames") == 0)
            tile_frames = det_cli_long(UNIT, a, det_cli_value(UNIT, a, argc, argv, &i), 1, 100000);
        else if (strcmp(a, "--threads") == 0)
            thread_spec = det_cli_value(UNIT, a, argc, argv, &i);
    }

    /* Validate every parameter before producing any output, so an invalid
     * argument fails cleanly instead of after a partial report. */
    ncounts = det_cli_threads(UNIT, thread_spec, counts, DET_MAX_THREADS);

    DetSession sess;
    det_session_open(&sess, UNIT, &cli, IMAGE_BYTES);

    Sim sim;
    memset(&sim, 0, sizeof(sim));
    if (cli.load) {
        decode((const unsigned char*)sess.state, &sim);
        /* schedule scale is a pure function of the seed the file carried */
        sim.schedule_scale = 1.0 + (double)(det_context_seed(sess.ctx) % 17) / 1000.0;
    } else {
        fstep_init(&sim.clk, step, (int)catchup);
        sim.schedule_scale = 1.0 + (double)(det_context_seed(sess.ctx) % 17) / 1000.0;
    }

    if (cli.load) {
        /* Restored state is already past its warmup; only an explicit
         * --advance moves it, so --load alone reproduces the saved digest. */
        if (advance_given) run_frames(&sim, (int)advance);
    } else {
        if (advance_given)
            det_cli_die(UNIT, "--advance", "only meaningful together with --load");
        if (warmup > 0) run_frames(&sim, (int)warmup);
        run_frames(&sim, (int)frames);
    }
    encode(&sim, (unsigned char*)sess.state);

    uint64_t state_hash = state_digest((const unsigned char*)sess.state);

    printf("%s.seed=%016llx\n", UNIT, (unsigned long long)det_context_seed(sess.ctx));
    printf("%s.step=%.17g\n", UNIT, sim.clk.step);
    printf("%s.frames=%ld\n", UNIT, cli.load ? advance : frames);
    printf("%s.total_steps=%lld\n", UNIT, sim.clk.total_steps);
    printf("%s.dropped=%d\n", UNIT, sim.clk.dropped);
    printf("%s.state_hash=%016llx\n", UNIT, (unsigned long long)state_hash);

    uint64_t first = 0;
    int table_equal = 1;
    for (int i = 0; i < ncounts; i++) {
        uint64_t d = tile_table((int)tiles, step, (int)catchup, (int)tile_frames,
                                sim.schedule_scale, counts[i]);
        if (i == 0) first = d;
        else if (d != first) table_equal = 0;
        printf("%s.threads.%d.digest=%016llx\n", UNIT, counts[i],
               (unsigned long long)d);
    }
    printf("%s.table_equal=%d\n", UNIT, table_equal);
    printf("%s.table_hash=%016llx\n", UNIT, (unsigned long long)first);
    if (!table_equal)
        det_cli_die(UNIT, "threads", "digest depends on thread count");

    det_session_finish(&sess, &cli, state_hash, 0);
    det_session_close(&sess);
    return 0;
}

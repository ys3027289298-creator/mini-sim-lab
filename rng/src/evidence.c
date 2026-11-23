/* rng evidence driver.
 *
 * Emits a canonical digest of the stream state after a deterministic draw
 * schedule, a thread-independence table (independent streams per job), and
 * the parameters needed to replay the run.  Same inputs => identical stdout.
 *
 * Canonical state image (8 bytes): the uint64 stream state.  Snapshot/restore
 * is exercised explicitly, so a digest mismatch means the stream itself
 * changed, not the bookkeeping around it.
 */
#include "cli.h"
#include "detcontext.h"
#include "evidence.h"
#include "rng.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define UNIT "rng"
#define IMAGE_BYTES 8u

static void encode(const Stream* s, unsigned char* out) { det_store_u64(out, s->state); }
static void decode(const unsigned char* in, Stream* s) { s->state = det_load_u64(in); }

/* Deterministic draw schedule: a fixed mix of next(), range() and fork() so
 * the digest covers all three primitives. */
static void run_draws(Stream* s, int draws, uint64_t tag) {
    for (int i = 0; i < draws; i++) {
        if (i % 11 == 10) {
            Stream f = stream_fork(s, tag + (uint64_t)i);
            (void)stream_next(&f);
        } else if (i % 3 == 0) {
            (void)stream_range(s, (uint32_t)(2 + (i % 13)));
        } else {
            (void)stream_next(s);
        }
    }
}

static uint64_t draw_digest(uint64_t seed, int draws, uint64_t tag) {
    Stream s;
    stream_init(&s, seed);
    DetHash h;
    det_hash_init(&h);
    det_hash_u64(&h, seed);
    Stream baseline = s;
    run_draws(&s, draws, tag);
    /* Snapshot mid-run must reproduce the remaining draws exactly. */
    Stream snap = s;
    uint64_t a = stream_next(&s);
    uint64_t b = stream_next(&snap);
    det_hash_u64(&h, s.state);
    det_hash_u64(&h, a);
    det_hash_u64(&h, b);
    det_hash_u64(&h, baseline.state);
    return det_hash_value(&h);
}

/* ---- thread independence ------------------------------------------------- */

typedef struct {
    uint64_t seed;
    int draws;
    int ntiles;
    uint64_t* out;
} TileJob;

static void tile_fn(void* arg, int job) {
    const TileJob* tj = (const TileJob*)arg;
    /* Each job has its own stream and writes only out[job]; recombined below
     * in index order, so thread count cannot affect the digest. */
    tj->out[job] = draw_digest(tj->seed + (uint64_t)job * 0x9E3779B97F4A7C15ULL,
                               tj->draws, (uint64_t)job);
}

static uint64_t tile_table(uint64_t seed, int draws, int ntiles, int nthreads) {
    TileJob tj = { seed, draws, ntiles, calloc((size_t)ntiles, sizeof(uint64_t)) };
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
    for (int i = 0; i < ntiles; i++) det_hash_u64(&h, tj.out[i]);
    free(tj.out);
    return det_hash_value(&h);
}

static const char* const MODOPTS[] = {
    "--draws", "--advance", "--tiles", "--threads", NULL
};

int main(int argc, char** argv) {
    DetCli cli = det_cli_common(UNIT, argc, argv, MODOPTS);

    long draws = 500;
    long advance = 0;
    int advance_given = 0;
    long tiles = 64;
    int counts[DET_MAX_THREADS];
    int ncounts = 0;
    const char* thread_spec = "1,2,4,8";
    uint64_t tag = 0x5EED;

    for (int i = 1; i < argc; i++) {
        const char* a = argv[i];
        if (strcmp(a, "--draws") == 0)
            draws = det_cli_long(UNIT, a, det_cli_value(UNIT, a, argc, argv, &i), 1, 1000000);
        else if (strcmp(a, "--advance") == 0) {
            advance = det_cli_long(UNIT, a, det_cli_value(UNIT, a, argc, argv, &i), 1, 1000000);
            advance_given = 1;
        }
        else if (strcmp(a, "--tiles") == 0)
            tiles = det_cli_long(UNIT, a, det_cli_value(UNIT, a, argc, argv, &i), 1, 100000);
        else if (strcmp(a, "--threads") == 0)
            thread_spec = det_cli_value(UNIT, a, argc, argv, &i);
    }

    /* Validate every parameter before producing any output. */
    ncounts = det_cli_threads(UNIT, thread_spec, counts, DET_MAX_THREADS);

    DetSession sess;
    det_session_open(&sess, UNIT, &cli, IMAGE_BYTES);

    /* Module stream seed is the context seed mixed with the module name;
     * report the context seed so all three drivers print the same value. */
    uint64_t seed = det_context_seed(sess.ctx);
    Stream s;
    if (cli.load) {
        decode((const unsigned char*)sess.state, &s);
        if (advance_given) run_draws(&s, (int)advance, tag);
    } else {
        if (advance_given)
            det_cli_die(UNIT, "--advance", "only meaningful together with --load");
        stream_init(&s, det_unit_seed(sess.ctx, UNIT));
        run_draws(&s, (int)draws, tag);
    }
    encode(&s, (unsigned char*)sess.state);

    uint64_t state_hash = det_hash_image((const unsigned char*)sess.state, IMAGE_BYTES);

    printf("%s.seed=%016llx\n", UNIT, (unsigned long long)seed);
    printf("%s.draws=%ld\n", UNIT, cli.load ? advance : draws);
    printf("%s.state_hash=%016llx\n", UNIT, (unsigned long long)state_hash);

    uint64_t first = 0;
    int table_equal = 1;
    for (int i = 0; i < ncounts; i++) {
        uint64_t d = tile_table(seed, (int)draws, (int)tiles, counts[i]);
        if (i == 0) first = d;
        else if (d != first) table_equal = 0;
        printf("%s.threads.%d.digest=%016llx\n", UNIT, counts[i], (unsigned long long)d);
    }
    printf("%s.table_equal=%d\n", UNIT, table_equal);
    printf("%s.table_hash=%016llx\n", UNIT, (unsigned long long)first);
    if (!table_equal)
        det_cli_die(UNIT, "threads", "digest depends on thread count");

    det_session_finish(&sess, &cli, state_hash, 0);
    det_session_close(&sess);
    return 0;
}

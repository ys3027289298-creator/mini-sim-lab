/* pool evidence driver.
 *
 * Emits a canonical digest of the pool state after a deterministic operation
 * script, a thread-independence table (one independent pool per job), and the
 * parameters needed to replay the run.  Same inputs => identical stdout.
 *
 * Canonical state image (216 bytes, little-endian): for each of POOL_CAP
 * slots, in_use(u32), gen(u32), value(i32); then used(u32), high_water(u32),
 * checksum of outstanding handles (u64).  Live struct layout never reaches
 * the digest.
 */
#include "cli.h"
#include "detcontext.h"
#include "evidence.h"
#include "pool.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define UNIT "pool"
#define IMAGE_BYTES (POOL_CAP * 12u + 16u)

/* Outstanding handle digest: proves stale-handle rejection is stable, not
 * just that the slot bookkeeping matches. */
static uint64_t handle_digest(const Pool* p) {
    DetHash h;
    det_hash_init(&h);
    for (int i = 0; i < POOL_CAP; i++) {
        if (p->in_use[i]) {
            det_hash_u32(&h, (uint32_t)i);
            det_hash_u32(&h, p->gen[i]);
            det_hash_i64(&h, p->value[i]);
        }
    }
    return det_hash_value(&h);
}

static void encode(const Pool* p, unsigned char* out) {
    size_t off = 0;
    for (int i = 0; i < POOL_CAP; i++) {
        det_store_u32(out + off, (uint32_t)p->in_use[i]); off += 4;
        det_store_u32(out + off, p->gen[i]); off += 4;
        det_store_u32(out + off, (uint32_t)p->value[i]); off += 4;
    }
    det_store_u32(out + off, (uint32_t)p->used); off += 4;
    det_store_u32(out + off, (uint32_t)p->high_water); off += 4;
    det_store_u64(out + off, handle_digest(p)); off += 8;
}

static void decode(const unsigned char* in, Pool* p) {
    pool_init(p);
    size_t off = 0;
    for (int i = 0; i < POOL_CAP; i++) {
        p->in_use[i] = (int)(int32_t)det_load_u32(in + off); off += 4;
        p->gen[i] = det_load_u32(in + off); off += 4;
        p->value[i] = (int)(int32_t)det_load_u32(in + off); off += 4;
    }
    p->used = (int)(int32_t)det_load_u32(in + off); off += 4;
    p->high_water = (int)(int32_t)det_load_u32(in + off); off += 4;
    /* handle_digest is derived; not stored back into the pool. */
}

/* Deterministic op script.  op index -> small integer decisions, so no
 * randomness and no address-dependent ordering enters the result. */
static void run_script(Pool* p, int ops, int salt) {
    Handle live[POOL_CAP];
    int nlive = 0;
    for (int i = 0; i < POOL_CAP; i++) { live[i].index = (unsigned)-1; live[i].gen = 0; }
    for (int op = 0; op < ops; op++) {
        int mix = (op * 2654435761u) >> 16;
        int action = mix % 10;
        if (action < 6) {
            int v = (op * 7 + salt) % 1000;
            Handle h = pool_alloc(p, v);
            if (h.index != (unsigned)-1 && nlive < POOL_CAP) live[nlive++] = h;
        } else if (action < 9 && nlive > 0) {
            int which = (mix >> 8) % nlive;
            pool_free(p, live[which]);
            live[which] = live[--nlive];   /* order preserved by index, not address */
        } else {
            /* probe a stale or live handle deterministically */
            int out = 0;
            if (nlive > 0) {
                Handle h = live[(mix >> 4) % nlive];
                (void)pool_get(p, h, &out);
            }
            Handle stale = { (unsigned)((mix >> 12) % POOL_CAP), (unsigned)(mix & 0xff) };
            (void)pool_get(p, stale, &out);
        }
    }
}

static uint64_t script_digest(uint64_t seed, int ops, int salt) {
    Pool p;
    pool_init(&p);
    run_script(&p, ops, salt);
    unsigned char img[IMAGE_BYTES];
    encode(&p, img);
    DetHash h;
    det_hash_init(&h);
    det_hash_u64(&h, seed);
    det_hash_bytes(&h, img, sizeof(img));
    return det_hash_value(&h);
}

/* ---- thread independence ------------------------------------------------- */

typedef struct {
    uint64_t seed;
    int ops;
    int ntiles;
    uint64_t* out;
} TileJob;

static void tile_fn(void* arg, int job) {
    const TileJob* tj = (const TileJob*)arg;
    /* Each job gets its own pool and salt, so no shared mutable state exists
     * between jobs; the fold below recombines them in index order. */
    tj->out[job] = script_digest(tj->seed, tj->ops, job * 7919);
}

static uint64_t tile_table(uint64_t seed, int ops, int ntiles, int nthreads) {
    TileJob tj = { seed, ops, ntiles, calloc((size_t)ntiles, sizeof(uint64_t)) };
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
    "--ops", "--advance", "--tiles", "--threads", "--salt", NULL
};

int main(int argc, char** argv) {
    DetCli cli = det_cli_common(UNIT, argc, argv, MODOPTS);

    long ops = 400;
    long advance = 0;
    int advance_given = 0;
    long tiles = 64;
    long salt = 0;
    int counts[DET_MAX_THREADS];
    int ncounts = 0;
    const char* thread_spec = "1,2,4,8";

    for (int i = 1; i < argc; i++) {
        const char* a = argv[i];
        if (strcmp(a, "--ops") == 0)
            ops = det_cli_long(UNIT, a, det_cli_value(UNIT, a, argc, argv, &i), 1, 1000000);
        else if (strcmp(a, "--advance") == 0) {
            advance = det_cli_long(UNIT, a, det_cli_value(UNIT, a, argc, argv, &i), 1, 1000000);
            advance_given = 1;
        }
        else if (strcmp(a, "--tiles") == 0)
            tiles = det_cli_long(UNIT, a, det_cli_value(UNIT, a, argc, argv, &i), 1, 100000);
        else if (strcmp(a, "--salt") == 0)
            salt = det_cli_long(UNIT, a, det_cli_value(UNIT, a, argc, argv, &i), 0, 1000000);
        else if (strcmp(a, "--threads") == 0)
            thread_spec = det_cli_value(UNIT, a, argc, argv, &i);
    }

    /* Validate every parameter before producing any output. */
    ncounts = det_cli_threads(UNIT, thread_spec, counts, DET_MAX_THREADS);

    DetSession sess;
    det_session_open(&sess, UNIT, &cli, IMAGE_BYTES);

    Pool p;
    if (cli.load) {
        decode((const unsigned char*)sess.state, &p);
        if (advance_given) run_script(&p, (int)advance, (int)salt);
    } else {
        if (advance_given)
            det_cli_die(UNIT, "--advance", "only meaningful together with --load");
        pool_init(&p);
        run_script(&p, (int)ops, (int)salt);
    }
    encode(&p, (unsigned char*)sess.state);

    uint64_t state_hash = det_hash_image((const unsigned char*)sess.state, IMAGE_BYTES);

    printf("%s.seed=%016llx\n", UNIT, (unsigned long long)det_context_seed(sess.ctx));
    printf("%s.ops=%ld\n", UNIT, cli.load ? advance : ops);
    printf("%s.salt=%ld\n", UNIT, salt);
    printf("%s.used=%d\n", UNIT, p.used);
    printf("%s.high_water=%d\n", UNIT, p.high_water);
    printf("%s.state_hash=%016llx\n", UNIT, (unsigned long long)state_hash);

    uint64_t first = 0;
    int table_equal = 1;
    for (int i = 0; i < ncounts; i++) {
        uint64_t d = tile_table(det_context_seed(sess.ctx), (int)ops, (int)tiles, counts[i]);
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


/* simlab: integration of the three modules across shards and threads.
 *
 * Determinism design (output is independent of thread count and scheduling):
 *   - Work is partitioned into NSHARD fixed shards, addressed by shard index.
 *   - Each shard owns private Clock, Pool and RNG state. The RNG stream is
 *     stream_fork()ed from a common base using the shard index as tag, so the
 *     random sequence consumed by a shard depends only on its index, never on
 *     which thread runs it or when.
 *   - Threads merely execute shards; results are merged in shard-index order,
 *     so scheduling cannot reorder the output.
 *   - No clock, wall-time, pointer address, thread id or other ambient value
 *     reaches the output; all values derive from seed + shard index.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>

#include "fixedstep.h"
#include "pool.h"
#include "rng.h"

#define NSHARD 8
#define BASE_SEED 0x5EED1234ULL
#define MAX_FRAMES 6
#define MAX_TMP 4

/* Frame wall-dt schedule: includes a frame that trips the catch-up cap. */
static const double frame_dt[MAX_FRAMES] = {
    0.01, 0.025, 0.005, 0.015, 0.1, 0.02
};

typedef struct {
    int sid;
    int total_steps;
    int dropped;
    int high_water;
    int dangling_caught;
    int stale_value_caught;
    uint64_t checksum;
} ShardResult;

typedef struct {
    ShardResult* results;   /* one slot per shard, written by index */
    int nthread;
    int next;               /* ticket counter for work stealing */
    pthread_mutex_t ticket_lock;
} Work;

static void run_shard(int sid, ShardResult* out) {
    Clock clock_;
    clock_init(&clock_, 0.01, 4);

    Pool pool_;
    pool_init(&pool_);

    Stream base;
    stream_init(&base, BASE_SEED);
    Stream rng_ = stream_fork(&base, (uint64_t)sid + 1ULL);

    int dangling_caught = 0;
    int stale_value_caught = 0;
    uint64_t checksum = 0x123456789ABCDEF0ULL ^ (uint64_t)sid;

    for (int f = 0; f < MAX_FRAMES; f++) {
        int ticks = clock_advance(&clock_, frame_dt[f]);

        Handle tmp[MAX_TMP];
        int ntmp = 0;
        for (int t = 0; t < ticks; t++) {
            /* One persistent entity per tick while capacity allows. */
            int value = (int)stream_range(&rng_, 1000);
            Handle h = pool_alloc(&pool_, value);
            if (pool_valid(&pool_, h)) {
                checksum ^= (uint64_t)value * 0x9E3779B97F4A7C15ULL
                          + (uint64_t)(f + 1) * 31ULL + (uint64_t)(t + 1);
            }

            /* Short-lived allocations, freed before the frame ends. */
            int k = (int)(stream_range(&rng_, 3) + 1); /* 1..3 */
            for (int j = 0; j < k && ntmp < MAX_TMP; j++) {
                Handle th = pool_alloc(&pool_, (int)stream_range(&rng_, 100));
                tmp[ntmp++] = th;
            }
        }
        for (int j = 0; j < ntmp; j++) {
            if (pool_valid(&pool_, tmp[j])) pool_free(&pool_, tmp[j]);
        }

        /* A deliberately stale handle must always be rejected. */
        Handle fake = { (unsigned)(sid % POOL_CAP), 0U };
        int got = 0;
        if (pool_get(&pool_, fake, &got) == -1) dangling_caught++;
        if (!pool_valid(&pool_, fake)) stale_value_caught++;

        checksum = checksum * 1099511628211ULL
                 ^ ((uint64_t)(unsigned)ticks << 1)
                 ^ ((uint64_t)(unsigned)clock_.dropped << 7)
                 ^ (uint64_t)(unsigned)pool_.used;
    }

    out->sid = sid;
    out->total_steps = (int)clock_.total_steps;
    out->dropped = clock_.dropped;
    out->high_water = pool_.high_water;
    out->dangling_caught = dangling_caught;
    out->stale_value_caught = stale_value_caught;
    out->checksum = checksum;
}

static void* worker(void* arg) {
    Work* w = (Work*)arg;
    for (;;) {
        pthread_mutex_lock(&w->ticket_lock);
        int sid = w->next++;
        pthread_mutex_unlock(&w->ticket_lock);
        if (sid >= NSHARD) break;
        run_shard(sid, &w->results[sid]); /* write slot fixed by shard index */
    }
    return NULL;
}

int main(int argc, char** argv) {
    int nthread = 1;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-t") == 0 && i + 1 < argc) {
            nthread = atoi(argv[++i]);
        }
    }
    if (nthread < 1) nthread = 1;
    if (nthread > NSHARD) nthread = NSHARD;

    ShardResult results[NSHARD];
    memset(results, 0, sizeof(results));

    Work w;
    w.results = results;
    w.nthread = nthread;
    w.next = 0;
    pthread_mutex_init(&w.ticket_lock, NULL);

    pthread_t* th = (pthread_t*)calloc((size_t)nthread, sizeof(pthread_t));
    for (int i = 0; i < nthread; i++) {
        if (pthread_create(&th[i], NULL, worker, &w) != 0) {
            fprintf(stderr, "pthread_create failed\n");
            return 2;
        }
    }
    for (int i = 0; i < nthread; i++) pthread_join(th[i], NULL);
    pthread_mutex_destroy(&w.ticket_lock);
    free(th);

    /* Merge strictly by shard index: thread scheduling never orders output. */
    int all_ok = 1;
    for (int sid = 0; sid < NSHARD; sid++) {
        ShardResult* r = &results[sid];
        printf("shard=%d steps=%d dropped=%d high_water=%d "
               "dangling_caught=%d stale_caught=%d checksum=%016llx\n",
               r->sid, r->total_steps, r->dropped, r->high_water,
               r->dangling_caught, r->stale_value_caught,
               (unsigned long long)r->checksum);
        if (r->sid != sid) all_ok = 0;
        if (r->dangling_caught != MAX_FRAMES) all_ok = 0;
        if (r->stale_value_caught != MAX_FRAMES) all_ok = 0;
        if (r->high_water <= 0 || r->high_water > POOL_CAP) all_ok = 0;
    }
    /* Thread count deliberately stays out of the output: it selects how
       shards are executed, not what the shards compute. */
    printf("shards=%d all_ok=%d\n", NSHARD, all_ok);
    return all_ok ? 0 : 1;
}

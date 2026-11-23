
/* sim: deterministic replay harness for mini-sim-lab.
 *
 * A 100Hz fixed-step simulation of up to POOL_CAP entities driven by the
 * fixedstep clock, the generation-handle pool and the splitmix RNG.
 *
 * Two builds:
 *   sim          deterministic build (ns fixed-point clock, per-entity
 *                RNG substreams keyed by stable spawn id, static slot
 *                partitions). Output is independent of thread count and
 *                of how frame dt values are grouped.
 *   sim-legacy   -DSIM_LEGACY -DFIXEDSTEP_LEGACY: pre-fix behaviour.
 *                Double/fmod clock plus a shared RNG stream and pool
 *                mutations reached from worker threads in atomic-claim
 *                order, so replay trajectories depend on grouping and
 *                scheduling. Kept only to reproduce the reported drift.
 *
 * Usage: sim <threads> <merge:0|1> <out.rec> [seed]
 *
 * The frame dt list is generated from the seed and is identical for
 * every run. merge=1 feeds the same timestamps pairwise summed, i.e.
 * the same simulation time presented in half as many frames.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <math.h>
#include <pthread.h>
#include <sched.h>

#include "fixedstep.h"
#include "pool.h"
#include "rng.h"

#define SIM_STEP       0.01   /* 100 Hz physics */
#define MAX_FRAMES     300
#define MAX_STEPS      (MAX_FRAMES * 8)            /* hard cap = catch-up limit */
#define MAX_RECORDS    (MAX_STEPS * POOL_CAP)
#define MAX_THREADS    32
#define REC_MAGIC      0x52544c4d54534552ULL /* "RETSMLTR" */

typedef struct {
    Handle  handle;
    int     alive;
    uint32_t spawn_id;
    double  x, y, vx, vy;
    Stream  rng;
    int     dead;
} Entity;

#pragma pack(push, 1)
typedef struct {
    uint64_t magic;
    uint32_t version;     /* 1 = fixed build, 0 = legacy build */
    uint32_t threads;
    uint32_t merge;
    uint32_t seed;
    uint32_t pool_cap;
    uint64_t frames;
    uint64_t total_steps;
    uint64_t spawns;
    uint64_t despawns;
    uint64_t dropped;
    uint64_t n_records;
    uint64_t payload_bytes;
} RecHeader;

typedef struct {
    uint32_t slot;
    uint32_t gen;
    uint32_t spawn_id;
    double   x, y, vx, vy;
} EntityRec;

typedef struct {
    uint64_t step;
    uint32_t n;
} StepRec;
#pragma pack(pop)

typedef struct Sim Sim;
typedef struct { Sim* sim; int wid; } WorkerArg;

struct Sim {
    Pool pool;
    Entity ent[POOL_CAP];
    Clock clock;
    Stream spawn_rng;
    uint32_t next_spawn_id;
    uint64_t spawns, despawns, total_steps;
    int threads;

    /* staging: dts are quantised to integer nanoseconds at generation
       time, before any merging, so frame grouping cannot change them */
    long long frame_dts_ns[MAX_FRAMES];
    int n_frames;
    StepRec steps[MAX_STEPS];
    EntityRec recs[MAX_RECORDS];
    uint64_t n_records;
    uint64_t cur_step;
    int despawn_queue[POOL_CAP];
    int n_despawn;

    /* worker control: generation-based barrier, so a slow worker can
       never consume a later round's wake-up or miss its own round */
    WorkerArg warg[MAX_THREADS];
    pthread_t workers[MAX_THREADS];
    pthread_mutex_t mtx;
    pthread_cond_t cv_work;
    pthread_cond_t cv_done;
    unsigned gen;         /* rounds issued */
    unsigned done;        /* workers that finished the current gen */
    int stop;
#ifdef SIM_LEGACY
    Stream shared_rng;
    uint32_t claim;
#endif
};

/* ---------------- frame dt generation (shared by both builds) ----------- */
static void make_frame_dts(Sim* sim, uint32_t seed) {
    Stream s;
    stream_init(&s, 0xD1CE ^ ((uint64_t)seed << 32));
    for (int i = 0; i < MAX_FRAMES; i++) {
        /* ~60 Hz frames with occasional hitches that exercise the
           catch-up/drop path; values deliberately not exactly binary
           representable. Cast to signed BEFORE subtracting, otherwise
           the unsigned wraparound turns negative jitter into +42949s */
        double dt = 0.016 + (double)((int32_t)stream_range(&s, 1601) - 800) * 1e-5;
        if (stream_range(&s, 40) == 0) dt += 0.10;
        /* quantise once, here; everything downstream is integer */
        sim->frame_dts_ns[i] = (long long)llround(dt * 1e9);
    }
    sim->n_frames = MAX_FRAMES;
}

/* ---------------- entity helpers ----------------------------------------- */
static void spawn_entities(Sim* sim, int n_new) {
    for (int k = 0; k < n_new; k++) {
        Handle h = pool_alloc(&sim->pool, (int)sim->next_spawn_id);
        if (h.index == (unsigned)-1) return;       /* pool full */
        Entity* e = &sim->ent[h.index];
        e->handle = h;
        e->alive = 1;
        e->dead = 0;
        e->spawn_id = sim->next_spawn_id++;
        e->x = (double)(e->spawn_id % 10);
        e->y = (double)((e->spawn_id * 7) % 10);
        e->vx = 0.05;
        e->vy = -0.03;
        /* private substream keyed by the stable spawn id, never by slot */
        Stream base;
        stream_init(&base, 0x5EED0000ULL);
        e->rng = stream_fork(&base, (uint64_t)e->spawn_id + 1);
        sim->spawns++;
    }
}

static void snapshot_step(Sim* sim) {
    /* one StepRec per executed step, steps[s].n counts the entity rows
       that immediately follow it in the output stream */
    uint64_t s = sim->total_steps - 1;
    if (s >= MAX_STEPS) return;
    StepRec* sr = &sim->steps[s];
    sr->step = sim->cur_step;
    sr->n = 0;
    for (int i = 0; i < POOL_CAP; i++) {
        Entity* e = &sim->ent[i];
        if (!e->alive || !pool_valid(&sim->pool, e->handle)) continue;
        if (sim->n_records >= MAX_RECORDS) break;
        EntityRec* r = &sim->recs[sim->n_records];
        r->slot = (uint32_t)i;
        r->gen = e->handle.gen;
        r->spawn_id = e->spawn_id;
        r->x = e->x;
        r->y = e->y;
        r->vx = e->vx;
        r->vy = e->vy;
        sr->n++;
        sim->n_records++;
    }
}

/* ---------------- per-step update ----------------------------------------- */
#ifndef SIM_LEGACY
static void update_entity_fixed(Sim* sim, int slot) {
    Entity* e = &sim->ent[slot];
    if (!e->alive || !pool_valid(&sim->pool, e->handle)) return;

    double jx = (double)((int32_t)stream_range(&e->rng, 2001) - 1000) * 1e-9;
    double jy = (double)((int32_t)stream_range(&e->rng, 2001) - 1000) * 1e-9;
    e->x += e->vx * SIM_STEP + jx;
    e->y += e->vy * SIM_STEP + jy;
    e->vx += jx * 0.5;
    e->vy += jy * 0.5;
    if (e->x >= 10.0) e->x -= 10.0;
    if (e->y < 0.0)  e->y += 10.0;

    if (stream_range(&e->rng, 100) < 5) e->dead = 1;
}
#endif

#ifdef SIM_LEGACY
static void update_entity_legacy(Sim* sim, int slot) {
    Entity* e = &sim->ent[slot];
    if (!e->alive || !pool_valid(&sim->pool, e->handle)) return;

    /* one shared RNG and pool mutation under a single mutex, reached in
       atomic-claim order: the draw sequence depends on scheduling */
    pthread_mutex_lock(&sim->mtx);
    uint32_t r1 = stream_range(&sim->shared_rng, 2001);
    uint32_t r2 = stream_range(&sim->shared_rng, 2001);
    uint32_t kill = stream_range(&sim->shared_rng, 100);
    if (kill < 5) {
        pool_free(&sim->pool, e->handle);
        e->alive = 0;
        sim->despawns++;
    }
    pthread_mutex_unlock(&sim->mtx);

    double jx = (double)((int32_t)r1 - 1000) * 1e-9;
    double jy = (double)((int32_t)r2 - 1000) * 1e-9;
    e->x += e->vx * SIM_STEP + jx;
    e->y += e->vy * SIM_STEP + jy;
    e->vx += jx * 0.5;
    e->vy += jy * 0.5;
    if (e->x >= 10.0) e->x -= 10.0;
    if (e->y < 0.0)  e->y += 10.0;
}
#endif

static void* worker_main(void* arg) {
    WorkerArg* a = arg;
    Sim* sim = a->sim;
    int wid = a->wid;
#ifdef SIM_LEGACY
    (void)wid;
#endif
    unsigned my_gen = 0;
    pthread_mutex_lock(&sim->mtx);
    for (;;) {
        /* wait until a new round is issued (or shutdown) */
        while (sim->gen == my_gen && !sim->stop)
            pthread_cond_wait(&sim->cv_work, &sim->mtx);
        if (sim->stop) { pthread_mutex_unlock(&sim->mtx); return NULL; }
        my_gen = sim->gen;
        unsigned round = my_gen;
        pthread_mutex_unlock(&sim->mtx);

#ifdef SIM_LEGACY
        for (;;) {
            uint32_t slot = __atomic_fetch_add(&sim->claim, 1, __ATOMIC_SEQ_CST);
            if (slot >= POOL_CAP) break;
            update_entity_legacy(sim, (int)slot);
            /* widen the scheduling window so claim order varies across
               runs and thread counts; the fixed build never does this */
            if ((slot & 1) == 0) sched_yield();
        }
#else
        /* static, contiguous partition: slot i is always owned by the
           same worker id, and each entity only touches its own state */
        int lo = (int)((long long)wid * POOL_CAP / sim->threads);
        int hi = (int)((long long)(wid + 1) * POOL_CAP / sim->threads);
        for (int slot = lo; slot < hi; slot++) update_entity_fixed(sim, slot);
#endif

        pthread_mutex_lock(&sim->mtx);
        sim->done++;
        if (sim->done == (unsigned)sim->threads) pthread_cond_signal(&sim->cv_done);
        (void)round;
    }
}

static void run_step(Sim* sim) {
    sim->cur_step++;
    sim->total_steps++;

    /* spawn decisions are made on the driver thread before workers run,
       from a stream nobody else touches */
    if (stream_range(&sim->spawn_rng, 100) < 25) {
        spawn_entities(sim, 1);
    }

    if (sim->threads <= 1) {
#ifdef SIM_LEGACY
        sim->claim = 0;
        for (int slot = 0; slot < POOL_CAP; slot++) update_entity_legacy(sim, slot);
#else
        for (int slot = 0; slot < POOL_CAP; slot++) update_entity_fixed(sim, slot);
#endif
    } else {
        pthread_mutex_lock(&sim->mtx);
        sim->done = 0;
        sim->gen++;
#ifdef SIM_LEGACY
        sim->claim = 0;
#endif
        pthread_cond_broadcast(&sim->cv_work);
        while (sim->done < (unsigned)sim->threads)
            pthread_cond_wait(&sim->cv_done, &sim->mtx);
        pthread_mutex_unlock(&sim->mtx);
    }

#ifndef SIM_LEGACY
    /* staged frees applied in stable slot order on the driver thread */
    sim->n_despawn = 0;
    for (int i = 0; i < POOL_CAP; i++)
        if (sim->ent[i].alive && sim->ent[i].dead)
            sim->despawn_queue[sim->n_despawn++] = i;
    for (int k = 0; k < sim->n_despawn; k++) {
        int slot = sim->despawn_queue[k];
        Entity* e = &sim->ent[slot];
        if (pool_free(&sim->pool, e->handle) == 0) {
            e->alive = 0;
            e->dead = 0;
            sim->despawns++;
        }
    }
#endif

    snapshot_step(sim);
}

/* ---------------- output -------------------------------------------------- */
static uint64_t fnv1a(const uint8_t* p, size_t n) {
    uint64_t h = 1469598103934665603ULL;
    for (size_t i = 0; i < n; i++) { h ^= p[i]; h *= 1099511628211ULL; }
    return h;
}

static int write_output(const Sim* sim, const char* path, int merge, uint32_t seed) {
    FILE* f = fopen(path, "wb");
    if (!f) { perror("fopen"); return 1; }

    /* rebuild step headers interleaved with entity rows in one buffer so
       the on-disk layout itself is hashable end to end */
    static uint8_t buf[sizeof(RecHeader) +
                       MAX_STEPS * sizeof(StepRec) +
                       MAX_RECORDS * sizeof(EntityRec)];
    size_t off = 0;

    RecHeader hdr;
    memset(&hdr, 0, sizeof(hdr));
    hdr.magic = REC_MAGIC;
#ifdef SIM_LEGACY
    hdr.version = 0;
#else
    hdr.version = 1;
#endif
    hdr.threads = (uint32_t)sim->threads;
    hdr.merge = (uint32_t)merge;
    hdr.seed = seed;
    hdr.pool_cap = POOL_CAP;
    hdr.frames = (uint64_t)(merge ? sim->n_frames / 2 : sim->n_frames);
    hdr.total_steps = sim->total_steps;
    hdr.spawns = sim->spawns;
    hdr.despawns = sim->despawns;
    hdr.dropped = (uint64_t)sim->clock.dropped;
    hdr.n_records = 0;
    hdr.payload_bytes = 0;

    memcpy(buf + off, &hdr, sizeof(hdr));
    off += sizeof(hdr);

    /* one StepRec per executed step, each followed by its entity rows */
    uint64_t row = 0;
    for (uint64_t s = 0; s < sim->total_steps; s++) {
        StepRec sr = sim->steps[s];
        memcpy(buf + off, &sr, sizeof(sr));
        off += sizeof(sr);
        for (uint32_t k = 0; k < sr.n; k++) {
            memcpy(buf + off, &sim->recs[row++], sizeof(EntityRec));
            off += sizeof(EntityRec);
        }
    }

    RecHeader* h = (RecHeader*)buf;
    h->n_records = row;
    h->payload_bytes = off - sizeof(RecHeader);
    uint64_t hash = fnv1a(buf, off);

    fwrite(buf, 1, off, f);
    fclose(f);

    printf("mode=%s threads=%d merge=%d seed=%u\n",
#ifdef SIM_LEGACY
           "legacy",
#else
           "fixed",
#endif
           sim->threads, merge, seed);
    printf("frames=%llu total_steps=%llu dropped=%llu spawns=%llu despawns=%llu\n",
           (unsigned long long)hdr.frames, (unsigned long long)sim->total_steps,
           (unsigned long long)sim->clock.dropped,
           (unsigned long long)sim->spawns, (unsigned long long)sim->despawns);
    printf("entity_rows=%llu bytes=%zu fnv1a=%016llx\n",
           (unsigned long long)row, off, (unsigned long long)hash);
    return 0;
}

int main(int argc, char** argv) {
    if (argc < 4) {
        fprintf(stderr, "usage: %s <threads> <merge:0|1> <out.rec> [seed]\n", argv[0]);
        return 2;
    }
    int threads = atoi(argv[1]);
    int merge = atoi(argv[2]);
    const char* out = argv[3];
    uint32_t seed = (uint32_t)(argc > 4 ? (unsigned)strtoul(argv[4], NULL, 10) : 1001u);
    if (threads < 1 || threads > 32 || (merge != 0 && merge != 1)) {
        fprintf(stderr, "bad arguments\n");
        return 2;
    }

    Sim* sim = calloc(1, sizeof(Sim));
    sim->threads = threads;
    pthread_mutex_init(&sim->mtx, NULL);
    pthread_cond_init(&sim->cv_work, NULL);
    pthread_cond_init(&sim->cv_done, NULL);

    clock_init(&sim->clock, SIM_STEP, 8);
    pool_init(&sim->pool);
    stream_init(&sim->spawn_rng, 0x5BA1 ^ ((uint64_t)seed << 16));
#ifdef SIM_LEGACY
    stream_init(&sim->shared_rng, 0x1234 ^ seed);
#endif
    make_frame_dts(sim, seed);

    if (threads > 1) {
        /* N workers cover partitions wid 0..N-1; the driver only
           orchestrates and must not also do partition 0's work */
        for (int t = 0; t < threads; t++) {
            sim->warg[t].sim = sim;
            sim->warg[t].wid = t;
            pthread_create(&sim->workers[t], NULL, worker_main, &sim->warg[t]);
        }
    }

#ifndef SIM_LEGACY
    long long budget = 0;   /* step allowance banked across original frames */
#endif
    for (int f = 0; f < sim->n_frames; f++) {
        long long dt_ns;
#ifdef SIM_LEGACY
        if (merge) {
            if (f % 2 != 0) continue;
            dt_ns = sim->frame_dts_ns[f] +
                    (f + 1 < sim->n_frames ? sim->frame_dts_ns[f + 1] : 0);
        } else {
            dt_ns = sim->frame_dts_ns[f];
        }
#else
        int orig_frames;
        if (merge) {
            if (f % 2 != 0) continue;
            /* integer addition of pre-quantised values: exactly the same
               total nanoseconds as the unmerged run */
            dt_ns = sim->frame_dts_ns[f] +
                    (f + 1 < sim->n_frames ? sim->frame_dts_ns[f + 1] : 0);
            orig_frames = (f + 1 < sim->n_frames) ? 2 : 1;
        } else {
            dt_ns = sim->frame_dts_ns[f];
            orig_frames = 1;
        }
#endif

#ifdef SIM_LEGACY
        /* original behaviour: cap inside the clock, per submitted frame */
        int run = clock_advance_ns(&sim->clock, dt_ns);
#else
        /* the cap accrues per ORIGINAL frame regardless of how the
           timestamps are submitted, so merged frames bank twice the
           allowance and can never lose work the unmerged run would do */
        budget += (long long)sim->clock.max_catchup * orig_frames;

        int available = clock_drain_ns(&sim->clock, dt_ns);
        int run = available;
        if ((long long)run > budget) {
            int skipped = run - (int)budget;
            run = (int)budget;
            clock_unstep(&sim->clock, skipped);
            sim->clock.dropped += skipped;
        }
        budget -= run;
#endif
        for (int k = 0; k < run; k++) run_step(sim);
    }

    if (threads > 1) {
        pthread_mutex_lock(&sim->mtx);
        sim->stop = 1;
        pthread_cond_broadcast(&sim->cv_work);
        pthread_mutex_unlock(&sim->mtx);
        for (int t = 0; t < threads; t++) pthread_join(sim->workers[t], NULL);
    }
    pthread_mutex_destroy(&sim->mtx);
    pthread_cond_destroy(&sim->cv_work);
    pthread_cond_destroy(&sim->cv_done);

    int rc = write_output(sim, out, merge, seed);
    free(sim);
    return rc;
}

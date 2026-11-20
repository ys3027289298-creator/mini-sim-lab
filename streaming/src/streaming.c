
/* streaming: layered chunk streaming for large levels. One-shot loading
   freezes the frame while the whole level reads from disk; streaming reads
   chunks in the background ordered by distance/occlusion priority, covers
   missing chunks with placeholder tiles, and diffs every frame against the
   one-shot reference pixel by pixel. With streaming off, behavior is
   bit-identical to the old one-shot path. */
#include <stdio.h>
#include <string.h>

#define MAX_CHUNKS   64
#define MAX_DEPS     2
#define VIEW_RADIUS  5     /* chunks this close to the camera are wanted */
#define TILE_PIXELS  4     /* each chunk renders as a 4-pixel tile */
#define FRAME_PIXELS ((VIEW_RADIUS * 2 + 1) * TILE_PIXELS)
#define IO_BUDGET    24    /* disk bytes read per tick (background) */
#define RESIDENT_CAP 256   /* max bytes of chunk data resident at once */
#define PLACEHOLDER  0xFF  /* flat tile shown while a chunk is missing */

typedef struct {
    int x;                 /* world slot on the camera path */
    int size;              /* bytes on disk == bytes in memory */
    int occluder;          /* hides chunks behind it from the camera */
    int deps[MAX_DEPS];    /* chunk ids that must be resident first */
    int ndeps;
} ChunkDef;

typedef enum { CH_ABSENT, CH_QUEUED, CH_LOADING, CH_RESIDENT, CH_FAILED } ChunkState;

typedef struct {
    const ChunkDef* def;
    int ndef;
    ChunkState st[MAX_CHUNKS];
    int pending[MAX_CHUNKS];   /* bytes left to read */
    int resident_bytes;
    int mem_peak_bytes;
    int resident_peak;
    int evictions;
    int err_missing_dep;
    int err_duplicate_load;
} World;

typedef struct {
    long total_diff_pixels;
    int  frames_with_diff;
    int  placeholder_frames;
    int  worst_frame_diff;
    int  final_frame_diff;
    int  stall_ticks;          /* ticks the frame is frozen on disk reads */
} RunStats;

static int iabs(int v) { return v < 0 ? -v : v; }

static unsigned char chunk_pixel(int id, int p) {
    unsigned h = (unsigned)id * 2654435761u ^ (unsigned)(p + 1) * 2246822519u;
    h ^= h >> 13; h *= 3266489917u; h ^= h >> 16;
    return (unsigned char)(h & 0xFF);
}

static void world_init(World* w, const ChunkDef* def, int ndef) {
    memset(w, 0, sizeof(*w));
    w->def = def;
    w->ndef = ndef;
}

/* Idempotent queue: validates the dependency manifest, silently skips
   chunks already tracked. A dep id outside the manifest is a hard error. */
static int ensure_load(World* w, int id) {
    if (id < 0 || id >= w->ndef) return -1;
    if (w->st[id] != CH_ABSENT) return 0;
    for (int k = 0; k < w->def[id].ndeps; k++) {
        int dep = w->def[id].deps[k];
        if (dep < 0 || dep >= w->ndef) {
            w->err_missing_dep++;
            w->st[id] = CH_FAILED;
            return -1;
        }
    }
    w->st[id] = CH_QUEUED;
    w->pending[id] = w->def[id].size;
    return 0;
}

/* Public load request: asking for a chunk already queued/loading/resident
   is a duplicate and gets reported. */
static int request_load(World* w, int id) {
    if (id < 0 || id >= w->ndef) return -1;
    if (w->st[id] != CH_ABSENT) {
        w->err_duplicate_load++;
        return -1;
    }
    return ensure_load(w, id);
}

static int occluded(const World* w, int cam, int id) {
    int x = w->def[id].x;
    for (int k = 0; k < w->ndef; k++) {
        if (!w->def[k].occluder) continue;
        int xk = w->def[k].x;
        if (x > cam && xk > cam && xk < x) return 1;
        if (x < cam && xk < cam && xk > x) return 1;
    }
    return 0;
}

/* Lower loads first: near beats far, occluded waits, deps jump the queue. */
static long priority(const World* w, const unsigned char* protect, int cam, int id) {
    long p = (long)iabs(w->def[id].x - cam) * 4;
    if (occluded(w, cam, id)) p += 8;
    if (protect[id]) p -= 2;
    return p;
}

static int deps_resident(const World* w, int id) {
    for (int k = 0; k < w->def[id].ndeps; k++)
        if (w->st[w->def[id].deps[k]] != CH_RESIDENT) return 0;
    return 1;
}

static void evict(World* w, int cam, const unsigned char* wanted,
                  const unsigned char* protect) {
    while (w->resident_bytes > RESIDENT_CAP) {
        int victim = -1, vd = -1;
        for (int id = 0; id < w->ndef; id++) {
            if (w->st[id] != CH_RESIDENT || wanted[id] || protect[id]) continue;
            int d = iabs(w->def[id].x - cam);
            if (d > vd) { vd = d; victim = id; }
        }
        if (victim < 0) break;   /* everything resident is still needed */
        w->st[victim] = CH_ABSENT;
        w->resident_bytes -= w->def[victim].size;
        w->evictions++;
    }
}

static void stream_tick(World* w, int cam) {
    unsigned char wanted[MAX_CHUNKS] = {0};
    unsigned char protect[MAX_CHUNKS] = {0};
    for (int id = 0; id < w->ndef; id++) {
        if (iabs(w->def[id].x - cam) > VIEW_RADIUS) continue;
        wanted[id] = 1;
        for (int k = 0; k < w->def[id].ndeps; k++) {
            int dep = w->def[id].deps[k];
            if (dep >= 0 && dep < w->ndef) protect[dep] = 1;
        }
    }

    /* queue wanted chunks and the deps they sit on */
    for (int id = 0; id < w->ndef; id++) {
        if (!wanted[id]) continue;
        for (int k = 0; k < w->def[id].ndeps; k++)
            ensure_load(w, w->def[id].deps[k]);
        ensure_load(w, id);
    }

    /* a chunk whose dep failed can never complete: fail it too, loudly */
    for (int id = 0; id < w->ndef; id++) {
        if (w->st[id] != CH_QUEUED && w->st[id] != CH_LOADING) continue;
        for (int k = 0; k < w->def[id].ndeps; k++) {
            if (w->st[w->def[id].deps[k]] == CH_FAILED) {
                w->st[id] = CH_FAILED;
                w->err_missing_dep++;
            }
        }
    }

    evict(w, cam, wanted, protect);   /* make room before reading more */

    /* spend this tick's disk budget on the best candidate */
    int budget = IO_BUDGET;
    for (;;) {
        int best = -1;
        long bestp = 0x7fffffff;
        for (int id = 0; id < w->ndef; id++) {
            if (w->st[id] != CH_QUEUED && w->st[id] != CH_LOADING) continue;
            if (!deps_resident(w, id)) continue;
            long p = priority(w, protect, cam, id);
            if (p < bestp) { bestp = p; best = id; }
        }
        if (best < 0 || budget <= 0) break;
        int n = w->pending[best] < budget ? w->pending[best] : budget;
        w->pending[best] -= n;
        budget -= n;
        w->st[best] = CH_LOADING;
        if (w->pending[best] == 0) {
            w->st[best] = CH_RESIDENT;
            w->resident_bytes += w->def[best].size;
        }
    }

    evict(w, cam, wanted, protect);

    /* high-water marks, sampled after eviction so the cap is honest */
    if (w->resident_bytes > w->mem_peak_bytes)
        w->mem_peak_bytes = w->resident_bytes;
    int rc = 0;
    for (int id = 0; id < w->ndef; id++)
        if (w->st[id] == CH_RESIDENT) rc++;
    if (rc > w->resident_peak) w->resident_peak = rc;
}

static void render(const World* w, int cam, unsigned char* frame) {
    memset(frame, 0, FRAME_PIXELS);
    int n = 0;
    for (int id = 0; id < w->ndef && n + TILE_PIXELS <= FRAME_PIXELS; id++) {
        if (iabs(w->def[id].x - cam) > VIEW_RADIUS) continue;
        for (int p = 0; p < TILE_PIXELS; p++)
            frame[n++] = (w->st[id] == CH_RESIDENT)
                       ? chunk_pixel(id, p) : PLACEHOLDER;
    }
}

static void render_ref(const ChunkDef* def, int ndef, int cam,
                       unsigned char* frame) {
    memset(frame, 0, FRAME_PIXELS);
    int n = 0;
    for (int id = 0; id < ndef && n + TILE_PIXELS <= FRAME_PIXELS; id++) {
        if (iabs(def[id].x - cam) > VIEW_RADIUS) continue;
        for (int p = 0; p < TILE_PIXELS; p++)
            frame[n++] = chunk_pixel(id, p);
    }
}

/* Run the camera across the level. streaming=0 reproduces the old one-shot
   behavior: whole level up front, frame frozen for the duration. */
static RunStats run_level(const ChunkDef* def, int ndef, int ticks,
                          int streaming, int trace, World* out) {
    World w;
    RunStats s;
    world_init(&w, def, ndef);
    memset(&s, 0, sizeof(s));

    if (!streaming) {
        int total = 0;
        for (int id = 0; id < ndef; id++) {
            w.st[id] = CH_RESIDENT;
            total += def[id].size;
        }
        w.resident_bytes = total;
        w.mem_peak_bytes = total;
        w.resident_peak = ndef;
        s.stall_ticks = (total + IO_BUDGET - 1) / IO_BUDGET;
    }

    for (int t = 0; t < ticks; t++) {
        int cam = t;
        if (streaming) stream_tick(&w, cam);

        unsigned char f[FRAME_PIXELS], r[FRAME_PIXELS];
        render(&w, cam, f);
        render_ref(def, ndef, cam, r);

        int diff = 0;
        for (int i = 0; i < FRAME_PIXELS; i++)
            if (f[i] != r[i]) diff++;

        int missing = 0;
        for (int id = 0; id < ndef; id++) {
            if (iabs(def[id].x - cam) <= VIEW_RADIUS &&
                w.st[id] != CH_RESIDENT) { missing = 1; break; }
        }

        if (missing) s.placeholder_frames++;
        if (diff) {
            s.frames_with_diff++;
            s.total_diff_pixels += diff;
            if (diff > s.worst_frame_diff) s.worst_frame_diff = diff;
            if (streaming && trace)
                printf("placeholder_frame tick=%d diff_pixels=%d\n", t, diff);
        }
        s.final_frame_diff = diff;
    }

    if (out) *out = w;
    return s;
}

static void report(const char* mode, const RunStats* s, const World* w) {
    printf("mode=%s stall_ticks=%d mem_peak_bytes=%d resident_peak=%d "
           "placeholder_frames=%d diff_pixels=%ld worst_frame_diff=%d "
           "evictions=%d err_missing_dep=%d err_duplicate_load=%d\n",
           mode, s->stall_ticks, w->mem_peak_bytes, w->resident_peak,
           s->placeholder_frames, s->total_diff_pixels, s->worst_frame_diff,
           w->evictions, w->err_missing_dep, w->err_duplicate_load);
}

static void build_level(ChunkDef* d, int n) {
    for (int i = 0; i < n; i++) {
        d[i].x = i;
        d[i].size = 8 + (i * 7) % 13;
        d[i].occluder = (i % 6 == 3);
        d[i].ndeps = 0;
        if (i > 0 && i % 4 == 2) { d[i].deps[0] = i - 1; d[i].ndeps = 1; }
    }
}

static int failures = 0;
static void check(const char* name, int ok) {
    printf("%s=%d\n", name, ok ? 1 : 0);
    if (!ok) failures++;
}

int main(void) {
    enum { N = 24 };
    static ChunkDef level[N];
    build_level(level, N);
    int total_bytes = 0;
    for (int i = 0; i < N; i++) total_bytes += level[i].size;

    World wo, ws, ws2;
    RunStats one = run_level(level, N, N, 0, 0, &wo);
    report("oneshot", &one, &wo);
    RunStats st = run_level(level, N, N, 1, 1, &ws);
    report("stream", &st, &ws);

    check("oneshot_stall", one.stall_ticks > 0);
    check("oneshot_zero_diff",
          one.total_diff_pixels == 0 && one.placeholder_frames == 0);
    check("oneshot_whole_level",
          wo.mem_peak_bytes == total_bytes && wo.resident_peak == N);

    check("stream_no_stall", st.stall_ticks == 0);
    check("stream_uses_placeholder", st.placeholder_frames > 0);
    check("stream_diff_reported",
          st.total_diff_pixels > 0 &&
          st.frames_with_diff <= st.placeholder_frames);
    check("stream_converges", st.final_frame_diff == 0);
    check("stream_mem_capped", ws.mem_peak_bytes <= RESIDENT_CAP);
    check("stream_bounds_resident", ws.resident_peak < N);
    check("stream_evicts", ws.evictions > 0);
    check("stream_no_errors",
          ws.err_missing_dep == 0 && ws.err_duplicate_load == 0);

    RunStats st2 = run_level(level, N, N, 1, 0, &ws2);
    check("deterministic",
          memcmp(&st, &st2, sizeof(st)) == 0 &&
          ws.mem_peak_bytes == ws2.mem_peak_bytes &&
          ws.resident_peak == ws2.resident_peak);

    /* missing dependency: chunk 0 needs id 9, which is not in the manifest */
    ChunkDef bad[2] = {
        { 0, 8, 0, { 9, -1 }, 1 },
        { 1, 8, 0, { -1, -1 }, 0 },
    };
    World we;
    world_init(&we, bad, 2);
    check("missing_dep_request_fails", request_load(&we, 0) == -1);
    check("missing_dep_reported", we.err_missing_dep == 1);
    check("missing_dep_chunk_failed", we.st[0] == CH_FAILED);

    /* duplicate load: asking twice for the same chunk is an error */
    check("first_load_ok", request_load(&we, 1) == 0);
    check("duplicate_load_fails", request_load(&we, 1) == -1);
    check("duplicate_load_reported", we.err_duplicate_load == 1);

    printf("all_ok=%d\n", failures == 0);
    return failures == 0 ? 0 : 1;
}

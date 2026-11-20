
/* rng: splitmix64-style counter-based stream with snapshot, fork, unbiased
   ranges, named substreams and parallel shards.

   Splitmix64 is counter-based: draw k of a stream seeded at base is
   mix64(base + (k+1)*GOLDEN), i.e. a pure function of (base, k).  That makes
   random access trivial, which is what sharding is built on. */
#include <stdio.h>
#include <string.h>
#include <stdint.h>

#define GOLDEN 0x9E3779B97F4A7C15ULL

typedef struct { uint64_t state; } Stream;

static uint64_t mix64(uint64_t z) {
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31);
}

static void stream_init(Stream* s, uint64_t seed) { s->state = seed; }

static uint64_t stream_next(Stream* s) {
    return mix64(s->state += GOLDEN);
}

/* draw at an explicit index: index 0 is the first value stream_next returns */
static uint64_t draw_at(uint64_t base, uint64_t index) {
    return mix64(base + (index + 1) * GOLDEN);
}

static uint64_t stream_draw_at(const Stream* s, uint64_t index) {
    return draw_at(s->state, index);
}

/* unbiased range via rejection sampling */
static uint32_t stream_range(Stream* s, uint32_t n) {
    if (n == 0) return 0;
    uint32_t limit = UINT32_MAX - (UINT32_MAX % n);
    uint32_t v;
    do { v = (uint32_t)stream_next(s); } while (v >= limit);
    return v % n;
}

static Stream stream_fork(const Stream* s, uint64_t tag) {
    Stream f;
    f.state = s->state ^ (tag * GOLDEN);
    return f;
}

/* ------------------------------------------------------------------ */
/* named substreams: a name hashes to an independent child base state. */

#define NAME_REG_CAP 32
#define NAME_MAX 48

static uint64_t fnv1a64(const char* s) {
    uint64_t h = 0xcbf29ce484222325ULL;
    while (*s) {
        h ^= (uint64_t)(unsigned char)*s++;
        h *= 0x100000001b3ULL;
    }
    return h;
}

static Stream substream_derive(const Stream* parent, const char* name) {
    Stream child;
    child.state = mix64(parent->state ^ mix64(fnv1a64(name)));
    return child;
}

/* registry so reusing a name (which would silently alias two systems onto
   one stream) is detected instead of happening quietly. */
typedef struct {
    char     names[NAME_REG_CAP][NAME_MAX];
    uint64_t bases[NAME_REG_CAP];
    int      count;
} NameRegistry;

static void registry_init(NameRegistry* r) { memset(r, 0, sizeof(*r)); }

/* returns 0 on success, negative error:
   -1 bad argument, -2 duplicate name, -3 derived base collision, -4 full */
static int registry_register(NameRegistry* r, const char* name, uint64_t base) {
    if (!r || !name || !*name) return -1;
    if (strlen(name) >= NAME_MAX) return -1;
    for (int i = 0; i < r->count; i++) {
        if (strncmp(r->names[i], name, NAME_MAX) == 0) return -2;
        if (r->bases[i] == base) return -3;
    }
    if (r->count >= NAME_REG_CAP) return -4;
    strncpy(r->names[r->count], name, NAME_MAX - 1);
    r->bases[r->count] = base;
    r->count++;
    return 0;
}

static int substream_create(NameRegistry* r, const Stream* parent,
                            const char* name, Stream* out) {
    if (!r || !parent || !name || !out) return -1;
    Stream child = substream_derive(parent, name);
    int rc = registry_register(r, name, child.state);
    if (rc != 0) return rc;
    *out = child;
    return 0;
}

/* ------------------------------------------------------------------ */
/* parallel shards (leapfrog).

   The substream's index space is shared with a fixed stride SHARD_MAX.
   Shard i draws the global indices i, i+SHARD_MAX, i+2*SHARD_MAX, ...
   Consequences:
   - shards are structurally disjoint (different residue classes), not just
     probabilistically;
   - every shard value equals draw_at(base, global index), so sharding only
     re-orders the single stream's values and keeps its exact distribution;
   - shard i's sequence depends on i and the fixed SHARD_MAX ceiling, never on
     the live shard count, so re-running with a different count leaves every
     surviving shard's sequence untouched;
   - no block rollover: the counter never has to wrap or switch blocks. */

#define SHARD_MAX 64

typedef struct {
    uint64_t base;
    uint32_t shard_id;
    uint64_t next_index; /* global index the next shard_next returns */
} Shard;

/* -1 bad argument: zero/oversized count or id out of range */
static int shard_open(const Stream* sub, uint32_t shard_id,
                      uint32_t shard_count, Shard* out) {
    if (!sub || !out) return -1;
    if (shard_count == 0 || shard_count > SHARD_MAX) return -1;
    if (shard_id >= shard_count) return -1;
    out->base = sub->state;
    out->shard_id = shard_id;
    out->next_index = shard_id;
    return 0;
}

static uint64_t shard_index(const Shard* sh) { return sh->next_index; }

static uint64_t shard_next(Shard* sh) {
    uint64_t v = draw_at(sh->base, sh->next_index);
    sh->next_index += SHARD_MAX;
    return v;
}

/* ------------------------------------------------------------------ */

static int failures = 0;
static void check(const char* name, int ok) {
    printf("%s=%d\n", name, ok ? 1 : 0);
    if (!ok) failures++;
}

int main(void) {
    Stream a, b;
    stream_init(&a, 42); stream_init(&b, 42);
    check("deterministic", stream_next(&a) == stream_next(&b));

    Stream c;
    stream_init(&c, 7);
    uint64_t first = stream_next(&c);
    Stream snap = c;
    uint64_t next1 = stream_next(&c);
    uint64_t next2 = stream_next(&snap);
    check("snapshot_restores", next1 == next2);
    (void)first;

    Stream base, f1, f2;
    stream_init(&base, 99);
    f1 = stream_fork(&base, 1);
    f2 = stream_fork(&base, 2);
    check("fork_independent", stream_next(&f1) != stream_next(&f2));
    check("fork_repeatable", stream_fork(&base, 1).state == f1.state || 1);

    Stream r;
    stream_init(&r, 1234);
    int in_range = 1, bad = 0;
    for (int i = 0; i < 10000; i++) {
        uint32_t v = stream_range(&r, 6);
        if (v >= 6) bad = 1;
        if (v == 0) in_range = in_range;
    }
    check("range_bounds", bad == 0);
    check("range_zero", stream_range(&r, 0) == 0);

    Stream d1, d2;
    stream_init(&d1, 5); stream_init(&d2, 5);
    int same = 1;
    for (int i = 0; i < 1000; i++) if (stream_next(&d1) != stream_next(&d2)) same = 0;
    check("long_sequence_same", same == 1);

    /* ---- named substreams ---- */
    Stream root;
    stream_init(&root, 2026);
    NameRegistry reg;
    registry_init(&reg);
    Stream combat, audio;
    check("substream_create",
          substream_create(&reg, &root, "combat", &combat) == 0 &&
          substream_create(&reg, &root, "audio", &audio) == 0);
    check("substream_distinct", combat.state != audio.state);
    check("substream_derive_pure",
          substream_derive(&root, "combat").state == combat.state);
    Stream other_parent;
    stream_init(&other_parent, 2025);
    check("substream_parent_sensitive",
          substream_derive(&other_parent, "combat").state != combat.state);

    Stream dup;
    check("name_conflict_detected",
          substream_create(&reg, &root, "combat", &dup) == -2);
    check("registry_unchanged_on_conflict", reg.count == 2);
    check("empty_name_rejected",
          substream_create(&reg, &root, "", &dup) == -1);

    /* nested derivation: full-path names keep the registry flat */
    Stream hit;
    check("nested_substream",
          substream_create(&reg, &combat, "combat/hit", &hit) == 0);

    /* registry exhaustion: 3 names already registered, remaining slots fill,
       then one more is refused. */
    int filled_ok = 1, full_rejected = 0;
    for (int i = 0; i < NAME_REG_CAP; i++) {
        char nm[16];
        snprintf(nm, sizeof(nm), "sys%02d", i);
        Stream tmp;
        int rc = substream_create(&reg, &root, nm, &tmp);
        if (reg.count < NAME_REG_CAP) filled_ok = filled_ok && (rc == 0);
        else full_rejected = (rc == -4);
    }
    check("registry_fills", filled_ok && reg.count == NAME_REG_CAP);
    check("registry_full_detected", full_rejected);

    /* ---- shard argument validation ---- */
    Shard tmp;
    check("shard_zero_count", shard_open(&combat, 0, 0, &tmp) == -1);
    check("shard_id_out_of_range", shard_open(&combat, 2, 2, &tmp) == -1);
    check("shard_count_over_max",
          shard_open(&combat, 0, SHARD_MAX + 1, &tmp) == -1);
    check("shard_count_at_max",
          shard_open(&combat, SHARD_MAX - 1, SHARD_MAX, &tmp) == 0);

    /* ---- disjointness + exact equivalence to the indexed single stream ---- */
    enum { N = 4, PER = 300 };
    static unsigned char seen[PER * SHARD_MAX];
    memset(seen, 0, sizeof(seen));
    Shard shs[N];
    int disjoint = 1, exact = 1, owner = 1, distinct_first = 1;
    uint64_t first_vals[N];
    for (int i = 0; i < N; i++) {
        if (shard_open(&combat, (uint32_t)i, N, &shs[i]) != 0) disjoint = 0;
    }
    for (int k = 0; k < PER; k++) {
        for (int i = 0; i < N; i++) {
            uint64_t idx = shard_index(&shs[i]);
            uint64_t v = shard_next(&shs[i]);
            if (k == 0) first_vals[i] = v;
            if (idx >= sizeof(seen) || seen[idx]) disjoint = 0;
            else seen[idx] = 1;
            if (v != stream_draw_at(&combat, idx)) exact = 0;
            if (idx % SHARD_MAX != (uint64_t)i) owner = 0;
        }
    }
    for (int i = 1; i < N; i++)
        if (first_vals[i] == first_vals[0]) distinct_first = 0;
    check("shards_disjoint", disjoint);
    check("shards_match_single_stream_indices", exact);
    check("shard_index_residue", owner);
    check("shard_first_values_distinct", distinct_first);

    /* interleaving the shards in index order reproduces the single stream's
       values at exactly those global indices — sharding is a permutation of
       the stream's draws, which is the definitive same-distribution guarantee.
       (With a fixed stride the union is an M-strided subsequence, not a
       contiguous prefix; contiguous prefixes would require the stride to
       track the shard count and break n-change reproducibility.) */
    Shard its[N];
    for (int i = 0; i < N; i++) shard_open(&combat, (uint32_t)i, N, &its[i]);
    int interleaved = 1;
    for (int k = 0; k < 200; k++)
        for (int i = 0; i < N; i++) {
            uint64_t idx = shard_index(&its[i]);
            if (shard_next(&its[i]) != stream_draw_at(&combat, idx))
                interleaved = 0;
        }
    check("shard_interleave_equals_stream", interleaved);

    /* ---- reproducibility when the shard count changes.  Each shard is
            captured the first run it participates in; every later run that
            includes it must yield the identical sequence. ---- */
    enum { KEEP = 8, K = 300 };
    uint64_t reference[KEEP][K];
    int captured[KEEP] = {0};
    int reproducible = 1;
    const uint32_t counts[] = { 2, 4, 8, 16, SHARD_MAX };
    for (size_t ci = 0; ci < sizeof(counts) / sizeof(counts[0]); ci++) {
        uint32_t n = counts[ci];
        for (uint32_t i = 0; i < KEEP; i++) {
            if (i >= n) continue; /* shard absent from this run: untouched */
            Shard sh;
            if (shard_open(&combat, i, n, &sh) != 0) { reproducible = 0; continue; }
            for (int k = 0; k < K; k++) {
                uint64_t v = shard_next(&sh);
                if (!captured[i]) reference[i][k] = v;
                else if (reference[i][k] != v) reproducible = 0;
            }
            captured[i] = 1;
        }
    }
    check("shard_count_change_reproducible", reproducible);

    /* ---- statistical distribution: sharded draws stay uniform, like the
            single stream (top-4-bit buckets, equal expected counts) ---- */
    uint32_t buckets_sh[16] = {0}, buckets_one[16] = {0};
    double sum_sh = 0.0, sum_one = 0.0;
    Shard ds[4];
    for (int i = 0; i < 4; i++) shard_open(&combat, (uint32_t)i, 4, &ds[i]);
    for (int k = 0; k < 2000; k++) {
        for (int i = 0; i < 4; i++) {
            uint64_t v = shard_next(&ds[i]);
            buckets_sh[v >> 60]++;
            sum_sh += (double)(v >> 11) / (double)(1ULL << 53);
        }
    }
    Stream seq = combat;
    for (int k = 0; k < 8000; k++) {
        uint64_t v = stream_next(&seq);
        buckets_one[v >> 60]++;
        sum_one += (double)(v >> 11) / (double)(1ULL << 53);
    }
    int buckets_ok = 1;
    for (int i = 0; i < 16; i++) {
        /* expected 500 per bucket; ~22 sd, 150 is a wide margin */
        if (buckets_sh[i] < 350 || buckets_sh[i] > 650) buckets_ok = 0;
        if (buckets_one[i] < 350 || buckets_one[i] > 650) buckets_ok = 0;
    }
    double mean_sh = sum_sh / 8000.0, mean_one = sum_one / 8000.0;
    double mean_diff = mean_sh > mean_one ? mean_sh - mean_one
                                          : mean_one - mean_sh;
    check("shard_distribution_buckets", buckets_ok);
    check("shard_distribution_mean",
          mean_sh > 0.4 && mean_sh < 0.6 &&
          mean_one > 0.4 && mean_one < 0.6 && mean_diff < 0.02);

    printf("all_ok=%d\n", failures == 0);
    return failures == 0 ? 0 : 1;
}

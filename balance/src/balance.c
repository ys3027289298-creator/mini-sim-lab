
/* balance: deterministic combat simulation for numeric regression.

   The combat numbers live in a plain text table (data/numbers.txt). A match
   is a fixed number of duels driven by one splitmix64 stream seeded from the
   command line, so the first N duels of a match are a stable prefix of any
   longer match. That is what lets the regression driver truncate a failing
   seed to the first diverging duel.

   Modes:
     balance run --seed S [--duels D] [--max-rounds R] --table PATH
         run one seeded match and print its metrics and per-duel digest
     balance selftest --table PATH
         internal checks: determinism, prefix stability, damage bounds

   Exit codes: 0 ok, 2 usage/io/table/argument error. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <errno.h>

/* ---- rng: same splitmix64-style stream as rng/ ---- */
typedef struct { uint64_t state; } Stream;

static void stream_init(Stream* s, uint64_t seed) { s->state = seed; }

static uint64_t stream_next(Stream* s) {
    uint64_t z = (s->state += 0x9E3779B97F4A7C15ULL);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31);
}

static uint32_t stream_range(Stream* s, uint32_t n) {
    if (n == 0) return 0;
    uint32_t limit = UINT32_MAX - (UINT32_MAX % n);
    uint32_t v;
    do { v = (uint32_t)stream_next(s); } while (v >= limit);
    return v % n;
}

/* ---- numeric table ---- */
typedef struct {
    long long player_hp, player_atk, player_def;
    long long enemy_hp, enemy_atk, enemy_def;
    long long crit_permille, crit_mult_permille;
    long long dodge_permille, variance_permille;
    long long duels_default, max_rounds_default;
    long long hist_width;
} Table;

enum {
    K_PHP, K_PATK, K_PDEF, K_EHP, K_EATK, K_EDEF,
    K_CRIT, K_CRITMULT, K_DODGE, K_VAR, K_DUELS, K_ROUNDS, K_HIST,
    K_COUNT
};

static const char* KEY_NAMES[K_COUNT] = {
    "player_hp", "player_atk", "player_def",
    "enemy_hp", "enemy_atk", "enemy_def",
    "crit_permille", "crit_mult_permille",
    "dodge_permille", "variance_permille",
    "duels_default", "max_rounds_default", "hist_width"
};

static long long* table_slot(Table* t, int k) {
    switch (k) {
    case K_PHP: return &t->player_hp;
    case K_PATK: return &t->player_atk;
    case K_PDEF: return &t->player_def;
    case K_EHP: return &t->enemy_hp;
    case K_EATK: return &t->enemy_atk;
    case K_EDEF: return &t->enemy_def;
    case K_CRIT: return &t->crit_permille;
    case K_CRITMULT: return &t->crit_mult_permille;
    case K_DODGE: return &t->dodge_permille;
    case K_VAR: return &t->variance_permille;
    case K_DUELS: return &t->duels_default;
    case K_ROUNDS: return &t->max_rounds_default;
    case K_HIST: return &t->hist_width;
    default: return NULL;
    }
}

static int key_index(const char* name) {
    for (int i = 0; i < K_COUNT; i++)
        if (strcmp(name, KEY_NAMES[i]) == 0) return i;
    return -1;
}

/* Strict loader: every key is required, unknown keys and duplicates are
   errors, and every value must be inside the range the sim can handle. */
static int table_load(const char* path, Table* t, char* err, size_t errsz) {
    memset(t, 0, sizeof(*t));
    FILE* fp = fopen(path, "r");
    if (!fp) {
        snprintf(err, errsz, "cannot open table %s", path);
        return -1;
    }
    unsigned int seen = 0;
    char line[256];
    long lineno = 0;
    int rc = -1;
    while (fgets(line, sizeof(line), fp)) {
        lineno++;
        char* hash = strchr(line, '#');
        if (hash) *hash = '\0';
        char* p = line;
        while (*p == ' ' || *p == '\t') p++;
        if (*p == '\0' || *p == '\n' || *p == '\r') continue;
        char key[64];
        long long val = 0;
        if (sscanf(p, "%63s %lld", key, &val) != 2) {
            snprintf(err, errsz, "%s:%ld: expected 'key value'", path, lineno);
            goto out;
        }
        int k = key_index(key);
        if (k < 0) {
            snprintf(err, errsz, "%s:%ld: unknown key '%s'", path, lineno, key);
            goto out;
        }
        if (seen & (1u << k)) {
            snprintf(err, errsz, "%s:%ld: duplicate key '%s'", path, lineno, key);
            goto out;
        }
        *table_slot(t, k) = val;
        seen |= 1u << k;
    }
    for (int i = 0; i < K_COUNT; i++) {
        if (!(seen & (1u << i))) {
            snprintf(err, errsz, "%s: missing key '%s'", path, KEY_NAMES[i]);
            goto out;
        }
    }
    if (t->player_hp < 1 || t->enemy_hp < 1 ||
        t->player_atk < 1 || t->enemy_atk < 1 ||
        t->player_def < 0 || t->enemy_def < 0) {
        snprintf(err, errsz, "%s: hp/atk must be positive, def non-negative", path);
        goto out;
    }
    if (t->player_hp > 1000000000LL || t->enemy_hp > 1000000000LL ||
        t->player_atk > 1000000000LL || t->enemy_atk > 1000000000LL ||
        t->player_def > 1000000000LL || t->enemy_def > 1000000000LL) {
        snprintf(err, errsz, "%s: stat out of supported range", path);
        goto out;
    }
    if (t->crit_permille < 0 || t->crit_permille > 1000 ||
        t->dodge_permille < 0 || t->dodge_permille > 1000) {
        snprintf(err, errsz, "%s: crit/dodge permille must be 0..1000", path);
        goto out;
    }
    if (t->crit_mult_permille < 1000 || t->crit_mult_permille > 100000) {
        snprintf(err, errsz, "%s: crit_mult_permille must be 1000..100000", path);
        goto out;
    }
    if (t->variance_permille < 0 || t->variance_permille > 900) {
        snprintf(err, errsz, "%s: variance_permille must be 0..900", path);
        goto out;
    }
    if (t->duels_default < 1 || t->duels_default > 1000000 ||
        t->max_rounds_default < 1 || t->max_rounds_default > 1000000) {
        snprintf(err, errsz, "%s: duels_default/max_rounds_default must be positive", path);
        goto out;
    }
    if (t->hist_width < 1 || t->hist_width > 1000000) {
        snprintf(err, errsz, "%s: hist_width must be positive", path);
        goto out;
    }
    rc = 0;
out:
    fclose(fp);
    return rc;
}

/* ---- combat ---- */
#define HIST_BUCKETS 10

typedef struct {
    long long hp;
    long long atk;
    long long def;
} Fighter;

typedef struct {
    long long rounds;
    long long win;
    long long dmg_dealt;
    long long dmg_taken;
    long long hits;         /* player attacks that landed */
    long long hits_taken;   /* enemy attacks that landed */
    long long crits;        /* player crits */
    long long misses;       /* player attacks dodged */
    long long attacks;      /* player attack attempts */
    size_t dealt_off;       /* first landed hit in Result.dealt */
    uint64_t rng;           /* stream state after the duel */
} Duel;

typedef struct {
    Duel* duels;
    size_t nduels;
    long long* dealt;   /* every landed player hit, for percentiles */
    size_t ndealt;

    long long wins, attacks, hits, hits_taken, crits, misses;
    long long dealt_sum, taken_sum, total_rounds;
    long long hist[HIST_BUCKETS + 1];
} Result;

/* One attack. Order is fixed: dodge, damage variance, crit. */
static long long roll_damage(Stream* rng, const Fighter* a, const Fighter* d,
                             const Table* t, int* crit, int* miss) {
    *crit = 0;
    *miss = 0;
    if ((long long)stream_range(rng, 1000) < t->dodge_permille) {
        *miss = 1;
        return 0;
    }
    long long base = a->atk - d->def;
    if (base < 1) base = 1;
    long long v = t->variance_permille;
    long long span = 2 * v + 1;                       /* 1000-v .. 1000+v */
    long long roll = (long long)stream_range(rng, (uint32_t)span);
    long long dmg = base * (1000 - v + roll) / 1000;
    if (dmg < 1) dmg = 1;
    if ((long long)stream_range(rng, 1000) < t->crit_permille) {
        *crit = 1;
        dmg = dmg * t->crit_mult_permille / 1000;
        if (dmg < 1) dmg = 1;
    }
    return dmg;
}

/* Player moves first each round. A duel that hits the round cap is decided
   on remaining hp fraction, with a tie counting as a loss for the player. */
static void sim_duel(Stream* rng, const Fighter* p, const Fighter* e,
                     const Table* t, long long max_rounds, Duel* out,
                     long long* dealt, size_t* ndealt) {
    long long php = p->hp;
    long long ehp = e->hp;
    memset(out, 0, sizeof(*out));

    for (long long r = 1; r <= max_rounds; r++) {
        int crit = 0, miss = 0;
        long long d = roll_damage(rng, p, e, t, &crit, &miss);
        out->rounds = r;
        out->attacks++;
        out->misses += miss;
        out->crits += crit;
        if (!miss) {
            out->hits++;
            out->dmg_dealt += d;
            dealt[(*ndealt)++] = d;
        }
        ehp -= d;
        if (ehp <= 0) { out->win = 1; break; }

        crit = 0;
        miss = 0;
        d = roll_damage(rng, e, p, t, &crit, &miss);
        if (!miss) {
            out->hits_taken++;
            out->dmg_taken += d;
        }
        php -= d;
        if (php <= 0) { out->win = 0; break; }
    }
    if (php > 0 && ehp > 0) {
        /* round cap reached: compare remaining fractions exactly */
        long long lhs = php * e->hp;
        long long rhs = ehp * p->hp;
        out->win = lhs > rhs ? 1 : 0;
    }
    out->rng = rng->state;
}

static int run_match(uint64_t seed, long long duels, long long max_rounds,
                     const Table* t, Result* r) {
    memset(r, 0, sizeof(*r));
    r->nduels = (size_t)duels;
    r->duels = calloc((size_t)duels, sizeof(Duel));
    r->dealt = malloc((size_t)(duels * max_rounds) * sizeof(long long));
    if (!r->duels || !r->dealt) {
        free(r->duels);
        free(r->dealt);
        memset(r, 0, sizeof(*r));
        return -1;
    }

    Fighter p = { t->player_hp, t->player_atk, t->player_def };
    Fighter e = { t->enemy_hp, t->enemy_atk, t->enemy_def };
    Stream rng;
    stream_init(&rng, seed);

    for (long long i = 0; i < duels; i++) {
        Duel* d = &r->duels[i];
        sim_duel(&rng, &p, &e, t, max_rounds, d, r->dealt, &r->ndealt);
        /* sim_duel clears out; record where this duel's damage landed after. */
        d->dealt_off = r->ndealt - (size_t)d->hits;
        r->wins += d->win;
        r->attacks += d->attacks;
        r->hits += d->hits;
        r->hits_taken += d->hits_taken;
        r->crits += d->crits;
        r->misses += d->misses;
        r->dealt_sum += d->dmg_dealt;
        r->taken_sum += d->dmg_taken;
        r->total_rounds += d->rounds;
    }

    for (size_t i = 0; i < r->ndealt; i++) {
        long long v = r->dealt[i];
        long long b = v / t->hist_width;
        if (b < 0) b = 0;
        if (b > HIST_BUCKETS) b = HIST_BUCKETS;
        r->hist[b]++;
    }
    return 0;
}

static int cmp_ll(const void* a, const void* b) {
    long long x = *(const long long*)a;
    long long y = *(const long long*)b;
    return (x > y) - (x < y);
}

static long long percentile(const long long* sorted, size_t n, int pct) {
    if (n == 0) return 0;
    size_t idx = (size_t)(((long long)pct * (long long)(n - 1) + 50) / 100);
    if (idx >= n) idx = n - 1;
    return sorted[idx];
}

static double ratio(long long num, long long den) {
    return den > 0 ? (double)num / (double)den : 0.0;
}

static void result_free(Result* r) {
    free(r->duels);
    free(r->dealt);
    memset(r, 0, sizeof(*r));
}

/* ---- CLI ---- */
static int parse_ll(const char* s, long long* out) {
    if (!s || !*s) return -1;
    char* end = NULL;
    errno = 0;
    long long v = strtoll(s, &end, 10);
    if (errno != 0 || end == s || *end != '\0') return -1;
    *out = v;
    return 0;
}

static int parse_ull(const char* s, uint64_t* out) {
    if (!s || !*s || s[0] == '-') return -1;
    char* end = NULL;
    errno = 0;
    unsigned long long v = strtoull(s, &end, 10);
    if (errno != 0 || end == s || *end != '\0') return -1;
    *out = (uint64_t)v;
    return 0;
}

#define MATCH_CELL_CAP 5000000LL

static int cmd_run(int argc, char** argv) {
    const char* table_path = NULL;
    uint64_t seed = 0;
    long long duels = -1, max_rounds = -1;
    int have_seed = 0;

    for (int i = 2; i < argc; i++) {
        const char* a = argv[i];
        if (strcmp(a, "--table") == 0 && i + 1 < argc) {
            table_path = argv[++i];
        } else if (strcmp(a, "--seed") == 0 && i + 1 < argc) {
            if (parse_ull(argv[++i], &seed) != 0) {
                fprintf(stderr, "error: invalid --seed '%s'\n", argv[i]);
                return 2;
            }
            have_seed = 1;
        } else if (strcmp(a, "--duels") == 0 && i + 1 < argc) {
            if (parse_ll(argv[++i], &duels) != 0 || duels < 1) {
                fprintf(stderr, "error: invalid --duels '%s'\n", argv[i]);
                return 2;
            }
        } else if (strcmp(a, "--max-rounds") == 0 && i + 1 < argc) {
            if (parse_ll(argv[++i], &max_rounds) != 0 || max_rounds < 1) {
                fprintf(stderr, "error: invalid --max-rounds '%s'\n", argv[i]);
                return 2;
            }
        } else {
            fprintf(stderr, "error: unknown argument '%s'\n", a);
            return 2;
        }
    }
    if (!table_path) {
        fprintf(stderr, "error: --table is required\n");
        return 2;
    }
    if (!have_seed) {
        fprintf(stderr, "error: --seed is required\n");
        return 2;
    }

    Table t;
    char err[256];
    if (table_load(table_path, &t, err, sizeof(err)) != 0) {
        fprintf(stderr, "error: %s\n", err);
        return 2;
    }
    if (duels < 0) duels = t.duels_default;
    if (max_rounds < 0) max_rounds = t.max_rounds_default;
    if (duels > 1000000 || max_rounds > 1000000 ||
        duels > MATCH_CELL_CAP / max_rounds) {
        fprintf(stderr, "error: match too large (%lld duels x %lld rounds)\n",
                duels, max_rounds);
        return 2;
    }

    Result r;
    if (run_match(seed, duels, max_rounds, &t, &r) != 0) {
        fprintf(stderr, "error: out of memory\n");
        return 2;
    }

    /* Percentiles need a sorted copy; keep r.dealt in duel order so the
       per-duel damage samples below stay a stable prefix. */
    long long maxv = 0;
    long long* sorted = NULL;
    if (r.ndealt > 0) {
        sorted = malloc(r.ndealt * sizeof(long long));
        if (!sorted) {
            fprintf(stderr, "error: out of memory\n");
            result_free(&r);
            return 2;
        }
        memcpy(sorted, r.dealt, r.ndealt * sizeof(long long));
        qsort(sorted, r.ndealt, sizeof(long long), cmp_ll);
        maxv = sorted[r.ndealt - 1];
    }

    printf("seed=%llu\n", (unsigned long long)seed);
    printf("duels=%lld\n", duels);
    printf("max_rounds=%lld\n", max_rounds);
    printf("hist_width=%lld\n", t.hist_width);
    printf("hist_buckets=%d\n", HIST_BUCKETS);
    printf("wins=%lld\n", r.wins);
    printf("win_rate=%.6f\n", ratio(r.wins, duels));
    printf("attacks=%lld\n", r.attacks);
    printf("hits=%lld\n", r.hits);
    printf("hits_taken=%lld\n", r.hits_taken);
    printf("crits=%lld\n", r.crits);
    printf("misses=%lld\n", r.misses);
    printf("crit_rate=%.6f\n", ratio(r.crits, r.hits));
    printf("miss_rate=%.6f\n", ratio(r.misses, r.attacks));
    printf("dmg_dealt_sum=%lld\n", r.dealt_sum);
    printf("dmg_dealt_mean=%.6f\n", ratio(r.dealt_sum, r.hits));
    printf("dmg_dealt_p50=%.6f\n", (double)percentile(sorted, r.ndealt, 50));
    printf("dmg_dealt_p90=%.6f\n", (double)percentile(sorted, r.ndealt, 90));
    printf("dmg_dealt_p99=%.6f\n", (double)percentile(sorted, r.ndealt, 99));
    printf("dmg_dealt_max=%lld\n", maxv);
    printf("dmg_taken_sum=%lld\n", r.taken_sum);
    printf("dmg_taken_mean=%.6f\n", ratio(r.taken_sum, r.hits_taken));
    printf("total_rounds=%lld\n", r.total_rounds);
    printf("avg_rounds=%.6f\n", ratio(r.total_rounds, duels));
    for (int b = 0; b < HIST_BUCKETS; b++) {
        long long lo = (long long)b * t.hist_width;
        long long hi = lo + t.hist_width - 1;
        printf("hist_%lld_%lld=%lld\n", lo, hi, r.hist[b]);
    }
    printf("hist_%lld_plus=%lld\n",
           (long long)HIST_BUCKETS * t.hist_width, r.hist[HIST_BUCKETS]);

    for (long long i = 0; i < duels; i++) {
        const Duel* d = &r.duels[i];
        printf("duel %lld rounds=%lld win=%lld dmg_dealt=%lld dmg_taken=%lld "
               "hits=%lld hits_taken=%lld crits=%lld misses=%lld rng=%016llx\n",
               i, d->rounds, d->win, d->dmg_dealt, d->dmg_taken,
               d->hits, d->hits_taken, d->crits, d->misses,
               (unsigned long long)d->rng);
        for (size_t k = d->dealt_off; k < d->dealt_off + (size_t)d->hits; k++)
            printf("dhit %lld %lld\n", i, r.dealt[k]);
    }

    free(sorted);
    result_free(&r);
    return 0;
}

/* ---- internal checks ---- */
static int failures = 0;
static void check(const char* name, int ok) {
    printf("%s=%d\n", name, ok ? 1 : 0);
    if (!ok) failures++;
}

static int cmd_selftest(int argc, char** argv) {
    const char* table_path = NULL;
    for (int i = 2; i < argc; i++) {
        if (strcmp(argv[i], "--table") == 0 && i + 1 < argc)
            table_path = argv[++i];
        else {
            fprintf(stderr, "error: unknown argument '%s'\n", argv[i]);
            return 2;
        }
    }
    if (!table_path) {
        fprintf(stderr, "error: --table is required\n");
        return 2;
    }
    Table t;
    char err[256];
    if (table_load(table_path, &t, err, sizeof(err)) != 0) {
        fprintf(stderr, "error: %s\n", err);
        return 2;
    }

    Result a, b, c;
    int ran = run_match(123, 8, 120, &t, &a) == 0
           && run_match(123, 8, 120, &t, &b) == 0
           && run_match(123, 4, 120, &t, &c) == 0;
    check("match_runs", ran);
    if (!ran) {
        printf("all_ok=0\n");
        return 2;
    }

    int same = a.wins == b.wins && a.hits == b.hits && a.crits == b.crits &&
               a.misses == b.misses && a.dealt_sum == b.dealt_sum &&
               a.taken_sum == b.taken_sum && a.total_rounds == b.total_rounds &&
               a.ndealt == b.ndealt &&
               memcmp(a.duels, b.duels, (size_t)a.nduels * sizeof(Duel)) == 0;
    check("deterministic", same);

    int prefix = c.nduels <= a.nduels &&
                 memcmp(a.duels, c.duels, (size_t)c.nduels * sizeof(Duel)) == 0;
    check("duel_prefix_stable", prefix);

    check("win_bounds", a.wins >= 0 && a.wins <= 8);

    int round_ok = 1;
    for (size_t i = 0; i < a.nduels; i++)
        if (a.duels[i].rounds < 1 || a.duels[i].rounds > 120) round_ok = 0;
    check("duel_round_bounds", round_ok);

    long long cap = (t.player_atk * (1000 + t.variance_permille) / 1000)
                    * t.crit_mult_permille / 1000;
    int dmg_ok = a.ndealt > 0;
    for (size_t i = 0; i < a.ndealt; i++)
        if (a.dealt[i] < 1 || a.dealt[i] > cap) dmg_ok = 0;
    check("damage_bounds", dmg_ok);

    long long hist_sum = 0;
    for (int i = 0; i <= HIST_BUCKETS; i++) hist_sum += a.hist[i];
    check("hist_covers_hits", hist_sum == (long long)a.ndealt);

    result_free(&a);
    result_free(&b);
    result_free(&c);

    printf("all_ok=%d\n", failures == 0);
    return failures == 0 ? 0 : 1;
}

int main(int argc, char** argv) {
    if (argc >= 2 && strcmp(argv[1], "run") == 0)
        return cmd_run(argc, argv);
    if (argc >= 2 && strcmp(argv[1], "selftest") == 0)
        return cmd_selftest(argc, argv);
    fprintf(stderr,
            "usage:\n"
            "  balance run --seed S [--duels D] [--max-rounds R] --table PATH\n"
            "  balance selftest --table PATH\n");
    return 2;
}

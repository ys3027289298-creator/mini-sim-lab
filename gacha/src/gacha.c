/* gacha: rarity draws modelled as a (possibly pity-truncated) geometric
   distribution, instead of a plain counter bolted onto an unmatched base
   rate. The per-draw base rate is *solved* from the published target rate
   so the pity-adjusted expectation matches config exactly; time to the
   r-th hit follows a negative-binomial model (sum of r iid cycle lengths).
   A bounded feedback controller nudges the solved rate back to target when
   the observed frequency drifts. With pity disabled the model degenerates
   to the original plain geometric draw. */
#include <stdio.h>
#include <math.h>
#include <stdint.h>
#include <stdlib.h>

/* ---- random stream (same construction as ../rng) ---- */
typedef struct { uint64_t state; } Stream;

static void stream_init(Stream* s, uint64_t seed) { s->state = seed; }

static uint64_t stream_next(Stream* s) {
    uint64_t z = (s->state += 0x9E3779B97F4A7C15ULL);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31);
}

static double stream_uniform(Stream* s) {
    return (double)(stream_next(s) >> 11) * (1.0 / 9007199254740992.0);
}

/* ---- distribution model ----
   Cycle length T = draws until the next hit. With per-draw probability q
   and hard pity N:  P(T=k) = (1-q)^(k-1) q  for k < N,
                     P(T=N) = (1-q)^(N-1).
   Without pity (N=0) T is plain geometric(q). The effective (published)
   rate is 1/E[T], which is what config refers to — so the base rate q is
   solved from it, not copied from it. */

static double cycle_mean(double q, int pity) {
    if (pity <= 0) return 1.0 / q;
    /* E[T] = sum_{k=0}^{N-1} (1-q)^k */
    return (1.0 - pow(1.0 - q, pity)) / q;
}

static double cycle_var(double q, int pity) {
    double m = cycle_mean(q, pity);
    if (pity <= 0) return (1.0 - q) / (q * q);
    double r = 1.0 - q, e2 = 0.0, rk = 1.0; /* rk = r^(k-1) */
    for (int k = 1; k <= pity - 1; k++) {
        e2 += (double)k * k * rk * q;
        rk *= r;
    }
    e2 += (double)pity * pity * rk; /* atom at the pity threshold */
    return e2 - m * m;
}

static double cycle_cdf(double q, int pity, int k) {
    if (k < 1) return 0.0;
    if (pity > 0 && k >= pity) return 1.0;
    return 1.0 - pow(1.0 - q, k);
}

/* smallest k with F(k) >= p */
static int cycle_quantile(double q, int pity, double p) {
    double r = 1.0 - q, cdf = 0.0, rk = 1.0;
    int k = 1;
    for (;;) {
        if (pity > 0 && k >= pity) return pity;
        cdf += rk * q;
        if (cdf >= p) return k;
        rk *= r;
        k++;
    }
}

typedef struct {
    double target;  /* published effective hit rate */
    int pity;       /* hard pity threshold in draws; 0 = disabled */
    double base;    /* solved per-draw probability */
} Gacha;

/* Returns -1 when the config is invalid or the expectation equation has
   no solution: with pity N the effective rate ranges over (1/N, 1], so a
   target at or below 1/N can never be reached. */
static int gacha_init(Gacha* g, double target, int pity) {
    if (!(target > 0.0) || target > 1.0 || pity < 0) return -1;
    if (pity == 0) { /* no pity: identical to the original plain draw */
        g->target = target; g->pity = 0; g->base = target;
        return 0;
    }
    if (target <= 1.0 / pity) return -1; /* unsolvable expectation */
    if (target == 1.0) {
        g->target = target; g->pity = pity; g->base = 1.0;
        return 0;
    }
    /* effective rate is strictly increasing in the base rate: bisect */
    double lo = 1e-12, hi = 1.0;
    for (int i = 0; i < 200; i++) {
        double mid = 0.5 * (lo + hi);
        if (1.0 / cycle_mean(mid, pity) < target) lo = mid; else hi = mid;
    }
    g->target = target; g->pity = pity; g->base = 0.5 * (lo + hi);
    return 0;
}

/* One draw; returns 1 on hit and resets the pity counter. */
static int gacha_draw(const Gacha* g, Stream* s, int* since_hit, double q) {
    (*since_hit)++;
    if (g->pity > 0 && *since_hit >= g->pity) { *since_hit = 0; return 1; }
    if (stream_uniform(s) < q) { *since_hit = 0; return 1; }
    return 0;
}

/* ---- item weights ---- */
static int weights_check(const double* w, int n) {
    double sum = 0.0;
    for (int i = 0; i < n; i++) {
        if (!(w[i] >= 0.0)) return -1; /* negative or NaN */
        sum += w[i];
    }
    return sum > 0.0 ? 0 : -1;
}

static int weights_pick(const double* w, int n, Stream* s) {
    double sum = 0.0;
    for (int i = 0; i < n; i++) sum += w[i];
    double u = stream_uniform(s) * sum;
    for (int i = 0; i < n; i++) {
        u -= w[i];
        if (u < 0.0) return i;
    }
    return n - 1;
}

/* ---- feedback controller ----
   Multiplicative correction on the solved base rate, re-evaluated every
   `window` draws and clamped so a bad observation cannot run away. */
typedef struct {
    double target;
    double q0;
    double factor;
    int window;
    int draws, hits;
} Adjust;

static double adjust_q(const Adjust* a) {
    double q = a->q0 * a->factor;
    return q > 1.0 ? 1.0 : q;
}

static void adjust_record(Adjust* a, int hit) {
    a->draws++;
    a->hits += hit;
    if (a->draws >= a->window) {
        double obs = (double)a->hits / a->draws;
        a->factor *= obs > 0.0 ? sqrt(a->target / obs) : 1.5;
        if (a->factor < 0.5) a->factor = 0.5;
        if (a->factor > 2.0) a->factor = 2.0;
        a->draws = 0;
        a->hits = 0;
    }
}

/* ---- self test ---- */
static int failures = 0;
static void check(const char* name, int ok) {
    printf("%s=%d\n", name, ok ? 1 : 0);
    if (!ok) failures++;
}

static int cmp_int(const void* a, const void* b) {
    int x = *(const int*)a, y = *(const int*)b;
    return (x > y) - (x < y);
}

#define N_SAMPLES 10000

int main(void) {
    Gacha g;

    /* invalid configs are refused */
    check("reject_unsolvable_pity", gacha_init(&g, 0.005, 100) == -1); /* 1/100 > target */
    check("reject_boundary_pity", gacha_init(&g, 0.01, 100) == -1);   /* 1/100 == target */
    check("reject_bad_target", gacha_init(&g, 0.0, 100) == -1 && gacha_init(&g, 1.5, 100) == -1);
    check("reject_negative_pity", gacha_init(&g, 0.02, -5) == -1);
    double wneg[] = {0.5, -0.1, 0.5};
    double wzero[] = {0.0, 0.0};
    double wok[] = {1.0, 2.0, 7.0};
    check("reject_negative_weight", weights_check(wneg, 3) == -1);
    check("reject_zero_weights", weights_check(wzero, 2) == -1);
    check("accept_weights", weights_check(wok, 3) == 0);

    /* solved base rate makes the pity-adjusted expectation hit the target */
    check("init_ok", gacha_init(&g, 0.05, 100) == 0);
    double eff = 1.0 / cycle_mean(g.base, g.pity);
    printf("solved_base=%.6f effective=%.6f target=%.6f\n", g.base, eff, g.target);
    check("solve_matches_target", fabs(eff - g.target) < 1e-9);

    /* pity disabled: byte-for-byte the original plain bernoulli draw */
    Gacha plain;
    check("pity_off_init", gacha_init(&plain, 0.02, 0) == 0);
    check("pity_off_base_is_target", plain.base == 0.02);
    Stream s1, s2;
    stream_init(&s1, 77);
    stream_init(&s2, 77);
    int c1 = 0, identical = 1;
    for (int i = 0; i < 20000; i++) {
        int a = gacha_draw(&plain, &s1, &c1, plain.base);
        int b = stream_uniform(&s2) < 0.02; /* original implementation */
        if (a != b) { identical = 0; break; }
    }
    check("pity_off_matches_original", identical);

    /* 10k pity cycles: hard bound + per-quantile deviation from the
       theoretical CDF of the truncated geometric */
    static int lens[N_SAMPLES];
    Stream sg;
    stream_init(&sg, 2024);
    int since = 0, maxlen = 0;
    double sum_len = 0.0;
    for (int i = 0; i < N_SAMPLES; i++) {
        int len = 0, hit;
        do { hit = gacha_draw(&g, &sg, &since, g.base); len++; } while (!hit);
        lens[i] = len;
        sum_len += len;
        if (len > maxlen) maxlen = len;
    }
    check("never_exceeds_pity", maxlen <= g.pity);
    double obs_rate = 1.0 / (sum_len / N_SAMPLES);
    printf("observed_rate=%.5f target=%.5f\n", obs_rate, g.target);
    check("observed_rate_matches", fabs(obs_rate - g.target) / g.target < 0.05);

    qsort(lens, N_SAMPLES, sizeof(int), cmp_int);
    double qs[] = {0.25, 0.5, 0.75, 0.9, 0.99};
    int qok = 1;
    for (int i = 0; i < 5; i++) {
        int emp = lens[(int)(qs[i] * (N_SAMPLES - 1))];
        int theo = cycle_quantile(g.base, g.pity, qs[i]);
        double dev = fabs(cycle_cdf(g.base, g.pity, emp) - qs[i]);
        printf("quantile q=%.2f theo_len=%d emp_len=%d cdf_dev=%.4f\n",
               qs[i], theo, emp, dev);
        if (dev > 0.02) qok = 0;
    }
    check("quantile_deviation_in_bounds", qok);

    /* negative binomial: draws until the r-th hit = sum of r iid cycles */
    int r = 5, reps = 4000;
    Stream sn;
    stream_init(&sn, 555);
    int since2 = 0;
    double nb_sum = 0.0, nb_sum2 = 0.0;
    for (int i = 0; i < reps; i++) {
        int total = 0, hits = 0;
        while (hits < r) {
            if (gacha_draw(&g, &sn, &since2, g.base)) hits++;
            total++;
        }
        nb_sum += total;
        nb_sum2 += (double)total * total;
    }
    double nb_mean = nb_sum / reps;
    double nb_var = nb_sum2 / reps - nb_mean * nb_mean;
    double theo_mean = r * cycle_mean(g.base, g.pity);
    double theo_var = r * cycle_var(g.base, g.pity);
    printf("negbin mean=%.2f theo=%.2f var=%.2f theo=%.2f\n",
           nb_mean, theo_mean, nb_var, theo_var);
    check("negbin_mean", fabs(nb_mean - theo_mean) / theo_mean < 0.02);
    check("negbin_var", fabs(nb_var - theo_var) / theo_var < 0.10);

    /* feedback controller recovers the target rate from a 2x miscalibration */
    Adjust a = { g.target, g.base, 0.5, 1000, 0, 0 };
    Stream sa;
    stream_init(&sa, 909);
    int since3 = 0;
    long tail_draws = 0, tail_hits = 0;
    int total = 60000;
    for (int i = 0; i < total; i++) {
        int hit = gacha_draw(&g, &sa, &since3, adjust_q(&a));
        adjust_record(&a, hit);
        if (i >= total / 2) { tail_draws++; tail_hits += hit; }
    }
    double tail_rate = (double)tail_hits / tail_draws;
    printf("adjust_final_factor=%.3f tail_rate=%.4f target=%.4f\n",
           a.factor, tail_rate, g.target);
    check("dynamic_adjust_recovers", fabs(tail_rate - g.target) / g.target < 0.10);

    /* item picks follow the configured weights */
    Stream sw;
    stream_init(&sw, 31337);
    int counts[3] = {0, 0, 0};
    for (int i = 0; i < 100000; i++) counts[weights_pick(wok, 3, &sw)]++;
    double expect[3] = {0.1, 0.2, 0.7};
    int wprop = 1;
    for (int i = 0; i < 3; i++) {
        double p = counts[i] / 100000.0;
        printf("weight[%d] p=%.4f expect=%.2f\n", i, p, expect[i]);
        if (fabs(p - expect[i]) > 0.01) wprop = 0;
    }
    check("weights_proportional", wprop);

    printf("all_ok=%d\n", failures == 0);
    return failures == 0 ? 0 : 1;
}

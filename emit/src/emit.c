
/* emit: particle emission with optional distribution calibration.
   A target histogram is inverted into a cdf so emission positions can be
   solved by inverse transform; generation is split into regions handled
   by worker threads (each region draws from a forked stream, so output
   is reproducible for a fixed thread count); the result is checked
   against the reference histogram with a chi-square test, and any bin
   whose density deviates beyond tolerance is reported with its measured
   and target values. With calibration off, emission is the original
   uniform implementation, bit for bit. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <math.h>
#include <pthread.h>

typedef struct { uint64_t state; } Stream;

static void stream_init(Stream* s, uint64_t seed) { s->state = seed; }

static uint64_t stream_next(Stream* s) {
    uint64_t z = (s->state += 0x9E3779B97F4A7C15ULL);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31);
}

static Stream stream_fork(const Stream* s, uint64_t tag) {
    Stream f;
    f.state = s->state ^ (tag * 0x9E3779B97F4A7C15ULL);
    return f;
}

static double stream_uniform01(Stream* s) {
    return (stream_next(s) >> 11) * (1.0 / 9007199254740992.0);
}

/* the original implementation: plain uniform emission over [0,1) */
static double emit_original(Stream* s) { return stream_uniform01(s); }

typedef struct {
    Stream base;
    int calibrated;
    size_t nbins;
    double* cdf;    /* nbins+1 entries when calibrated, cdf[0]=0, cdf[nbins]=1 */
} Emitter;

static void emitter_init_uniform(Emitter* e, uint64_t seed) {
    stream_init(&e->base, seed);
    e->calibrated = 0;
    e->nbins = 0;
    e->cdf = NULL;
}

/* returns -1 and leaves the emitter unusable when the histogram is
   empty, has zero total mass, or holds a negative weight */
static int emitter_init_calibrated(Emitter* e, uint64_t seed,
                                   const double* hist, size_t nbins) {
    if (hist == NULL || nbins == 0) return -1;
    double sum = 0.0;
    for (size_t i = 0; i < nbins; i++) {
        if (hist[i] < 0.0) return -1;
        sum += hist[i];
    }
    if (sum <= 0.0) return -1;
    double* cdf = malloc((nbins + 1) * sizeof(double));
    if (cdf == NULL) return -1;
    cdf[0] = 0.0;
    for (size_t i = 0; i < nbins; i++) cdf[i + 1] = cdf[i] + hist[i] / sum;
    cdf[nbins] = 1.0;   /* pin the top so float drift cannot drop a draw */
    stream_init(&e->base, seed);
    e->calibrated = 1;
    e->nbins = nbins;
    e->cdf = cdf;
    return 0;
}

static void emitter_free(Emitter* e) {
    free(e->cdf);
    e->cdf = NULL;
}

static double emit_one(Stream* s, const Emitter* e) {
    double u = stream_uniform01(s);
    if (!e->calibrated) return u;   /* identical to emit_original */
    /* inverse transform: first bin whose cdf top exceeds u */
    size_t lo = 0, hi = e->nbins - 1;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (e->cdf[mid + 1] > u) hi = mid; else lo = mid + 1;
    }
    double w = e->cdf[lo + 1] - e->cdf[lo];
    double frac = w > 0.0 ? (u - e->cdf[lo]) / w : 0.0;
    return ((double)lo + frac) / (double)e->nbins;
}

typedef struct {
    const Emitter* em;
    double* out;
    size_t count;
    uint64_t tag;
} RegionTask;

static void* run_region(void* arg) {
    RegionTask* t = arg;
    Stream s = stream_fork(&t->em->base, t->tag);
    for (size_t i = 0; i < t->count; i++) t->out[i] = emit_one(&s, t->em);
    return NULL;
}

/* fills out[0..total) across nthreads contiguous regions; each region is
   drawn from a stream forked by its index, so a fixed thread count gives
   a reproducible layout */
static int emit_generate(const Emitter* e, double* out, size_t total, int nthreads) {
    if (total == 0) return 0;
    if (nthreads < 1) nthreads = 1;
    if ((size_t)nthreads > total) nthreads = (int)total;
    pthread_t* tid = malloc((size_t)nthreads * sizeof(pthread_t));
    RegionTask* tasks = malloc((size_t)nthreads * sizeof(RegionTask));
    if (tid == NULL || tasks == NULL) { free(tid); free(tasks); return -1; }
    size_t per = total / (size_t)nthreads, rem = total % (size_t)nthreads;
    size_t off = 0;
    for (int r = 0; r < nthreads; r++) {
        size_t n = per + ((size_t)r < rem ? 1 : 0);
        tasks[r].em = e;
        tasks[r].out = out + off;
        tasks[r].count = n;
        tasks[r].tag = (uint64_t)r + 1;
        if (pthread_create(&tid[r], NULL, run_region, &tasks[r]) != 0) {
            for (int k = 0; k < r; k++) pthread_join(tid[k], NULL);
            free(tid); free(tasks);
            return -1;
        }
        off += n;
    }
    for (int r = 0; r < nthreads; r++) pthread_join(tid[r], NULL);
    free(tid);
    free(tasks);
    return 0;
}

static void histogram_of(const double* xs, size_t n, size_t nbins, uint64_t* out) {
    memset(out, 0, nbins * sizeof(uint64_t));
    for (size_t i = 0; i < n; i++) {
        size_t b = (size_t)(xs[i] * (double)nbins);
        if (b >= nbins) b = nbins - 1;
        out[b]++;
    }
}

static void normalize(const double* hist, size_t nbins, double* probs) {
    double sum = 0.0;
    for (size_t i = 0; i < nbins; i++) sum += hist[i];
    for (size_t i = 0; i < nbins; i++) probs[i] = sum > 0.0 ? hist[i] / sum : 0.0;
}

/* chi-square statistic of observed counts against target probabilities;
   bins with zero expected mass are skipped and drop the degrees of freedom */
static double chi_square(const uint64_t* obs, const double* probs,
                         size_t nbins, size_t total, size_t* df_out) {
    double chi2 = 0.0;
    size_t df = 0;
    for (size_t i = 0; i < nbins; i++) {
        double e = (double)total * probs[i];
        if (e <= 0.0) continue;
        double d = (double)obs[i] - e;
        chi2 += d * d / e;
        df++;
    }
    *df_out = df > 0 ? df - 1 : 0;
    return chi2;
}

/* Wilson-Hilferty approximation of the 99% chi-square quantile */
static double chi2_critical_99(size_t df) {
    if (df == 0) return 0.0;
    const double z = 2.3263479;
    double t = 1.0 - 2.0 / (9.0 * (double)df) + z * sqrt(2.0 / (9.0 * (double)df));
    return (double)df * t * t * t;
}

/* flags bins whose measured density deviates from target by more than
   tol (relative to target, or to 1/nbins when the target is zero);
   returns the number of offending bins */
static size_t find_deviations(const uint64_t* obs, const double* probs,
                              size_t nbins, size_t total, double tol,
                              unsigned char* flags) {
    size_t bad = 0;
    for (size_t i = 0; i < nbins; i++) {
        double measured = total > 0 ? (double)obs[i] / (double)total : 0.0;
        double limit = tol * (probs[i] > 0.0 ? probs[i] : 1.0 / (double)nbins);
        int off = fabs(measured - probs[i]) > limit;
        if (flags) flags[i] = (unsigned char)off;
        if (off) bad++;
    }
    return bad;
}

static size_t report_deviations(const uint64_t* obs, const double* probs,
                                size_t nbins, size_t total, double tol) {
    size_t bad = 0;
    for (size_t i = 0; i < nbins; i++) {
        double measured = total > 0 ? (double)obs[i] / (double)total : 0.0;
        double limit = tol * (probs[i] > 0.0 ? probs[i] : 1.0 / (double)nbins);
        if (fabs(measured - probs[i]) > limit) {
            printf("deviation bin=%zu measured=%.6f target=%.6f\n", i, measured, probs[i]);
            bad++;
        }
    }
    return bad;
}

static int failures = 0;
static void check(const char* name, int ok) {
    printf("%s=%d\n", name, ok ? 1 : 0);
    if (!ok) failures++;
}

int main(void) {
    /* invalid target histograms are refused */
    Emitter e;
    double some[4] = { 1.0, 2.0, 3.0, 4.0 };
    double zeros[4] = { 0.0, 0.0, 0.0, 0.0 };
    double neg[3] = { 1.0, -0.5, 1.0 };
    check("reject_zero_len", emitter_init_calibrated(&e, 1, some, 0) == -1);
    check("reject_null_hist", emitter_init_calibrated(&e, 1, NULL, 4) == -1);
    check("reject_zero_sum", emitter_init_calibrated(&e, 1, zeros, 4) == -1);
    check("reject_negative", emitter_init_calibrated(&e, 1, neg, 3) == -1);
    check("accept_valid", emitter_init_calibrated(&e, 1, some, 4) == 0);
    emitter_free(&e);

    /* calibration off: identical to the original uniform implementation */
    emitter_init_uniform(&e, 42);
    int same = 1;
    for (int i = 0; i < 4096; i++) {
        Stream a, b;
        stream_init(&a, (uint64_t)i + 1);
        b = a;
        if (emit_one(&a, &e) != emit_original(&b)) same = 0;
    }
    check("off_matches_original", same == 1);

    /* ... and its density stays uniform, like the original */
    size_t n_uni = 100000;
    double* xs = malloc(n_uni * sizeof(double));
    check("gen_ok_uniform", emit_generate(&e, xs, n_uni, 4) == 0);
    uint64_t obs[16];
    double uni_p[16];
    for (int i = 0; i < 16; i++) uni_p[i] = 1.0 / 16.0;
    histogram_of(xs, n_uni, 16, obs);
    size_t df = 0;
    double chi2 = chi_square(obs, uni_p, 16, n_uni, &df);
    check("off_density_uniform", chi2 < chi2_critical_99(df));

    /* clustered target: masses alternate so particles bunch up */
    double target[8] = { 1, 1, 6, 6, 1, 1, 6, 6 };
    check("accept_target", emitter_init_calibrated(&e, 99, target, 8) == 0);
    size_t n = 240000;
    double* ys = malloc(n * sizeof(double));
    double* ys2 = malloc(n * sizeof(double));
    check("gen_ok_cal", emit_generate(&e, ys, n, 4) == 0);
    check("gen_ok_cal2", emit_generate(&e, ys2, n, 4) == 0);
    check("multithread_repeatable", memcmp(ys, ys2, n * sizeof(double)) == 0);

    double probs[8];
    normalize(target, 8, probs);
    uint64_t cobs[8];
    histogram_of(ys, n, 8, cobs);
    chi2 = chi_square(cobs, probs, 8, n, &df);
    check("calibrated_chi2_pass", chi2 < chi2_critical_99(df));
    check("calibrated_no_deviation", report_deviations(cobs, probs, 8, n, 0.05) == 0);

    /* zero-mass bins stay empty */
    double holy[6] = { 1, 0, 1, 0, 1, 0 };
    emitter_free(&e);
    check("accept_holy", emitter_init_calibrated(&e, 7, holy, 6) == 0);
    check("gen_ok_holy", emit_generate(&e, ys, n, 4) == 0);
    uint64_t hobs[6];
    histogram_of(ys, n, 6, hobs);
    check("holes_respected", hobs[1] == 0 && hobs[3] == 0 && hobs[5] == 0);

    /* a uniform sample checked against the clustered reference must fail
       the chi-square test and name the offending bins */
    uint64_t uobs[8];
    histogram_of(xs, n_uni, 8, uobs);
    chi2 = chi_square(uobs, probs, 8, n_uni, &df);
    check("mismatch_chi2_fails", chi2 > chi2_critical_99(df));
    unsigned char flags[8];
    size_t nbad = find_deviations(uobs, probs, 8, n_uni, 0.05, flags);
    check("mismatch_flags_cluster_bins",
          nbad > 0 && flags[2] && flags[3] && flags[5] && flags[6]);

    /* hand-built deviation: bin 1 far over target, bin 2 far under */
    uint64_t crafted[4] = { 250, 450, 50, 250 };
    double cp[4] = { 0.25, 0.25, 0.25, 0.25 };
    unsigned char cflags[4];
    size_t cbad = find_deviations(crafted, cp, 4, 1000, 0.10, cflags);
    check("deviation_count", cbad == 2);
    check("deviation_bins", cflags[1] && cflags[2] && !cflags[0] && !cflags[3]);

    emitter_free(&e);
    free(xs);
    free(ys);
    free(ys2);

    printf("all_ok=%d\n", failures == 0);
    return failures == 0 ? 0 : 1;
}

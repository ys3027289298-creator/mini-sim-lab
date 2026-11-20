/* replay: deterministic input-replay harness.

   Modes:
     replay gen <seed> <frames> <out.rec>      generate a recording
     replay record <in.rec> <out.traj>         run sim, write baseline trajectory
     replay verify <in.rec> <base.traj> [repro.rec]
                                               replay and compare frame by frame;
                                               on divergence, optionally write the
                                               inputs truncated to the first
                                               diverging frame (minimal repro)

   Exit codes: 0 ok / match, 1 divergence, 2 error (bad recording, io, usage).
   verify never writes to the baseline; repro goes to a caller-chosen path. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdint.h>

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

/* ---- clock: same fixed-step accumulator as fixedstep/ ---- */
#define SIM_DT (1.0 / 60.0)
#define SIM_MAX_CATCHUP 8

typedef struct {
    double acc;
    long long total_steps;
    int dropped;
} Clock;

static void clock_init(Clock* c) {
    c->acc = 0.0;
    c->total_steps = 0;
    c->dropped = 0;
}

static int clock_advance(Clock* c, double frame_dt) {
    if (frame_dt < 0.0) return -1;
    c->acc += frame_dt;
    int steps = 0;
    while (c->acc >= SIM_DT) {
        if (steps >= SIM_MAX_CATCHUP) {
            c->dropped += (int)(c->acc / SIM_DT);
            c->acc = fmod(c->acc, SIM_DT);
            break;
        }
        c->acc -= SIM_DT;
        steps++;
        c->total_steps++;
    }
    return steps;
}

/* ---- sim: a player with left/right/jump inputs and rng wind gusts ---- */
#define IN_LEFT  0x1
#define IN_RIGHT 0x2
#define IN_JUMP  0x4

#define FLAG_GROUND 0x1

typedef struct {
    double x, y, vx, vy;
    int on_ground;
    long long tick;
    Stream rng;
} Sim;

static void sim_init(Sim* s, uint64_t seed) {
    memset(s, 0, sizeof(*s));
    s->on_ground = 1;
    stream_init(&s->rng, seed);
}

static void sim_step(Sim* s, unsigned input) {
    if (s->tick % 90 == 0) {            /* deterministic gust */
        int g = (int)stream_range(&s->rng, 3) - 1;
        s->vx += g * 0.5;
    }
    double ax = 0.0;
    if (input & IN_LEFT)  ax -= 40.0;
    if (input & IN_RIGHT) ax += 40.0;
    s->vx += ax * SIM_DT;
    s->vx *= s->on_ground ? 0.90 : 0.99;
    if ((input & IN_JUMP) && s->on_ground) {
        s->vy = 9.0;
        s->on_ground = 0;
    }
    s->vy -= 25.0 * SIM_DT;
    s->x += s->vx * SIM_DT;
    s->y += s->vy * SIM_DT;
    if (s->y <= 0.0) {
        s->y = 0.0;
        s->vy = 0.0;
        s->on_ground = 1;
    }
    s->tick++;
}

static unsigned sim_flags(const Sim* s) {
    return s->on_ground ? FLAG_GROUND : 0u;
}

/* ---- recording: "replay 1" / "seed N" / "frame i t SEC input HEX" ---- */
typedef struct {
    double t;
    unsigned input;
} Frame;

typedef struct {
    uint64_t seed;
    Frame* frames;
    size_t n;
    size_t cap;
} Rec;

static void rec_free(Rec* r) {
    free(r->frames);
    r->frames = NULL;
    r->n = r->cap = 0;
}

static int rec_push(Rec* r, Frame f) {
    if (r->n == r->cap) {
        size_t cap = r->cap ? r->cap * 2 : 256;
        Frame* p = realloc(r->frames, cap * sizeof(*p));
        if (!p) return -1;
        r->frames = p;
        r->cap = cap;
    }
    r->frames[r->n++] = f;
    return 0;
}

/* Load a recording. Returns 0 on ok, -1 on error (err filled). */
static int rec_load(const char* path, Rec* r, char* err, size_t errsz) {
    FILE* fp = fopen(path, "r");
    if (!fp) {
        snprintf(err, errsz, "cannot open %s", path);
        return -1;
    }
    memset(r, 0, sizeof(*r));
    char line[256];
    long lineno = 0;
    int got_header = 0, got_seed = 0;
    long long expect = 0;
    double prev_t = 0.0;
    int rc = -1;

    while (fgets(line, sizeof(line), fp)) {
        lineno++;
        if (line[0] == '#' || line[0] == '\n' || line[0] == '\0') continue;
        if (!got_header) {
            int v = 0;
            if (sscanf(line, "replay %d", &v) != 1 || v != 1) {
                snprintf(err, errsz, "%s:%ld: expected 'replay 1' header", path, lineno);
                goto out;
            }
            got_header = 1;
            continue;
        }
        if (!got_seed) {
            unsigned long long seed = 0;
            if (sscanf(line, "seed %llu", &seed) != 1) {
                snprintf(err, errsz, "%s:%ld: expected 'seed N'", path, lineno);
                goto out;
            }
            r->seed = (uint64_t)seed;
            got_seed = 1;
            continue;
        }
        long long idx = -1;
        double t = 0.0;
        unsigned input = 0;
        char extra[8];
        if (sscanf(line, "frame %lld t %lf input %x %7s",
                   &idx, &t, &input, extra) != 3) {
            snprintf(err, errsz, "%s:%ld: malformed frame line", path, lineno);
            goto out;
        }
        if (idx != expect) {
            snprintf(err, errsz,
                     "%s:%ld: missing frame: expected frame %lld, got %lld",
                     path, lineno, expect, idx);
            goto out;
        }
        if (t < prev_t) {
            snprintf(err, errsz,
                     "%s:%ld: timestamp went backwards: frame %lld t=%.6f < %.6f",
                     path, lineno, idx, t, prev_t);
            goto out;
        }
        Frame f = { t, input };
        if (rec_push(r, f) != 0) {
            snprintf(err, errsz, "out of memory");
            goto out;
        }
        prev_t = t;
        expect++;
    }
    if (!got_header || !got_seed) {
        snprintf(err, errsz, "%s: incomplete header", path);
        goto out;
    }
    rc = 0;
out:
    fclose(fp);
    if (rc != 0) rec_free(r);
    return rc;
}

/* Save the first n frames of a recording (n <= r->n). */
static int rec_save(const char* path, const Rec* r, size_t n) {
    FILE* fp = fopen(path, "w");
    if (!fp) return -1;
    fprintf(fp, "replay 1\nseed %llu\n", (unsigned long long)r->seed);
    for (size_t i = 0; i < n; i++)
        fprintf(fp, "frame %zu t %.6f input %x\n", i, r->frames[i].t, r->frames[i].input);
    if (fclose(fp) != 0) return -1;
    return 0;
}

/* ---- trajectory: one line per frame, written by record, read by verify ---- */
typedef struct {
    long long frame;
    long long tick;
    double x, y, vx, vy;
    unsigned flags;
    uint64_t rng;
} TFrame;

typedef struct {
    uint64_t seed;
    TFrame* frames;
    size_t n;
    size_t cap;
} Traj;

static void traj_free(Traj* t) {
    free(t->frames);
    t->frames = NULL;
    t->n = t->cap = 0;
}

static int traj_push(Traj* t, TFrame f) {
    if (t->n == t->cap) {
        size_t cap = t->cap ? t->cap * 2 : 256;
        TFrame* p = realloc(t->frames, cap * sizeof(*p));
        if (!p) return -1;
        t->frames = p;
        t->cap = cap;
    }
    t->frames[t->n++] = f;
    return 0;
}

/* Run the sim over a recording, appending one trajectory frame per input frame. */
static int sim_run(const Rec* r, Traj* out) {
    Sim s;
    Clock c;
    sim_init(&s, r->seed);
    clock_init(&c);
    memset(out, 0, sizeof(*out));
    double prev_t = 0.0;
    for (size_t i = 0; i < r->n; i++) {
        double dt = r->frames[i].t - prev_t;
        prev_t = r->frames[i].t;
        int steps = clock_advance(&c, dt);
        for (int k = 0; k < steps; k++) sim_step(&s, r->frames[i].input);
        TFrame tf;
        tf.frame = (long long)i;
        tf.tick = s.tick;
        tf.x = s.x; tf.y = s.y;
        tf.vx = s.vx; tf.vy = s.vy;
        tf.flags = sim_flags(&s);
        tf.rng = s.rng.state;
        if (traj_push(out, tf) != 0) { traj_free(out); return -1; }
    }
    return 0;
}

static int traj_save(const char* path, const Traj* t, uint64_t seed) {
    FILE* fp = fopen(path, "w");
    if (!fp) return -1;
    fprintf(fp, "traj 1\nseed %llu\n", (unsigned long long)seed);
    for (size_t i = 0; i < t->n; i++) {
        const TFrame* f = &t->frames[i];
        fprintf(fp, "frame %lld tick %lld x %.9f y %.9f vx %.9f vy %.9f flags %x rng %016llx\n",
                f->frame, f->tick, f->x, f->y, f->vx, f->vy, f->flags,
                (unsigned long long)f->rng);
    }
    if (fclose(fp) != 0) return -1;
    return 0;
}

static int traj_load(const char* path, Traj* t, uint64_t* seed, char* err, size_t errsz) {
    FILE* fp = fopen(path, "r");
    if (!fp) {
        snprintf(err, errsz, "cannot open %s", path);
        return -1;
    }
    memset(t, 0, sizeof(*t));
    char line[320];
    long lineno = 0;
    int got_header = 0, got_seed = 0;
    long long expect = 0;
    int rc = -1;

    while (fgets(line, sizeof(line), fp)) {
        lineno++;
        if (line[0] == '#' || line[0] == '\n' || line[0] == '\0') continue;
        if (!got_header) {
            int v = 0;
            if (sscanf(line, "traj %d", &v) != 1 || v != 1) {
                snprintf(err, errsz, "%s:%ld: expected 'traj 1' header", path, lineno);
                goto out;
            }
            got_header = 1;
            continue;
        }
        if (!got_seed) {
            unsigned long long sv = 0;
            if (sscanf(line, "seed %llu", &sv) != 1) {
                snprintf(err, errsz, "%s:%ld: expected 'seed N'", path, lineno);
                goto out;
            }
            *seed = (uint64_t)sv;
            got_seed = 1;
            continue;
        }
        TFrame f;
        unsigned long long rng = 0;
        if (sscanf(line, "frame %lld tick %lld x %lf y %lf vx %lf vy %lf flags %x rng %llx",
                   &f.frame, &f.tick, &f.x, &f.y, &f.vx, &f.vy, &f.flags, &rng) != 8) {
            snprintf(err, errsz, "%s:%ld: malformed trajectory line", path, lineno);
            goto out;
        }
        f.rng = (uint64_t)rng;
        if (f.frame != expect) {
            snprintf(err, errsz,
                     "%s:%ld: missing frame: expected frame %lld, got %lld",
                     path, lineno, expect, f.frame);
            goto out;
        }
        if (traj_push(t, f) != 0) {
            snprintf(err, errsz, "out of memory");
            goto out;
        }
        expect++;
    }
    if (!got_header || !got_seed) {
        snprintf(err, errsz, "%s: incomplete header", path);
        goto out;
    }
    rc = 0;
out:
    fclose(fp);
    if (rc != 0) traj_free(t);
    return rc;
}

/* ---- compare ---- */
#define POS_EPS 1e-6

/* Returns 0 if identical, and fills *why with the first differing field. */
static int frame_diff(const TFrame* a, const TFrame* b, const char** why) {
    if (a->tick != b->tick) { *why = "tick"; return 1; }
    if (fabs(a->x - b->x) > POS_EPS) { *why = "x"; return 1; }
    if (fabs(a->y - b->y) > POS_EPS) { *why = "y"; return 1; }
    if (fabs(a->vx - b->vx) > POS_EPS) { *why = "vx"; return 1; }
    if (fabs(a->vy - b->vy) > POS_EPS) { *why = "vy"; return 1; }
    if (a->flags != b->flags) { *why = "flags"; return 1; }
    if (a->rng != b->rng) { *why = "rng"; return 1; }
    return 0;
}

/* ---- modes ---- */
static int cmd_gen(const char* seed_s, const char* frames_s, const char* out_path) {
    uint64_t seed = strtoull(seed_s, NULL, 0);
    long frames = strtol(frames_s, NULL, 0);
    if (frames <= 0) {
        fprintf(stderr, "error: frames must be > 0\n");
        return 2;
    }
    Stream rng;
    stream_init(&rng, seed ^ 0x5EC0ULL);
    Rec r;
    memset(&r, 0, sizeof(r));
    r.seed = seed;
    unsigned prev_input = 0;
    for (long i = 0; i < frames; i++) {
        /* mostly hold the previous input, sometimes pick a new one */
        unsigned input = prev_input;
        if (stream_range(&rng, 10) < 3)
            input = stream_range(&rng, 8);
        Frame f = { (double)(i + 1) / 60.0, input };
        if (rec_push(&r, f) != 0) {
            fprintf(stderr, "error: out of memory\n");
            rec_free(&r);
            return 2;
        }
        prev_input = input;
    }
    int rc = 0;
    if (rec_save(out_path, &r, r.n) != 0) {
        fprintf(stderr, "error: cannot write %s\n", out_path);
        rc = 2;
    }
    rec_free(&r);
    return rc;
}

static int cmd_record(const char* rec_path, const char* traj_path) {
    char err[256];
    Rec r;
    if (rec_load(rec_path, &r, err, sizeof(err)) != 0) {
        fprintf(stderr, "error: %s\n", err);
        return 2;
    }
    Traj t;
    if (sim_run(&r, &t) != 0) {
        fprintf(stderr, "error: out of memory\n");
        rec_free(&r);
        return 2;
    }
    int rc = 0;
    if (traj_save(traj_path, &t, r.seed) != 0) {
        fprintf(stderr, "error: cannot write %s\n", traj_path);
        rc = 2;
    } else {
        printf("recorded %zu frames -> %s\n", t.n, traj_path);
    }
    traj_free(&t);
    rec_free(&r);
    return rc;
}

static int cmd_verify(const char* rec_path, const char* traj_path, const char* repro_path) {
    char err[256];
    Rec r;
    if (rec_load(rec_path, &r, err, sizeof(err)) != 0) {
        fprintf(stderr, "error: %s\n", err);
        return 2;
    }
    Traj base;
    uint64_t base_seed = 0;
    if (traj_load(traj_path, &base, &base_seed, err, sizeof(err)) != 0) {
        fprintf(stderr, "error: %s\n", err);
        rec_free(&r);
        return 2;
    }
    if (base_seed != r.seed) {
        fprintf(stderr, "error: seed mismatch: recording %llu, baseline %llu\n",
                (unsigned long long)r.seed, (unsigned long long)base_seed);
        traj_free(&base);
        rec_free(&r);
        return 2;
    }
    Traj got;
    if (sim_run(&r, &got) != 0) {
        fprintf(stderr, "error: out of memory\n");
        traj_free(&base);
        rec_free(&r);
        return 2;
    }

    size_t common = got.n < base.n ? got.n : base.n;
    long diverge_at = -1;
    const char* why = "";
    for (size_t i = 0; i < common; i++) {
        if (frame_diff(&got.frames[i], &base.frames[i], &why)) {
            diverge_at = (long)i;
            break;
        }
    }
    if (diverge_at < 0 && got.n != base.n) {
        diverge_at = (long)common;
        why = "length";
    }

    if (diverge_at < 0) {
        printf("match %zu frames\n", got.n);
        traj_free(&got);
        traj_free(&base);
        rec_free(&r);
        return 0;
    }

    /* divergence: report, and write inputs truncated to the diverging frame */
    long f = diverge_at;
    if (f < (long)got.n && f < (long)base.n) {
        printf("diverge frame %ld field %s: got x=%.6f y=%.6f vx=%.6f vy=%.6f flags=%x rng=%016llx, "
               "want x=%.6f y=%.6f vx=%.6f vy=%.6f flags=%x rng=%016llx\n",
               f, why,
               got.frames[f].x, got.frames[f].y, got.frames[f].vx, got.frames[f].vy,
               got.frames[f].flags, (unsigned long long)got.frames[f].rng,
               base.frames[f].x, base.frames[f].y, base.frames[f].vx, base.frames[f].vy,
               base.frames[f].flags, (unsigned long long)base.frames[f].rng);
    } else {
        printf("diverge frame %ld field %s: recording has %zu frames, baseline has %zu\n",
               f, why, got.n, base.n);
    }
    if (repro_path) {
        /* keep frames 0..diverge_at inclusive so the repro still diverges */
        size_t keep = (size_t)diverge_at + 1;
        if (keep > r.n) keep = r.n;
        if (rec_save(repro_path, &r, keep) != 0) {
            fprintf(stderr, "error: cannot write repro %s\n", repro_path);
        } else {
            printf("repro %s: %zu frames (truncated at frame %ld)\n",
                   repro_path, keep, diverge_at);
        }
    }
    traj_free(&got);
    traj_free(&base);
    rec_free(&r);
    return 1;
}

int main(int argc, char** argv) {
    if (argc >= 2 && strcmp(argv[1], "gen") == 0 && argc == 5)
        return cmd_gen(argv[2], argv[3], argv[4]);
    if (argc >= 2 && strcmp(argv[1], "record") == 0 && argc == 4)
        return cmd_record(argv[2], argv[3]);
    if (argc >= 2 && strcmp(argv[1], "verify") == 0 && (argc == 4 || argc == 5))
        return cmd_verify(argv[2], argv[3], argc == 5 ? argv[4] : NULL);
    fprintf(stderr,
            "usage:\n"
            "  replay gen <seed> <frames> <out.rec>\n"
            "  replay record <in.rec> <out.traj>\n"
            "  replay verify <in.rec> <base.traj> [repro.rec]\n");
    return 2;
}

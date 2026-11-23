
/* fixedstep: fixed timestep accumulator with interpolation alpha and catch-up cap. */
#include "fixedstep.h"
#include <math.h>

void clock_init(Clock* c, double step, int max_catchup) {
    c->step = step;
    c->acc = 0.0;
    c->alpha = 0.0;
    c->max_catchup = max_catchup;
    c->total_steps = 0;
    c->dropped = 0;
    c->step_fp = 0;
    c->acc_fp = 0;
#ifdef FIXEDSTEP_LEGACY
    /* legacy double accumulator; fp fields stay unused */
    return;
#else
    if (step > 0.0)
        c->step_fp = (long long)llround(step * (double)CLOCK_FP_SCALE);
#endif
}

#ifdef FIXEDSTEP_LEGACY
/* ---- pre-fix implementation, retained for drift reproduction ------------- */
int clock_drain_ns(Clock* c, long long frame_dt_ns) {
    if (frame_dt_ns < 0) return -1;
    return clock_advance_ns(c, frame_dt_ns);
}

int clock_advance_ns(Clock* c, long long frame_dt_ns) {
    if (frame_dt_ns < 0) return -1;
    return clock_advance(c, (double)frame_dt_ns / (double)CLOCK_FP_SCALE);
}

int clock_unstep(Clock* c, int n) {
    (void)c; (void)n;
    /* legacy driver never drains; nothing to rewind */
    return -1;
}

int clock_advance(Clock* c, double frame_dt) {
    if (frame_dt < 0.0) return -1;
    c->acc += frame_dt;
    int steps = 0;
    while (c->acc >= c->step) {
        if (steps >= c->max_catchup) {
            /* discard the backlog instead of spiralling */
            c->dropped += (int)(c->acc / c->step);
            c->acc = fmod(c->acc, c->step);
            break;
        }
        c->acc -= c->step;
        steps++;
        c->total_steps++;
    }
    c->alpha = c->step > 0.0 ? c->acc / c->step : 0.0;
    return steps;
}
#else
/* ---- deterministic fixed-point implementation ---------------------------- */
int clock_advance_ns(Clock* c, long long frame_dt_ns) {
    if (frame_dt_ns < 0) return -1;
    if (c->step_fp <= 0) {
        /* degenerate clock: never step, never divide */
        c->acc = 0.0;
        c->alpha = 0.0;
        return 0;
    }

    c->acc_fp += frame_dt_ns;

    int steps = 0;
    while (c->acc_fp >= c->step_fp) {
        if (steps >= c->max_catchup) {
            long long backlog = c->acc_fp / c->step_fp;
            c->dropped += (int)backlog;
            c->acc_fp %= c->step_fp;
            break;
        }
        c->acc_fp -= c->step_fp;
        steps++;
        c->total_steps++;
    }

    c->acc = (double)c->acc_fp / (double)CLOCK_FP_SCALE;
    c->alpha = (double)c->acc_fp / (double)c->step_fp;
    return steps;
}

int clock_advance(Clock* c, double frame_dt) {
    if (frame_dt < 0.0) return -1;
    /* round-to-nearest quantisation on entry, then delegate */
    return clock_advance_ns(c,
        (long long)llround(frame_dt * (double)CLOCK_FP_SCALE));
}

/* uncapped: emit every step the accumulator holds, never drop */
int clock_drain_ns(Clock* c, long long frame_dt_ns) {
    if (frame_dt_ns < 0) return -1;
    if (c->step_fp <= 0) {
        c->acc = 0.0;
        c->alpha = 0.0;
        return 0;
    }
    c->acc_fp += frame_dt_ns;
    int steps = 0;
    while (c->acc_fp >= c->step_fp) {
        c->acc_fp -= c->step_fp;
        steps++;
        c->total_steps++;
    }
    c->acc = (double)c->acc_fp / (double)CLOCK_FP_SCALE;
    c->alpha = (double)c->acc_fp / (double)c->step_fp;
    return steps;
}

int clock_unstep(Clock* c, int n) {
    if (n < 0 || (long long)n > c->total_steps || c->step_fp <= 0)
        return -1;
    c->acc_fp += (long long)n * c->step_fp;
    c->total_steps -= n;
    c->acc = (double)c->acc_fp / (double)CLOCK_FP_SCALE;
    c->alpha = (double)c->acc_fp / (double)c->step_fp;
    return 0;
}
#endif

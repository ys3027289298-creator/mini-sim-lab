/* fixedstep: fixed timestep accumulator with interpolation alpha and catch-up cap. */
#include "fixedstep.h"

#include <math.h>

void fstep_init(FstepClock* c, double step, int max_catchup) {
    c->step = step;
    c->acc = 0.0;
    c->alpha = 0.0;
    c->max_catchup = max_catchup;
    c->total_steps = 0;
    c->dropped = 0;
}

size_t fstep_state_size(void) { return sizeof(FstepClock); }

int fstep_advance(FstepClock* c, double frame_dt) {
    if (!isfinite(frame_dt) || frame_dt < 0.0) return -1;
    if (!(c->step > 0.0)) {
        /* No fixed step exists; do not divide or modulo by zero. */
        c->alpha = 0.0;
        return 0;
    }
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
    c->alpha = c->acc / c->step;
    return steps;
}

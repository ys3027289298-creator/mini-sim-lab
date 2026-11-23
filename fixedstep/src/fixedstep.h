/* fixedstep: fixed timestep accumulator with interpolation alpha and catch-up cap. */
#ifndef FIXEDSTEP_H
#define FIXEDSTEP_H

#include <stddef.h>

typedef struct {
    double step;        /* fixed dt */
    double acc;
    double alpha;       /* interpolation factor for rendering */
    int max_catchup;    /* max steps per frame */
    long long total_steps;
    int dropped;        /* steps discarded due to catch-up cap */
} FstepClock;

void fstep_init(FstepClock* c, double step, int max_catchup);

/* Advances one frame.  Returns the number of fixed steps taken, or -1 if the
 * frame delta is negative or not finite.  A non-positive step advances
 * nothing: there is no fixed step to take. */
int fstep_advance(FstepClock* c, double frame_dt);

size_t fstep_state_size(void);

#endif /* FIXEDSTEP_H */

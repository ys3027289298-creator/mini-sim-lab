
/* fixedstep: fixed timestep accumulator with interpolation alpha and catch-up cap. */
#ifndef FIXEDSTEP_H
#define FIXEDSTEP_H

typedef struct {
    double step;        /* fixed dt */
    double acc;
    double alpha;       /* interpolation factor for rendering */
    int max_catchup;    /* max steps per frame */
    long long total_steps;
    int dropped;        /* steps discarded due to catch-up cap */
} Clock;

void clock_init(Clock* c, double step, int max_catchup);

/* Returns number of fixed steps consumed this frame, -1 on negative dt. */
int clock_advance(Clock* c, double frame_dt);

#endif /* FIXEDSTEP_H */
